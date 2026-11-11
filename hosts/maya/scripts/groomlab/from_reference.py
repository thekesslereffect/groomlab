# SPDX-License-Identifier: Apache-2.0
"""Guides from reference photos (experimental): the photos on the groom's reference cameras
(groomlab.reference: a camera lined up with the mesh, the photo on its image plane) give guides
that follow the hair they show, in a guide group of their own. Its UI is the Reference window
(ui/reference_window.py: the photo and the mesh side by side, landmarks, guide lines, mask
touch-ups, Generate, the measured look).

    import groomlab
    cams = groomlab.reference.list_references()
    result = groomlab.guides_from_reference(groom, cams, guide_count=400)
    # or with masks: [{"camera": cam, "mask": "C:/photos/front_mask.png"}, ...]

How (the core's guidesFromReference, reference_guides.h): each photo's strand directions (Gabor
filters), a shell from the masks' silhouettes, guides grown root to tip along the directions the
photos agree on, symmetric across the scalp when it is (or as asked). Without a mask the photo's
backdrop (its corners' color) is taken out; a plain backdrop works, a busy one needs a mask.

Flow hints (optional, never required): lines drawn along the hair on a photo, in two forms:
  - strokes: polylines in the photo's pixels (from its top-left, v down), each from the root end to
    the tip: [[[u, v], [u, v], ...], ...], or [{"points": [...], "directed": False}, ...] when only
    the line is known. Drawn on the photo in the Reference window (or with the older Reference
    Flow tool, flow_tool: looking through the reference camera, drag over the photo root to tip;
    Ctrl+drag erases), or set with set_reference_hints; kept on the groom (attribute
    glReferenceViews, saved with the scene: {"views": [{"camera", "image", "width", "height",
    "strokes": [{"points", "directed"}], "mask", "mask_strokes": [{"points", "radius",
    "include"}]}]}), or given per view as "hints".
Mask touch-ups (optional): brush lines on a photo that mark hair ("include") or backdrop, taken
as sure by the mask's refinement and kept over it (mask_strokes, set_mask_strokes).
  - a painted overlay ("hint_image"): a PNG at the photo's size, the strokes opaque on a
    transparent layer, painted in any image editor; a stroke that gets lighter along its length
    (dark at the root end, light at the tip) gives its way too, a flat one only its line.
Near a hint its line replaces the photo's strand directions, and its way decides which way the
guides there leave their roots (and, with symmetry, their mirror images').

The measured look: besides the guides, the photos are measured for the hair's color (melanin,
redness, tips), its clumps (spacing and tightness across the flow), curls, flyaways and frizz,
its color variation and where it thins (the skin showing through). generate keeps the values on
the groom (measured_look); measured_look_preview lists them against the graph's;
apply_measured_look sets them, adding the Clump, Curl, frizz Noise or Flyaway node a value needs
where the usual chain has it (Clump, Curl, Noise, Flyaway before HairColor), and Scatter's
density from a vertex map (refDensity) where the hair thins. Rough measures, a starting point.
"""

import json
import os

from maya import cmds
from maya.api import OpenMaya as om

from groomlab.base import ensure_plugin, kept_selection, undo_chunk
from groomlab.guides import guides_from_curves, list_guide_groups, remove_guide_group

GROUP = "fromReference"


def _mesh(shape):
    """A Maya mesh (world space) as a groomlab_core ScalpMesh."""
    import groomlab_core as gl
    sel = om.MSelectionList()
    sel.add(shape)
    fn = om.MFnMesh(sel.getDagPath(0))
    points = [(p.x, p.y, p.z) for p in fn.getPoints(om.MSpace.kWorld)]
    counts, indices = fn.getTriangles()
    triangles = [tuple(indices[i:i + 3]) for i in range(0, len(indices), 3)]
    return gl.ScalpMesh(points, triangles)


VIEWS_ATTR = "glReferenceViews"
FLOW_CONTEXT = "groomLabReferenceFlowContext"


def _groom_shape(groom):
    shapes = cmds.ls(groom, type="groomLabShape", long=True) or \
        cmds.listRelatives(groom, shapes=True, type="groomLabShape", fullPath=True) or []
    if not shapes:
        raise ValueError("%s is not a groom" % groom)
    return shapes[0]


