# SPDX-License-Identifier: Apache-2.0
"""Tonic-style tubes in the Tube tool: tubes drawn root first (issue 61) and the tube hierarchy
(issue 62).

Root-first tubes: the artist clicks the root's outline on the scalp one CV at a time; closing it on
its first CV makes a tube guide rooted at the outline's middle, its tube's root section that
outline exactly (a library shape of its own, "rootTube_<n>"), going out along a drawn curve or
straight off the scalp. The tube's root footprint is that outline fixed on the scalp (the engine
lays it along the scalp itself, not the guide: combing never moves it), and Interpolate's
fillTubes gives every strand rooted in it to that tube, so the hair rooted in a root's area follows
only that root's guides; no region map. Editing a root's CVs later moves the guide to the new
outline's middle and gives its shape the new outline.

A root's CVs are kept on the scalp itself (OUTLINES_ATTR on the groom: each CV a scalp face and
its barycentric weights), apart from the guide: combing, brushing or retwisting the guide never
moves them. Only drawing the root and editing it (make_root_tube, edit_root_tube) set them; the
regions, Edit roots and snapping read them.

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
from functools import reduce

from maya import cmds

import groomlab
from groomlab import tube_mesh as tm

ROOT_GROUP = "rootTubes"   # the root group new root tubes go into (when no group is picked)
SHAPE_PREFIX = "rootTube_"  # each root tube's library shape: its root outline
MAX_LEVELS = 9              # the tube hierarchy's deepest level (1: the base)
LEVEL_SUFFIX = "_L"         # level n's group: <base>_L<n>
SNAP_PIXELS = 12            # how near (in view pixels) a CV snaps to a neighbouring root's CV or edge
OUTLINES_ATTR = "groomLabRootOutlines"  # the groom's string attribute: each root tube's CVs on the scalp (JSON)
ROOT_OVERLAP = 0.015       # a root tube's footprint this share larger than its outline: sealed neighbours overlap (tube.h kRootTubeOverlap)
_RECENTER_PASSES = 4        # the guide moved onto its root shape's box middle at most so often
_PATH_SMOOTHING = 0.25     # a drawn path averaged along it over this share of its root's size (a Gaussian's
                            # sigma): the hand's shakes go, a curl as wide as its tube stays
_SPAN_ROOTS = 1.0           # a drawn guide's spans about this many root sizes long (Point density 1) ...
_SPAN_TURN = math.radians(15.0)  # ... and turning at most this much each, so a curl keeps its points
_SPANS = (3, 200)           # ... between these many
CLEARANCE = 0.35     # a drawn guide popped this many root sizes out where it dips into the scalp ...
_CLEAR_RAMP = 2.0    # ... reached this many root sizes along it from the root ...
_CLEAR_SPREAD = 1.5  # ... the lift spread smoothly over this many root sizes along it
MAX_LEAN = math.radians(50.0)  # a root tube's guide leaves the scalp leaning at most this far off its normal
_PATH_LEAVE = 0.2           # the least share of the path over which it bends from the scalp's normal into the stroke


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
    return next((s for s in groomlab.tube_shapes(groom, owned=True) if s["name"] == name), None)


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


def _length(points):
    return sum(tm._dist(a, b) for a, b in zip(points, points[1:]))


def _end_line(pts, m):
    """The straight line the last `m` of `pts` lie along (least squares by index): its point at
    the last index and its step per index."""
    m = max(2, min(m, len(pts)))
    tail = pts[-m:]
    mean_i = (m - 1) / 2.0
    mean_p = tm._mul(reduce(tm._add, tail), 1.0 / m)
    den = sum((i - mean_i) ** 2 for i in range(m))
    step = tm._mul(reduce(tm._add, [tm._mul(tm._sub(q, mean_p), i - mean_i) for i, q in enumerate(tail)]), 1.0 / den)
    return tm._add(mean_p, tm._mul(step, m - 1 - mean_i)), step


def _gaussian(pts, sigma):
    """`pts` (evenly spaced) averaged with their neighbours, a Gaussian of `sigma` samples; past
    each end the curve carried on along the straight line its last stretch lies along (so the ends
    are smoothed too, neither pinned to a shaky last point nor pulled in)."""
    n = len(pts)
    reach = int(3.0 * sigma) + 1
    weights = [math.exp(-0.5 * (k / sigma) ** 2) for k in range(-reach, reach + 1)]
    m = int(2.0 * sigma) + 2
    end, end_step = _end_line(pts, m)
    start, start_step = _end_line(pts[::-1], m)

    def at(i):
        if i < 0:
            return tm._add(start, tm._mul(start_step, -i))
        if i >= n:
            return tm._add(end, tm._mul(end_step, i - (n - 1)))
        return pts[i]
    out = []
    for i in range(n):
        acc, total = (0.0, 0.0, 0.0), 0.0
        for k, w in zip(range(-reach, reach + 1), weights):
            acc = tm._add(acc, tm._mul(at(i + k), w))
            total += w
        out.append(tm._mul(acc, 1.0 / total))
    # The root held, the shift it takes faded out along the first stretch (pinning the root point
    # alone would leave a hook there).
    shift = tm._sub(pts[0], out[0])
    for i in range(min(reach, n)):
        w = 0.5 * (1.0 + math.cos(math.pi * i / float(reach)))
        out[i] = tm._add(out[i], tm._mul(shift, w))
    return out


def _lean(direction, normal):
    """How far (radians) `direction` leans off the scalp's `normal`."""
    return math.acos(max(-1.0, min(1.0, tm._dot(tm._unit(direction), tm._unit(normal)))))


