# SPDX-License-Identifier: Apache-2.0
"""GroomLab > Utilities: moving a groom's hair in and out of plain Maya geometry, and a groom's
look onto other grooms. Every function takes its choices as arguments (the menu's dialogs only
fill them in), and each is one undo step.

    from groomlab import utils
    curves = utils.guides_to_curves(groom)                  # a curve per guide, tagged glGuideId
    ...edit the curves (CVs, deformers, a sculpt)...
    utils.curves_to_guides(groom, curves["group"])          # the tagged ones reshape their guides
    utils.strands_to_curves(groom, max_curves=20000)        # the evaluated strands as curves
    utils.to_mesh(groom, style="ribbon")            # ... or as one polygon mesh
    utils.bake_guides(groom, "scatter", group="baked")      # a node's output as editable guides
    utils.transfer_to_scalp(groom, "newHead", fit=True)     # the groom onto another scalp, maps projected
    utils.transfer_to_scalp(groom, "newHead", method="markersWarp",
                            markers=[("head.vtx[10]", "newHead.vtx[12]"), ...])  # by marker pairs
    utils.copy_graph(groom, ["hair2", "hair3"])             # the look onto other grooms

Curves and meshes are built by groomLabConvert through the API in one go (20,000 curves in a
few seconds); strands beyond a budget are thinned by strand ID (the ones a lower preview
density keeps), never at random.
"""

import json
import math
import os
import shutil

from maya import cmds

import groomlab
from groomlab.bake import bake_guides  # noqa: F401  (Node to Guides: part of the utilities)

DEFAULT_MAX_CURVES = 20000
DEFAULT_MAX_STRANDS = 20000
MESH_STYLES = ("tube", "ribbon")
MESH_MAKES = ("auto", "strands", "scales", "feathers", "cards", "instances")  # what to_mesh and a ToMesh node make
GUIDE_MODES = ("keep", "replace")


def _convert(groom, **flags):
    root, made, total = cmds.groomLabConvert(groom, **flags)
    return root, int(made), int(total)


def _curve_transforms(root):
    """The transforms of every curve under `root` (full paths), in the order they were made."""
    shapes = cmds.listRelatives(root, allDescendents=True, type="nurbsCurve", fullPath=True) or []
    return list(reversed(cmds.listRelatives(shapes, parent=True, fullPath=True) or [])) if shapes else []


# ---------------------------------------------------------------------------
# Guides <-> curves


def guides_to_curves(groom, group=None, selection=False, name=None, keep_hierarchy=True, degree=1):
    """A NURBS curve per guide, as authored (the shapes brushes edit, on the current scalp).

    Which guides: the selected ones with `selection`, else group `group`'s, else every guide.
    The curves go under one group transform (`name`, default "<groom>_guideCurves"); with
    `keep_hierarchy` under a transform per guide group inside it, nested as the groups are.
    Each curve's transform carries glGuideId (the guide's ID) and glGuideGroup (its group), so
    curves_to_guides can bring edits back. `degree` 1 (default): the CVs are the guide's points
    exactly; 3: a cubic through them. One undo step.

    Returns {"group": the group transform, "curves": [curve transforms], "count": n}.
    """
    if degree not in (1, 3):
        raise ValueError("degree is 1 or 3")
    flags = {"guideCurves": True, "degree": degree}
    if selection:
        ids = groomlab.selected_guides(groom)
        if not ids:
            raise ValueError("no guides selected")
        flags["guide"] = list(ids)
    elif group:
        flags["group"] = group
    if not keep_hierarchy:
        flags["flat"] = True
    if name:
        flags["name"] = name
    root, made, _total = _convert(groom, **flags)
    return {"group": root, "curves": _curve_transforms(root), "count": made}


def _curve_shapes(curves):
    """nurbsCurve shapes of `curves`: curve transforms or shapes, or groups holding curves."""
    if isinstance(curves, str):
        curves = [curves]
    shapes = []
    for node in curves or []:
        if not cmds.objExists(node):
            raise ValueError("%s does not exist" % node)
        if cmds.nodeType(node) == "nurbsCurve":
            found = [node]
        else:
            found = cmds.listRelatives(node, allDescendents=True, type="nurbsCurve", fullPath=True) or []
        for shape in cmds.ls(found, long=True, noIntermediate=True) or []:
            if shape not in shapes:
                shapes.append(shape)
    return shapes


def _tag(shape, attr):
    """A curve's string tag: on its transform (as guides_to_curves and sims write it), else its shape."""
    for node in (cmds.listRelatives(shape, parent=True, fullPath=True)[0], shape):
        if cmds.attributeQuery(attr, node=node, exists=True):
            return cmds.getAttr(node + "." + attr) or ""
    return ""


def all_guide_ids(groom):
    """Every guide ID of the groom (decimal strings)."""
    ids = []
    for g in groomlab.list_guide_groups(groom):
        ids += groomlab.guide_ids(groom, g["name"])
    return ids


def curves_to_guides(groom, curves, group="primary", update=True, tube_radius=0.1, tube_tip_radius=None):
    """Curves back into the groom's guides: `curves` are curve transforms, shapes, or groups
    holding them (a guides_to_curves group, say).

    With `update`, a curve tagged glGuideId whose guide still exists reshapes that guide in
    place (same ID, its children following; the curve's edit points become its points, a
    degree 1 curve's CVs). Curves whose shape did not change leave their guide alone, so a
    child whose curve was not touched keeps following its edited parent. Every other curve
    (untagged, an ID the groom does not have, or a second curve with an ID already used)
    becomes a new primary guide of root group `group`. One undo step.

    Returns {"updated": [IDs reshaped], "unchanged": [IDs left as they were], "added": [new IDs]}.
    """
    shapes = _curve_shapes(curves)
    if not shapes:
        raise ValueError("no curves given")
    existing = set(all_guide_ids(groom)) if update else set()
    reshape, fresh, claimed = [], [], set()
    for shape in shapes:
        guide = _tag(shape, "glGuideId").strip() if update else ""
        if guide in existing and guide not in claimed:
            claimed.add(guide)
            reshape.append((guide, shape))
        else:
            fresh.append(shape)
    with groomlab.undo_chunk("curves to guides"):
        updated = list(cmds.groomLabGuides(groom, reshape=reshape) or []) if reshape else []
        added = list(groomlab.guides_from_curves(groom, fresh, group=group, tube_radius=tube_radius,
                                                 tube_tip_radius=tube_tip_radius) or []) if fresh else []
    changed = set(updated)
    return {"updated": updated, "unchanged": [g for g, _ in reshape if g not in changed], "added": added}


# ---------------------------------------------------------------------------
# Strands -> curves / mesh


def strands_to_curves(groom, node=None, density=None, max_curves=DEFAULT_MAX_CURVES, attributes=True, name=None,
                      degree=1):
    """The groom's strands as NURBS curves (a curve per strand, under one group transform).

    `node`: the graph node whose output to convert (None: the groom's output; a node giving
    guides works too), evaluated at `density` (0-1, None: 1, every strand) as the groom sees
    its inputs now. At most `max_curves` curves: beyond it strands are thinned by strand ID
    (the ones a lower preview density keeps; a warning says how many), never at random. With
    `attributes`, each curve's shape carries glStrandId, glWidth (per point) and the strands'
    channels (gl<Channel>: glColorR, glClumpId...; a number per strand channel, an array per
    point channel). `degree` 1 (default): CVs on the strand's points; 3: a cubic through them.
    One undo step.

    Returns {"group", "curves": [curve transforms], "count": curves made, "total": strands
    the node gave, "capped": whether max_curves thinned them}.
    """
    if degree not in (1, 3):
        raise ValueError("degree is 1 or 3")
    flags = {"strandCurves": True, "node": node or "", "density": float(density or 1.0),
             "maxCount": int(max_curves), "degree": degree}
    if attributes:
        flags["attributes"] = True
    if name:
        flags["name"] = name
    root, made, total = _convert(groom, **flags)
    capped = total > int(max_curves)
    if capped:
        cmds.warning("GroomLab: %d of %d strands made curves (max_curves %d; thinned by strand ID)"
                     % (made, total, int(max_curves)))
    return {"group": root, "curves": _curve_transforms(root), "count": made, "total": total, "capped": capped}


