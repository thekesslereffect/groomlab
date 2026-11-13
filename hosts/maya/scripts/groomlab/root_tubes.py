# SPDX-License-Identifier: Apache-2.0
"""Tonic-style tubes in the Tube tool: tubes drawn root first (issue 61) and the tube hierarchy
(issue 62).

Root-first tubes: the artist clicks the root's outline on the scalp one CV at a time; closing it on
its first CV makes a tube guide rooted at the outline's middle, its tube's root section that
outline exactly (a library shape of its own, "rootTube_<n>"), going out along a drawn curve or
straight off the scalp. Each root tube is its own region: a color vertex map on the scalp
(REGION_MAP) holds every root tube's area in a color of its own, black elsewhere, and the groom's
Interpolate nodes with no region map read it, so the hair rooted in a tube's area follows only that
tube's guides. Editing a root's CVs later moves the guide to the new outline's middle and gives
its shape the new outline; the regions follow.

    from groomlab import root_tubes
    guide = root_tubes.make_root_tube(groom, outline, path=None, length=2.0)
    cvs = root_tubes.root_outline(groom, guide)            # the root's CVs, on the scalp
    root_tubes.edit_root_tube(groom, guide, moved_cvs)       # one undo step, regions updated

The tube hierarchy: a base group of tubes (level 1) and below it groups that split each tube in
two (ChildGenerationParams::split: side by side across the tube's longer side), level by level, up
to MAX_LEVELS. Each level is a child group of the one above it ("<base>_L<n>"), its guides
following their parents', so shaping a tube carries the smaller tubes inside it.

    root_tubes.add_tube_level(groom)                          # every leaf tube split in two
    root_tubes.tube_levels(groom)                             # {"base", "levels": [groups]}
    root_tubes.show_tube_level(groom, "tubes", 2)             # level 2 shown, the others hidden
"""

import colorsys
import json
import math

from maya import cmds

import groomlab
from groomlab import tube_mesh as tm

ROOT_GROUP = "rootTubes"   # the root group new root tubes go into (when no group is picked)
SHAPE_PREFIX = "rootTube_"  # each root tube's library shape: its root outline
REGION_MAP = "tubeRoots"   # the color vertex map holding each root tube's area
MAX_LEVELS = 9              # the tube hierarchy's deepest level (1: the base)
LEVEL_SUFFIX = "_L"         # level n's group: <base>_L<n>
SNAP_PIXELS = 12            # how near (in view pixels) a CV snaps to a neighbouring root's CV or edge
ROOT_LIFT = 0.05            # the guide's second point off the scalp: a share of its length
_RECENTER_PASSES = 4        # the guide moved onto its root shape's box middle at most so often


# ---------------------------------------------------------------------------
# Small helpers


def _hierarchy(groom):
    return groomlab.get_hierarchy(groom) or {}


def _guides_by_id(data):
    return {int(g["id"]): g for g in data.get("guides", [])}


def _groups_by_id(data):
    return {int(g["id"]): g for g in data.get("groups", [])}


def _group_named(data, name):
    return next((g for g in data.get("groups", []) if g["name"] == name), None)


def _shape_of(tube):
    return (tube or {}).get("shape", "")


def _library_shape(groom, name):
    return next((s for s in groomlab.tube_shapes(groom) if s["name"] == name), None)


def _frames(groom, guide):
    return json.loads(cmds.groomLabGuides(groom, tubeFrames=str(guide), evaluated=True))


def _scalp_surface(groom):
    from groomlab import utils
    surface = utils._scalp_surface(groom)
    if surface is None:
        raise ValueError("the groom has no scalp to root a tube on")
    return surface