def _leaned(direction, normal, most=None):
    """`direction` (a unit vector) leaning at most `most` (MAX_LEAN) off `normal`: turned toward
    it where it leans more, as it is otherwise."""
    most = MAX_LEAN if most is None else most
    n = tm._unit(normal)
    d = tm._unit(direction)
    if _lean(d, n) <= most:
        return d
    side = tm._sub(d, tm._mul(n, tm._dot(d, n)))
    side = tm._unit(side) if tm._length(side) > 1e-9 else tm._unit(tm._cross(n, (1.0, 0.0, 0.0) if abs(n[0]) < 0.9 else (0.0, 1.0, 0.0)))
    return tm._add(tm._mul(n, math.cos(most)), tm._mul(side, math.sin(most)))


def _seat(points, normal):
    """A guide's points with its first span leaning at most MAX_LEAN off the scalp's normal at its
    root (the span's length kept), so its tube's section at the root stays a fair outline."""
    out = [tuple(p) for p in points]
    first = tm._sub(out[1], out[0])
    out[1] = tm._add(out[0], tm._mul(_leaned(first, normal), tm._length(first)))
    return out


def _smooth_path(path, root, normal, size):
    """A drawn stroke as the guide follows it: its first point taken to `root`, the hand's jitter
    averaged out along it (a Gaussian over _PATH_SMOOTHING of the root's `size`, at points evenly
    along it: the shakes, and the back-and-forth of a hand slowing, smoothed away while its bends,
    curls and spirals as wide as the tube stay; its root held, its tip smoothed too), leaving the
    root the stroke's way (at most MAX_LEAN off the scalp's `normal`: below)."""
    drawn = [tuple(p) for p in path]
    drawn[0] = tuple(root)
    if _length(drawn) <= 1e-9:
        raise ValueError("the tube needs a length")
    sigma = max(_PATH_SMOOTHING * float(size), 1e-6)
    # Thinned first: a point nearer the last one kept than half the smoothing dropped, so where
    # the hand went slowly (its start, mostly) its shakes don't count as length.
    pts = [drawn[0]]
    for q in drawn[1:]:
        if tm._dist(q, pts[-1]) >= 0.5 * sigma:
            pts.append(q)
    if tm._dist(drawn[-1], pts[-1]) > 1e-12:
        pts[-1 if len(pts) > 1 else len(pts):] = [drawn[-1]]
    if len(pts) < 2:
        pts = [drawn[0], drawn[-1]]
    total = _length(pts)
    sigma = min(sigma, 0.1 * total)  # (a stroke shorter than its root's size: still its own shape)
    samples = int(min(max(4.0 * total / sigma, 50), 4000))
    pts = _resample(pts, samples)
    pts = _gaussian(pts, sigma / (total / float(samples - 1)))
    # From the root the guide goes the stroke's way, as drawn. Only a stroke leaving the root
    # leaning more than MAX_LEAN off the scalp's normal (so flat along the scalp that the tube's
    # section there would thin to nothing) starts at MAX_LEAN instead and bends into the stroke:
    # a quadratic Bezier from the root that way, its middle point where that line and the
    # stroke's line meet (one bend, never past the stroke's way), meeting the stroke no sooner
    # than _PATH_LEAVE along it (where those lines meet ahead on both; else at its tip).
    total = _length(pts)
    probe = pts[min(len(pts) - 1, max(2, int(0.05 * len(pts))))]
    first = tm._sub(probe, pts[0])
    if _lean(first, normal) <= MAX_LEAN + 1e-6:
        return pts
    start_way = _leaned(first, normal)

    def corner(i):
        q = pts[i]
        way = tm._unit(tm._sub(pts[min(i + 2, len(pts) - 1)], pts[max(i - 2, 0)]))
        w0 = tm._sub(pts[0], q)
        b = tm._dot(start_way, way)
        den = 1.0 - b * b
        if den < 1e-6:
            return tm.lerp(pts[0], q, 0.5)
        d, e = tm._dot(start_way, w0), tm._dot(way, w0)
        t = (b * e - d) / den        # along the start's way from the root
        u = (e - b * d) / den        # along the stroke's way from q (negative: behind q)
        if t <= 0.0 or u >= 0.0:
            return None
        return tm.lerp(tm._add(pts[0], tm._mul(start_way, t)), tm._add(q, tm._mul(way, u)), 0.5)
    least = max(_PATH_LEAVE * total, 3.0 * float(size))
    along, k, mid = 0.0, None, None
    for i in range(1, len(pts)):
        along += tm._dist(pts[i - 1], pts[i])
        if along < least and i < len(pts) - 1:
            continue
        mid = corner(i)
        if mid is not None:
            k = i
            break
    if k is None:  # (no single bend reaches it anywhere: a gentle curve to its tip)
        k = len(pts) - 1
        mid = tm._add(pts[0], tm._mul(start_way, 0.5 * tm._dist(pts[0], pts[k])))
    q = pts[k]
    curve = []
    for i in range(96):
        t = i / 96.0
        curve.append(tm._add(tm._add(tm._mul(pts[0], (1 - t) ** 2), tm._mul(mid, 2 * t * (1 - t))), tm._mul(q, t * t)))
    return curve + pts[k:]


