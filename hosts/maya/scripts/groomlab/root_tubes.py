# SPDX-License-Identifier: Apache-2.0
"""Root-first tubes in the Tube tool (issue 61) and the tube hierarchy (issue 62), in Maya.

The work is the core's (engine/core/include/groomlab/root_tubes.h), shared with Blender and the
app; these call it through groomLabGuides -rootTubes (each edit one undo step) and keep the names
the Tube tool and scripts use.

Root-first tubes: the artist clicks the root's outline on the scalp one CV at a time; closing it on
its first CV makes a tube guide rooted at the outline's middle, its tube's root section that
outline exactly (a library shape of its own, "rootTube_<n>"), going out along a drawn curve or
straight off the scalp. The tube's root footprint is that outline fixed on the scalp: combing never
moves it, and Interpolate's fillTubes gives every strand rooted in it to that tube. A root's CVs are
kept with its guide (saved with the groom), apart from its shape: only drawing the root and editing
it set them.

    from groomlab import root_tubes
    guide = root_tubes.make_root_tube(groom, outline, path=None, length=2.0)
    cvs = root_tubes.root_outline(groom, guide)            # the root's CVs, on the scalp
    root_tubes.edit_root_tube(groom, guide, moved_cvs)       # one undo step

The tube hierarchy: a base group of tubes (level 1) and below it groups that split each tube in
two, side by side across the tube's longer side, level by level, up to MAX_LEVELS. Each level is a
child group of the one above it ("<base>_L<n>"), its guides following their parents'.

    root_tubes.add_tube_level(groom)                          # every leaf tube split in two
    root_tubes.tube_levels(groom)                             # {"base", "levels": [groups]}
    root_tubes.show_tube_level(groom, "tubes", 2)             # level 2 shown, the others hidden

Subdividing roots (issue 85): a root cut into Voronoi cells, each a root tube of its own, smaller
and more numerous where the vertex map tubeSubdivisionDensity is high (1) than where it is low
(0); the groom remembers each cut, so painting the map again re-cuts only where it changed.

    name = root_tubes.density_map_for(groom, [guide])         # its own map: tubeSubdivisionDensity01, ...
    root_tubes.ensure_density_map(groom, name)                # made at 0.5 if missing
    root_tubes.subdivide(groom, [guide], name, multiplier=1)  # {"cells", "removed", "kept"}
    root_tubes.apply_density(groom, [cell], name)             # cut again where its map changed
"""

import json

from maya import cmds

import groomlab
import groomlab_core as gl
from groomlab import tube_mesh as tm

ROOT_GROUP = "rootTubes"   # the root group new root tubes go into (when no group is picked)
SHAPE_PREFIX = "rootTube_"  # each root tube's library shape: its root outline
MAX_LEVELS = 9              # the tube hierarchy's deepest level (1: the base)
LEVEL_SUFFIX = "_L"         # level n's group: <base>_L<n>
SNAP_PIXELS = 12            # how near (in view pixels) a CV snaps to a neighbouring root's CV or edge
ROOT_OVERLAP = 0.015        # a root tube's footprint this share larger than its outline (tube.h kRootTubeOverlap)
CLEARANCE = 0.35            # a drawn guide popped this many root sizes out where it dips into the scalp ...
_CLEAR_RAMP = 2.0           # ... reached this many root sizes along it from the root (root_tubes.cpp)
MAX_LEAN = 0.8726646259971648  # (50 degrees) a root tube's guide leaves the scalp leaning at most this far off its normal
DENSITY_MAP = "tubeSubdivisionDensity"  # the one subdivision density vertex map (1: the most, smallest roots)
DENSITY_START = 0.5                     # its value where nothing is painted


# ---------------------------------------------------------------------------
# The core


def _request(groom, op, **args):
    """groomLabGuides -rootTubes: the core's root tube work (an edit one undo step), its JSON answer."""
    _adopt_scene_outlines(groom)
    args["op"] = op
    return json.loads(cmds.groomLabGuides(groom, rootTubes=json.dumps(args)) or "null")


# GroomLab 0.49.0 kept a root tube's CVs on the scene (this attribute), not with its guide. A groom
# from it has them read once into its guides and the attribute dropped. Remove this after a few
# releases (DEVELOPING.md's rule is no migrations: this one only spares outlines drawn with 0.49.0).
_SCENE_OUTLINES = "groomLabRootOutlines"