def reference_views(groom):
    """What the groom keeps of its reference photos: {"views": [{"camera", "image", "width",
    "height", "strokes": [{"points": [[u, v], ...], "directed"}]}]} (photo pixels, root first)."""
    shape = _groom_shape(groom)
    if not cmds.attributeQuery(VIEWS_ATTR, node=shape, exists=True):
        return {"views": []}
    try:
        data = json.loads(cmds.getAttr(shape + "." + VIEWS_ATTR) or "{}")
    except ValueError:
        data = {}
    data.setdefault("views", [])
    return data


def _store_views(groom, data):
    shape = _groom_shape(groom)
    if not cmds.attributeQuery(VIEWS_ATTR, node=shape, exists=True):
        cmds.addAttr(shape, longName=VIEWS_ATTR, dataType="string")
    cmds.setAttr(shape + "." + VIEWS_ATTR, json.dumps(data), type="string")


def _view_entry(data, camera):
    """The groom's entry for a reference camera (made when missing)."""
    from groomlab import reference
    camera = reference._camera(camera)
    for v in data["views"]:
        if v.get("camera") == camera:
            return v
    entry = {"camera": camera, "image": reference.image_path(camera), "width": cmds.getAttr(camera + ".glRefWidth"),
             "height": cmds.getAttr(camera + ".glRefHeight"), "strokes": []}
    data["views"].append(entry)
    return entry


def _clean(strokes):
    out = []
    for s in strokes or []:
        points = s["points"] if isinstance(s, dict) else s
        out.append({"points": [[float(p[0]), float(p[1])] for p in points],
                    "directed": bool(s.get("directed", True)) if isinstance(s, dict) else True})
    return out


def set_reference_hints(groom, camera, strokes):
    """Set the flow hint strokes kept for a reference camera on the groom (photo pixels, root end
    first: [[u, v], ...] each, or {"points": [...], "directed": bool}); [] clears them. One undo step."""
    with undo_chunk("reference flow"):
        data = reference_views(groom)
        _view_entry(data, camera)["strokes"] = _clean(strokes)
        _store_views(groom, data)


def reference_hints(groom, camera):
    """The flow hint strokes kept for a reference camera ([] without)."""
    from groomlab import reference
    camera = reference._camera(camera)
    for v in reference_views(groom)["views"]:
        if v.get("camera") == camera:
            return v.get("strokes", [])
    return []


def add_reference_stroke(groom, camera, points, directed=True):
    """One more flow stroke (photo pixels, root end first): what the Reference Flow tool calls on
    each drag. One undo step. Returns the stroke's index."""
    with undo_chunk("reference flow stroke"):
        data = reference_views(groom)
        strokes = _view_entry(data, camera)["strokes"]
        strokes.extend(_clean([{"points": points, "directed": directed}]))
        _store_views(groom, data)
    return len(strokes) - 1


def _segment_distance(p, a, b):
    du, dv = b[0] - a[0], b[1] - a[1]
    l2 = du * du + dv * dv
    t = max(0.0, min(1.0, ((p[0] - a[0]) * du + (p[1] - a[1]) * dv) / l2)) if l2 > 0 else 0.0
    return ((p[0] - a[0] - t * du) ** 2 + (p[1] - a[1] - t * dv) ** 2) ** 0.5


def erase_reference_strokes(groom, camera, path, radius=None):
    """Erase the flow strokes that come within `radius` photo pixels (default: a fiftieth of the
    photo's width) of `path` (photo pixels; one point for a click): the Reference Flow tool's
    Ctrl+drag. One undo step. Returns how many went."""
    data = reference_views(groom)
    entry = _view_entry(data, camera)
    if radius is None:
        radius = float(entry.get("width") or 1000) / 50.0
    path = [tuple(p) for p in path]

    def near(stroke):
        pts = stroke["points"]
        for q in path:
            if len(pts) == 1 and ((q[0] - pts[0][0]) ** 2 + (q[1] - pts[0][1]) ** 2) ** 0.5 <= radius:
                return True
            for a, b in zip(pts, pts[1:]):
                if _segment_distance(q, a, b) <= radius:
                    return True
        return False
    kept = [s for s in entry["strokes"] if not near(s)]
    gone = len(entry["strokes"]) - len(kept)
    if gone:
        with undo_chunk("reference flow erase"):
            entry["strokes"] = kept
            _store_views(groom, data)
    return gone