def _turning(pts):
    """How far (radians) a polyline turns, all told."""
    out = 0.0
    for a, b, c in zip(pts, pts[1:], pts[2:]):
        u, v = tm._sub(b, a), tm._sub(c, b)
        lu, lv = tm._length(u), tm._length(v)
        if lu > 1e-12 and lv > 1e-12:
            out += math.acos(max(-1.0, min(1.0, tm._dot(u, v) / (lu * lv))))
    return out


def _even_chords(points, count):
    """`count` points along a polyline (dense: a smoothed stroke), its ends kept, each the same
    straight distance from the last (the guide's spans all one length, however it bends)."""
    pts = [tuple(p) for p in points]
    if count < 2 or len(pts) < 2:
        return pts[:1] * max(count, 1)

    def walk(step):
        out, j = [pts[0]], 1
        for _ in range(count - 1):
            here = out[-1]
            while j < len(pts) and tm._dist(pts[j], here) < step:
                j += 1
            if j >= len(pts):
                return out, False
            # On the segment (pts[j - 1], pts[j]): the point `step` from `here`.
            a, b = pts[j - 1], pts[j]
            lo, hi = 0.0, 1.0
            for _ in range(40):
                m = 0.5 * (lo + hi)
                if tm._dist(tm.lerp(a, b, m), here) < step:
                    lo = m
                else:
                    hi = m
            out.append(tm.lerp(a, b, hi))
        return out, True
    lo, hi = 0.0, _length(pts) / float(count - 1)
    best = None
    for _ in range(50):
        step = 0.5 * (lo + hi)
        out, whole = walk(step)
        if whole:
            best = out
            lo = step  # (all placed: longer spans may still fit)
        else:
            hi = step
    if best is None:
        return _resample(pts, count)
    best[-1] = pts[-1]
    return best


def path_spans(points, size, density=1.0):
    """How many spans a drawn guide gets: about one every _SPAN_ROOTS root sizes (`size`) along it
    and enough that none turns more than _SPAN_TURN, both times `density`, within _SPANS: a short
    stroke few, a long or a curling one more."""
    density = max(float(density), 1e-3)
    total = _length(points)
    along = total / max(_SPAN_ROOTS * float(size) / density, 1e-9)
    # (its turns at the shape's scale: on points half a root size apart, not the fine samples)
    coarse = _resample(points, int(min(max(total / (0.5 * max(float(size), 1e-9)), 2), 2000)) + 1)
    turns = _turning(coarse) / (_SPAN_TURN / density)
    return int(min(max(math.ceil(max(along, turns)), _SPANS[0]), _SPANS[1]))


def _clear_of_scalp(points, surface, size):
    """A guide popped out of the scalp, its drawn shape kept: only where it dips under a small
    clearance (CLEARANCE of the root's `size`, growing from nothing at the root over _CLEAR_RAMP
    sizes along it) is it lifted, along the scalp's normal, and the lifts are spread wide and
    smooth along the guide (a Gaussian over _CLEAR_SPREAD sizes), a few passes adding up until no
    point is left under, so the guide's bends, curls and course stay as drawn and it rises gently
    over the scalp where it dipped in. The root stays."""
    pts = [tuple(p) for p in points]
    if surface is None or len(pts) < 3 or size <= 0.0:
        return pts
    along = [0.0]
    for a, b in zip(pts, pts[1:]):
        along.append(along[-1] + tm._dist(a, b))
    step = along[-1] / float(len(pts) - 1) if along[-1] > 0.0 else 1.0
    clearance = CLEARANCE * size
    want = []
    for d in along:
        x = min(d / (_CLEAR_RAMP * size), 1.0)
        want.append(clearance * x * x * (3.0 - 2.0 * x))
    sigma = max(_CLEAR_SPREAD * size / step, 1.0)  # (in points)
    reach = int(math.ceil(3.0 * sigma))
    kernel = [math.exp(-0.5 * (k / sigma) ** 2) for k in range(-reach, reach + 1)]
    peak = max(kernel)
    for _ in range(6):
        lifts = []
        for i, p in enumerate(pts):
            q, n = surface(p)
            short = want[i] - tm._dot(tm._sub(p, q), n)  # (how far under its clearance)
            lifts.append(tm._mul(n, short) if short > 0.02 * clearance and i > 0 else (0.0, 0.0, 0.0))
        if not any(tm._length(v) > 0.0 for v in lifts):
            break
        spread = []
        for i in range(len(pts)):
            total = (0.0, 0.0, 0.0)
            for k, w in enumerate(kernel):
                j = i + k - reach
                if 0 <= j < len(pts):
                    total = tm._add(total, tm._mul(lifts[j], w / peak))
            spread.append(total)
        # (each pass lifts a point at most what it was short: the spread adds up, never overshoots far)
        top = max(tm._length(v) for v in spread)
        need = max(tm._length(v) for v in lifts)
        scale = min(1.0, need / top) if top > 0.0 else 0.0
        spread[0] = (0.0, 0.0, 0.0)
        pts = [tm._add(p, tm._mul(v, scale)) for p, v in zip(pts, spread)]
    return pts


