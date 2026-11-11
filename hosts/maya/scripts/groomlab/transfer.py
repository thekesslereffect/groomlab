# SPDX-License-Identifier: Apache-2.0
"""Moving a groom onto another scalp mesh: guides, parts, flow, maps and color sets."""

import json
import math

from groomlab.base import no_undo
from maya import cmds

from groomlab.base import ensure_plugin, undo_chunk, _original_shape, _scalp_shape, _shape_of
from groomlab.params import get_graph
from groomlab.groom import clear_rest_mesh, set_rest_mesh

# The transfer methods (engine transfer.h), in the order the dialogs list them.
METHODS = ("closest", "bounds", "markersAlign", "markersWarp", "uv")
MARKER_METHODS = ("markersAlign", "markersWarp")
METHOD_LABELS = {
    "closest": "Closest point",
    "bounds": "Fit bounding boxes",
    "markersAlign": "Markers: align (move, turn, scale)",
    "markersWarp": "Markers: warp (follows proportions)",
    "uv": "Matching UVs",
}


def transfer_groom(groom, scalp, fit=False, maps=True, color_sets=False, report=None, method=None, markers=None,
                   keep_lengths=False, fit_size=False):
    """Move a groom to another scalp mesh: a new sculpt, a retopologized head, another character.

    Guides, parts and flow arrows are re-rooted on the new scalp at the current frame (do it at
    the bind pose); their shapes turn with the surface, and guide layers ride with the guides.
    Where they land is the transfer `method` (engine transfer.h):

      "closest"       the closest point of the new scalp (the default)
      "bounds"        the old scalp's bounding box moved and scaled onto the new one's first
                      (what `fit` asks for when no method is given)
      "markersAlign"  3 or more marker pairs give the best move, turn and uniform scale
      "markersWarp"   4 or more pairs (not on one plane) bend space so each marker lands on its
                      pair: a head of other proportions; fewer falls back to align, warned about
      "uv"            both meshes laid out alike in UV: each root at its UV on the new scalp

    `markers`: the pairs, each (old, new) or {"old": old, "new": new}; a side is a world point
    (x, y, z), a mesh component ("head.vtx[12]", its world position) or a stored side
    {"face", "u", "v", "point"}. Or the JSON text get_transfer_markers gives. The pairs are
    snapped onto the two meshes and stored on the groom (transferMarkers, saved in .groom files);
    None reuses the stored ones. With every method but closest a guide's shape turns and scales
    with the mapping at its root; `keep_lengths` keeps its length (it only turns).

    `fit_size` fits the groom to the new scalp's size: the new scalp is taken as the old one
    resized, so the groom's scale becomes k * sqrt(new area / old area) (its scale reference kept):
    lengths, widths and clumps follow, densities keep the strand count and guide shapes scale about
    their roots (they turn with the mapping without scaling, as with `keep_lengths`). Off (the
    default): the groom keeps the size shown (its reference set again for the new scalp).

    `maps` (on by default) projects the painted scalp maps onto the new scalp by position: a
    vertex map gets values for the new vertices, a texture map a new image at the new scalp's
    UVs, written under the groom's masks folder with a free name (the old file is left as it
    is: other grooms or the old scene may read it). Off, vertex maps stay with the old mesh's
    vertices and texture maps keep their image as it lies on the old UVs.

    `color_sets` also copies the Maya color sets the graph reads (by name) from the old scalp
    mesh onto the new one (transferAttributes by closest point, after the same fit, its history
    then baked into the new mesh); a new scalp with deformers is left alone.

    One undo step. Returns what could not be carried, sorted: maps that stayed behind and color
    sets the graph reads that the new scalp lacks. `report`, a dict, is filled with "projected"
    (maps projected), "colorSets" (color sets copied) and "missing" (the same as returned).
    """
    ensure_plugin()
    mesh = _shape_of(scalp, "mesh")
    old_mesh = _scalp_shape(groom)
    from groomlab import utils
    read = utils.graph_maps(get_graph(groom) or json.loads(cmds.groomLabDefaultGraph()))
    before = _scalp_map_entries(groom)
    old_sets = set(cmds.polyColorSet(old_mesh, query=True, allColorSets=True) or []) if old_mesh else set()
    copied_sets, stuck_sets = [], []
    method = transfer_method(method, fit)
    flags = {"scalp": mesh, "method": method, "maps": bool(maps)}
    if keep_lengths:
        flags["keepLengths"] = True
    if markers is not None:
        flags["markers"] = markers_json(markers)
    scaling = cmds.getAttr(groom + ".scaleSource") != 2
    k = cmds.getAttr(groom + ".groomScale") if scaling else 1.0  # the guides move as shown
    old_area = cmds.getAttr(groom + ".scaleArea") if scaling else 0.0
    if fit_size and scaling:
        flags["keepLengths"] = True  # (the groom's scale grows them: the mapping only turns them)
    with undo_chunk("transfer"):
        clear_rest_mesh(groom)  # the old scalp's (its placement node too)
        cmds.groomLabTransfer(groom, **flags)
        if _original_shape(mesh):
            set_rest_mesh(groom)
        # The groom keeps the scale it had (its params and guides look as they did): the new
        # scalp's size is k times the groom's 1x. Fitting the size: k grows with the area, the
        # new scalp the old one resized (k * sqrt(new area / old area)).
        if scaling and k > 0.0:
            area = cmds.getAttr(groom + ".scaleArea")
            if area > 0.0:
                target = k * math.sqrt(area / old_area) if fit_size and old_area > 0.0 else k
                cmds.setAttr(groom + ".scaleReference", area / (target * target))
                transform = cmds.getAttr(groom + ".scaleTransform")
                if transform > 0.0:
                    cmds.setAttr(groom + ".scaleTransformReference", transform / target)
        if color_sets and old_mesh:
            copied_sets, stuck_sets = _copy_color_sets(old_mesh, mesh, _color_sets_read(read, old_mesh, mesh, before),
                                                       method, get_transfer_markers(groom))
    after = _scalp_map_entries(groom)
    projected = sorted(n for n, e in after.items() if n in before and e != before[n]) if maps else []
    # What the graph (or, with maps, the groom) holds that did not come along: a vertex map without
    # a value per vertex of the new scalp, a texture map left on the old UVs (with maps), a Maya
    # color set of the old mesh the graph reads. Guide layers travel with the guides.
    have = set(cmds.polyColorSet(mesh, query=True, allColorSets=True) or [])
    count = cmds.polyEvaluate(mesh, vertex=True)
    missing = set(stuck_sets)
    for name, entry in after.items():
        channels = 4 if entry.get("channels") == 4 else 1
        fits = entry.get("kind") != "vertices" or len(entry.get("values") or []) == count * channels
        if fits:
            have.add(name)
        if maps and (not fits or (entry.get("kind") == "texture" and name not in projected)):
            missing.add(name)
        elif not fits and read.get(name) == "map":
            missing.add(name)
    for name, how in read.items():
        if name not in have and (how == "colorSet" or (name in old_sets and name not in after)):
            missing.add(name)
    if report is not None:
        report.update({"projected": projected, "colorSets": copied_sets, "missing": sorted(missing)})
    return sorted(missing)