def _clean_mask(strokes):
    return [{"points": [[float(p[0]), float(p[1])] for p in s["points"]], "radius": float(s.get("radius", 8.0)),
             "include": bool(s.get("include", True))} for s in strokes or []]


def mask_strokes(groom, camera):
    """The mask touch-ups kept for a reference camera: [{"points": [[u, v], ...], "radius" (photo
    pixels), "include" (hair, else backdrop)}] ([] without)."""
    from groomlab import reference
    camera = reference._camera(camera)
    for v in reference_views(groom)["views"]:
        if v.get("camera") == camera:
            return v.get("mask_strokes", [])
    return []


def set_mask_strokes(groom, camera, strokes):
    """Set the mask touch-ups kept for a reference camera ([] clears them). One undo step."""
    with undo_chunk("reference mask strokes"):
        data = reference_views(groom)
        _view_entry(data, camera)["mask_strokes"] = _clean_mask(strokes)
        _store_views(groom, data)


def photo_state(groom, camera):
    """What the Reference window edits for one photo, as the core's ReferencePhotoEditor reads it
    (JSON text): the camera's landmark pairs (glRefPairs: each side's points in order, a pair
    missing one side only at the end), the groom's flow strokes and mask touch-ups for it."""
    from groomlab import reference
    pairs = reference.pairs(camera)
    images = [p["image"] for p in pairs if p.get("image")]
    meshes = [p["mesh"] for p in pairs if p.get("mesh")]
    landmarks = [{"image": images[i] if i < len(images) else None, "mesh": meshes[i] if i < len(meshes) else None}
                 for i in range(max(len(images), len(meshes)))]
    return json.dumps({"landmarks": landmarks, "strokes": reference_hints(groom, camera),
                       "mask_strokes": mask_strokes(groom, camera)})


def store_photo_state(groom, camera, text, errors=None):
    """Keep what the Reference window edited for one photo (photo_state's form): the landmarks as
    the camera's pairs (each complete pair's solve error from `errors`, pixels), the strokes and
    mask touch-ups on the groom. Not an undo step of its own: the window keeps its own stack, and
    scripts wrap it in theirs."""
    from groomlab import reference
    state = json.loads(text or "{}")
    pairs = []
    for i, p in enumerate(state.get("landmarks", [])):
        entry = {"image": p.get("image"), "mesh": p.get("mesh")}
        if errors and i < len(errors) and entry["image"] and entry["mesh"]:
            entry["error"] = errors[i]
        pairs.append(entry)
    reference._store_pairs(camera, pairs)
    data = reference_views(groom)
    entry = _view_entry(data, camera)
    entry["strokes"] = _clean(state.get("strokes"))
    entry["mask_strokes"] = _clean_mask(state.get("mask_strokes"))
    _store_views(groom, data)


def flow_tool(groom, camera, activate=True):
    """The Reference Flow tool for a groom and one of its reference cameras: looking through the
    camera, drag over the photo from root to tip to draw a flow stroke; Ctrl+drag (or Ctrl+click
    on one) erases. The strokes draw as arrows on the photo while the tool is on. Returns the
    tool's context."""
    from groomlab import reference
    ensure_plugin()
    camera = reference._camera(camera)
    if not cmds.contextInfo(FLOW_CONTEXT, exists=True):
        cmds.groomLabReferenceFlowCtx(FLOW_CONTEXT)
    cmds.groomLabReferenceFlowCtx(FLOW_CONTEXT, edit=True, camera=camera, groom=_groom_shape(groom))
    if activate:
        cmds.setToolTo(FLOW_CONTEXT)
    return FLOW_CONTEXT


def _camera_view(groom, camera, image=None, mask=None, hints=None, hint_image=None, masks=None):
    """A view for the core from a reference camera: its world matrix, focal length and film back,
    the photo's size; the photo (its image plane's, unless given), the mask, the flow hints and the
    mask touch-ups (the ones the groom keeps for the camera, unless given)."""
    from groomlab import reference
    camera = reference._camera(camera)
    view = {
        "camera": reference.camera_dict(camera),
        "image": image or reference.image_path(camera),
        "mask": mask,
        "hints": hints if hints is not None else reference_hints(groom, camera),
        "hint_image": hint_image,
        "mask_strokes": masks if masks is not None else mask_strokes(groom, camera),
    }
    if not view["image"] or not os.path.isfile(view["image"]):
        raise ValueError("%s's photo is missing: %s" % (camera, view["image"]))
    return view