def _adopt_scene_outlines(groom):
    if not cmds.objExists(groom) or not cmds.attributeQuery(_SCENE_OUTLINES, node=groom, exists=True):
        return
    try:
        stored = json.loads(cmds.getAttr(groom + "." + _SCENE_OUTLINES) or "{}")
    except ValueError:
        stored = {}
    with groomlab.undo_chunk("keep root outlines with their guides"):
        for guide, bound in sorted(stored.items()):
            try:
                outline = _scene_outline(groom, bound)
                cmds.groomLabGuides(groom, rootTubes=json.dumps({"op": "keepOutline", "guide": int(guide),
                                                                 "outline": [list(p) for p in outline]}))
            except (ValueError, RuntimeError):
                pass  # (a root tube gone, or the scalp changed under it: its tube's section stands in)
        cmds.deleteAttr(groom, attribute=_SCENE_OUTLINES)


def _scene_outline(groom, bound):
    """0.49.0's records ([scalp, Maya face, fan triangle, w0, w1, w2] each) as world points."""
    from maya.api import OpenMaya as om
    from groomlab import utils
    meshes = [m for m in [groomlab.scalp_mesh(groom)] + [s["mesh"] for s in groomlab.scalps(groom)] if m]
    fns = [utils._om_mesh(m) for m in meshes]
    out = []
    for mi, face, k, w0, w1, w2 in bound:
        if mi >= len(fns) or face >= fns[mi].numPolygons:
            raise ValueError("the scalp changed under a root outline")
        verts = fns[mi].getPolygonVertices(face)
        if k + 1 >= len(verts):
            raise ValueError("the scalp changed under a root outline")
        a, b, c = (tuple(fns[mi].getPoint(verts[i], om.MSpace.kWorld))[:3] for i in (0, k, k + 1))
        out.append(tm._add(tm._add(tm._mul(a, w0), tm._mul(b, w1)), tm._mul(c, w2)))
    return out


def _tuple(v):
    return (float(v[0]), float(v[1]), float(v[2])) if not hasattr(v, "x") else (v.x, v.y, v.z)


def _tuples(points):
    return [_tuple(p) for p in points]


def _lists(points):
    return [list(_tuple(p)) for p in points]


# ---------------------------------------------------------------------------
# Small helpers (the Tube tool and the tests use some)


def _hierarchy(groom):
    return groomlab.get_hierarchy(groom) or {}


def _guides_by_id(data):
    return {int(g["id"]): g for g in data.get("guides", [])}


def _shape_of(tube):
    return (tube or {}).get("shape", "")


def _scalp_surface(groom):
    """surface(point) -> (the nearest point on the scalp, the normal there), Maya's meshes'."""
    from groomlab import utils
    surface = utils._scalp_surface(groom)
    if surface is None:
        raise ValueError("the groom has no scalp to root a tube on")
    return surface


def _centroid(points):
    """The area centroid of a closed outline (in its best plane; the mean where it has no area)."""
    return _tuple(gl.outline_centroid(_tuples(points)))


def _resample(points, count):
    """`count` points evenly along a polyline (2 or more)."""
    return _tuples(gl.resample_polyline(_tuples(points), int(count)))


def _length(points):
    return sum(tm._dist(a, b) for a, b in zip(points, points[1:]))


def _turning(pts):
    """How far (radians) a polyline turns, all told."""
    return float(gl.polyline_turning(_tuples(pts)))


def _lean(direction, normal):
    """How far (radians) `direction` leans off the scalp's `normal`."""
    return float(gl.lean_off(_tuple(direction), _tuple(normal)))


def path_spans(points, size, density=1.0):
    """How many spans a drawn guide gets (about one per root size along it, enough that none turns
    more than 15 degrees, both times `density`, 3 to 200)."""
    return int(gl.root_path_spans(_tuples(points), float(size), float(density)))


def _guide_path(root, normal, path, length, segments, size=None, density=1.0, surface=None):
    """The guide's points: from `root` straight off the scalp, or along the drawn `path` smoothed
    over the root's `size` and evenly spaced (the core's rootTubeGuidePath; `surface`: unused, the
    core keeps a guide clear of the groom's scalp when it makes the tube)."""
    return _tuples(gl.root_tube_guide_path(_tuple(root), _tuple(normal), _tuples(path or []), float(length), int(segments),
                                           float(size or 0.0), float(density)))


def _library_shape(groom, name):
    return next((s for s in groomlab.tube_shapes(groom) if s["name"] == name), None)


def _smooth_normal(groom, point):
    """The scalp's normal at `point` as GroomLab reads it (ScalpMesh::normal: area-weighted
    corner normals blended across the triangle)."""
    return tuple(_request(groom, "surface", points=[list(_tuple(point))])[0][1])