def _centroid(points):
    """The area centroid of a closed outline (3D points roughly on a plane): the polygon's centroid
    in its best plane (Newell's normal), the mean where it has no area."""
    n = len(points)
    mean = tm._mean(points)
    normal = (0.0, 0.0, 0.0)
    for i in range(n):
        a, b = points[i], points[(i + 1) % n]
        normal = tm._add(normal, ((a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]),
                                  (a[0] - b[0]) * (a[1] + b[1])))
    if tm._length(normal) < 1e-12:
        return mean
    nz = tm._unit(normal)
    u = tm._unit(tm._cross(nz, (1.0, 0.0, 0.0) if abs(nz[0]) < 0.9 else (0.0, 1.0, 0.0)))
    v = tm._cross(nz, u)
    flat = [tm.to_plane(p, mean, u, v) for p in points]
    area, cx, cy = 0.0, 0.0, 0.0
    for i in range(n):
        (x0, y0), (x1, y1) = flat[i], flat[(i + 1) % n]
        c = x0 * y1 - x1 * y0
        area += c
        cx += (x0 + x1) * c
        cy += (y0 + y1) * c
    if abs(area) < 1e-18:
        return mean
    return tm.from_plane((cx / (3.0 * area), cy / (3.0 * area)), mean, u, v)


def _resample(points, count):
    """`count` points evenly along a polyline (2 or more)."""
    pts = [tuple(p) for p in points]
    if len(pts) < 2:
        return pts
    along = [0.0]
    for a, b in zip(pts, pts[1:]):
        along.append(along[-1] + tm._dist(a, b))
    total = along[-1]
    if total <= 1e-12:
        return [pts[0]] * count
    out, k = [], 0
    for j in range(count):
        s = total * j / float(count - 1)
        while k < len(pts) - 2 and along[k + 1] < s:
            k += 1
        span = along[k + 1] - along[k]
        f = (s - along[k]) / span if span > 1e-12 else 0.0
        out.append(tm.lerp(pts[k], pts[k + 1], min(max(f, 0.0), 1.0)))
    return out


def _guide_path(root, normal, path, length, segments):
    """The guide's points: from `root` straight off the scalp (`length`), or along the drawn `path`
    (its first point taken to the root), `segments` + 1 of them, the second straight off the scalp
    so the root section lies on it."""
    segments = max(int(segments), 2)
    if path and len(path) >= 2:
        drawn = [tuple(p) for p in path]
        drawn[0] = tuple(root)
        pts = _resample(drawn, segments + 1)
        total = sum(tm._dist(a, b) for a, b in zip(pts, pts[1:]))
    else:
        total = float(length)
        pts = [tm._add(root, tm._mul(normal, total * i / float(segments))) for i in range(segments + 1)]
    if total <= 1e-9:
        raise ValueError("the tube needs a length")
    pts[1] = tm._add(root, tm._mul(normal, max(ROOT_LIFT * total, tm._dist(pts[0], pts[1]) * 0.5)))
    return pts


def _reshape(groom, guide, points):
    from groomlab import utils
    utils._reshape(groom, guide, points)


def _new_guide(groom, points, group):
    """A primary guide through `points` exactly, in root group `group` (made when missing)."""
    from groomlab import utils
    if not any(g["name"] == group for g in groomlab.list_guide_groups(groom)):
        group = groomlab.add_guide_group(groom, group)
    curve = utils._curve("glRootTube", points)
    try:
        guide = groomlab.guides_from_curves(groom, [curve], group=group)[0]
    finally:
        cmds.delete(curve)
    _reshape(groom, guide, points)
    return guide


def _new_shape_name(groom):
    taken = set(s["name"] for s in groomlab.tube_shapes(groom))
    n = 1
    while "%s%d" % (SHAPE_PREFIX, n) in taken:
        n += 1
    return "%s%d" % (SHAPE_PREFIX, n)


def _root_fit(groom, guide, outline):
    """The guide's root frame and the outline in it: (center, u, v, flat points (u, v))."""
    frames = _frames(groom, guide)
    center, u, v = tm.frame_at(frames, 0.0)
    return center, u, v, [tm.to_plane(p, center, u, v) for p in outline]


def _solve3(m, r):
    """m (3x3) x = r by Cramer's rule (None when singular)."""
    def det(a):
        return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    d = det(m)
    if abs(d) < 1e-18:
        return None
    out = []
    for c in range(3):
        a = [list(row) for row in m]
        for k in range(3):
            a[k][c] = r[k]
        out.append(det(a) / d)
    return out