def transfer_method(method=None, fit=False):
    """The method a transfer uses: `method` when given (one of METHODS), else bounds with `fit`."""
    if method is None:
        return "bounds" if fit else "closest"
    if method not in METHODS:
        raise ValueError("transfer method %r: one of %s" % (method, ", ".join(METHODS)))
    return method


def _side(side):
    """One side of a marker pair as stored: {"point": [x, y, z]} (and face, u, v when known)."""
    if isinstance(side, dict):
        out = {k: side[k] for k in ("face", "u", "v") if k in side}
        if "point" in side:
            out["point"] = [float(c) for c in side["point"]]
        if "face" not in out and "point" not in out:
            raise ValueError("a marker side needs a point or a face: %r" % (side,))
        return out
    if isinstance(side, str):
        if not cmds.objExists(side):
            raise ValueError("%s does not exist" % side)
        pts = cmds.xform(side, query=True, translation=True, worldSpace=True)
        n = len(pts) // 3
        if n == 0:
            raise ValueError("%s is not a point of a mesh" % side)
        return {"point": [sum(pts[i * 3 + k] for i in range(n)) / n for k in range(3)]}
    point = [float(c) for c in side]
    if len(point) != 3:
        raise ValueError("a marker point has 3 coordinates: %r" % (side,))
    return {"point": point}