def guides_from_reference(groom, views, group=GROUP, replace=True, guide_count=300, symmetry="auto",
                          max_length=None, occluders=None, tube_radius=0.1, **options):
    """Grow guides from reference photos into guide group `group` (replaced when `replace`).

    `views`: reference cameras (groomlab.reference), or dicts {"camera": camera, "mask": path,
    "image": path, "hints": strokes, "hint_image": path} to give a mask (white where the hair is),
    another image or flow hints (see above; the strokes the groom keeps for the camera otherwise). `symmetry`: "auto"
    (the scalp's topology when it mirrors), "x", "topology" or "none". `max_length`: the longest
    hair (scene units; None: the scalp's size; negative: that many times the rim of hair the
    photos show past the scalp, for fur). `occluders`: meshes that hide hair (a head under a scalp
    cap). Other `options` go to groomlab_core.guides_from_reference (orientation, priors...).

    Returns the core's result without the guides' points: {"guide_ids", "symmetry", "roots_tried",
    "roots_unseen", "roots_bald", "look" ({"melanin", "redness", "base_color", "clump_size", ...}),
    "measured" (measured_look_values: graph params by node type), ...}. One undo step."""
    import groomlab_core as gl
    from groomlab.groom import scalp_mesh

    ensure_plugin()
    shape = scalp_mesh(groom)
    if not shape:
        raise ValueError("%s has no scalp" % groom)
    scalp = _mesh(shape)
    prepared = []
    for v in views:
        if isinstance(v, dict):
            prepared.append(_camera_view(groom, v["camera"], v.get("image"), v.get("mask"), v.get("hints"),
                                         v.get("hint_image"), v.get("mask_strokes")))
        else:
            prepared.append(_camera_view(groom, v))
    kwargs = dict(options)
    kwargs.update(guide_count=guide_count, symmetry=symmetry)
    if max_length is not None:
        kwargs["max_length"] = float(max_length)
    if occluders:
        kwargs["occluders"] = [_mesh(m) for m in occluders]
    result = gl.guides_from_reference(scalp, prepared, **kwargs)

    ids = []
    with undo_chunk("guides from reference"), kept_selection():  # (the groom stays the active one)
        if replace and group in [g["name"] for g in list_guide_groups(groom)]:
            remove_guide_group(groom, group)
        curves = []
        try:
            for g in result["guides"]:
                curves.append(cmds.curve(degree=1, point=[tuple(p) for p in g["points"]]))
            if curves:
                ids = guides_from_curves(groom, curves, group=group, tube_radius=tube_radius)
        finally:
            if curves:
                cmds.delete(curves)
    out = {k: v for k, v in result.items() if k not in ("guides", "scalp_coverage")}
    out["guide_ids"] = list(ids or [])
    box = cmds.exactWorldBoundingBox(shape)
    out["measured"] = measured_look_values(result, max(box[3] - box[0], box[4] - box[1], box[5] - box[2]))
    return out


# ---------------------------------------------------------------------------
# The measured look

# The look nodes a measured value may add, in the usual chain's order (the presets'): each goes
# before the first node of the chain that comes after it.
LOOK_ORDER = ("Clump", "Curl", "Noise", "Flyaway")
_LATER = ("HairColor", "Bands", "Width", "Resample", "Cards", "Collide", "Cull", "Output")
DENSITY_MAP = "refDensity"


def _r(value, digits=3):
    return round(float(value), digits)