def _guide_path(root, normal, path, length, segments, size=None, density=1.0, surface=None):
    """The guide's points: from `root` straight off the scalp (`length`, `segments` spans), or along
    the drawn `path` smoothed (_smooth_path, over the root's `size`) and (with the scalp's
    `surface`) kept off the scalp softly (_clear_of_scalp), as many evenly spaced along it as its
    length and its turns call for (path_spans, times `density`); its first span leaning at most
    MAX_LEAN off the scalp's `normal`."""
    if path and len(path) >= 2:
        size = float(size) if size else 0.05 * _length([tuple(p) for p in path])
        smooth = _smooth_path(path, root, normal, size)
        pts = _even_chords(smooth, path_spans(smooth, size, density) + 1)
        if surface is not None:
            pts = _even_chords(_clear_of_scalp(pts, surface, size), len(pts))
    else:
        segments = max(int(segments), 2)
        if float(length) <= 1e-9:
            raise ValueError("the tube needs a length")
        pts = [tm._add(root, tm._mul(normal, float(length) * i / float(segments))) for i in range(segments + 1)]
    return _seat(pts, normal)


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
    taken = set(s["name"] for s in groomlab.tube_shapes(groom, owned=True))
    n = 1
    while "%s%d" % (SHAPE_PREFIX, n) in taken:
        n += 1
    return "%s%d" % (SHAPE_PREFIX, n)


def _turn(vec, a, b):
    """`vec` turned by the least rotation taking unit `a` onto unit `b` (frames.h's Rotation)."""
    a, b = tm._unit(a), tm._unit(b)
    c = tm._dot(a, b)
    if c < -1.0 + 1e-6:
        return tm._mul(vec, -1.0)
    k = tm._cross(a, b)
    return tm._add(tm._add(tm._mul(vec, c), tm._cross(k, vec)), tm._mul(k, tm._dot(k, vec) / (1.0 + c)))