def markers_json(markers):
    """Marker pairs as the JSON groomLabTransfer -markers takes (transfer.h): from JSON text, or a
    list of (old, new) / {"old", "new"} pairs whose sides are points, components or stored sides."""
    if isinstance(markers, str):
        return json.dumps(json.loads(markers or "[]"))
    out = []
    for pair in markers or []:
        old, new = (pair["old"], pair["new"]) if isinstance(pair, dict) else pair
        out.append({"old": _side(old), "new": _side(new)})
    return json.dumps(out)


def get_transfer_markers(groom):
    """The groom's stored marker pairs (its last transfer's, or set_transfer_markers'): a list of
    {"old": {"face", "u", "v", "point"}, "new": {...}}."""
    text = cmds.getAttr(_shape(groom) + ".transferMarkers") or ""
    return json.loads(text) if text.strip() else []


def set_transfer_markers(groom, markers):
    """Stores marker pairs on the groom (markers_json's forms; [] clears them), for a transfer
    that reuses them. Undoable."""
    cmds.setAttr(_shape(groom) + ".transferMarkers", markers_json(markers) if markers else "", type="string")


def snap_markers(groom, scalp, markers):
    """The marker pairs with each old side on the groom's scalp and each new side on `scalp`
    (the closest points: a face and barycentrics, and the point there). Changes nothing."""
    ensure_plugin()
    text = cmds.groomLabTransfer(groom, scalp=_shape_of(scalp, "mesh"), markers=markers_json(markers), snapMarkers=True)
    return json.loads(text or "[]")


def pair_from_selection(groom, scalp, selection=None):
    """The marker pair a selection gives (Add Pair From Selection): points on the groom's scalp
    and on the new `scalp`, each side the components selected on that mesh (a vertex, or the
    middle of an edge, a face or several), whichever order they were picked in. `selection`
    (default: Maya's, flattened) may also hold transforms of locators or other points' names.
    Returns the pair snapped onto the meshes ({"old": {...}, "new": {...}}); ValueError when
    either mesh has nothing selected."""
    old_shape = cmds.ls(_scalp_shape(groom) or [], long=True)
    new_shape = cmds.ls(_shape_of(scalp, "mesh"), long=True)
    items = selection if selection is not None else cmds.ls(selection=True, flatten=True, long=True) or []
    old, new = [], []
    for item in items:
        if "." not in item:
            continue  # a whole object: not a point
        node = item.split(".")[0]
        shapes = cmds.ls(node, type="mesh", long=True) or cmds.listRelatives(node, shapes=True, type="mesh",
                                                                             fullPath=True) or []
        if set(shapes) & set(old_shape):
            old.append(item)
        elif set(shapes) & set(new_shape):
            new.append(item)
    if not old or not new:
        old_name = (old_shape or ["the groom's scalp"])[0].split("|")[-1]
        new_name = new_shape[0].split("|")[-1]
        raise ValueError("select a point (a vertex, edge or face) on %s and one on %s, then add the pair" %
                         (old_name, new_name))
    return snap_markers(groom, scalp, [(_side_of(old), _side_of(new))])[0]