def measured_look_values(result, scalp_size):
    """The graph values the photos suggest, by node type, from guides_from_reference's core result
    (its "look" and "scalp_coverage"): {"HairColor": {...}, "Clump": {...}, "Curl": {...}, "Noise"
    (frizz): {...}, "Flyaway": {...}, "Scatter": {"densityMap": [a factor per scalp vertex]}}; a
    node is left out when the photos show none of it (no clumps, curls, strays, frizz or thinning)."""
    import groomlab_core as gl
    look = result.get("look") or {}
    out = {}
    if not look.get("ok"):
        return out
    color = {"mode": "melanin", "melanin": _r(look["melanin"]), "redness": _r(look["redness"])}
    if look.get("tip_strength", 0.0) > 0.0:
        color["tipStrength"] = _r(look["tip_strength"])
        color["tipColor"] = [_r(c) for c in look["tip_color"]]
    out["HairColor"] = color
    if look.get("clumped"):
        out["Clump"] = {"clumpBy": "size", "size": _r(look["clump_size"]), "strength": _r(look["clump_strength"])}
    if look.get("curly"):
        out["Curl"] = {"radius": _r(look["curl_radius"]), "frequency": _r(look["curl_frequency"])}
    if look.get("frizz_fraction", 0.0) > 0.0:
        out["Noise"] = {"fraction": _r(look["frizz_fraction"]), "amplitude": _r(0.005 * scalp_size),
                        "frequency": 10.0, "octaves": 1}
    if look.get("flyaway_fraction", 0.0) > 0.0:
        out["Flyaway"] = {"fraction": _r(look["flyaway_fraction"])}
    coverage = result.get("scalp_coverage") or []
    seen = [c for c in coverage if c >= 0.0]
    if seen:
        factors = [_r(gl.density_of_coverage(c), 3) if c >= 0.0 else 1.0 for c in coverage]
        thin = sum(1 for c in seen if gl.density_of_coverage(c) < 0.6)
        if thin >= max(3, 0.03 * len(seen)):
            out["Scatter"] = {"densityMap": factors}
    return out


def measured_look(groom):
    """The look the last generate measured from the photos (measured_look_values), or None."""
    return reference_views(groom).get("measured") or None


def _chain(model):
    """The output's strands chain, source first."""
    chain, node = [], model.output
    while node and node not in chain:
        chain.insert(0, node)
        node = model.source_of(node, "strands")
    return chain


def _is_frizz(model, name):
    return model.node_type(name) == "Noise" and (name.startswith("frizz") or float(model.param(name, "frequency")) >= 8.0)


def _target(model, op_type, chain):
    """The node a measured op_type's values go to, or None (to add)."""
    for name in chain:
        if model.node_type(name) != op_type:
            continue
        if op_type == "Noise" and not _is_frizz(model, name):
            continue  # frizz is its own Noise, not the hair's overall wave
        return name
    return None


def _insert_at(model, op_type, chain):
    """Where a new op_type node goes: before the first chain node that comes after it (a later
    look node, or the color, width and output end of the chain)."""
    rank = LOOK_ORDER.index(op_type)
    for name in chain:
        t = model.node_type(name)
        if not model.source_of(name, "strands"):
            continue
        if t in _LATER or (t in LOOK_ORDER and LOOK_ORDER.index(t) > rank):
            return name
    return model.output


def measured_look_preview(groom, measured=None):
    """What apply_measured_look would change: [{"type", "node" (None: added), "param", "current"
    (None for a new node), "value"}], the density map as one row ("densityMap": the share of the
    scalp it thins)."""
    from groomlab import graph as gm
    measured = measured or measured_look(groom)
    if not measured:
        return []
    model = gm.GraphModel.from_groom(groom)
    chain = _chain(model)
    rows = []
    for op_type in ("HairColor",) + LOOK_ORDER + ("Scatter",):
        values = measured.get(op_type)
        if not values:
            continue
        node = model.first_of(op_type) \
            if op_type in ("HairColor", "Scatter") else _target(model, op_type, chain)
        for param, value in values.items():
            if param == "densityMap":
                thin = sum(1 for f in value if f < 0.6) / float(max(1, len(value)))
                rows.append({"type": op_type, "node": node, "param": "density", "current": None,
                             "value": "map %s (thinner on %d%% of the scalp)" % (DENSITY_MAP, round(100 * thin))})
                continue
            rows.append({"type": op_type, "node": node, "param": param,
                         "current": model.param(node, param) if node else None, "value": value})
    return rows


def _set_vertex_map(groom, name, values):
    """A vertex map's values (one per scalp vertex) on the groom's scalpMaps."""
    maps = json.loads(cmds.getAttr(groom + ".scalpMaps") or "{}")
    entry = maps.get(name)
    if not isinstance(entry, dict) or entry.get("kind") != "vertices":
        raise ValueError("no vertex map named %s" % name)
    if len(entry.get("values", [])) != len(values):
        raise ValueError("the scalp has %d vertices, the measured density %d" % (len(entry.get("values", [])), len(values)))
    entry["values"] = [float(v) for v in values]
    cmds.setAttr(groom + ".scalpMaps", json.dumps(maps, separators=(",", ":")), type="string")