def _affine_error(src, dst):
    """How far an affine map can take `src` points onto `dst` (each its own): the least squares'
    summed squared residual."""
    m = [[0.0] * 3 for _ in range(3)]
    for x, y in src:
        row = (x, y, 1.0)
        for i in range(3):
            for j in range(3):
                m[i][j] += row[i] * row[j]
    err = 0.0
    for axis in (0, 1):
        r = [sum(row[i] * q[axis] for row, q in zip(((x, y, 1.0) for x, y in src), dst)) for i in range(3)]
        coef = _solve3(m, r)
        if coef is None:
            return float("inf")
        err += sum((coef[0] * x + coef[1] * y + coef[2] - q[axis]) ** 2 for (x, y), q in zip(src, dst))
    return err


def _candidates(given, stored):
    """The library's points (`stored`: as GroomLab keeps them, fitted into its box and maybe turned
    and reordered) matched to the `given` points they came from, point for point: the cyclic
    orders an affine map takes onto them as well as the best one does (both run counter-
    clockwise: no mirror image). A symmetric outline (a regular polygon) matches at several."""
    n = len(given)
    if len(stored) != n:
        return []
    scored = []
    for shift in range(n):
        cand = list(stored[shift:]) + list(stored[:shift])
        scored.append((_affine_error(cand, given), cand))
    least = min(e for e, _c in scored)
    size = sum(x * x + y * y for x, y in given) or 1.0
    return [c for e, c in scored if e <= least + 1e-9 * size]


def _correspond(given, stored):
    """The best of _candidates (the first, when several match as well)."""
    found = _candidates(given, stored)
    return found[0] if found else None


def _best_section(pts, stored):
    """The section (fit_section: twist, width, height, center offset) that puts the library's
    points on `pts`: of the orders matching as well (a symmetric outline), the one whose box's
    middle lies nearest the guide, so fitting again after the guide moved there picks it again.
    None when they do not match."""
    best = None
    for fitted in _candidates(pts, stored):
        section = tm.fit_section(pts, fitted)
        off = math.hypot(*section[3])
        if best is None or off < best[0] - 1e-12:
            best = (off, section)
    return best[1] if best else None


def _fit(groom, guide, shape, outline):
    """The tube's root section on `outline`: `shape` (a library name; made or given these points)
    fitted to the outline in the guide's root frame. Returns (twist, width, height, center offset
    (u, v)) and the outline's points as the shape keeps them."""
    _center, _u, _v, flat = _root_fit(groom, guide, outline)
    pts, _indices = tm.shape_points(flat)
    if _library_shape(groom, shape) is None:
        groomlab.add_tube_shape(groom, shape, pts, smooth=False)
    else:
        groomlab.set_tube_shape_points(groom, shape, pts, smooth=False)
    section = _best_section(pts, _library_shape(groom, shape)["points"])
    if section is None:
        raise RuntimeError("the root outline's shape %s does not match its CVs" % shape)
    return section


def _settle(groom, guide, shape, outline, passes=2):
    """The root section fitted again on the guide as it now lies (its tube set: the roll GroomLab
    works out on the scalp follows the section's size), the whole tube turned and scaled as much
    (`passes` times, or until it holds)."""
    for _ in range(passes):
        _center, _u, _v, flat = _root_fit(groom, guide, outline)
        pts, _indices = tm.shape_points(flat)
        section = _best_section(pts, _library_shape(groom, shape)["points"])
        if section is None:
            return
        twist, w, h, _off = section
        tube = groomlab.get_tube(groom, guide)
        w0, t0 = tube["width"][0] or 1e-9, tube["twist"][0]
        a0 = tube["aspect"][0] or 1.0
        a_new = min(max(h / w, 0.01), 100.0) if w > 1e-12 else a0
        twist += 2.0 * math.pi * round((t0 - twist) / (2.0 * math.pi))
        if abs(twist - t0) < 1e-5 and abs(w / w0 - 1.0) < 1e-5 and abs(a_new / a0 - 1.0) < 1e-5:
            return
        groomlab.set_tube(groom, guides=[guide], width=[x * w / w0 for x in tube["width"]],
                          aspect=[min(max(a * a_new / a0, 0.01), 100.0) for a in tube["aspect"]],
                          twist=[t + twist - t0 for t in tube["twist"]])


def _project(groom, points):
    """Points taken onto the scalp (the nearest point on it)."""
    surface = _scalp_surface(groom)
    return [surface(p)[0] for p in points]