def _side_of(components):
    """The middle of some components' points, world space."""
    points = []
    for c in components:
        pts = cmds.xform(c, query=True, translation=True, worldSpace=True) or []
        points += [pts[i:i + 3] for i in range(0, len(pts) - 2, 3)]
    return [sum(p[k] for p in points) / len(points) for k in range(3)]


# The markers shown while the Transfer Groom window is open: a locator per side (old yellow, new
# cyan) with its number, in one group outside the undo queue, deleted when the window closes.
MARKER_GROUP = "groomLabTransferMarkers"


def show_markers(groom, scalp, markers):
    """Shows marker pairs as numbered locators on both meshes (replacing any shown). Returns the
    group (None without markers)."""
    clear_markers()
    if not markers:
        return None
    meshes = [m for m in (_scalp_shape(groom), _shape_of(scalp, "mesh") if scalp else None) if m]
    size = 1.0
    if meshes:
        box = cmds.exactWorldBoundingBox(meshes)
        size = max(sum((box[i + 3] - box[i]) ** 2 for i in range(3)) ** 0.5, 1e-6)
    with no_undo():
        group = cmds.group(empty=True, name=MARKER_GROUP)
        for i, pair in enumerate(markers):
            for side, color in (("old", 17), ("new", 18)):
                point = pair[side].get("point")
                if point is None:
                    continue
                loc = cmds.spaceLocator(name="glMarker%s%d" % (side.capitalize(), i + 1))[0]
                cmds.xform(loc, translation=point, worldSpace=True)
                for axis in "XYZ":
                    cmds.setAttr("%s.localScale%s" % (loc, axis), size * 0.015)
                cmds.setAttr(loc + ".overrideEnabled", True)
                cmds.setAttr(loc + ".overrideColor", color)
                note = cmds.annotate(loc, text=str(i + 1), point=[point[0], point[1] + size * 0.03, point[2]])
                note = cmds.listRelatives(note, parent=True, fullPath=True)[0]
                cmds.setAttr(cmds.listRelatives(note, shapes=True, fullPath=True)[0] + ".displayArrow", False)
                cmds.parent([loc, note], group)
        return cmds.ls(group, long=True)[0]


def clear_markers():
    """Removes the shown markers (show_markers' group)."""
    if cmds.objExists(MARKER_GROUP):
        with no_undo():
            cmds.delete(MARKER_GROUP)


# ---- Picking points on a mesh in a view (the Transfer Groom window's marker views) -------------

def _mesh_fn(mesh):
    from maya.api import OpenMaya as om
    sel = om.MSelectionList()
    sel.add(_shape_of(mesh, "mesh"))
    return om.MFnMesh(sel.getDagPath(0))


def view_ray(view, x, y):
    """The world ray under port pixel (x, y) of an M3dView (maya.api.OpenMayaUI; origin bottom left):
    (origin, unit direction) as 3-tuples."""
    from maya.api import OpenMaya as om
    near, far = om.MPoint(), om.MPoint()
    view.viewToWorld(int(round(x)), int(round(y)), near, far)
    d = far - near
    length = d.length() or 1.0
    return (near.x, near.y, near.z), (d.x / length, d.y / length, d.z / length)


def pick_mesh(mesh, origin, direction):
    """The ray's nearest hit on `mesh` (world space): {"point": [x, y, z], "face": Maya polygon,
    "triangle": its triangle, "u", "v": barycentrics in that triangle}, or None on a miss."""
    from maya.api import OpenMaya as om
    hit = _mesh_fn(mesh).closestIntersection(om.MFloatPoint(*origin), om.MFloatVector(*direction), om.MSpace.kWorld,
                                             1e9, False)
    if not hit or hit[2] < 0:
        return None
    p = hit[0]
    return {"point": [p.x, p.y, p.z], "face": hit[2], "triangle": hit[3], "u": hit[4], "v": hit[5]}