def _smooth_normal(groom, point):
    """The scalp's normal at `point` as GroomLab reads it (ScalpMesh::normal): on the triangle of
    Maya's split the point lies in, its corners' normals blended, each corner's the sum of the
    normals of the triangles round it weighted by their areas (not Maya's own vertex normals,
    which weigh by angle: on a coarse scalp the two tilt a root's footprint apart, and strands at
    a shared edge go to the neighbour)."""
    from maya.api import OpenMaya as om
    from groomlab import utils
    q = om.MPoint(*point)
    best = None
    for shape in _scalp_meshes(groom):
        fn = utils._om_mesh(shape)
        hit, face = fn.getClosestPoint(q, om.MSpace.kWorld)
        d = hit.distanceTo(q)
        if best is None or d < best[0]:
            best = (d, shape, fn, face, (hit.x, hit.y, hit.z))
    if best is None:
        raise ValueError("the groom has no scalp")
    _d, shape, fn, face, at = best
    dag = om.MSelectionList().add(shape).getDagPath(0)
    polys = om.MItMeshPolygon(dag)

    def triangles(f):
        polys.setIndex(f)
        pts, ids = polys.getTriangles(om.MSpace.kWorld)
        return [(tuple(ids[3 * t:3 * t + 3]), [tuple(pts[3 * t + j])[:3] for j in range(3)]) for t in range(len(ids) // 3)]

    pick = None
    for ids, corners in triangles(face):
        w = _barycentric(at, *corners)
        if pick is None or min(w) > min(pick[1]):
            pick = (ids, w)
    ids, w = pick
    verts = om.MItMeshVertex(dag)
    n = (0.0, 0.0, 0.0)
    for vid, weight in zip(ids, w):
        verts.setIndex(vid)
        total = (0.0, 0.0, 0.0)
        for f in verts.getConnectedFaces():
            for tids, (p0, p1, p2) in triangles(f):
                if vid in tids:
                    total = tm._add(total, tm._cross(tm._sub(p1, p0), tm._sub(p2, p0)))
        n = tm._add(n, tm._mul(tm._unit(total), weight))
    return tm._unit(n)


def _footprint(groom, guide):
    """The guide's root footprint as GroomLab lays it (TubeSpine::setRootSurface): its root
    section turned flat onto the scalp, by the least rotation taking the guide's root way onto the
    scalp's normal there. (center, u, v, normal): u and v the section's axes so turned. Scatter's
    roots on a shaped tube and Interpolate's fillTubes go by it, so a root outline is fitted in it:
    the hair rooted inside the outline as drawn is the tube's, however far the guide leans."""
    frames = _frames(groom, guide)
    fixed = frames.get("footprint")
    if fixed:  # a root tube: fixed on the scalp (TubeSpine::setRootOnScalp), whatever the guide does
        return (tuple(frames["points"][0]), tuple(fixed["u"]), tuple(fixed["v"]), tuple(fixed["n"]))
    center, u, v = tm.frame_at(frames, 0.0)
    way = tm._unit(tm._cross(u, v))
    n = _smooth_normal(groom, center)
    if tm._dot(way, n) < -1.0 + 1e-4:  # (against the scalp: GroomLab leaves it as it is)
        return center, u, v, way
    return center, _turn(u, way, n), _turn(v, way, n), n


def _root_fit(groom, guide, outline):
    """The guide's root footprint and the outline in it (taken into its plane along its normal),
    ROOT_OVERLAP larger about the guide: (center, u, v, flat points (u, v)). Sealed neighbours'
    footprints each lie flat on a plane of their own, so where they share an edge a hair-thin
    crack can open between them, and a strand rooted in it would be in no tube; a little larger,
    they overlap there instead (the tube a strand lies most centrally in takes it)."""
    center, u, v, _n = _footprint(groom, guide)
    k = 1.0 + ROOT_OVERLAP
    return center, u, v, [tuple(x * k for x in tm.to_plane(p, center, u, v)) for p in outline]


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
    # (A root tube's own shape, "owned": the library refuses the name otherwise, as the core's root
    # tools make and fit theirs.)
    op = "add" if _library_shape(groom, shape) is None else "set"
    cmds.groomLabGuides(groom, editTubeShape=json.dumps({"op": op, "name": shape, "points": [list(p) for p in pts],
                                                          "smooth": False, "owned": True}))
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


def _plane(points):
    """A closed outline's plane: (its area centroid, u, v, normal), normal by Newell's method."""
    center = _centroid(points)
    n = len(points)
    normal = (0.0, 0.0, 0.0)
    for i in range(n):
        a, b = points[i], points[(i + 1) % n]
        normal = tm._add(normal, ((a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]),
                                  (a[0] - b[0]) * (a[1] + b[1])))
    nz = tm._unit(normal) if tm._length(normal) > 1e-12 else (0.0, 1.0, 0.0)
    u = tm._unit(tm._cross(nz, (1.0, 0.0, 0.0) if abs(nz[0]) < 0.9 else (0.0, 1.0, 0.0)))
    return center, u, tm._cross(nz, u), nz


def _scalp_meshes(groom):
    """The groom's scalps, its own first, then the others by index (as the region map joins them)."""
    return [m for m in [groomlab.scalp_mesh(groom)] + [s["mesh"] for s in groomlab.scalps(groom)] if m]


def _barycentric(q, a, b, c):
    v0, v1, v2 = tm._sub(b, a), tm._sub(c, a), tm._sub(q, a)
    d00, d01, d11 = tm._dot(v0, v0), tm._dot(v0, v1), tm._dot(v1, v1)
    d20, d21 = tm._dot(v2, v0), tm._dot(v2, v1)
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-30:
        return 1.0, 0.0, 0.0
    wb = (d11 * d20 - d01 * d21) / den
    wc = (d00 * d21 - d01 * d20) / den
    return 1.0 - wb - wc, wb, wc


def _bind(groom, points):
    """Points on the scalp as the scalp holds them: [mesh, face, fan triangle, w0, w1, w2] each (the
    nearest point of the nearest scalp, in the face's fan triangle (vertex 0, k, k + 1) it lies in)."""
    from maya.api import OpenMaya as om
    from groomlab import utils
    fns = [utils._om_mesh(m) for m in _scalp_meshes(groom)]
    out = []
    for p in points:
        q = om.MPoint(*p)
        best = None
        for mi, fn in enumerate(fns):
            hit, face = fn.getClosestPoint(q, om.MSpace.kWorld)
            d = hit.distanceTo(q)
            if best is None or d < best[0]:
                best = (d, mi, face, (hit.x, hit.y, hit.z))
        if best is None:
            raise ValueError("the groom has no scalp to keep a root outline on")
        _d, mi, face, at = best
        fn = fns[mi]
        corners = [tuple(fn.getPoint(v, om.MSpace.kWorld))[:3] for v in fn.getPolygonVertices(face)]
        pick = None
        for k in range(1, len(corners) - 1):
            w = _barycentric(at, corners[0], corners[k], corners[k + 1])
            if pick is None or min(w) > min(pick[1]):
                pick = (k, w)
        out.append([mi, int(face), pick[0]] + [round(x, 7) for x in pick[1]])
    return out


def _unbind(groom, bound):
    """World points from _bind's records (the scalp as it is now)."""
    from maya.api import OpenMaya as om
    from groomlab import utils
    fns = [utils._om_mesh(m) for m in _scalp_meshes(groom)]
    out = []
    for mi, face, k, w0, w1, w2 in bound:
        if mi >= len(fns) or face >= fns[mi].numPolygons:
            raise ValueError("the scalp changed under a root outline")
        fn = fns[mi]
        verts = fn.getPolygonVertices(face)
        if k + 1 >= len(verts):
            raise ValueError("the scalp changed under a root outline")
        a, b, c = (tuple(fn.getPoint(verts[i], om.MSpace.kWorld))[:3] for i in (0, k, k + 1))
        out.append(tm._add(tm._add(tm._mul(a, w0), tm._mul(b, w1)), tm._mul(c, w2)))
    return out


def _stored_outlines(groom):
    if not cmds.attributeQuery(OUTLINES_ATTR, node=groom, exists=True):
        return {}
    try:
        data = json.loads(cmds.getAttr(groom + "." + OUTLINES_ATTR) or "{}")
    except ValueError:
        return {}
    return data if isinstance(data, dict) else {}


def _store_outline(groom, guide, outline):
    """Keep `guide`'s root CVs (world points on the scalp) on the scalp (OUTLINES_ATTR), dropping
    those of guides no longer root tubes. Undoable."""
    data = _stored_outlines(groom)
    data[str(int(guide))] = _bind(groom, outline)
    live = set(str(g) for g in root_tubes(groom))
    data = dict((k, v) for k, v in data.items() if k in live)
    if not cmds.attributeQuery(OUTLINES_ATTR, node=groom, exists=True):
        cmds.addAttr(groom, longName=OUTLINES_ATTR, dataType="string")
    cmds.setAttr(groom + "." + OUTLINES_ATTR, json.dumps(data, separators=(",", ":")), type="string")


# ---------------------------------------------------------------------------
# Root-first tubes


def make_root_tube(groom, outline, path=None, length=1.0, segments=12, taper=0.5, group=None, regions=True,
                   density=1.0):
    """A tube guide grown from its root's outline: `outline` the root's CVs on the scalp (world
    points, 3 or more, in order round it). The guide roots at the outline's middle and goes along
    the drawn `path` (world points from the root out; its first point is taken to the root:
    smoothed over the root's size, its points as many as its length and turns call for, times
    `density`: _guide_path), or with none straight off the scalp `length` long, in `segments`
    segments. Its tube: a library
    shape of its own (rootTube_<n>, removed with the last guide that takes it) whose outline at
    the root is the drawn one (straight edges between the CVs), its width going to `taper` times
    the root's at the tip. The tube is swept from its root along the guide with no twist (its
    sections do not roll on the scalp: roll_on_scalp off). Into `group` (default ROOT_GROUP, made when missing).
    (`regions`: no effect, kept for older callers.) One undo step.
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
        radius = max(tm._dist(root, p) for p in outline)
        points = _guide_path(root, normal, path, length, segments, radius, density, surface)
        guide = _new_guide(groom, points, group or ROOT_GROUP)
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
            points = _guide_path(moved_root, normal, path, length, segments, radius, density, surface)
            _reshape(groom, guide, points)
            twist, w, h, off = _fit(groom, guide, shape, outline)
        groomlab.set_tube(groom, guides=[guide], shape=shape, width=[w, w * float(taper)],
                          aspect=min(max(h / w, 0.01), 100.0) if w > 1e-12 else 1.0, twist=twist)
        _settle(groom, guide, shape, outline)
        _store_outline(groom, guide, outline)
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


def root_outline(groom, guide):
    """A root tube's CVs as world points on the scalp: as drawn or last edited, kept on the scalp
    (OUTLINES_ATTR), whatever has been done to the guide since. A root tube made before they were
    kept: its tube's root section (section_outline)."""
    bound = _stored_outlines(groom).get(str(int(guide)))
    if bound:
        try:
            return _unbind(groom, bound)
        except ValueError:
            pass
    return section_outline(groom, guide)


def section_outline(groom, guide, on_scalp=True):
    """A root tube's tube's root footprint as world points: its shape's control points placed in
    its root section laid flat on the scalp (_footprint), taken onto the scalp (`on_scalp`: along
    the footprint's normal, the way they were read off it, so they come back where they were
    drawn)."""
    tube = groomlab.get_tube(groom, guide)
    shape = _library_shape(groom, tube["shape"])
    if shape is None or shape["builtin"]:
        raise ValueError("guide %s is not a root tube (its shape is %s)" % (guide, tube["shape"]))
    center, u, v, n = _footprint(groom, guide)
    w = tube["width"][0]
    h = w * tube["aspect"][0]
    flat = tm.place_outline(shape["points"], tube["twist"][0], w, h, (0.0, 0.0))
    world = [tm.from_plane(p, center, u, v) for p in flat]
    return _project_along(groom, world, n) if on_scalp else world


def edit_root_tube(groom, guide, outline, regions=True):
    """A root tube given a new root outline (`outline`: its CVs, world points, as root_outline
    gives them, moved; kept on the scalp as its outline): the guide moved whole so it roots at the new outline's middle, its shape
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
        # Seated on its root again: its first span leaning at most MAX_LEAN (a brush may have
        # laid it over), so its section there stays a fair outline.
        moved = _seat(moved, normal)
        _reshape(groom, guide, moved)
        twist, w, h, off = _fit(groom, guide, shape, outline)
        for _ in range(_RECENTER_PASSES):  # (on the box's middle, as make_root_tube puts it)
            if math.hypot(*off) <= 1e-5 * max(w, 1e-9):
                break
            center, u, v, _flat = _root_fit(groom, guide, outline)
            root2, normal2 = surface(tm.from_plane(off, center, u, v))
            move = tm._sub(root2, moved[0])
            moved = [root2] + [tm._add(p, move) for p in moved[1:]]
            moved = _seat(moved, normal2)
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
        _store_outline(groom, guide, outline)
    return guide


def root_center(groom, guide):
    """A root tube's root middle on the scalp (its outline's area centroid, taken onto it) and the
    scalp's normal there."""
    return _scalp_surface(groom)(_centroid(root_outline(groom, guide)))