def _project_along(groom, points, direction):
    """Points taken onto the scalp along `direction` (the nearest hit on the line through each,
    either way), else to the nearest point on it. The inverse of flattening a scalp point onto a
    root's plane (to_plane drops it along the plane's normal): an outline read off the scalp into
    the root plane and put back lands on the same CVs, however the scalp curves there."""
    from maya.api import OpenMaya as om
    from groomlab import utils
    fns = [utils._om_mesh(s) for s in sorted(utils._groom_meshes(groom))]
    surface = _scalp_surface(groom)
    d = om.MFloatVector(*tm._unit(direction))
    out = []
    for p in points:
        source = om.MFloatPoint(*p)
        best = None
        for fn in fns:
            try:
                found = fn.closestIntersection(source, d, om.MSpace.kWorld, 1e6, True)
            except RuntimeError:
                continue
            if not found or found[2] < 0:
                continue
            if best is None or abs(found[1]) < best[0]:
                best = (abs(found[1]), (found[0].x, found[0].y, found[0].z))
        near = surface(p)[0]
        # (A hit far past the nearest point: the line grazes the scalp; the nearest point instead.)
        if best is None or best[0] > 4.0 * tm._dist(p, near) + 1e-6:
            out.append(tuple(near))
        else:
            out.append(best[1])
    return out


# ---------------------------------------------------------------------------
# Root-first tubes


def make_root_tube(groom, outline, path=None, length=1.0, segments=12, taper=0.5, group=None, regions=True):
    """A tube guide grown from its root's outline: `outline` the root's CVs on the scalp (world
    points, 3 or more, in order round it). The guide roots at the outline's middle and goes along
    the drawn `path` (world points from the root out; its first point is taken to the root), or
    with none straight off the scalp `length` long, in `segments` segments. Its tube: a library
    shape of its own (rootTube_<n>, removed with the last guide that takes it) whose outline at
    the root is the drawn one (straight edges between the CVs), its width going to `taper` times
    the root's at the tip. The tube is swept from its root along the guide with no twist (its
    sections do not roll on the scalp: roll_on_scalp off). Into `group` (default ROOT_GROUP, made when missing). With
    `regions`, the tube's area becomes a region of its own (update_regions). One undo step.
    Returns the guide's ID."""
    outline = [tuple(p) for p in outline]
    if len(outline) < 3:
        raise ValueError("a root outline needs at least 3 CVs")
    if len(outline) > tm.MAX_SHAPE_POINTS:
        raise ValueError("a root outline takes at most %d CVs" % tm.MAX_SHAPE_POINTS)
    surface = _scalp_surface(groom)
    with groomlab.undo_chunk("root tube"):
        outline = [surface(p)[0] for p in outline]
        root, normal = surface(_centroid(outline))
        points = _guide_path(root, normal, path, length, segments)
        guide = _new_guide(groom, points, group or ROOT_GROUP)
        radius = max(tm._dist(root, p) for p in outline)
        # A placeholder shaped tube first, so the guide's frame is laid across the scalp (as it
        # will lie), then the outline read in it.
        groomlab.set_tube(groom, guides=[guide], shape="ribbon", width=radius, aspect=1.0, twist=0.0,
                          roll_on_scalp=False)
        shape = _new_shape_name(groom)
        twist, w, h, off = _fit(groom, guide, shape, outline)
        # The shape fills its box: the guide runs through the box's middle, which may lie off the
        # area centroid (an uneven outline). Moved there and fitted again (till it holds: the
        # root plane turns a hair with the scalp as the root moves).
        for _ in range(_RECENTER_PASSES):
            if math.hypot(*off) <= 1e-5 * max(w, 1e-9):
                break
            center, u, v, _flat = _root_fit(groom, guide, outline)
            moved_root, normal = surface(tm.from_plane(off, center, u, v))
            if path and len(path) >= 2:  # the drawn curve moved with its root
                delta = tm._sub(moved_root, points[0])
                path = [tm._add(p, delta) for p in path]
            points = _guide_path(moved_root, normal, path, length, segments)
            _reshape(groom, guide, points)
            twist, w, h, off = _fit(groom, guide, shape, outline)
        groomlab.set_tube(groom, guides=[guide], shape=shape, width=[w, w * float(taper)],
                          aspect=min(max(h / w, 0.01), 100.0) if w > 1e-12 else 1.0, twist=twist)
        _settle(groom, guide, shape, outline)
        if regions:
            update_regions(groom)
    return guide