def _project(groom, points):
    """Points taken onto the scalp (the nearest point on it)."""
    return [tuple(q) for q, _n in _request(groom, "surface", points=_lists(points))]


# ---------------------------------------------------------------------------
# Root-first tubes


def make_root_tube(groom, outline, path=None, length=1.0, segments=12, taper=0.5, group=None, regions=True,
                   density=1.0):
    """A tube guide grown from its root's outline: `outline` the root's CVs on the scalp (world
    points, 3 or more, in order round it), along the drawn `path` (world points from the root out)
    or with none straight off the scalp `length` long in `segments` segments; its tube a library
    shape of its own whose root is the drawn outline, its width going to `taper` times the root's at
    the tip. Into `group` (default ROOT_GROUP, made when missing). (`regions`: no effect, kept for
    older callers.) One undo step. Returns the guide's ID."""
    outline = _lists(outline)
    if len(outline) < 3:
        raise ValueError("a root outline needs at least 3 CVs")
    if len(outline) > tm.MAX_SHAPE_POINTS:
        raise ValueError("a root outline takes at most %d CVs" % tm.MAX_SHAPE_POINTS)
    return int(_request(groom, "make", outline=outline, path=_lists(path or []), length=float(length), segments=int(segments),
                        taper=float(taper), density=float(density), group=group or ""))


def is_root_tube(guide_data):
    """Whether a guide (the stored hierarchy's dict) is a root-first tube: a primary whose tube
    takes a rootTube_ shape."""
    return not guide_data.get("parent") and _shape_of(guide_data.get("tube")).startswith(SHAPE_PREFIX)


def root_tubes(groom, guides=None):
    """The root-first tubes' guide IDs (of `guides` when given: those among them, or the root
    tubes their tubes descend from), in ID order."""
    return [int(g) for g in _request(groom, "rootTubes", guides=[int(g) for g in (guides or [])])]


def root_outline(groom, guide):
    """A root tube's CVs as world points on the scalp: as drawn or last edited, whatever has been
    done to the guide since (a root tube made before they were kept: section_outline)."""
    return _tuples(_request(groom, "outline", guide=int(guide)))


def section_outline(groom, guide, on_scalp=True):
    """A root tube's tube's root footprint as world points: its shape's control points placed in
    its root section laid flat on the scalp (`on_scalp`: taken onto the scalp)."""
    return _tuples(_request(groom, "sectionOutline", guide=int(guide), onScalp=bool(on_scalp)))


def edit_root_tube(groom, guide, outline, regions=True):
    """A root tube given a new root outline (as root_outline gives it, moved): the guide moved
    whole to root at the new outline's middle, its shape the new outline, its tube scaled,
    flattened and turned as much (its taper and what brushes painted kept). Its child tubes follow.
    One undo step."""
    outline = _lists(outline)
    if len(outline) < 3:
        raise ValueError("a root outline needs at least 3 CVs")
    _request(groom, "edit", guide=int(guide), outline=outline)
    return guide


def root_center(groom, guide):
    """A root tube's root middle on the scalp (its outline's centroid, taken onto it) and the
    scalp's normal there."""
    point, normal = _request(groom, "center", guide=int(guide))
    return tuple(point), tuple(normal)


def carried(groom, outline, start, to, held=()):
    """`outline` (a root's CVs on the scalp) carried over the scalp from its middle at `start` to
    `to`: each CV turned with the scalp's normal from there to here, set down at `to` and taken
    onto the scalp. CVs whose index is in `held` stay where they are (sealed onto a neighbour)."""
    return _tuples(_request(groom, "carry", outline=_lists(outline), start=list(_tuple(start)), to=list(_tuple(to)),
                            held=sorted(int(i) for i in held)))


def sealed(groom, guide, outline=None, others=None, tolerance=None):
    """The indices of `guide`'s root CVs (`outline`: as root_outline gives them) lying on another
    root tube's outline (one of its CVs, or on one of its edges): sealed onto a neighbour."""
    outline = outline if outline is not None else root_outline(groom, guide)
    others = others if others is not None else snap_targets(groom, skip=[guide])
    return [int(i) for i in _request(groom, "sealed", outline=_lists(outline), others=[_lists(cvs) for _g, cvs in others],
                                     tolerance=-1.0 if tolerance is None else float(tolerance))]