def carried(groom, outline, start, to, held=()):
    """`outline` (a root's CVs on the scalp) carried over the scalp from its middle at `start` to
    `to` (world points: the middle and where it goes): each CV turned with the scalp's normal from
    there to here, set down at `to` and taken onto the scalp, so the shape keeps its size on a
    curved head. CVs whose index is in `held` stay where they are (sealed onto a neighbour)."""
    surface = _scalp_surface(groom)
    c0, n0 = surface(start)
    c1, n1 = surface(to)
    held = set(held)
    out = []
    for i, p in enumerate(outline):
        if i in held:
            out.append(tuple(p))
        else:
            out.append(tuple(surface(tm._add(c1, _turn(tm._sub(p, c0), n0, n1)))[0]))
    return out


def sealed(groom, guide, outline=None, others=None, tolerance=None):
    """The indices of `guide`'s root CVs (`outline`: as root_outline gives them) lying on another
    root tube's outline (one of its CVs, or on one of its edges): sealed onto a neighbour."""
    outline = outline if outline is not None else root_outline(groom, guide)
    others = others if others is not None else snap_targets(groom, skip=[guide])
    scale = max(tm._dist(a, b) for a, b in zip(outline, outline[1:] + outline[:1]))
    tol = tolerance if tolerance is not None else 1e-3 * max(scale, 1e-6)
    held = []
    for i, p in enumerate(outline):
        for _g, cvs in others:
            n = len(cvs)
            hit = False
            for k in range(n):
                a, b = cvs[k], cvs[(k + 1) % n]
                ab = tm._sub(b, a)
                span = tm._dot(ab, ab)
                f = min(max(tm._dot(tm._sub(p, a), ab) / span, 0.0), 1.0) if span > 1e-18 else 0.0
                if tm._dist(p, tm._add(a, tm._mul(ab, f))) <= tol:
                    hit = True
                    break
            if hit:
                held.append(i)
                break
    return held