def is_root_tube(guide_data):
    """Whether a guide (the stored hierarchy's dict) is a root-first tube: a primary whose tube
    takes a rootTube_ shape."""
    return not guide_data.get("parent") and _shape_of(guide_data.get("tube")).startswith(SHAPE_PREFIX)


def root_tubes(groom, guides=None):
    """The root-first tubes' guide IDs (of `guides` when given: those among them, or the root
    tubes their tubes descend from), in ID order."""
    data = _hierarchy(groom)
    by_id = _guides_by_id(data)
    roots = set()
    if guides:
        for g in guides:
            gd = by_id.get(int(g))
            while gd is not None and gd.get("parent"):
                gd = by_id.get(int(gd["parent"]))
            if gd is not None and is_root_tube(gd):
                roots.add(int(gd["id"]))
    else:
        roots = set(i for i, gd in by_id.items() if is_root_tube(gd))
    return sorted(roots)


def root_outline(groom, guide, on_scalp=True):
    """A root tube's CVs as world points: its shape's control points placed in its root section,
    taken onto the scalp (`on_scalp`: along the root section's normal, the way they were read off
    it, so the CVs come back where they were drawn)."""
    tube = groomlab.get_tube(groom, guide)
    shape = _library_shape(groom, tube["shape"])
    if shape is None or shape["builtin"]:
        raise ValueError("guide %s is not a root tube (its shape is %s)" % (guide, tube["shape"]))
    center, u, v = tm.frame_at(_frames(groom, guide), 0.0)
    w = tube["width"][0]
    h = w * tube["aspect"][0]
    flat = tm.place_outline(shape["points"], tube["twist"][0], w, h, (0.0, 0.0))
    world = [tm.from_plane(p, center, u, v) for p in flat]
    return _project_along(groom, world, tm._cross(u, v)) if on_scalp else world


def edit_root_tube(groom, guide, outline, regions=True):
    """A root tube given a new root outline (`outline`: its CVs, world points, as root_outline
    gives them, moved): the guide moved whole so it roots at the new outline's middle, its shape
    the new outline, its root section fitted to it and the rest of the tube scaled, flattened and
    turned as much (its taper and what brushes painted along it kept). Its child tubes follow.
    `regions`: the regions updated. One undo step."""
    outline = [tuple(p) for p in outline]
    if len(outline) < 3:
        raise ValueError("a root outline needs at least 3 CVs")
    surface = _scalp_surface(groom)
    tube = groomlab.get_tube(groom, guide)
    shape = tube["shape"]
    with groomlab.undo_chunk("edit root tube"):
        if tube.get("rollOnScalp", True):  # (a root tube made before it was swept with no roll)
            groomlab.set_tube(groom, guides=[guide], roll_on_scalp=False)
        outline = [surface(p)[0] for p in outline]
        frames = _frames(groom, guide)
        old = [tuple(p) for p in frames["points"]]
        root, normal = surface(_centroid(outline))
        move = tm._sub(root, old[0])
        moved = [root] + [tm._add(p, move) for p in old[1:]]
        _reshape(groom, guide, moved)
        twist, w, h, off = _fit(groom, guide, shape, outline)
        for _ in range(_RECENTER_PASSES):  # (on the box's middle, as make_root_tube puts it)
            if math.hypot(*off) <= 1e-5 * max(w, 1e-9):
                break
            center, u, v, _flat = _root_fit(groom, guide, outline)
            root2, _n = surface(tm.from_plane(off, center, u, v))
            move = tm._sub(root2, moved[0])
            moved = [root2] + [tm._add(p, move) for p in moved[1:]]
            _reshape(groom, guide, moved)
            twist, w, h, off = _fit(groom, guide, shape, outline)
        w0 = tube["width"][0] or 1e-9
        a0 = tube["aspect"][0] or 1.0
        t0 = tube["twist"][0]
        a_new = min(max(h / w, 0.01), 100.0) if w > 1e-12 else 1.0
        groomlab.set_tube(groom, guides=[guide], width=[x * w / w0 for x in tube["width"]],
                          aspect=[min(max(a * a_new / a0, 0.01), 100.0) for a in tube["aspect"]],
                          twist=[t + twist - t0 for t in tube["twist"]])
        _settle(groom, guide, shape, outline)
        if regions:
            update_regions(groom)
    return guide


