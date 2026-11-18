# SPDX-License-Identifier: Apache-2.0
"""Modeled hair tubes read as GroomLab sees them (GroomLab > Utilities > Tubes to Curves and Convert
Tubes to GroomLab Tubes). No Maya: a mesh comes in as points and polygons.

The work is the core's (engine/core/include/groomlab/tube_mesh.h, groomlab_core.tube_mesh), shared
with the other hosts; these are its names as Maya's Python has always called them.

A modeled tube is a quad grid rolled into a tube, open at its ends (no caps): edge loops across it
(its sections), edge runs along it (its columns). Its end may close on one vertex (a pointed tip,
triangles round it). Each tube is read as rings: ring k is section k's points, in the same column
order on every ring, ring 0 at the root.

    shells = tube_mesh.read_tubes(points, polygons)     # [Tube] (or TubeError saying what to fix)
    tube.merge_bevels()                                # close loops and close runs averaged to one
    tube.columns()                                     # a point list per edge run, root to tip
    tube.centers()                                     # each section's area centroid

fit_section and fit_sections fit a GroomLab tube's sections (tube.h) to the rings.
"""

import math

import groomlab_core

_core = groomlab_core.tube_mesh

BEVEL_RATIO = _core.BEVEL_RATIO            # a gap under this share of the gaps beside it is a bevel
MAX_SHAPE_POINTS = _core.MAX_SHAPE_POINTS  # the most control points a tube shape takes
VOLUME_REACH = _core.VOLUME_REACH          # how far out the outermost ring of volume curves may sit
COVER_DIRECTIONS = _core.COVER_DIRECTIONS
UNCOVERED = _core.UNCOVERED                # the share of a modeled section a fitted one may leave outside it
FIT_PASSES = _core.FIT_PASSES              # how often a tube guide is moved onto its fitted sections' centers
# The guide groups the conversions make and the guide map that gives each tube a value of its own.
SHAPE_GROUP, CENTER_GROUP, GUIDE_GROUP = _core.SHAPE_GROUP, _core.CENTER_GROUP, _core.GUIDE_GROUP
REGION_MAP = _core.REGION_MAP

# A shell that is not a tube GroomLab can read, saying what to fix (a ValueError).
TubeError = _core.TubeMeshError

# One modeled tube: Tube(rings, name="tube", pointed=False, pointed_root=False, smooth_rings=None).
# Its rings, smooth_rings, cage_rings, name, pointed, pointed_root, merged_loops, merged_runs,
# sides, smooth; where it came from (shell, flipped, signature); reverse(), use_smooth(),
# end_sizes(), centers(), volume_curves(count), section_loops(per_span), columns(), merge_bevels().
Tube = _core.ModeledTube


# ---------------------------------------------------------------------------
# Small vector math (tuples), for the Python that places what the core works out


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _mul(a, k):
    return (a[0] * k, a[1] * k, a[2] * k)


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _length(a):
    return math.sqrt(_dot(a, a))


def _dist(a, b):
    return _length(_sub(a, b))


def _mean(points):
    n = float(len(points))
    return (sum(p[0] for p in points) / n, sum(p[1] for p in points) / n, sum(p[2] for p in points) / n)


def _unit(a):
    n = _length(a)
    return _mul(a, 1.0 / n) if n > 1e-12 else (0.0, 0.0, 0.0)


def lerp(a, b, f):
    return tuple(x + (y - x) * f for x, y in zip(a, b))


def to_plane(point, center, u, v):
    d = _sub(point, center)
    return (_dot(d, u), _dot(d, v))


def from_plane(xy, center, u, v):
    return _add(center, _add(_mul(u, xy[0]), _mul(v, xy[1])))


# ---------------------------------------------------------------------------
# The core's work (tube_mesh.h says what each does)

section_center = _core.section_center    # (ring): a section's area centroid
section_radius = _core.section_radius    # (ring): its mean distance from its center
ring_point = _core.ring_point            # (ring, turn): the point that share of the way round it
volume_reach = _core.volume_reach        # (count): how far out the outermost volume curves sit
volume_layout = _core.volume_layout      # (count): [(radius, turn)] of the curves round the center one
bspline_loop = _core.bspline_loop        # (points, per_span): a closed cubic B-spline's points
limit_points = _core.limit_points        # (points, polygons): each vertex on the smooth surface
read_tubes = _core.read_tubes            # (points, polygons, name="tube", smooth=False): [Tube]

signed_area = _core.signed_area          # (points2d)
fit_section = _core.fit_section          # (points2d, shape, near=None): (twist, width, height, (cx, cy))
shape_points = _core.shape_points        # (points2d, limit=MAX_SHAPE_POINTS): (points, indices)
match_fitted = _core.match_fitted        # (given, fitted): each given point's fitted point, or None
place_outline = _core.place_outline      # (outline, twist, width, height, center)
plane_section = _core.plane_section      # (columns, origin, normal): where the tube crosses a plane
ray_reach = _core.ray_reach              # (polygon, angle): how far it reaches that way, or None
cover_scale = _core.cover_scale          # (section, outline, uncovered=0.05, limits=(1.0, 1.35))
section_box = _core.section_box          # (points2d, twist): (width, height, center)
polyline_param = _core.polyline_param    # (points, p): where p lies nearest, 0 to 1
frame_at = _core.frame_at                # (frames {points, u, v}, t): (center, u, v)
_closest_on_outline = _core.closest_on_outline  # (q, placed, outline): (the outline's point, its distance)

rooted_curve = _core.rooted_curve        # (points, root_on_scalp): the root moved onto the scalp, fading out
tube_guide_path = _core.tube_guide_path  # (centers, root, normal): a tube guide's points
root_shape = _core.root_shape            # (tube, frames, t): (points, smooth, {root vertex: its point})
fit_sections = _core.fit_sections        # (frames, tube, shape_uv, vertices, root_points, outline, uncovered)
smoothed_cover = _core.smoothed_cover    # (sections, keep): the cover factors evened out
section_aspects = _core.section_aspects  # (sections): each section's height over width

region_values = _core.region_values                # (count): a gray a tube
extra_region_values = _core.extra_region_values    # (used, count): grays for tubes joining a map
rounded_region_value = _core.rounded_region_value  # (value): as a tube map keeps it
is_tube_map_name = _core.is_tube_map_name          # (name): tubes, tubes2...
unused_name = _core.unused_name                    # (base, taken): base, base2, base3...
shape_name_for = _core.shape_name_for              # (base, taken, builtin): a library shape's name


def orient(tube, root_distance=None):
    """Put the tube's root first: the end nearer the scalp (`root_distance(point)`: a point's
    distance to it), else the wider end (an open end before a pointed one). `tube.flipped` says
    whether it swapped the ends."""
    _core.orient_tube(tube, root_distance)
    return tube


def prepare(tube, ignore_bevels=True, root_distance=None):
    """The tube as the conversions take it: root first (orient), bevels merged with
    `ignore_bevels` (told apart as modeled: the smooth surface rounds them away), then on its
    smooth surface when it was read with one."""
    _core.prepare_tube(tube, ignore_bevels, root_distance)
    return tube