def _children(data, guide):
    """`guide`'s child guides (the stored hierarchy's dicts), in ID order."""
    return sorted((g for g in data.get("guides", []) if g.get("parent") and int(g["parent"]) == int(guide)),
                  key=lambda g: int(g["id"]))


def _tube_copy(tube, twist_by=0.0):
    """set_tube's arguments giving a guide `tube` (get_tube's dict): its lists (the twist turned
    by `twist_by`), its mid and tip profiles and holds."""
    kwargs = {"width": list(tube["width"]), "aspect": list(tube["aspect"]),
              "twist": [t + twist_by for t in tube["twist"]]}
    for key, arg in (("midShape", "mid_shape"), ("tipShape", "tip_shape"), ("rootHold", "root_hold"),
                     ("midHold", "mid_hold"), ("tipHold", "tip_hold"), ("midAt", "mid_at")):
        if key in tube:
            kwargs[arg] = tube[key]
    return kwargs


def duplicate_root_tube(groom, guide, to, regions=True, start=None):
    """A copy of root tube `guide` with its root set down at `to` (a world point, taken onto the
    scalp; with `start`, the point carried to `to` instead of its root's middle: several roots
    copied as one group, each carried alike): its outline and its guide carried there over the
    scalp as one (carried), its tube a
    copy (a root shape of its own, the same widths, flatness, twist and profiles), and the tube
    hierarchy under it copied level by level (each level's split made again, each child given its
    counterpart's shape and tube). Into the same groups. With `regions`, the regions updated. One
    undo step. Returns the new guide's ID."""
    data = _hierarchy(groom)
    by_id = _guides_by_id(data)
    groups = _groups_by_id(data)
    if int(guide) not in by_id or not is_root_tube(by_id[int(guide)]):
        raise ValueError("guide %s is not a root tube" % guide)
    surface = _scalp_surface(groom)
    outline = root_outline(groom, guide)
    start, n0 = surface(start if start is not None else _centroid(outline))
    c1, n1 = surface(to)

    def move(p):
        return tm._add(c1, _turn(tm._sub(p, start), n0, n1))
    new_outline = carried(groom, outline, start, to)
    points = [move(p) for p in _frames(groom, guide)["points"]]
    points[0] = surface(points[0])[0]
    tube = groomlab.get_tube(groom, guide)
    group = groups[int(by_id[int(guide)]["group"])]["name"]
    with groomlab.undo_chunk("duplicate root tube"):
        new = _new_guide(groom, points, group)
        groomlab.set_tube(groom, guides=[new], shape="ribbon", width=tube["width"][0], aspect=1.0, twist=0.0,
                          roll_on_scalp=False)
        shape = _new_shape_name(groom)
        twist, w, h, _off = _fit(groom, new, shape, new_outline)
        w0 = tube["width"][0] or 1e-9
        a0 = tube["aspect"][0] or 1.0
        turn = twist - tube["twist"][0]
        kwargs = _tube_copy(tube, turn)
        a_new = min(max(h / w, 0.01), 100.0) if w > 1e-12 else 1.0
        kwargs["width"] = [x * w / w0 for x in tube["width"]]
        kwargs["aspect"] = [min(max(a * a_new / a0, 0.01), 100.0) for a in tube["aspect"]]
        groomlab.set_tube(groom, guides=[new], shape=shape, **kwargs)
        _settle(groom, new, shape, new_outline)
        _store_outline(groom, new, new_outline)
        # The hierarchy under it: each parent's children made again (its split), each the copy of
        # its counterpart, parents first.
        made = {int(guide): int(new)}
        level = [int(guide)]
        while level:
            below = []
            for old_parent in level:
                kids = _children(data, old_parent)
                if not kids:
                    continue
                kid_group = groups[int(kids[0]["group"])]["name"]
                parent_group = groups[int(by_id[old_parent]["group"])]["name"]
                before = set(int(i) for i in groomlab.guide_ids(groom, kid_group)) \
                    if any(g["name"] == kid_group for g in groomlab.list_guide_groups(groom)) else set()
                groomlab.generate_children(groom, parent_group=parent_group, name=kid_group, count=len(kids),
                                           guides=[made[old_parent]], split=True)
                fresh = sorted(set(int(i) for i in groomlab.guide_ids(groom, kid_group)) - before)
                shapes = {}
                for old, copy in zip(kids, fresh):
                    made[int(old["id"])] = copy
                    below.append(int(old["id"]))
                    shapes[str(copy)] = [list(move(q)) for q in _frames(groom, old["id"])["points"]]
                    groomlab.set_tube(groom, guides=[copy], **_tube_copy(groomlab.get_tube(groom, old["id"]), turn))
                if shapes:
                    cmds.groomLabGuides(groom, setPoints=json.dumps(shapes))
            level = below
    return new