# ---------------------------------------------------------------------------
# Regions: each root tube's area a color of its own


def region_colors(count):
    """`count` colors far apart (hues a golden step apart, three brightnesses), none black."""
    out = []
    for k in range(count):
        hue = (k * 0.618033988749895) % 1.0
        value = (0.95, 0.7, 0.5)[(k // 7) % 3]
        out.append(colorsys.hsv_to_rgb(hue, 0.8, value))
    return out


def _scalp_points(groom):
    """The scalps' vertices in world space, in the order the groom joins them (its own scalp, then
    the others by index), and each scalp's MFnMesh with its first vertex's index."""
    from groomlab import utils
    meshes = [groomlab.scalp_mesh(groom)] + [s["mesh"] for s in groomlab.scalps(groom)]
    points, fns = [], []
    for shape in meshes:
        if not shape:
            continue
        from maya.api import OpenMaya as om
        fn = utils._om_mesh(shape)
        fns.append((fn, len(points)))
        points.extend((p.x, p.y, p.z) for p in fn.getPoints(om.MSpace.kWorld))
    return points, fns


def _inside(poly, x, y):
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / ((yj - yi) or 1e-30) + xi:
            inside = not inside
        j = i
    return inside


def _region_map(groom):
    """The region map (made when missing: a color vertex map, black) and the Interpolate nodes
    with no region map set to read it. Returns whether some Interpolate reads another map."""
    from groomlab import graph as gm
    if REGION_MAP not in groomlab.list_maps(groom):
        groomlab.add_map(groom, REGION_MAP, kind="vertices", color=(0.0, 0.0, 0.0, 1.0))
    model = gm.GraphModel.from_groom(groom, gm.OperatorCatalog.from_maya())
    changed, other = False, False
    for node in list(model.order):
        if model.node_type(node) != "Interpolate":
            continue
        current = model.param(node, "regionMap")
        if not current:
            model.set_param(node, "regionMap", REGION_MAP)
            changed = True
        elif current != REGION_MAP:
            other = True
    if changed:
        model.save(groom)
    return other


def update_regions(groom):
    """Every root tube's area painted in its own color on the region map (REGION_MAP; black outside
    them), which the Interpolate nodes with no region map read: the hair rooted in a tube's area
    follows that tube's guides only. A tube's area: the scalp's vertices inside its root outline
    (and the vertices round its root, so a tube smaller than the scalp's faces still has its
    region). Returns {"tubes": count, "vertices": painted, "other": an Interpolate reads another
    region map}."""
    tubes = root_tubes(groom)
    other = _region_map(groom)
    maps = json.loads(cmds.getAttr(groom + ".scalpMaps") or "{}")
    entry = maps.get(REGION_MAP)
    points, fns = _scalp_points(groom)
    if not isinstance(entry, dict) or len(entry.get("values", [])) != 4 * len(points):
        raise RuntimeError("the region map %s does not match the scalp's %d vertices" % (REGION_MAP, len(points)))
    values = [0.0, 0.0, 0.0, 1.0] * len(points)
    colors = region_colors(len(tubes))
    painted = 0
    from maya.api import OpenMaya as om
    for guide, color in zip(tubes, colors):
        try:
            outline = root_outline(groom, guide, on_scalp=False)
            frames = _frames(groom, guide)
        except (RuntimeError, ValueError):
            continue
        center, u, v = tm.frame_at(frames, 0.0)
        n = tm._unit(tm._cross(u, v))
        flat = [tm.to_plane(p, center, u, v) for p in outline]
        reach = max(math.hypot(x, y) for x, y in flat)
        hits = set()
        for i, p in enumerate(points):
            d = tm._sub(p, center)
            if tm._dot(d, d) > reach * reach * 1.5:
                continue
            if abs(tm._dot(d, n)) > reach:  # (the scalp's far side)
                continue
            x, y = tm._dot(d, u), tm._dot(d, v)
            if _inside(flat, x, y):
                hits.add(i)
        # The vertices of the face under the root: its guide's own color, however small the tube.
        root = om.MPoint(*frames["points"][0])
        for fn, first in fns:
            hit, face = fn.getClosestPoint(root, om.MSpace.kWorld)
            if hit.distanceTo(root) < max(reach, 1e-6):
                hits.update(first + k for k in fn.getPolygonVertices(face))
        for i in hits:
            values[4 * i:4 * i + 3] = list(color)
        painted += len(hits)
    entry["values"] = [round(float(x), 3) for x in values]
    entry["channels"] = 4
    cmds.setAttr(groom + ".scalpMaps", json.dumps(maps, separators=(",", ":")), type="string")
    return {"tubes": len(tubes), "vertices": painted, "other": other}


# ---------------------------------------------------------------------------
# Snapping: a new CV onto a neighbouring root


def snap_targets(groom, skip=None):
    """The root tubes' outlines (world CVs on the scalp) a CV can snap to: [(guide, [points])],
    leaving out the guides in `skip`."""
    skip = set(int(s) for s in (skip or []))
    out = []
    for guide in root_tubes(groom):
        if guide in skip:
            continue
        try:
            out.append((guide, root_outline(groom, guide)))
        except (RuntimeError, ValueError):
            pass
    return out


def snap_point(p, outlines, to_screen, pixels=SNAP_PIXELS):
    """`p` (world) snapped onto the nearest neighbouring root: one of its CVs within `pixels` on
    screen, else the nearest point of its edges within `pixels`; else `p`. `to_screen(point)`:
    its view coordinates. Returns (point, snapped: "cv", "edge" or None)."""
    sx, sy = to_screen(p)
    best = None
    for _guide, cvs in outlines:
        for q in cvs:
            qx, qy = to_screen(q)
            d = math.hypot(qx - sx, qy - sy)
            if d <= pixels and (best is None or d < best[0]):
                best = (d, tuple(q))
    if best:
        return best[1], "cv"
    for _guide, cvs in outlines:
        n = len(cvs)
        for i in range(n):
            a, b = cvs[i], cvs[(i + 1) % n]
            ab = tm._sub(b, a)
            span = tm._dot(ab, ab)
            f = min(max(tm._dot(tm._sub(p, a), ab) / span, 0.0), 1.0) if span > 1e-18 else 0.0
            q = tm._add(a, tm._mul(ab, f))
            qx, qy = to_screen(q)
            d = math.hypot(qx - sx, qy - sy)
            if d <= pixels and (best is None or d < best[0]):
                best = (d, q)
    if best:
        return best[1], "edge"
    return tuple(p), None


# ---------------------------------------------------------------------------
# The tube hierarchy


def level_group(base, level):
    """The group of `level` (1: the base) in base group `base`'s hierarchy."""
    return base if level <= 1 else "%s%s%d" % (base, LEVEL_SUFFIX, level)


def _top_of(groups, name):
    """The top group above group `name` (list_guide_groups' dicts by name), or None."""
    g = groups.get(name)
    seen = set()
    while g is not None and g["parent"] and g["name"] not in seen:
        seen.add(g["name"])
        g = groups.get(g["parent"])
    return g


def base_group(groom, guides=None, picked=None, groups=None):
    """The base group (level 1) the hierarchy works on: the top group of the first of `guides`,
    else of the group `picked`, else the first top group holding guides. `groups`:
    list_guide_groups(groom) when the caller has read it."""
    groups = groups if groups is not None else groomlab.list_guide_groups(groom)
    by_name = dict((g["name"], g) for g in groups)
    if guides:
        data = _hierarchy(groom)
        by_id = _guides_by_id(data)
        names = dict((int(g["id"]), g["name"]) for g in data.get("groups", []))
        for g in guides:
            gd = by_id.get(int(g))
            if gd is not None:
                top = _top_of(by_name, names.get(int(gd["group"])))
                if top:
                    return top["name"]
    if picked:
        top = _top_of(by_name, picked)
        if top:
            return top["name"]
    for g in groups:
        if not g["parent"] and g["count"]:
            return g["name"]
    return None


def tube_levels(groom, guides=None, picked=None):
    """The hierarchy's levels: {"base": the base group, "levels": [the group of each level, the
    base first]}: each level's group a child group of the one above, named <base>_L<n>."""
    groups = groomlab.list_guide_groups(groom)
    base = base_group(groom, guides, picked, groups)
    if not base:
        return {"base": None, "levels": []}
    by_name = dict((g["name"], g) for g in groups)
    levels = [base]
    for level in range(2, MAX_LEVELS + 1):
        g = by_name.get(level_group(base, level))
        if g is None or g["parent"] != levels[-1]:
            break
        levels.append(g["name"])
    return {"base": base, "levels": levels}


def _plain_round(tube):
    return (tube.get("shape") == "round" and all(abs(a - 1.0) < 1e-6 for a in tube.get("aspect", [1.0]))
            and all(abs(t) < 1e-9 for t in tube.get("twist", [0.0])))


def add_tube_level(groom, guides=None, picked=None):
    """Split tubes in two (the Tube hierarchy's Add): each of the leaf tubes (the deepest tubes of
    the hierarchy under `guides`, or under every guide of the base group when none are given) gets
    two child tubes, side by side across its longer side, sharing its root shape, in the next
    level's group (made when missing). A plain round tube is first laid across the scalp (its
    aspect a hair off 1), so its halves lie side by side along the scalp. Up to MAX_LEVELS. One
    undo step. Returns {"base", "levels", "split": how many tubes were split}."""
    data = _hierarchy(groom)
    info = tube_levels(groom, guides, picked)
    base, levels = info["base"], info["levels"]
    if not base:
        raise ValueError("the groom has no guides to split")
    groups = _groups_by_id(data)
    level_of = {}
    for depth, name in enumerate(levels):
        g = _group_named(data, name)
        if g is not None:
            level_of[int(g["id"])] = depth + 1
    by_id = _guides_by_id(data)
    children = {}
    for gd in data.get("guides", []):
        if gd.get("parent") and int(gd["group"]) in level_of:
            children.setdefault(int(gd["parent"]), []).append(int(gd["id"]))
    if guides:
        start = [int(g) for g in guides if int(g) in by_id and int(by_id[int(g)]["group"]) in level_of]
    else:
        start = [i for i, gd in by_id.items() if level_of.get(int(gd["group"])) == 1]
    if not start:
        raise ValueError("no tubes of %s to split: select tubes of the hierarchy, or none for all" % base)
    leaves, todo, seen = [], list(start), set()
    while todo:
        g = todo.pop()
        if g in seen:
            continue
        seen.add(g)
        kids = children.get(g, [])
        if kids:
            todo.extend(kids)
        else:
            leaves.append(g)
    by_level = {}
    for g in leaves:
        by_level.setdefault(level_of[int(by_id[g]["group"])], []).append(g)
    if all(level >= MAX_LEVELS for level in by_level):
        raise ValueError("the tubes are split %d levels deep already (the most)" % MAX_LEVELS)
    split = 0
    with groomlab.undo_chunk("tube hierarchy: add a level"):
        for level, parents in sorted(by_level.items()):
            if level >= MAX_LEVELS:
                continue
            group = groups[int(by_id[parents[0]]["group"])]["name"]
            round_ones = [g for g in parents if _plain_round(by_id[g].get("tube", {}))]
            if round_ones:
                groomlab.set_tube(groom, guides=round_ones, aspect=0.999)
            groomlab.generate_children(groom, parent_group=group, name=level_group(base, level + 1), count=2,
                                       guides=parents, split=True)
            split += len(parents)
    info = tube_levels(groom, picked=base)
    info["split"] = split
    return info


def remove_tube_level(groom, base):
    """Remove the deepest level of base group `base`'s hierarchy (its group and guides). One undo
    step. Returns the levels left."""
    info = tube_levels(groom, picked=base)
    if len(info["levels"]) <= 1:
        raise ValueError("%s has no level under it to remove" % base)
    groomlab.remove_guide_group(groom, info["levels"][-1])
    return tube_levels(groom, picked=base)


def show_tube_level(groom, base, level):
    """Show level `level` (1: the base) of `base`'s hierarchy and hide its other levels (the guide
    groups' eyes; groups outside the hierarchy are left as they are). Returns the level's group."""
    levels = tube_levels(groom, picked=base)["levels"]
    if not levels:
        raise ValueError("no tube hierarchy on %s" % base)
    level = min(max(int(level), 1), len(levels))
    for k, name in enumerate(levels):
        groomlab.show_guide_group(groom, name, visible=(k + 1 == level))
    return levels[level - 1]