def duplicate_root_tube(groom, guide, to, regions=True, start=None):
    """A copy of root tube `guide` with its root set down at `to` (with `start`, that point carried
    to `to` instead of its root's middle: several roots copied as one group): its outline and guide
    carried there over the scalp, its tube a copy (a root shape of its own), and the tube hierarchy
    under it copied level by level. Into the same groups. One undo step. Returns the new guide's ID."""
    args = {"guide": int(guide), "to": list(_tuple(to))}
    if start is not None:
        args["start"] = list(_tuple(start))
    return int(_request(groom, "duplicate", **args))


def delete_root_tubes(groom, guides, regions=True):
    """Remove root tubes `guides` and the tube hierarchy under each (their root shapes go with them,
    and a level left with no guides its group). One undo step. Returns how many guides went."""
    guides = [int(g) for g in guides]
    if not guides:
        return 0
    return int(_request(groom, "delete", guides=guides))


# ---------------------------------------------------------------------------
# Each root tube's color


def region_colors(count):
    """`count` colors far apart (hues a golden step apart, three brightnesses), none black: the
    core's rootTubeColor, every host's."""
    import groomlab_core
    return [tuple(groomlab_core.root_tube_color(k)) for k in range(count)]


def root_colors(groom, tubes=None):
    """{guide: (r, g, b)}: each root tube's color, as the engine draws its tubes and guides.
    `tubes`: root_tubes(groom) when not given."""
    tubes = root_tubes(groom) if tubes is None else tubes
    return dict(zip(tubes, region_colors(len(tubes))))


def footprint_error(groom, guide):
    """How far (a share of the root's size) root tube `guide`'s footprint lies off its kept
    outline made ROOT_OVERLAP larger: 0 when its tube was fitted to it as it is now."""
    return float(_request(groom, "footprintError", guide=int(guide)))


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
        except RuntimeError:
            pass
    return out


def snap_point(p, outlines, to_screen, pixels=SNAP_PIXELS):
    """`p` (world) snapped onto the nearest neighbouring root: one of its CVs within `pixels` on
    screen, else the nearest point of its edges within `pixels`; else `p`. `to_screen(point)`: its
    view coordinates. Returns (point, snapped: "cv", "edge" or None)."""
    point, how = gl.snap_root_cv(_tuple(p), [_tuples(cvs) for _g, cvs in outlines], to_screen, float(pixels))
    return _tuple(point), how


# ---------------------------------------------------------------------------
# The tube hierarchy


def level_group(base, level):
    """The group of `level` (1: the base) in base group `base`'s hierarchy."""
    return gl.tube_level_group(base, int(level))


def tube_levels(groom, guides=None, picked=None):
    """The hierarchy's levels: {"base": the base group, "levels": [the group of each level, the
    base first]}: each level's group a child group of the one above, named <base>_L<n>."""
    return _request(groom, "levels", guides=[int(g) for g in (guides or [])], picked=picked or "")


def base_group(groom, guides=None, picked=None, groups=None):
    """The base group (level 1) the hierarchy works on: the top group of the first of `guides`,
    else of the group `picked`, else the first top group holding guides."""
    return tube_levels(groom, guides, picked)["base"]


def add_tube_level(groom, guides=None, picked=None):
    """Split tubes in two (the Tube hierarchy's Add): each of the leaf tubes under `guides` (or
    under every guide of the base group when none are given) gets two child tubes, side by side
    across its longer side, sharing its root shape, in the next level's group. Up to MAX_LEVELS.
    One undo step. Returns {"base", "levels", "split": how many tubes were split}."""
    base = tube_levels(groom, guides, picked)["base"]
    split = _request(groom, "addLevel", guides=[int(g) for g in (guides or [])], picked=picked or "")
    info = tube_levels(groom, picked=base)
    info["split"] = int(split)
    return info


def remove_tube_level(groom, base, guides=None):
    """Undo a split (the Tube hierarchy's Remove). With `guides` (the selection), only theirs: a
    selected tube that is split loses its deepest split tubes, a selected split tube goes back into
    its parent with its siblings. Without, the hierarchy's deepest level goes everywhere. A level
    left with no guides goes with its group. One undo step. Returns the levels left ({"base",
    "levels", "removed": how many tubes went, None for a whole level})."""
    removed = _request(groom, "removeLevel", base=base, guides=[int(g) for g in (guides or [])])
    out = tube_levels(groom, picked=base)
    out["removed"] = int(removed) if guides else None
    return out