def to_mesh(groom, node=None, make="auto", style="tube", sides=4, width_scale=1.0, max_strands=DEFAULT_MAX_STRANDS,
            density=None, strand_uvs=False, name=None):
    """The groom (or a node's output) as one polygon mesh.

    `make` "strands": the strands as tubes or ribbons; "scales": the scales' plates bent onto the
    skin; "feathers": the barbs as tubes or ribbons and the rachis tubes; "cards": the Cards nodes'
    cards (quads, in one atlas); "instances": every instance's mesh where it sits (an Instance
    node's scene meshes); "auto" (default): the scales when there are some, else instances, else
    cards, else feathers, else the strands. For tubes, `style` "tube":
    `sides`-sided tubes along each strand, as wide as its widths (times `width_scale`), the tip
    closed; "ribbon": flat strips as wide as the strand, lying along the scalp (their faces turned
    by the scalp's normal at the root, carried along the strand). UVs: u across, v root (0) to tip
    (1); with `strand_uvs` a second UV set "strandUVs" gives every strand its own cell of a grid.
    `node`, `density` and the cap (`max_strands`, thinned by strand ID with a warning; scales and
    feathers are never thinned) as strands_to_curves. The mesh gets the default shader. One undo
    step. A snapshot of the current frame: for a mesh that follows the animation, a ToMesh node
    (add_to_mesh) and its mesh in the scene (to_mesh_shape).

    Returns {"mesh": the transform, "strands", "total", "capped", "vertices", "faces"}.
    """
    if make not in MESH_MAKES:
        raise ValueError("make is %s, not %r" % (", ".join(MESH_MAKES), make))
    if style not in MESH_STYLES:
        raise ValueError("style is tube or ribbon, not %r" % style)
    flags = {"strandMesh": True, "node": node or "", "density": float(density or 1.0), "maxCount": int(max_strands),
             "make": make, "style": style, "sides": int(sides), "widthScale": float(width_scale)}
    if strand_uvs:
        flags["strandUVs"] = True
    if name:
        flags["name"] = name
    with groomlab.undo_chunk("to mesh"):
        root, made, total = _convert(groom, **flags)
        # The command gives the mesh the default shader itself: a sets command here, in the same
        # undo step, would hold the node its undo deletes and its redo makes anew.
        mesh = cmds.listRelatives(root, shapes=True, type="mesh", fullPath=True)[0]
    capped = total > int(max_strands)
    if capped:
        cmds.warning("GroomLab: the mesh holds %d of %d strands (max_strands %d; thinned by strand ID)"
                     % (made, total, int(max_strands)))
    return {"mesh": root, "strands": made, "total": total, "capped": capped,
            "vertices": cmds.polyEvaluate(mesh, vertex=True), "faces": cmds.polyEvaluate(mesh, face=True)}