def pick_in_view(view, mesh, x, y):
    """The point of `mesh` under port pixel (x, y) of an M3dView (pick_mesh's dict), or None."""
    origin, direction = view_ray(view, x, y)
    return pick_mesh(mesh, origin, direction)


def closest_on_mesh(mesh, point):
    """The closest point of `mesh` (world space) to `point`, [x, y, z]."""
    from maya.api import OpenMaya as om
    p, _face = _mesh_fn(mesh).getClosestPoint(om.MPoint(*point), om.MSpace.kWorld)
    return [p.x, p.y, p.z]


def mesh_bounds(mesh):
    """(center [x, y, z], diagonal length) of a mesh's world bounding box."""
    box = cmds.exactWorldBoundingBox(mesh)
    center = [(box[i] + box[i + 3]) * 0.5 for i in range(3)]
    return center, max(sum((box[i + 3] - box[i]) ** 2 for i in range(3)) ** 0.5, 1e-6)


def describe_marker(i, pair):
    """A marker pair as the Markers list shows it: "1: old face 12 (x, y, z) -> new face 40 (x, y, z)"."""
    def side(s):
        p = s.get("point")
        where = "(%.2f, %.2f, %.2f)" % tuple(p) if p else ""
        return ("face %d %s" % (s["face"], where) if "face" in s else where).strip()
    return "%d: old %s -> new %s" % (i + 1, side(pair["old"]), side(pair["new"]))


def _shape(groom):
    from groomlab.base import GROOM_TYPE
    shapes = cmds.ls(groom, type=GROOM_TYPE, long=True) or cmds.listRelatives(groom, shapes=True, type=GROOM_TYPE,
                                                                               fullPath=True) or []
    if not shapes:
        raise ValueError("%s is not a groom" % groom)
    return shapes[0]


def _scalp_map_entries(groom):
    """The groom's scalpMaps entries, {name: entry}."""
    return json.loads(cmds.getAttr(groom + ".scalpMaps") or "{}")


def _color_sets_read(read, old_mesh, new_mesh, entries):
    """Color sets of the old mesh the graph reads by name (not the groom's own maps, which lie on
    the scalp as color sets of their names) that the new mesh lacks."""
    old = set(cmds.polyColorSet(old_mesh, query=True, allColorSets=True) or [])
    new = set(cmds.polyColorSet(new_mesh, query=True, allColorSets=True) or [])
    return sorted(n for n in read if n in old and n not in new and n not in entries)


def _place_by_markers(node, markers):
    """Moves, turns and scales a world-parented transform by the similarity that best takes the
    markers' old points onto their new ones."""
    import groomlab_core
    from maya.api import OpenMaya as om
    old = [m["old"]["point"] for m in markers]
    new = [m["new"]["point"] for m in markers]
    s = groomlab_core.fit_similarity(old, new)
    r = [[row.x, row.y, row.z] for row in s.rotation]
    t = [s.translation.x, s.translation.y, s.translation.z]
    k = s.scale
    # Maya's row-vector matrices: p' = p M with M's upper 3x3 (k R)^T and translation t.
    m = om.MMatrix([k * r[0][0], k * r[1][0], k * r[2][0], 0.0,
                    k * r[0][1], k * r[1][1], k * r[2][1], 0.0,
                    k * r[0][2], k * r[1][2], k * r[2][2], 0.0,
                    t[0], t[1], t[2], 1.0])
    current = om.MMatrix(cmds.xform(node, query=True, matrix=True, worldSpace=True))
    cmds.xform(node, matrix=list(current * m), worldSpace=True)


def _deformed(mesh):
    return bool(cmds.ls(cmds.listHistory(mesh, pruneDagObjects=True) or [], type="geometryFilter"))