def apply_measured_look(groom, measured=None, types=None):
    """Set the measured look (measured_look, or `measured`) on the groom's graph: each node type's
    values on its node (the first HairColor and Scatter; the chain's Clump, Curl, frizz Noise and
    Flyaway), the node added where the usual chain has it when the graph has none. `types`: only
    these node types. Scatter's density reads a vertex map (refDensity, made or refilled) where the
    photos show the hair thinning. Returns the nodes set ({type: node}). One undo step."""
    from groomlab import graph as gm
    from groomlab import painting
    measured = dict(measured or measured_look(groom) or {})
    if not measured:
        raise ValueError("nothing measured yet: Generate first")
    if types is not None:
        measured = {k: v for k, v in measured.items() if k in types}
    done = {}
    with undo_chunk("apply measured look"):
        model = gm.GraphModel.from_groom(groom)
        for op_type in ("HairColor",) + LOOK_ORDER:
            values = measured.get(op_type)
            if not values:
                continue
            chain = _chain(model)
            if op_type == "HairColor":
                node = model.first_of("HairColor")
                if node is None:
                    node = model.add_node("HairColor")
                    model.insert_before(_insert_at_color(model, chain), node)
            else:
                node = _target(model, op_type, chain)
                if node is None:
                    node = model.add_node(op_type, name=model.unique_name("frizz" if op_type == "Noise" else op_type))
                    model.insert_before(_insert_at(model, op_type, chain), node)
            for param, value in values.items():
                model.set_param(node, param, value)
            done[op_type] = node
        model.save(groom)
        density = (measured.get("Scatter") or {}).get("densityMap")
        if density:
            scatter = model.first_of("Scatter")
            if scatter:
                painting.use_map(groom, scatter, "density", DENSITY_MAP, kind="vertices")
                _set_vertex_map(groom, DENSITY_MAP, density)
                done["Scatter"] = scatter
    return done


def _insert_at_color(model, chain):
    for name in chain:
        if model.node_type(name) in ("Width", "Resample", "Cards", "Output") and model.source_of(name, "strands"):
            return name
    return model.output


# ---------------------------------------------------------------------------
# What Groom Tools > Photos > Guides from Photos does


def view_mask(groom, camera):
    """The hair mask kept for a reference camera on the groom (a path), or None (automatic)."""
    from groomlab import reference
    camera = reference._camera(camera)
    for v in reference_views(groom)["views"]:
        if v.get("camera") == camera:
            return v.get("mask") or None
    return None


def set_view_mask(groom, camera, path):
    """Keep a hair mask (an image the photo's size, white where the hair is) for a reference camera
    on the groom; None: the automatic one (the photo's plain backdrop taken out). One undo step."""
    if path and not os.path.isfile(path):
        raise ValueError("no mask image at %s" % path)
    with undo_chunk("reference mask"):
        data = reference_views(groom)
        _view_entry(data, camera)["mask"] = path.replace("\\", "/") if path else None
        _store_views(groom, data)


def clear_reference_strokes(groom, camera):
    """Forget the flow strokes drawn on a reference camera's photo. One undo step."""
    set_reference_hints(groom, camera, [])


def generate(groom, cameras=None, guide_count=300, symmetry="auto", max_length=None, select=True, **options):
    """Guides from Photos as the Groom Tools run it: every reference camera (or `cameras`), each
    with the mask and the flow strokes the groom keeps for it, into the fromReference group
    (replaced). The measured look is kept on the groom for apply_measured_look. With `select`,
    the new guides are selected. Returns guides_from_reference's result."""
    from groomlab import reference
    cameras = cameras or reference.list_references()
    if not cameras:
        raise ValueError("no reference photos: import one (Groom Tools > Photos)")
    views = [{"camera": c, "mask": view_mask(groom, c)} for c in cameras]
    with undo_chunk("guides from reference"):  # the guides, the measured look and the selection: one step
        result = guides_from_reference(groom, views, guide_count=guide_count, symmetry=symmetry, max_length=max_length,
                                       **options)
        data = reference_views(groom)
        data["measured"] = result.get("measured") or None
        _store_views(groom, data)
        if select and result["guide_ids"]:
            from groomlab.guides import select_guides
            try:
                select_guides(groom, group=GROUP)
            except (RuntimeError, ValueError):
                pass
    return result