def to_mesh_nodes(groom):
    """The groom's ToMesh nodes (its own, not those inside compounds), in graph order."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    return model.of_type("ToMesh")


@groomlab.undoable("add to mesh")
def add_to_mesh(groom, after=None, make="auto", style="tube", sides=4, width_scale=1.0, density=None,
                max_strands=DEFAULT_MAX_STRANDS, strand_uvs=False):
    """A ToMesh node: what it is handed as a live polygon mesh (`make` as to_mesh: auto, strands,
    scales or feathers), drawn by the groom instead of the strands (its displayMesh) and given on
    its outMesh (to_mesh_shape connects a Maya mesh to it; export_mesh writes it). It goes right
    after node `after` (a node making strands: what it makes is what the mesh shows, e.g. after a
    Simulate's Interpolate, before Noise) or, by default, just before the output; one already
    reading that node takes the settings instead. The node hands its strands on as they are. Its
    settings: `style` "tube" (`sides` around) or "ribbon", `width_scale` times the strands'
    widths, `density` (0-1) of the strands evaluated, at most `max_strands` (0: all; thinned by
    strand ID) and `strand_uvs` (a second UV set, "strandUVs"). Every ToMesh node making the
    output joins the one mesh. Returns the node's name."""
    from groomlab import graph as gm
    if make not in MESH_MAKES:
        raise ValueError("make is %s, not %r" % (", ".join(MESH_MAKES), make))
    if style not in MESH_STYLES:
        raise ValueError("style is tube or ribbon, not %r" % style)
    params = {"make": make, "style": style, "sides": int(sides), "widthScale": float(width_scale),
              "density": float(1.0 if density is None else density), "maxStrands": int(max_strands),
              "strandUVs": bool(strand_uvs)}
    model = gm.GraphModel.from_groom(groom)
    if after is None:
        anchor = model.output
        source = model.source_of(anchor, "strands")
        if source is not None and model.node_type(source) == "ToMesh":
            node = source
        else:
            node = model.add_node("ToMesh", name=model.unique_name("toMesh"), params=params)
            model.insert_before(anchor, node)
    else:
        if after not in model.nodes:
            raise ValueError("%s's graph has no node %r" % (groom, after))
        if model.output_type(after) != "Strands":
            raise ValueError("%r does not make strands (a ToMesh node reads strands)" % after)
        readers = [d for d, i in model.downstream_of(after) if model.node_type(d) == "ToMesh" and i == "strands"]
        if readers:
            node = readers[0]
        else:
            node = model.add_node("ToMesh", name=model.unique_name("toMesh"), params=params)
            model.insert_after(after, node)
    for key, value in params.items():
        model.set_param(node, key, value)
    if node not in model.feeding_output():
        cmds.warning("GroomLab: %s does not make the output (it ends a branch): it makes no mesh" % node)
    model.save(groom)
    return node


@groomlab.undoable("mesh in scene")
def to_mesh_shape(groom, name=None):
    """The ToMesh nodes' mesh in the scene: a mesh connected to the groom's outMesh, following the
    animation (a tube mesh keeps the same vertices and faces on every frame: export it with
    Alembic), with the default shader; returns its transform. The one already there when there is
    one. While it exists the groom leaves drawing the meshes to it. One undo step."""
    existing = cmds.listConnections(groom + ".outMesh", destination=True, source=False, shapes=True) or []
    if existing:
        return cmds.listRelatives(existing[0], parent=True, fullPath=True)[0]
    transform = cmds.listRelatives(groom, parent=True)[0]
    root = cmds.createNode("transform", name=name or (transform.split(":")[-1] + "_mesh"))
    mesh = cmds.createNode("mesh", name=root.split("|")[-1] + "Shape", parent=root)
    cmds.connectAttr(groom + ".outMesh", mesh + ".inMesh")
    cmds.sets(mesh, edit=True, forceElement="initialShadingGroup")
    return cmds.ls(root, long=True)[0]


# ---------------------------------------------------------------------------
# Modeled tubes -> curves / GroomLab tubes (tube_mesh reads them)

TUBE_GROUPS = {"shape": "tubesShape", "centers": "tubesCenter", "tubes": "tubes"}


def _mesh_shapes(nodes):
    """The mesh shapes under `nodes` (meshes, their transforms, groups holding them), once each."""
    shapes = []
    for node in nodes or []:
        if not cmds.objExists(node):
            raise ValueError("%s does not exist" % node)
        found = [node] if cmds.nodeType(node) == "mesh" else \
            cmds.listRelatives(node, allDescendents=True, type="mesh", fullPath=True) or []
        for shape in cmds.ls(found, long=True, noIntermediate=True) or []:
            if shape not in shapes:
                shapes.append(shape)
    return shapes


def _groom_meshes(groom):
    """The groom's scalps (its own and its others), as long mesh shape names."""
    if not groom:
        return set()
    shapes = [groomlab.scalp_mesh(groom)] + [s["mesh"] for s in groomlab.scalps(groom)]
    return set(cmds.ls([s for s in shapes if s], long=True) or [])


def selected_tube_meshes(groom=None):
    """The selected meshes (or groups of them) that are not the groom's scalps: the tubes to convert."""
    own = _groom_meshes(groom)
    from groomlab.base import GROOM_TYPE
    nodes = [n for n in cmds.ls(selection=True, long=True, objectsOnly=True) or []
             if cmds.nodeType(n) != GROOM_TYPE and not cmds.listRelatives(n, children=True, type=GROOM_TYPE)]
    return [s for s in _mesh_shapes(nodes) if s not in own]


def _om_mesh(shape):
    from maya.api import OpenMaya as om
    sel = om.MSelectionList()
    sel.add(shape)
    return om.MFnMesh(sel.getDagPath(0))


def smooth_preview(shape):
    """Whether the mesh is drawn smoothed (smooth mesh preview: the 2 or 3 key)."""
    try:
        return bool(cmds.getAttr(shape + ".displaySmoothMesh"))
    except (RuntimeError, ValueError):
        return False


def read_mesh_tubes(shape, smooth=None):
    """A mesh's tubes (tube_mesh.Tube per shell, in world space), named after its transform. With
    `smooth` (default: when the mesh is drawn smoothed, smooth_preview) each tube also carries its
    vertices where they lie on the smooth surface (Tube.smooth_rings; Tube.use_smooth reads them)."""
    from maya.api import OpenMaya as om
    from groomlab import tube_mesh
    fn = _om_mesh(shape)
    points = [(p.x, p.y, p.z) for p in fn.getPoints(om.MSpace.kWorld)]
    counts, connects = fn.getVertices()
    polygons, at = [], 0
    for c in counts:
        polygons.append(list(connects[at:at + c]))
        at += c
    name = cmds.listRelatives(shape, parent=True)[0].split(":")[-1]
    return tube_mesh.read_tubes(points, polygons, name=name,
                                smooth=smooth_preview(shape) if smooth is None else smooth)


def _scalp_distance(groom):
    """A function of a point: its distance to the groom's nearest scalp (None without a scalp)."""
    from maya.api import OpenMaya as om
    fns = [_om_mesh(s) for s in sorted(_groom_meshes(groom))]
    if not fns:
        return None

    def distance(p):
        q = om.MPoint(*p)
        return min(q.distanceTo(fn.getClosestPoint(q, om.MSpace.kWorld)[0]) for fn in fns)
    return distance


def _scalp_closest(groom):
    """A function of a point: the nearest point on the groom's scalps (None without a scalp)."""
    from maya.api import OpenMaya as om
    fns = [_om_mesh(s) for s in sorted(_groom_meshes(groom))]
    if not fns:
        return None

    def closest(p):
        q = om.MPoint(*p)
        hit = min((fn.getClosestPoint(q, om.MSpace.kWorld)[0] for fn in fns), key=q.distanceTo)
        return (hit.x, hit.y, hit.z)
    return closest


ROOT_FADE = 0.25  # the share of a curve's length over which _rooted's move fades away


def _rooted(points, closest):
    """`points` with the root moved onto the scalp, the move fading out over the first ROOT_FADE of
    the curve. A guide made from a curve is moved whole so its root lands on the scalp; each curve
    of a tube would move by its own amount (the middle of a root loop lies deeper than its edge)
    and the inner ones could leave the tube near its thin tip. Rooted here first, nothing moves
    them whole, and every curve keeps its place in the tube past the root."""
    if not closest or len(points) < 2:
        return points
    from groomlab import tube_mesh as tm
    move = tm._sub(closest(points[0]), points[0])
    along = [0.0]
    for a, b in zip(points, points[1:]):
        along.append(along[-1] + tm._dist(a, b))
    fade = ROOT_FADE * along[-1]
    if fade <= 1e-12:
        return points
    out = []
    for p, s in zip(points, along):
        k = max(0.0, 1.0 - s / fade)
        out.append(tm._add(p, tm._mul(move, k * k * (3.0 - 2.0 * k))))  # smooth to nothing
    return out


def _read_tubes(meshes, groom, ignore_bevels, smooth=None):
    """Every tube of `meshes`, root first, bevels merged with `ignore_bevels`, on the smooth
    surface with `smooth` (None: each mesh as it is drawn, read_mesh_tubes): (tubes, skipped:
    {mesh: why})."""
    from groomlab import tube_mesh
    shapes = _mesh_shapes(meshes)
    if not shapes:
        raise ValueError("select the modeled tubes (meshes)")
    distance = _scalp_distance(groom) if groom else None
    tubes, skipped = [], {}
    for shape in shapes:
        try:
            found = read_mesh_tubes(shape, smooth)
        except tube_mesh.TubeError as e:
            skipped[shape.split("|")[-2] if shape.count("|") > 1 else shape] = str(e)
            continue
        for index, tube in enumerate(found):
            # Where it came from and how it was read (a live link reads it again the same way).
            tube.mesh, tube.shell, tube.signature = shape, index, (len(tube.rings), tube.sides)
            before = tube_mesh.section_center(tube.rings[0])
            tube_mesh.orient(tube, distance)
            tube.flipped = tube_mesh.section_center(tube.rings[0]) != before
            if ignore_bevels:
                tube.merge_bevels()  # (bevels told apart as modeled: the smooth surface rounds them away)
            tubes.append(tube.use_smooth())
    if not tubes:
        raise ValueError("none of the selected meshes is an open tube GroomLab can read: " +
                         "; ".join("%s (%s)" % kv for kv in sorted(skipped.items())))
    return tubes, skipped


def _curve(name, points, parent=None):
    """A curve through `points` (a cubic through them; 2 or 3 points: a line), the points its edit points."""
    pts = [tuple(p) for p in points]
    degree = 3 if len(pts) >= 4 else 1
    curve = cmds.curve(name=name, editPoint=pts, degree=degree) if degree == 3 else \
        cmds.curve(name=name, point=pts, degree=1)
    if parent:
        curve = cmds.parent(curve, parent)[0]
    return cmds.ls(curve, long=True)[0]


def _guides_point_per_loop(groom, curves, group):
    """`curves` as guides of `group`, each with exactly its curve's edit points (a point per edge
    loop of its tube, as modeled): guides made from curves are resampled evenly, so they are
    given their curves' points after."""
    if not curves:
        return []
    ids = list(groomlab.guides_from_curves(groom, curves, group=group) or [])
    if ids:
        cmds.groomLabGuides(groom, reshape=[(str(i), _curve_shapes(c)[0]) for i, c in zip(ids, curves)])
    return ids


def _new_group_name(groom, base):
    """`base`, or base2, base3... : a guide group name the groom does not have yet."""
    taken = set(g["name"] for g in groomlab.list_guide_groups(groom))
    name, n = base, 1
    while name in taken:
        n += 1
        name = "%s%d" % (base, n)
    return name


def tubes_to_curves(meshes, groom=None, centers=True, centers_group=True, existing_groups=False, ignore_bevels=True,
                    name=None, shapes=True, center_count=1, smooth=True, region_map=True, live_link=False):
    """Modeled hair tubes (open quad tubes: `meshes`, or groups of them) as NURBS curves: with `shapes`
    one per edge run along each tube (its shape curves, a point per edge loop), and with `centers` one through
    each tube's middle (each section's area centroid); `center_count` above 1 fills the tube with
    that many (the center curve and others in rings round it: tube_mesh.volume_layout). With `ignore_bevels`, loops (or runs) lying
    much closer together than the ones beside them, a bevel holding a crease, count as one, their
    mean. With `smooth` each vertex is read where it lies on the mesh's smooth surface (as smooth
    mesh preview draws it), else as modeled; None: as each mesh is drawn. Roots: the end nearer the
    scalp with a groom, else each tube's wider end.

    With `groom` the curves become its guides (and leave the scene): the shape curves in guide
    group "tubesShape", the center curves in "tubesCenter" with `centers_group`, else with the shape
    curves in "tubesShape". With `existing_groups` they join those groups when the groom has them
    (made when it does not); else each conversion makes new ones, numbered after the first
    (tubesShape, tubesShape2...). Without a groom they stay in the scene as Maya curves under one group
    (`name`, default "tubeCurves"): a group per tube of its curves (<tube>_curve1...), and with
    `centers_group` one group of every center curve ("tubeCenterCurves"), else each center curve in
    its tube's group. With a groom and `region_map`, a guide map ("tubes", a free name) gives each
    tube's guides (shape and center) a value of their own, neighbouring tubes far apart in gray, and
    every Interpolate whose regionMap is empty takes it: each strand then blends only the guides of
    the tube rooted nearest it (regions from a guide map, Interpolate's regionMap). When an
    Interpolate already reads a tube map, the new tubes join it at values of their own. With a groom
    and `live_link`, the guides keep following their tubes (tube_links: the mesh's edits and moves
    reshape them, their roots sliding over the scalp). One undo step.

    Returns {"tubes": n, "shape": [curves or guide IDs], "centers": [...], "group": the scene group
    (None with a groom), "groups": {"shape": guide group, "centers": guide group} (with a groom),
    "regionMap": the guide map (with a groom and `region_map`, else None), "liveLink": whether the
    guides follow their tubes (with a groom), "skipped": {mesh: why}}.
    Neither `shapes` nor `centers`: ValueError.
    """
    if not shapes and not centers:
        raise ValueError("Choose shape curves, center curves or both")
    with groomlab.undo_chunk("tubes to curves"):
        tubes, skipped = _read_tubes(meshes, groom, ignore_bevels, smooth)
        root = cmds.group(empty=True, name=name or "tubeCurves")
        root = cmds.ls(root, long=True)[0]
        center_holder = (cmds.group(empty=True, name="tubeCenterCurves", parent=root)
                         if centers and centers_group else None)
        shape_curves, centers_made = [], []
        shape_tube, center_tube = [], []  # each curve's tube (its index)
        closest = _scalp_closest(groom) if groom else None  # (guides: rooted here, see _rooted)
        for t, tube in enumerate(tubes):
            holder = (cmds.group(empty=True, name=tube.name + "_curves", parent=root)
                      if shapes or (centers and not centers_group) else None)
            if shapes:
                for i, column in enumerate(tube.columns()):
                    shape_curves.append(_curve("%s_curve%d" % (tube.name, i + 1), _rooted(column, closest), holder))
                    shape_tube.append(t)
            if centers:
                for i, points in enumerate(tube.volume_curves(center_count)):
                    centers_made.append(_curve("%s_center%s" % (tube.name, i + 1 if i else ""),
                                               _rooted(points, closest), center_holder or holder))
                    center_tube.append(t)
        result = {"tubes": len(tubes), "skipped": skipped, "group": root, "groups": {}, "regionMap": None}
        if not groom:
            result.update(shape=shape_curves, centers=centers_made)
            return result

        def group_for(base):
            return base if existing_groups else _new_group_name(groom, base)
        shape_group = group_for(TUBE_GROUPS["shape"]) if shapes or (centers and not centers_group) else None
        groups = {"shape": shape_group,
                  "centers": (group_for(TUBE_GROUPS["centers"]) if centers_group else shape_group) if centers else None}
        result["shape"] = _guides_point_per_loop(groom, shape_curves, groups["shape"])
        result["centers"] = _guides_point_per_loop(groom, centers_made, groups["centers"])
        cmds.delete(root)
        result.update(group=None, groups=groups)
        by_tube = [[] for _ in tubes]
        shape_ids, center_ids = [[] for _ in tubes], [[] for _ in tubes]
        for ids, owners, mine in ((result["shape"], shape_tube, shape_ids), (result["centers"], center_tube, center_ids)):
            for guide, t in zip(ids or [], owners):
                by_tube[t].append(guide)
                mine[t].append(guide)
        values = [None] * len(tubes)
        if region_map and tubes:
            result["regionMap"], values = _tube_region_map(groom, by_tube)
        if live_link:
            from groomlab import tube_links
            uuids = dict((mesh, cmds.ls(mesh, uuid=True)[0]) for mesh in set(t.mesh for t in tubes))
            tube_links.add_links(groom, [
                tube_links.link_record(tube, uuids[tube.mesh], shape_ids[t], center_ids[t],
                                       center_count if centers else 0, groups, ignore_bevels, smooth,
                                       result["regionMap"], values[t])
                for t, tube in enumerate(tubes)])
        result["liveLink"] = bool(live_link)
    return result


TUBE_REGION_MAP = "tubes"


def tube_region_values(count):
    """`count` distinct grays for tubes, one each: evenly spaced in (0, 1), handed out a golden step
    apart so neighbouring tubes (made one after another) differ by about a third or more."""
    if count <= 0:
        return []
    values = [round((k + 1.0) / (count + 1.0), 4) for k in range(count)]
    step = max(1, int(round(count * 0.381966)))
    while math.gcd(step, count) != 1:
        step += 1
    return [values[(t * step) % count] for t in range(count)]


def _is_tube_map_name(name):
    rest = (name or "")[len(TUBE_REGION_MAP):]
    return (name or "").startswith(TUBE_REGION_MAP) and (rest == "" or rest.isdigit())


def _region_values_in_use(groom, name):
    """The distinct values the groom's guides have in guide map `name`, 0 (no tube) left out."""
    used = set()
    for guide in groomlab.get_hierarchy(groom).get("guides", []):
        value = (guide.get("attributes") or {}).get(name)
        if value:
            used.add(round(float(value), 4))
    return sorted(used)


def _extra_region_values(used, count):
    """`count` tube values for tubes added to a map whose tubes have `used`: tube_region_values of
    the whole count, the ones well clear of every value already used."""
    n = len(used) + count
    while True:
        gap = 0.5 / (n + 1.0)
        free = [v for v in tube_region_values(n) if all(abs(v - u) > gap for u in used)]
        if len(free) >= count:
            return free[:count]
        n += 1


def _tube_region_map(groom, by_tube):
    """Each tube's guides at a value of their own in a guide map, every other guide at 0, read by
    every Interpolate whose regionMap is empty. When an Interpolate already reads a tube map
    ("tubes", "tubes2"...: an earlier conversion's), the new tubes join it at values none of its
    tubes has (conversions add tubes, never take the earlier ones' regions); else a new map
    ("tubes", a free name) with values tube_region_values. Returns (its name, each tube's value)."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom, gm.OperatorCatalog.from_maya())
    taken = set(groomlab.list_maps(groom))
    interpolates = [node for node in model.order if model.node_type(node) == "Interpolate"]
    reading = [model.param(node, "regionMap") for node in interpolates]
    joined = next((m for m in reading if m and _is_tube_map_name(m) and m in taken and
                   groomlab.map_kind(groom, m) == "guides"), None)
    if joined:
        name = joined
        values = _extra_region_values(_region_values_in_use(groom, name), len(by_tube))
    else:
        name, n = TUBE_REGION_MAP, 1
        while name in taken:
            n += 1
            name = "%s%d" % (TUBE_REGION_MAP, n)
        groomlab.add_map(groom, name, value=0.0, kind="guides")
        values = tube_region_values(len(by_tube))
    for guides, value in zip(by_tube, values):
        if guides:
            groomlab.set_guide_attribute(groom, name, value, guides=list(guides))
    changed = False
    for node in interpolates:
        if not model.param(node, "regionMap"):
            model.set_param(node, "regionMap", name)
            changed = True
    if changed:
        model.save(groom)
    return name, values


def _shape_name(groom, base, taken):
    """A library shape name from `base` (a mesh's name): only the characters a shape name takes, unique."""
    import re
    clean = re.sub(r"[^A-Za-z0-9 _.\-]", "_", base).strip() or "tube"
    clean = clean[:56]
    if clean.lower() in [s.lower() for s in groomlab.TUBE_SHAPES]:
        clean += "_shape"
    name, n = clean, 1
    while name in taken:
        n += 1
        name = "%s_%d" % (clean, n)
    return name


def _tube_frames(groom, guide, evaluated=False):
    """A guide's points and section axes (u, v: its frames turned by its roll). `evaluated`: the
    guide as drawn, its roll worked out on the scalp for the tube it has now."""
    if evaluated:
        return json.loads(cmds.groomLabGuides(groom, tubeFrames=str(guide), evaluated=True))
    return json.loads(cmds.groomLabGuides(groom, tubeFrames=str(guide)))


def _scalp_surface(groom):
    """A function of a point: (the nearest point on the groom's scalps, the scalp's normal there),
    or None without a scalp."""
    from maya.api import OpenMaya as om
    fns = [_om_mesh(s) for s in sorted(_groom_meshes(groom))]
    if not fns:
        return None

    def nearest(p):
        q = om.MPoint(*p)
        best = None
        for fn in fns:
            hit, normal = fn.getClosestPointAndNormal(q, om.MSpace.kWorld)[:2]
            d = q.distanceTo(hit)
            if best is None or d < best[0]:
                best = (d, (hit.x, hit.y, hit.z), (normal.x, normal.y, normal.z))
        n = best[2]
        length = math.sqrt(sum(c * c for c in n)) or 1.0
        return best[1], tuple(c / length for c in n)
    return nearest


ROOT_LIFT = 0.35  # how far the guide's second point stands off the scalp: a share of the way to the next loop


def _tube_path(centers, surface):
    """The guide's points through a tube's section centers: its root on the scalp under the root
    section, and its second point straight off the scalp from there (a share of the way to the next
    section), so the root section lies on the scalp as the modeled root loop does (a section is
    square to the guide). Without a scalp: the centers."""
    centers = [tuple(c) for c in centers]
    if not surface or len(centers) < 2:
        return centers
    root, normal = surface(centers[0])
    step = math.sqrt(sum((a - b) ** 2 for a, b in zip(centers[1], root)))
    lift = tuple(r + n * ROOT_LIFT * step for r, n in zip(root, normal))
    return [root, lift] + centers[1:]


def _tube_columns(tube):
    """The tube's surface as point lists along it (Tube.section_loops' points, column by column),
    and how many of them per vertex round a loop (vertex i: column i * per)."""
    per = SHAPE_SAMPLES if tube.smooth else 1
    loops = tube.section_loops(per)
    return [[loop[j] for loop in loops] for j in range(len(loops[0]))], per


def _fit_sections(frames, tube, shape_uv, columns, root_points=0, outline=None, uncovered=None):
    """The section at each of the guide's points, where the modeled tube crosses the plane square to
    the guide there (tube_mesh.plane_section: the surface as drawn, so a bend's leaning loops
    count as the tube's real thickness): [{t, twist, width, height, center}]. The twist puts each
    shape point nearest the edge through its own vertex, so the tube turns as the modeled edges
    turn and no more. The root's shape is the root loop's, so the first `root_points` (the root and
    the point straight off the scalp) take the root loop itself and match it. Along the tube only
    its thickness counts, not its shape: each section centered on the cut, as flat as the cut is
    across the shape's axes (tube_mesh.section_box), and sized so the shape's `outline` (its drawn
    curve, in shape coordinates) holds the cut's area: the modeled volume. With `uncovered`, each
    section's `cover`: how much wider it must be drawn for at most that share of the cut to lie
    outside it (tube_mesh.cover_scale; 1 at the root points: the root loop is the shape). A section
    with no size (a pointed tip) keeps the last twist."""
    from groomlab import tube_mesh as tm
    surface, per = _tube_columns(tube)
    points = frames["points"]
    n = len(points)
    sections, last = [], 0.0
    for i in range(n):
        t = i / float(n - 1)
        center, u, v = tm.frame_at(frames, t)
        if i < root_points:
            cut = [col[0] for col in surface]
        else:
            cut = tm.plane_section(surface, center, tm._unit(tm._cross(u, v)))
        flat = [tm.to_plane(p, center, u, v) for p in cut]
        twist, w, h, c = tm.fit_section([flat[k * per] for k in columns], shape_uv, near=last)
        if w > 0.0:
            w, h, c = tm.section_box(flat, twist)
            if outline and i >= root_points and w > 0.0 and h > 0.0:
                held = abs(tm.signed_area(outline)) * w * h
                cut_area = abs(tm.signed_area(flat))
                if held > 1e-18 and cut_area > 0.0:
                    k = math.sqrt(cut_area / held)
                    w, h = w * k, h * k
        cover = 1.0
        if uncovered is not None and outline and i >= root_points and w > 0.0 and h > 0.0:
            drawn = tm.place_outline(outline, twist, w, h, (0.0, 0.0))
            cover = tm.cover_scale([(x - c[0], y - c[1]) for x, y in flat], drawn, uncovered)
        last = twist
        sections.append({"t": t, "twist": twist, "width": w, "height": h, "center": tm.from_plane(c, center, u, v),
                         "cover": cover})
    return sections


def _smoothed_cover(sections, keep):
    """The sections' cover factors evened out along the tube (two passes of 1-2-1), the first
    `keep` (the root points) left at 1: the widths change gently where the shape parts from the
    modeled sections."""
    k = [1.0 if i < keep else s.get("cover", 1.0) for i, s in enumerate(sections)]
    for _ in range(2):
        k = [k[i] if i < keep else (k[max(i - 1, 0)] + 2.0 * k[i] + k[min(i + 1, len(k) - 1)]) * 0.25
             for i in range(len(k))]
    return [max(1.0, f) for f in k]


def _reshape(groom, guide, points):
    """The guide through `points` exactly (they become its points)."""
    curve = _curve("glTubeFit", points)
    try:
        cmds.groomLabGuides(groom, reshape=[(str(guide), _curve_shapes(curve)[0])])
    finally:
        cmds.delete(curve)


def _fit_tube(groom, guide, tube, shape_uv, columns, surface=None, outline=None, passes=2, padding=1.0, step=None,
              uncovered=None):
    """Fit guide `guide` and its tube to the modeled `tube`, against the guide as drawn (its frames
    and the roll GroomLab works out on the scalp). Each pass cuts the tube square to the guide at
    each of its points (_fit_sections), moves those points to the cuts' centers (the root and the
    point straight off the scalp stay: the root lies flat on the scalp) and sets the widths and
    aspects. The roll follows the guide's path and its sections' size, never their twist: so
    last, on the guide as it now stays, the twists alone. The widths are set as fitted times their
    cover factor (`uncovered`: the share of each modeled section left outside it, _fit_sections;
    evened out, the root's 1) times `padding` (before the twists: a width moves the roll).
    `step()` after each pass and the last (passes + 1 calls). Returns the sections."""
    sections = []
    for _ in range(passes):
        frames = _tube_frames(groom, guide, evaluated=True)
        if len(frames["points"]) < 2:
            return sections
        keep = 2 if surface else 0
        sections = _fit_sections(frames, tube, shape_uv, columns, keep, outline, uncovered)
        path = [tuple(p) for p in frames["points"][:keep]] + [s["center"] for s in sections[keep:]]
        _reshape(groom, guide, path)
        aspects, prev = [], 1.0
        for s in sections:
            a = s["height"] / s["width"] if s["width"] > 1e-9 else prev
            prev = min(max(a, 0.01), 100.0)
            aspects.append(prev)
        cover = _smoothed_cover(sections, keep)
        groomlab.set_tube(groom, guides=[guide], width=[s["width"] * k * padding for s, k in zip(sections, cover)],
                          aspect=aspects)
        if step:
            step()
    frames = _tube_frames(groom, guide, evaluated=True)
    sections = _fit_sections(frames, tube, shape_uv, columns, 2 if surface else 0, outline)
    groomlab.set_tube(groom, guides=[guide], twist=[s["twist"] for s in sections])
    if step:
        step()
    return sections


SHAPE_SAMPLES = 4  # a smoothed section's shape: points per edge of its loop


def _section_shape(tube, frames, t):
    """The tube's root section as a shape, in the guide's section axes at `t`: a point per vertex
    of its root loop, read smooth where each lies on the smooth surface and drawn as a smooth curve
    through them (the profile editor's smooth curve, as smooth mesh preview rounds the loop), else
    the vertices as modeled with straight edges. Returns (points (u, v), smooth, {ring vertex: the
    index of its point in points})."""
    from groomlab import tube_mesh as tm
    center, u, v = tm.frame_at(frames, t)
    flat = [tm.to_plane(p, center, u, v) for p in tube.rings[0]]
    pts, indices = tm.shape_points(flat)
    return pts, tube.smooth, {i: j for j, i in enumerate(indices)}


# Width compensation (learned on modeled locks whose sections round out away from the root): a
# converted tube keeps its root loop's shape all along, so where the modeled sections part from that
# shape, a section holding their area still leaves some of them outside it, and the tube reads
# thinner than the mesh. Each section is drawn just wide enough that at most this share of the
# modeled section lies outside it (tube_mesh.cover_scale): where the shapes agree, nothing is added
# (a round lock: 0 to 4%); where they part, more (a kite-shaped root on rounder sections: 10 to
# 18%). Never thinner than the volume, at most 35% wider, the root as modeled, evened along.
TUBE_UNCOVERED = 0.05
FIT_PASSES = 2


class Cancelled(Exception):
    """A long conversion stopped by the artist (its progress callback said so). `made`: whether it
    had changed the scene yet (its one undo step takes that back; nothing made, no step to undo)."""

    def __init__(self, made):
        super(Cancelled, self).__init__("conversion cancelled")
        self.made = made


def tubes_to_groomlab_tubes(groom, meshes, ignore_bevels=True, group=None, smooth=True,
                            uncovered=TUBE_UNCOVERED, padding=1.0, progress=None):
    """Modeled hair tubes (open quad tubes: `meshes`, or groups of them) as GroomLab tube guides,
    one to one, to keep shaping without touching the mesh: a guide through each tube's middle,
    rooted on the scalp under its root loop and leaving it square, so the root section lies on the
    scalp as the loop does; its tube's shape the root loop exactly (a custom shape in the groom's
    library, one per distinct section; the mesh's name); and along it the width and flatness of
    each of the tube's edge loops, turned as its edges turn (so a flat tube's flat side faces as
    modeled). `smooth` (default): the tube as smooth mesh preview draws it, its limit surface: the
    root shape the smooth curve round the root loop, each loop where it lies on that surface; else
    straight-edged, through the vertices as modeled; None: as each mesh is drawn now.
    `ignore_bevels`: close loops and runs (a bevel) count as one. Each section past the root is
    widened so at most `uncovered` of the modeled section lies outside it (TUBE_UNCOVERED; None:
    as fitted, the volume alone), then every width times `padding`. The guides go into a new root group `group` (default
    "tubes", numbered when taken). One undo step. `progress(done, total, text)`, when given, is
    called as the work goes (reading the meshes, then each tube's guide and shape and each fitting
    pass: steps of equal count, so done / total is how far along it is); returning False stops
    the conversion (Cancelled raised, after its undo chunk closed: one undo takes it all back).

    Returns {"guides": [IDs], "group": the guide group, "shapes": [library shapes made],
    "skipped": {mesh: why}}.
    """
    from groomlab import tube_mesh as tm
    if not groom:
        raise ValueError("select the groom the tubes go to")
    count = {"done": 0, "total": 1}

    def advance(text):
        count["done"] += 1
        if progress and progress(count["done"], count["total"], text) is False:
            raise Cancelled(made=count["done"] > 1)  # (past reading the meshes: guides made)

    with groomlab.undo_chunk("tubes to GroomLab tubes"):
        if progress and progress(0, 1, "Reading the tubes...") is False:
            raise Cancelled(made=False)
        tubes, skipped = _read_tubes(meshes, groom, ignore_bevels, smooth)
        count["total"] = 1 + len(tubes) * (2 + FIT_PASSES)
        advance("Read %d tube%s" % (len(tubes), "" if len(tubes) == 1 else "s"))
        surface = _scalp_surface(groom)
        group = group or _new_group_name(groom, TUBE_GROUPS["tubes"])
        library = {s["name"]: s for s in groomlab.tube_shapes(groom)}
        made_shapes, guides = [], []
        for number, tube in enumerate(tubes):
            label = "Tube %d of %d (%s)" % (number + 1, len(tubes), tube.name)
            # The guide through the tube's middle, rooted flat on the scalp, with a placeholder shaped
            # tube so its sections lie as they will (on the scalp: twist 0 at the root).
            curve = _curve("glTubeCenter", tube.centers())
            try:
                guide = groomlab.guides_from_curves(groom, [curve], group=group)[0]
            finally:
                cmds.delete(curve)
            _reshape(groom, guide, _tube_path(tube.centers(), surface))
            source = next((r for r in tube.rings if tm.section_radius(r) > 1e-9), tube.rings[0])
            radius = tm.section_radius(source)
            groomlab.set_tube(groom, guides=[guide], shape="ribbon", width=radius)
            frames = _tube_frames(groom, guide, evaluated=True)
            t = tm.polyline_param(frames["points"], tm.section_center(tube.rings[0]))
            pts, smooth_shape, at = _section_shape(tube, frames, t)
            # The library shape: one with this outline already (this run's or an earlier
            # conversion's: converting again makes no copies), its own name's if that one is
            # unused (taken anew), else a new one. Every library shape costs each read of the
            # groom's guides, so copies piling up slow the tools.
            name = None
            for shape in library.values():
                if (not shape["builtin"] and len(shape["points"]) == len(pts) and bool(shape["smooth"]) == smooth_shape
                        and tm.match_fitted(pts, shape["points"])):
                    name = shape["name"]
                    break
            if name is None:
                own = library.get(_shape_name(groom, tube.name, {}))
                if own is not None and not own["builtin"] and not own.get("users"):
                    name = own["name"]
                    groomlab.set_tube_shape_points(groom, name, pts, smooth=smooth_shape)
                else:
                    name = _shape_name(groom, tube.name, library)
                    groomlab.add_tube_shape(groom, name, pts, smooth=smooth_shape)
                library[name] = next(s for s in groomlab.tube_shapes(groom) if s["name"] == name)
                made_shapes.append(name)
            fitted_uv = tm.match_fitted(pts, library[name]["points"]) or library[name]["points"]
            columns = sorted(at)
            shape_uv = [fitted_uv[at[i]] for i in columns]
            groomlab.set_tube(groom, guides=[guide], shape=name, width=radius, aspect=1.0, twist=0.0)
            advance(label + ": its guide and shape")
            import groomlab_core  # (the shape's curve as drawn: its area, which the sections hold)
            drawn = [tuple(p) for p in groomlab_core.tube_shape_curve(library[name]["points"], library[name]["smooth"])]
            _fit_tube(groom, guide, tube, shape_uv, columns, surface, outline=drawn, passes=FIT_PASSES,
                      padding=padding, uncovered=uncovered, step=lambda: advance(label + ": fitting along it"))
            guides.append(guide)
    return {"guides": guides, "group": group, "shapes": made_shapes, "skipped": skipped}


# ---------------------------------------------------------------------------
# Node -> guides


def bake_nodes(groom):
    """The graph's nodes whose output bakes into guides (Strands or Guides), in graph order."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    out = []
    for name in model.order:
        try:
            if model.output_type(name) in ("Strands", "Guides"):
                out.append(name)
        except gm.GraphError:
            pass
    return out


# ---------------------------------------------------------------------------
# Transfer


def _count(n, word):
    return "%d %s%s" % (n, word, "" if n == 1 else "s")


def transfer_to_scalp(groom, scalp, fit=False, maps=True, color_sets=False, method=None, markers=None,
                      keep_lengths=False, fit_size=False):
    """Move the groom onto another scalp mesh (groomlab.transfer_groom: guides, parts and flow
    arrows re-rooted on the new scalp where `method` takes them: "closest" (default), "bounds"
    (what `fit` asks for: the old scalp's bounding box moved and scaled onto the new one's),
    "markersAlign", "markersWarp" (by marker pairs: `markers`, else the ones stored on the groom)
    or "uv"; `keep_lengths` keeps guide lengths; `fit_size` fits the groom to the new scalp's size
    (its scale grows with the area); `maps` projects the painted scalp maps onto it;
    `color_sets` copies the Maya color sets the graph reads onto the new mesh). One undo step.

    Returns {"guides", "parts", "flow": how many moved (parts and flow arrows that fall apart on
    the new scalp are dropped: "partsDropped", "flowDropped"), "projected": painted maps
    projected onto the new scalp, "colorSets": color sets copied onto it, "missing": what could
    not be carried (maps left behind, color sets the graph reads that the new scalp lacks),
    "message"}.
    """
    groomlab.ensure_plugin()
    mesh = groomlab._shape_of(scalp, "mesh")
    guides = sum(g["count"] for g in groomlab.list_guide_groups(groom))
    parts_before = len(groomlab.list_parts(groom))
    flow_before = len(groomlab.flow_arrows(groom)) + len(groomlab.flow_sinks(groom))
    report = {}
    from groomlab.transfer import transfer_method, METHOD_LABELS
    method = transfer_method(method, fit)
    missing = groomlab.transfer_groom(groom, mesh, maps=maps, color_sets=color_sets, report=report, method=method,
                                      markers=markers, keep_lengths=keep_lengths, fit_size=fit_size)
    parts = len(groomlab.list_parts(groom))
    flow = len(groomlab.flow_arrows(groom)) + len(groomlab.flow_sinks(groom))
    target = cmds.listRelatives(mesh, parent=True)[0]
    how = "" if method == "closest" else " (%s)" % METHOD_LABELS[method].lower()
    text = "moved %s, %s and %s onto %s%s" % (_count(guides, "guide"), _count(parts, "part"),
                                             _count(flow, "flow arrow"), target, how)
    if report["projected"]:
        text += "; projected %s: %s" % (_count(len(report["projected"]), "painted map"), ", ".join(report["projected"]))
    if report["colorSets"]:
        text += "; copied %s: %s" % (_count(len(report["colorSets"]), "color set"), ", ".join(report["colorSets"]))
    dropped = []
    if parts < parts_before:
        dropped.append(_count(parts_before - parts, "part"))
    if flow < flow_before:
        dropped.append(_count(flow_before - flow, "flow arrow"))
    if dropped:
        text += "; dropped (did not fit the new scalp): " + ", ".join(dropped)
    if missing:
        text += "; not carried (they stay with the old mesh): " + ", ".join(missing) + \
                (" (see the Script Editor's warnings)" if maps else
                 " (transfer with painted maps projected, or copy the color sets)")
    return {"guides": guides, "parts": parts, "flow": flow, "partsDropped": parts_before - parts,
            "flowDropped": flow_before - flow, "projected": report["projected"], "colorSets": report["colorSets"],
            "missing": missing, "method": method, "message": text}


# ---------------------------------------------------------------------------
# Copy the look



def graph_maps(graph):
    """Painted maps a graph reads by name: Map nodes' maps, HairColor's map and Interpolate's
    regionMap (not an image file's path: the core's rule, presets.is_image_path), guide maps (guide
    layers) and color set maps (a vertex map or a mesh color set: color set maps), inside compounds
    too. {name: how it is read}."""
    from groomlab import presets
    found = {}
    for node in graph.get("nodes", []):
        params = node.get("params") or {}
        if node.get("type") == "Map":
            found.setdefault(params.get("name", "mask"), "map")
        for param, how, kind in (("map", "map", "HairColor"), ("regionMap", "map", "Interpolate")):
            value = params.get(param) if node.get("type") == kind else None
            if isinstance(value, str) and value and not presets.is_image_path(value):
                found.setdefault(value, how)
        for value in params.values():
            if isinstance(value, dict):
                if value.get("guide"):
                    found.setdefault(value["guide"], "guides")
                if value.get("colorSet"):
                    found.setdefault(value["colorSet"], "colorSet")
        if node.get("type") == "Compound":
            for name, how in graph_maps((params.get("definition") or {}).get("graph") or {}).items():
                found.setdefault(name, how)
    return found


def _topology(mesh):
    return (cmds.polyEvaluate(mesh, vertex=True), cmds.polyEvaluate(mesh, face=True),
            cmds.polyEvaluate(mesh, triangle=True))


def _scalp_source(groom):
    plugs = cmds.listConnections(groom + ".inMesh", plugs=True, source=True, destination=False) or []
    return plugs[0] if plugs else None


def _copy_guides(source, target, fit):
    """Target's guides become source's: copied as they are onto the same scalp or one of the
    same topology (bound to its faces, they sit where the same faces are), else moved across
    with the transfer (bounding boxes fitted with `fit`). Returns "copied" or "transferred"."""
    data = cmds.getAttr(source + ".guideData") or ""
    src_mesh, dst_mesh = groomlab.scalp_mesh(source), groomlab.scalp_mesh(target)
    same = _same_scalp(source, target)
    if groomlab.selected_guides(target):
        groomlab.clear_guide_selection(target)  # its IDs are the old guides'
    if same or not data or not (src_mesh and dst_mesh):
        cmds.setAttr(target + ".guideData", data, type="string")
        return "copied"
    # Across scalps: the source's guides on the source's scalp, then moved to the target's
    # (groomLabTransfer). The target's own parts and flow stay out of it, and its scalp and rest
    # mesh are as they were after.
    scalp_plug = _scalp_source(target)
    rest_plugs = cmds.listConnections(target + ".restMesh", plugs=True, source=True, destination=False) or []
    parts, flow = cmds.getAttr(target + ".partData") or "", cmds.getAttr(target + ".flowData") or ""
    cmds.setAttr(target + ".partData", "", type="string")
    cmds.setAttr(target + ".flowData", "", type="string")
    cmds.setAttr(target + ".guideData", data, type="string")
    cmds.connectAttr(_scalp_source(source), target + ".inMesh", force=True)
    cmds.groomLabTransfer(target, scalp=dst_mesh, fitBounds=bool(fit))
    if scalp_plug and not cmds.isConnected(scalp_plug, target + ".inMesh"):
        cmds.connectAttr(scalp_plug, target + ".inMesh", force=True)
    if rest_plugs:
        cmds.connectAttr(rest_plugs[0], target + ".restMesh", force=True)
    cmds.setAttr(target + ".partData", parts, type="string")
    cmds.setAttr(target + ".flowData", flow, type="string")
    return "transferred"


def _copy_map(source, target, name, entry):
    """Copy one scalpMaps entry of source's onto target: a vertex map when the scalps have as
    many vertices, a texture map as a copy of its file in the target's masks folder (a free
    name: never another groom's file). The entry to store, or None with the reason."""
    kind = entry.get("kind")
    if kind == "vertices":
        src, dst = groomlab.scalp_mesh(source), groomlab.scalp_mesh(target)
        if not (src and dst) or cmds.polyEvaluate(src, vertex=True) != cmds.polyEvaluate(dst, vertex=True):
            return None, "a vertex map, and the scalps' vertices differ"
        return dict(entry), None
    if kind == "texture":
        path = groomlab.resolve_path(entry.get("texture", ""), source)
        if not path or not os.path.exists(path):
            return None, "its image is missing (%s)" % entry.get("texture", "")
        dest = groomlab.resolve_path(groomlab._map_texture_default(target, name), target) or \
            groomlab.asset_folder("masks") + "/%s/%s.png" % (groomlab.asset_name(target), name)
        dest = groomlab._unique_path(dest)
        if not os.path.isdir(os.path.dirname(dest)):
            os.makedirs(os.path.dirname(dest))
        shutil.copyfile(path, dest)
        copied = dict(entry)
        copied["texture"] = groomlab.portable_path(dest)
        return copied, None
    return None, "a %s map" % kind


def _same_scalp(source, target):
    """Whether the grooms' scalps are one mesh or two of the same topology (a map bound to the
    vertices and UVs fits both as it is)."""
    src, dst = groomlab.scalp_mesh(source), groomlab.scalp_mesh(target)
    return bool(src and dst and (cmds.ls(src, long=True) == cmds.ls(dst, long=True) or _topology(src) == _topology(dst)))


def copy_graph(source, targets, guides="keep", maps=False, fit=True):
    """Give other grooms `source`'s look: each target's graph becomes a copy of source's, with
    source's control values (the ctl_ sliders and keyed params' attributes, made on each target;
    their keys stay on the source).

    `guides` "keep" leaves each target's guides; "replace" gives it source's guides too: copied
    onto the same scalp or one of the same topology, else moved onto the target's scalp with
    the transfer (bounding boxes fitted with `fit`). Painted maps the graph reads by name that a
    target lacks are reported; with `maps` they come along: onto the same scalp or one of the
    same topology copied as they are (a vertex map's values; a texture map as a copy of its
    image), onto another scalp projected by position (groomLabTransfer -mapsFrom, bounding
    boxes fitted with `fit`: new values for its vertices, a new image at its UVs). New images
    go under the target's masks folder with a fresh name, so the grooms never paint into one
    file. A guide layer comes only with replaced guides. All targets in one undo step.

    Returns {target: {"guides": "kept"|"copied"|"transferred", "copied": [maps copied as they
    are], "projected": [maps projected onto another scalp], "missing": {map: why}}}.
    """
    if guides not in GUIDE_MODES:
        raise ValueError("guides is keep or replace, not %r" % guides)
    if isinstance(targets, str):
        targets = [targets]
    targets = [groomlab._shape_of(t, groomlab.GROOM_TYPE) for t in targets]
    source = groomlab._shape_of(source, groomlab.GROOM_TYPE)
    targets = [t for t in targets if cmds.ls(t, long=True) != cmds.ls(source, long=True)]
    if not targets:
        raise ValueError("pick other grooms to copy %s's graph to" % source)
    graph = groomlab.get_graph(source)
    wanted = graph_maps(graph or json.loads(cmds.groomLabDefaultGraph()))
    source_maps = {m["name"]: m for m in groomlab.maps(source)}
    source_entries = json.loads(cmds.getAttr(source + ".scalpMaps") or "{}")
    # The values of its controls (a vector's channels one by one; the groom's attributes made by set_graph).
    controls = [a for a in cmds.listAttr(source, userDefined=True) or [] if a.startswith(groomlab.CONTROL_PREFIX)
                and not cmds.attributeQuery(a, node=source, listChildren=True)]
    report = {}
    with groomlab.undo_chunk("copy graph"):
        for target in targets:
            groomlab.set_graph(target, json.loads(json.dumps(graph)))
            for attr in controls:
                if cmds.attributeQuery(attr, node=target, exists=True):
                    cmds.setAttr(target + "." + attr, cmds.getAttr(source + "." + attr))
            how = "kept"
            if guides == "replace":
                how = _copy_guides(source, target, fit)
            have = {m["name"] for m in groomlab.maps(target)}
            if target and groomlab.scalp_mesh(target):
                have |= set(cmds.polyColorSet(groomlab.scalp_mesh(target), query=True, allColorSets=True) or [])
            entries = json.loads(cmds.getAttr(target + ".scalpMaps") or "{}")
            same = _same_scalp(source, target)
            copied, to_project, missing = [], [], {}
            for name, how_read in sorted(wanted.items()):
                if name in have:
                    continue
                if name not in source_maps:
                    if how_read != "colorSet":
                        missing[name] = "the source groom has no map named %s either" % name
                    continue  # a color set of the source's mesh: not a groom's map
                kind = source_maps[name]["kind"]
                if kind == "guides":
                    missing[name] = "a guide layer: it comes with the guides (guides='replace'), or paint it"
                    continue
                if not maps:
                    missing[name] = "a %s map (copy it with maps=True)" % kind
                    continue
                if not same and groomlab.scalp_mesh(source) and groomlab.scalp_mesh(target):
                    to_project.append(name)
                    continue
                entry, why = _copy_map(source, target, name, source_entries.get(name, {}))
                if entry is None:
                    missing[name] = why
                    continue
                entries[name] = entry
                copied.append(name)
            if copied:
                cmds.setAttr(target + ".scalpMaps", json.dumps(entries, separators=(",", ":")), type="string")
            projected = []
            if to_project:
                projected = sorted(cmds.groomLabTransfer(target, mapsFrom=source, mapName=to_project,
                                                         fitBounds=bool(fit)) or [])
                for name in to_project:
                    if name not in projected:
                        missing[name] = "could not be projected onto %s's scalp (see the Script Editor)" % target
            report[target] = {"guides": how, "copied": copied, "projected": projected, "missing": missing}
    return report