def delete_root_tubes(groom, guides, regions=True):
    """Remove root tubes `guides` and the tube hierarchy under each (their guides; their root
    shapes go with them, and a level left with no guides its group), their kept outlines dropped.
    (`regions`: no effect, kept for older callers.) One undo step. Returns how many guides went."""
    guides = [int(g) for g in guides]
    if not guides:
        return 0
    with groomlab.undo_chunk("delete root tubes"):
        gone = cmds.groomLabGuides(groom, removeGuides=True, guide=[str(g) for g in guides]) or 0
        data = _stored_outlines(groom)
        live = set(str(g) for g in root_tubes(groom))
        kept = dict((k, v) for k, v in data.items() if k in live)
        if kept != data:
            cmds.setAttr(groom + "." + OUTLINES_ATTR, json.dumps(kept, separators=(",", ":")), type="string")
    return gone


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


def root_colors(groom, tubes=None):
    """{guide: (r, g, b)}: each root tube's color (tube k of root_tubes region_colors' k-th, as the
    engine's rootTubeColor draws its tubes and guides), for the Tube tool's outlines. `tubes`:
    root_tubes(groom) when not given."""
    tubes = root_tubes(groom) if tubes is None else tubes
    return dict(zip(tubes, region_colors(len(tubes))))


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


def footprint_error(groom, guide):
    """How far (a share of the root's size) root tube `guide`'s footprint lies off its kept
    outline made ROOT_OVERLAP larger: 0 when its tube was fitted to it as it is now."""
    outline = root_outline(groom, guide)
    c, u, v, n = _footprint(groom, guide)
    k = 1.0 + ROOT_OVERLAP
    want = [tuple(x * k for x in tm.to_plane(p, c, u, v)) for p in outline]
    have = [tm.to_plane(p, c, u, v) for p in section_outline(groom, guide, on_scalp=False)]
    size = max(math.hypot(x, y) for x, y in want) or 1.0
    return max(min(math.hypot(a[0] - b[0], a[1] - b[1]) for b in have) for a in want) / size


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


def remove_tube_level(groom, base, guides=None):
    """Undo a split (the Tube hierarchy's Remove). With `guides` (the selection), only theirs: a
    selected tube that is split loses its deepest split tubes (the deepest level under it), a
    selected tube that is a split tube itself goes back into its parent with its siblings; the
    other tubes keep their levels. Without, the hierarchy's deepest level goes everywhere (its group
    and guides). A level left with no guides goes with its group. One undo step. Returns the
    levels left ({"base", "levels", "removed": how many tubes went})."""
    info = tube_levels(groom, picked=base)
    if len(info["levels"]) <= 1:
        raise ValueError("%s has no level under it to remove" % base)
    if not guides:
        groomlab.remove_guide_group(groom, info["levels"][-1])
        out = tube_levels(groom, picked=base)
        out["removed"] = None
        return out
    data = _hierarchy(groom)
    level_of = {}
    for depth, name in enumerate(info["levels"]):
        g = _group_named(data, name)
        if g is not None:
            level_of[int(g["id"])] = depth + 1
    by_id = _guides_by_id(data)
    children = {}
    for gd in data.get("guides", []):
        if gd.get("parent") and int(gd["group"]) in level_of:
            children.setdefault(int(gd["parent"]), []).append(int(gd["id"]))

    def level(g):
        return level_of.get(int(by_id[g]["group"]), 0)

    gone = set()
    for g in (int(x) for x in guides):
        if g not in by_id or not level(g):
            continue
        if children.get(g):
            below, todo = [], list(children[g])
            while todo:
                k = todo.pop()
                if children.get(k):
                    todo.extend(children[k])
                else:
                    below.append(k)
            deepest = max(level(k) for k in below)
            gone.update(k for k in below if level(k) == deepest)
        elif level(g) > 1 and by_id[g].get("parent"):
            gone.update(children.get(int(by_id[g]["parent"]), [g]))
    if not gone:
        raise ValueError("no split tubes to remove under the selection: select a split tube, or one that is split")
    with groomlab.undo_chunk("tube hierarchy: remove a level"):
        cmds.groomLabGuides(groom, removeGuides=True, guide=[str(g) for g in sorted(gone)])
        # (a level emptied goes with its group: the removal drops it, groomLabGuides -removeGuides)
    out = tube_levels(groom, picked=base)
    out["removed"] = len(gone)
    return out


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