def _copy_color_sets(old_mesh, new_mesh, names, method, markers=None):
    """Copies the color sets `names` from old_mesh onto new_mesh by closest point (a copy of the
    old mesh first placed as the method places the groom: moved and scaled onto the new one's
    bounding box for bounds, moved, turned and scaled by the markers' best similarity for the
    marker methods (a warp's bend is not followed); uv samples by UV instead), with nothing left
    live on new_mesh (the transferAttributes baked). Returns (copied, not copied):
    a new mesh with deformers is not touched (its rig would be baked too)."""
    if not names:
        return [], []
    if _deformed(new_mesh):
        cmds.warning("GroomLab: %s has deformers; copy the color sets %s onto it by hand (Mesh > Transfer "
                     "Attributes, then delete its non-deformer history)" % (new_mesh, ", ".join(names)))
        return [], list(names)
    source = _bare_copy(old_mesh, "groomLabColorSource")
    copy = None
    try:
        if method in MARKER_METHODS and markers and len(markers) >= 3:
            _place_by_markers(source, markers)
        if method == "bounds":
            a, b = cmds.exactWorldBoundingBox(source), cmds.exactWorldBoundingBox(new_mesh)
            center_a = [(a[i] + a[i + 3]) * 0.5 for i in range(3)]
            center_b = [(b[i] + b[i + 3]) * 0.5 for i in range(3)]
            diag = lambda box: sum((box[i + 3] - box[i]) ** 2 for i in range(3)) ** 0.5
            scale = diag(b) / diag(a) if diag(a) > 0 else 1.0
            cmds.scale(scale, scale, scale, source, pivot=center_a, relative=True)
            cmds.move(center_b[0] - center_a[0], center_b[1] - center_a[1], center_b[2] - center_a[2], source,
                      relative=True, worldSpace=True)
        # A mesh with history takes the colors and has it baked. One without (a cleaned-up
        # head, the usual new scalp): a baked transferAttributes does not undo cleanly there
        # (Maya leaves the colors, or a redo the wrong points), so a copy of it takes them and
        # groomLabCopyColorSets makes them on the mesh as its own data, undoably.
        history = cmds.listHistory(new_mesh, pruneDagObjects=True) or []
        if history:
            target = cmds.listRelatives(new_mesh, parent=True, fullPath=True)[0]
        else:
            copy = _bare_copy(new_mesh, "groomLabColorTarget")
            target = copy
        for name in names:
            cmds.transferAttributes(source, target, transferPositions=0, transferNormals=0, transferUVs=0,
                                    transferColors=1, sourceColorSet=name, targetColorSet=name,
                                    sampleSpace=3 if method == "uv" else 0, searchMethod=3, colorBorders=1)
        if history:
            cmds.delete(new_mesh, constructionHistory=True)
        else:
            shape = cmds.listRelatives(copy, shapes=True, noIntermediate=True, fullPath=True)[0]
            cmds.delete(shape, constructionHistory=True)
            cmds.groomLabCopyColorSets(new_mesh, source=shape, colorSet=list(names))
    finally:
        cmds.delete([n for n in (source, copy) if n and cmds.objExists(n)])
    return list(names), []


def _bare_copy(mesh, name):
    """A copy of a mesh's transform at world level holding its shape alone (no children, no
    intermediate shapes, unlocked)."""
    copy = cmds.duplicate(cmds.listRelatives(mesh, parent=True, fullPath=True)[0], name=name, returnRootsOnly=True)[0]
    kept = False
    for child in cmds.listRelatives(copy, children=True, fullPath=True) or []:
        if kept or cmds.nodeType(child) != "mesh" or cmds.getAttr(child + ".intermediateObject"):
            cmds.delete(child)
        else:
            kept = True
    for attr in ("translate", "rotate", "scale", "shear"):
        cmds.setAttr(copy + "." + attr, lock=False)
    if cmds.listRelatives(copy, parent=True):
        copy = cmds.parent(copy, world=True)[0]
    return cmds.ls(copy, long=True)[0]
