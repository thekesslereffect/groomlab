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
import os
import shutil

from maya import cmds

import groomlab
from groomlab.bake import bake_guides  # noqa: F401  (Node to Guides: part of the utilities)

DEFAULT_MAX_CURVES = 20000
DEFAULT_MAX_STRANDS = 20000
MESH_STYLES = ("tube", "ribbon")
MESH_MAKES = ("auto", "strands", "scales", "feathers")  # what to_mesh and a ToMesh node make
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
    skin; "feathers": the barbs as tubes or ribbons and the rachis tubes; "auto" (default): the
    scales when there are some, else feathers, else the strands. For tubes, `style` "tube":
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
    return [n for n in model.order if model.node_type(n) == "ToMesh"]


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


# Image files' extensions (texture.h's isImagePath): HairColor's map naming one is a file, not a map.
_IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic", ".pnm", ".ppm", ".pgm")


def graph_maps(graph):
    """Painted maps a graph reads by name: Map nodes' maps, HairColor's map and Interpolate's
    regionMap (not an image file's path), guide maps (guide layers) and color set maps (a vertex map
    or a mesh color set: color set maps), inside compounds too.
    {name: how it is read}."""
    found = {}
    for node in graph.get("nodes", []):
        params = node.get("params") or {}
        if node.get("type") == "Map":
            found.setdefault(params.get("name", "mask"), "map")
        for param, how, kind in (("map", "map", "HairColor"), ("regionMap", "map", "Interpolate")):
            value = params.get(param) if node.get("type") == kind else None
            if isinstance(value, str) and value and not value.lower().endswith(_IMAGE_EXTENSIONS):
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