def set_tube_view(groom, group):
    """Lock view (the Tube hierarchy's): only guide group `group`'s tubes draw, whichever guides
    show ("": the tubes as their guides). The groom's tubeViewGroup."""
    from maya import cmds
    if cmds.attributeQuery("tubeViewGroup", node=groom, exists=True):
        if (cmds.getAttr(groom + ".tubeViewGroup") or "") != (group or ""):
            cmds.setAttr(groom + ".tubeViewGroup", group or "", type="string")


def set_mixed_view(groom, base, level):
    """The Tube hierarchy's Mixed view: working on level `level` of `base`'s hierarchy, the levels
    above shown where no level below continues them (their guides with no children); 0 (or no base):
    off. The groom's mixedView ("<base>:<level>")."""
    from maya import cmds
    want = "%s:%d" % (base, int(level)) if base and level and int(level) > 0 else ""
    if cmds.attributeQuery("mixedView", node=groom, exists=True):
        if (cmds.getAttr(groom + ".mixedView") or "") != want:
            cmds.setAttr(groom + ".mixedView", want, type="string")


def show_tube_level(groom, base, level, locked=0):
    """Show level `level` (1: the base) of `base`'s hierarchy and hide its other levels (the guide
    groups' eyes; groups outside the hierarchy are left as they are); with `locked` (Lock view)
    level `locked`'s tubes alone draw (set_tube_view), its guides as the level picked. Returns level
    `level`'s group."""
    shown = _request(groom, "levelVisibility", base=base, level=int(level), locked=int(locked or 0))
    levels = tube_levels(groom, picked=base)["levels"]
    for name, visible in shown.items():
        groomlab.show_guide_group(groom, name, visible=bool(visible))
    # (The tubes: Lock view's level alone; else as their guides.)
    set_tube_view(groom, levels[int(locked) - 1] if locked and 0 < int(locked) <= len(levels) else "")
    level = max(1, min(int(level), len(levels))) if levels else 1
    return levels[level - 1] if levels else None


# ---------------------------------------------------------------------------
# Subdividing roots (issue 85)


def density_map_for(groom, guides):
    """The density map to paint for cutting `guides` (root tubes, or tubes of their hierarchies):
    the map of the subdivision the first is a cell of, else a new one (tubeSubdivisionDensity01,
    02, ...: the first no subdivision or map of the groom takes)."""
    return density_map_info(groom, guides)["map"]


def density_map_info(groom, guides):
    """density_map_for's map and whether it is new ({"map", "fresh"}: fresh when none of `guides`
    is a cell of a cut, so cutting them starts a cut of their own)."""
    from groomlab import painting
    answer = _request(groom, "densityMap", guides=[int(g) for g in guides], taken=list(painting.list_maps(groom)))
    return {"map": answer["map"], "fresh": bool(answer.get("fresh"))}


def ensure_density_map(groom, name):
    """The subdivision density map `name` (a gray vertex map), made at DENSITY_START when the groom
    has none of that name. Returns its name."""
    from groomlab import painting
    if name not in painting.list_maps(groom):
        painting.add_map(groom, name, value=DENSITY_START, kind="vertices")
    return name


def _cut(answer):
    return {"cells": [int(g) for g in answer.get("cells", [])], "removed": int(answer.get("removed", 0)),
            "kept": int(answer.get("kept", 0))}


def subdivide(groom, guides, map=None, multiplier=1.0):
    """Cut root tubes `guides` (a smaller tube of the hierarchy stands for its root tube) into
    Voronoi cells at the density its map `map` gives (density_map_for; DENSITY_START where the
    groom has no such map), times `multiplier` (2: about twice the cells), each a level 1 root tube
    following the old tube, the hierarchy under it made again in each. One undo step. Returns
    {"cells": the new root tubes, "removed": guides taken out, "kept": 0}."""
    name = map or density_map_for(groom, guides)
    return _cut(_request(groom, "subdivide", guides=[int(g) for g in guides], map=name, multiplier=float(multiplier)))


def apply_density(groom, guides=None, map=None, multiplier=1.0, force=False):
    """Painting the density map `map` finished: the subdivisions cut with it cut again where it
    changed (all of a root again when `multiplier` changed, or with `force`: the card's Apply), and
    `guides` (root tubes not cut yet) cut with it. Without `map`: every subdivision by its own map,
    each keeping its multiplier. One undo step. Returns {"cells": the root tubes made, "removed",
    "kept": cells left as they were}."""
    args = {"guides": [int(g) for g in (guides or [])], "map": map or "", "multiplier": float(multiplier) if map else 0.0,
            "force": bool(force)}
    return _cut(_request(groom, "applyDensity", **args))
