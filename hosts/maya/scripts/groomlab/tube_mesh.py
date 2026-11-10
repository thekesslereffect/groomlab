# SPDX-License-Identifier: Apache-2.0
"""Modeled hair tubes read as GroomLab sees them (GroomLab > Utilities > Tubes to Curves and Convert
Tubes to GroomLab Tubes). Plain Python, no Maya: a mesh comes in as points and polygons.

A modeled tube is a quad grid rolled into a tube, open at its ends (no caps): edge loops across it
(its sections), edge runs along it (its columns). Its end may close on one vertex (a pointed tip,
triangles round it). Each tube is read as rings: ring k is section k's points, in the same column
order on every ring, ring 0 at the root.

    shells = tube_mesh.read_tubes(points, polygons)     # [Tube] (or TubeError saying what to fix)
    tube.merge_bevels()                                # close loops and close runs averaged to one
    tube.columns()                                     # a point list per edge run, root to tip
    tube.centers()                                     # each section's area centroid

fit_section and fit_shape fit a GroomLab tube's sections (tube.h) to the rings.
"""

import math

# A gap between two loops (or two runs) under this share of the gaps beside it is a bevel.
BEVEL_RATIO = 0.35
# The most control points a tube shape takes (tube.h: kMaxTubeShapePoints).
MAX_SHAPE_POINTS = 64


class TubeError(ValueError):
    """A shell that is not a tube GroomLab can read, saying what to fix."""


# ---------------------------------------------------------------------------
# Small vector math (tuples)


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


def section_center(ring):
    """A section's area centroid (the middle of its volume there), in the plane it best lies in;
    the points' mean when it has no area (a pointed tip)."""
    mid = _mean(ring)
    total, acc = 0.0, (0.0, 0.0, 0.0)
    normal = (0.0, 0.0, 0.0)
    n = len(ring)
    for i in range(n):  # Newell's normal
        a, b = ring[i], ring[(i + 1) % n]
        normal = _add(normal, ((a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]), (a[0] - b[0]) * (a[1] + b[1])))
    normal = _unit(normal)
    if normal == (0.0, 0.0, 0.0):
        return mid
    for i in range(n):  # a fan round the mean: each triangle's centroid weighted by its area
        a, b = ring[i], ring[(i + 1) % n]
        area = _dot(_cross(_sub(a, mid), _sub(b, mid)), normal) * 0.5
        total += area
        acc = _add(acc, _mul(_mean((mid, a, b)), area))
    if abs(total) < 1e-12:
        return mid
    return _mul(acc, 1.0 / total)


def ring_point(ring, turn):
    """The point `turn` (0..1) of the way round a section's outline, by length, from its first point."""
    n = len(ring)
    edges = [_dist(ring[i], ring[(i + 1) % n]) for i in range(n)]
    total = sum(edges)
    if total < 1e-12:
        return ring[0]
    left = (turn % 1.0) * total
    for i, e in enumerate(edges):
        if left <= e or i == n - 1:
            return lerp(ring[i], ring[(i + 1) % n], min(1.0, left / e) if e > 1e-12 else 0.0)
        left -= e
    return ring[0]


# How far out the outermost ring of volume curves may sit (a share of the way to the tube's
# surface), and how fast it gets there: a few curves stay near the center, more spread out to make
# room (the reach grows with the square root of the count, as an evenly filled disc's does).
VOLUME_REACH = 0.8
VOLUME_BASE = 0.4
VOLUME_SPREAD = 0.1


def volume_reach(count):
    """How far out the outermost ring of `count` volume curves sits: 0.6 for 4, 0.72 for 10,
    VOLUME_REACH from 16."""
    return min(VOLUME_REACH, VOLUME_BASE + VOLUME_SPREAD * math.sqrt(max(1, count)))


def volume_layout(count):
    """Where the curves other than the center one go when `count` curves fill a tube: [(radius, turn)],
    radius a share of the way from the center to the surface, turn a share of the way round. Rings
    round the center, evenly spaced out to volume_reach(count), each holding curves in proportion to its
    size (as a honeycomb would: 6, 12, 18...), spread evenly round it, every other ring turned
    half a step so the curves sit between those of the ring inside."""
    extra = max(0, int(count) - 1)
    if not extra:
        return []
    rings = 1
    while 3 * rings * (rings + 1) < extra:
        rings += 1
    weights = list(range(1, rings + 1))
    exact = [extra * w / float(sum(weights)) for w in weights]
    sizes = [max(1, int(e)) for e in exact]
    while sum(sizes) > extra:  # (each ring kept with at least one)
        sizes[sizes.index(max(sizes))] -= 1
    order = sorted(range(rings), key=lambda j: exact[j] - int(exact[j]), reverse=True)
    for j in order[:extra - sum(sizes)]:
        sizes[j] += 1
    out = []
    reach = volume_reach(count)
    for j, size in enumerate(sizes):
        radius = reach * (j + 1) / float(rings)
        offset = 0.5 / size if j % 2 else 0.0
        out.extend((radius, offset + i / float(size)) for i in range(size))
    return out


def bspline_loop(points, per_span):
    """A closed uniform cubic B-spline's points through control polygon `points`, `per_span` each
    span, the first of span i at control point i's limit ((p[i-1] + 4 p[i] + p[i+1]) / 6): a
    smoothed mesh's open border, or the section a smooth surface draws round an edge loop."""
    n = len(points)
    out = []
    for i in range(n):
        p0, p1, p2, p3 = points[i - 1], points[i], points[(i + 1) % n], points[(i + 2) % n]
        for k in range(per_span):
            t = k / float(per_span)
            t2, t3 = t * t, t * t * t
            b0 = (1 - t) ** 3 / 6.0
            b1 = (3 * t3 - 6 * t2 + 4) / 6.0
            b2 = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6.0
            b3 = t3 / 6.0
            out.append(tuple(b0 * a + b1 * b + b2 * c + b3 * d for a, b, c, d in zip(p0, p1, p2, p3)))
    return out


def section_radius(ring):
    """A section's mean distance from its center."""
    c = section_center(ring)
    return sum(_dist(p, c) for p in ring) / float(len(ring))


# ---------------------------------------------------------------------------
# The smooth (limit) surface


def limit_points(points, polygons):
    """Each vertex where it lies on the mesh's Catmull-Clark limit surface (the surface smooth mesh
    preview draws, at any level, converges to it), open borders smooth curves (edges kept sharp
    there, as Maya's and OpenSubdiv's boundary rules do). One subdivision step makes every face a
    quad; the limit masks then put each vertex's child where infinitely many steps would."""
    n_points = len(points)
    face_point = [_mean([points[v] for v in poly]) for poly in polygons]
    edge_faces = {}
    vertex_faces = [[] for _ in range(n_points)]
    for f, poly in enumerate(polygons):
        for i, v in enumerate(poly):
            w = poly[(i + 1) % len(poly)]
            edge_faces.setdefault((min(v, w), max(v, w)), []).append(f)
            vertex_faces[v].append(f)
    vertex_edges = [[] for _ in range(n_points)]
    for edge in edge_faces:
        vertex_edges[edge[0]].append(edge)
        vertex_edges[edge[1]].append(edge)

    def border(edge):
        return len(edge_faces[edge]) == 1

    edge_point = {}
    for edge, faces in edge_faces.items():
        a, b = points[edge[0]], points[edge[1]]
        if len(faces) == 2:
            edge_point[edge] = _mean([a, b, face_point[faces[0]], face_point[faces[1]]])
        else:
            edge_point[edge] = _mean([a, b])

    out = []
    for v in range(n_points):
        edges, faces = vertex_edges[v], vertex_faces[v]
        if not edges:
            out.append(points[v])
            continue
        p = points[v]
        borders = [e for e in edges if border(e)]
        if borders:
            if len(borders) != 2:  # (a corner or worse: left where it is)
                out.append(p)
                continue
            ends = [points[e[0] if e[1] == v else e[1]] for e in borders]
            child = _mul(_add(_add(ends[0], ends[1]), _mul(p, 6.0)), 1.0 / 8.0)
            e0, e1 = edge_point[borders[0]], edge_point[borders[1]]
            out.append(_mul(_add(_add(e0, e1), _mul(child, 4.0)), 1.0 / 6.0))
            continue
        n = float(len(edges))
        q = _mean([face_point[f] for f in faces])
        r = _mean([_mean([points[e[0]], points[e[1]]]) for e in edges])
        child = _mul(_add(_add(q, _mul(r, 2.0)), _mul(p, n - 3.0)), 1.0 / n)
        # After one step every face round the child is a quad: child, edge point, face point, edge point.
        sum_e = (0.0, 0.0, 0.0)
        for e in edges:
            sum_e = _add(sum_e, edge_point[e])
        sum_f = (0.0, 0.0, 0.0)
        for f in faces:
            sum_f = _add(sum_f, face_point[f])
        total = _add(_add(_mul(child, n * n), _mul(sum_e, 4.0)), sum_f)
        out.append(_mul(total, 1.0 / (n * (n + 5.0))))
    return out


# ---------------------------------------------------------------------------
# Reading tubes off a mesh


class Tube(object):
    """One modeled tube: `rings` (ring k: section k's points, columns in the same order on every
    ring, ring 0 at the root), `name`, and whether its last ring is a pointed tip (`pointed`, all
    its points one vertex) or its first is (`pointed_root`). `smooth_rings`: the same vertices on
    the mesh's smooth surface (limit_points), kept in step through every change (reverse, bevels:
    decided on `rings`, as modeled) until use_smooth puts them in place of `rings`."""

    def __init__(self, rings, name="tube", pointed=False, pointed_root=False, smooth_rings=None):
        self.rings = [list(r) for r in rings]
        self.smooth_rings = [list(r) for r in smooth_rings] if smooth_rings is not None else None
        self.cage_rings = None  # once use_smooth: the rings as modeled (the smooth ones in `rings`)
        self.name = name
        self.pointed = pointed
        self.pointed_root = pointed_root
        self.merged_loops = 0
        self.merged_runs = 0

    @property
    def sides(self):
        return len(self.rings[0]) if self.rings else 0

    def reverse(self):
        """Root and tip swapped."""
        self.rings.reverse()
        if self.smooth_rings is not None:
            self.smooth_rings.reverse()
        self.pointed, self.pointed_root = self.pointed_root, self.pointed

    def reverse_columns(self):
        """The columns in the other order round the tube."""
        self.rings = [list(reversed(r)) for r in self.rings]
        if self.smooth_rings is not None:
            self.smooth_rings = [list(reversed(r)) for r in self.smooth_rings]

    def use_smooth(self):
        """Read the tube on its smooth surface from now on (when it has one)."""
        if self.smooth_rings is not None:
            self.cage_rings, self.rings, self.smooth_rings = self.rings, self.smooth_rings, None
        return self

    @property
    def smooth(self):
        """Read on the smooth surface (use_smooth): its sections are smooth curves, not polygons."""
        return self.cage_rings is not None

    def end_sizes(self):
        """(root, tip): each end section's mean radius."""
        return section_radius(self.rings[0]), section_radius(self.rings[-1])

    def centers(self):
        """Each section's area centroid, root to tip: the tube's center curve."""
        return [section_center(r) for r in self.rings]

    def volume_curves(self, count):
        """`count` curves filling the tube, root to tip: the center curve first, then the others on
        rings round it (volume_layout), each at the same place in every section, so they follow
        the tube's shape and twist and stay inside it."""
        centers = self.centers()
        curves = [centers]
        for radius, turn in volume_layout(count):
            curves.append([lerp(c, ring_point(r, turn), radius) for c, r in zip(centers, self.rings)])
        return curves

    def section_loops(self, per_span=4):
        """Each ring's section as drawn, root to tip: read smooth (use_smooth), the curve the smooth
        surface draws round that edge loop (a cubic B-spline round the loop through the modeled
        rings: each control point the vertex's (above + 4 * it + below) / 6, the loop itself at an
        open end), `per_span` points per edge; else the loop's own points."""
        if self.cage_rings is None:
            return [list(r) for r in self.rings]
        cage = self.cage_rings
        n = len(cage)
        loops = []
        for k, ring in enumerate(cage):
            if section_radius(ring) <= 1e-12:  # a pointed end: its one point, as often as the others have
                loops.append([self.rings[k][0]] * (len(ring) * per_span))
                continue
            if k == 0 or k == n - 1:  # an open end: its own border curve
                control = ring
            else:
                below, above = cage[k - 1], cage[k + 1]
                control = [tuple((a + 4.0 * b + c) / 6.0 for a, b, c in zip(below[i], ring[i], above[i]))
                           for i in range(len(ring))]
            loops.append(bspline_loop(control, per_span))
        return loops

    def columns(self):
        """A point list per edge run along the tube, root to tip (a pointed end: each run ends on its vertex)."""
        return [[r[i] for r in self.rings] for i in range(self.sides)]

    def merge_bevels(self, ratio=BEVEL_RATIO):
        """Average away bevels: edge loops lying much closer together than the loops beside them
        become one loop (their mean; the root's and the tip's loop kept as they are), and edge runs
        lying much closer together round the tube than the runs beside them become one run. So
        the tube reads as it would without its bevels. Returns (loops removed, runs removed)."""
        before_loops, before_runs = len(self.rings), self.sides
        self._merge_loops(ratio)
        self._merge_runs(ratio)
        self.merged_loops += before_loops - len(self.rings)
        self.merged_runs += before_runs - self.sides
        return before_loops - len(self.rings), before_runs - self.sides

    def _merge_loops(self, ratio):
        rings = self.rings
        if len(rings) < 3:
            return
        gaps = [sum(_dist(a, b) for a, b in zip(rings[k], rings[k + 1])) / float(self.sides)
                for k in range(len(rings) - 1)]
        close = _close_gaps(gaps, ratio, cyclic=False)
        plan, k = [], 0  # each new loop: the old loops it is the mean of
        while k < len(rings):
            group = [k]
            while group[-1] < len(gaps) and close[group[-1]]:
                group.append(group[-1] + 1)
            if len(group) == 1:
                plan.append(group)
            elif group[0] == 0:
                plan.append([0])  # the root's loop stays where it is
            elif group[-1] == len(rings) - 1:
                plan.append([len(rings) - 1])  # and the tip's
            else:
                plan.append(group)
            k = group[-1] + 1
        sides = self.sides

        def merged(old):
            return [old[g[0]] if len(g) == 1 else [_mean([old[j][i] for j in g]) for i in range(sides)]
                    for g in plan]
        self.rings = merged(rings)
        if self.smooth_rings is not None:
            self.smooth_rings = merged(self.smooth_rings)

    def _merge_runs(self, ratio):
        n = self.sides
        if n <= 3:
            return
        shaped = [r for r in self.rings if section_radius(r) > 1e-9] or self.rings
        gaps = [sum(_dist(r[i], r[(i + 1) % n]) for r in shaped) / float(len(shaped)) for i in range(n)]
        close = _close_gaps(gaps, ratio, cyclic=True)
        if not any(close) or all(close):
            return
        # Groups of runs joined by close gaps, round the tube from a gap that is not close.
        start = (next(i for i in range(n) if not close[i]) + 1) % n
        groups, current = [], [start]
        for step in range(1, n):
            i = (start + step) % n
            if close[(i - 1) % n]:
                current.append(i)
            else:
                groups.append(current)
                current = [i]
        groups.append(current)
        if len(groups) < 3:
            return
        self.rings = [[_mean([r[i] for i in g]) for g in groups] for r in self.rings]
        if self.smooth_rings is not None:
            self.smooth_rings = [[_mean([r[i] for i in g]) for g in groups] for r in self.smooth_rings]


def _close_gaps(gaps, ratio, cyclic):
    """Which gaps are bevels: under `ratio` of the larger gap beside them."""
    n = len(gaps)
    out = []
    for i in range(n):
        side = []
        if cyclic or i > 0:
            side.append(gaps[(i - 1) % n])
        if cyclic or i < n - 1:
            side.append(gaps[(i + 1) % n])
        out.append(bool(side) and gaps[i] < ratio * max(side))
    return out


def _shells(vertex_count, edges):
    parent = list(range(vertex_count))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for a, b in edges:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    groups = {}
    for v in range(vertex_count):
        groups.setdefault(find(v), []).append(v)
    return [sorted(g) for g in groups.values()]


def _boundary_loops(boundary_edges):
    """Boundary edges as ordered vertex loops."""
    nbrs = {}
    for a, b in boundary_edges:
        nbrs.setdefault(a, []).append(b)
        nbrs.setdefault(b, []).append(a)
    if any(len(v) != 2 for v in nbrs.values()):
        raise TubeError("its open edges touch at a vertex (non-manifold)")
    loops, seen = [], set()
    for start in sorted(nbrs):
        if start in seen:
            continue
        loop, prev, cur = [start], None, start
        seen.add(start)
        while True:
            a, b = nbrs[cur]
            nxt = a if a != prev else b
            if nxt == start:
                break
            loop.append(nxt)
            seen.add(nxt)
            prev, cur = cur, nxt
        loops.append(loop)
    return loops


def _walk(start, nbrs, ends, shell_size):
    """Rings from boundary loop `start`, across the tube, to the boundary loop in `ends` or a pointed tip."""
    rings = [list(start)]
    used = set(start)
    prev = set()
    while True:
        cur = rings[-1]
        cur_set = set(cur)
        nxt = []
        for v in cur:
            ahead = [w for w in nbrs[v] if w not in cur_set and w not in prev]
            if len(ahead) != 1:
                if not ahead and len(rings) > 1:
                    raise TubeError("an edge loop stops short of the tube's end")
                raise TubeError("it is not a clean quad tube (a vertex where %d edges run on, not 1)" % len(ahead))
            nxt.append(ahead[0])
        distinct = set(nxt)
        if len(distinct) == 1 and len(cur) > 1:  # a pointed tip
            rings.append(nxt)
            used |= distinct
            pointed = True
            break
        if len(distinct) != len(nxt):
            raise TubeError("its edge loops change how many points they have along the tube "
                            "(only a pointed tip may close them)")
        if distinct & used:
            raise TubeError("its edge loops run back on themselves")
        rings.append(nxt)
        used |= distinct
        prev = cur_set
        if any(distinct == e for e in ends):
            pointed = False
            break
        if len(used) >= shell_size:
            raise TubeError("its edge loops never reach its other end")
    if len(used) != shell_size:
        raise TubeError("it has vertices off its edge loops")
    return rings, pointed


def read_tubes(points, polygons, name="tube", smooth=False):
    """The tubes of a mesh: `points` its vertex positions, `polygons` each face's vertex indices.
    One Tube per shell, named `name` (shells after the first: name_2, name_3...). Ring 0 at the
    end that is open (either, both open). Raises TubeError on a shell that is not an open quad
    tube, saying which and what to fix. With `smooth` each tube also carries its vertices on the
    smooth surface (Tube.smooth_rings, limit_points)."""
    smooth_points = limit_points(points, polygons) if smooth else None
    edge_faces = {}
    for poly in polygons:
        for i in range(len(poly)):
            a, b = poly[i], poly[(i + 1) % len(poly)]
            key = (a, b) if a < b else (b, a)
            edge_faces[key] = edge_faces.get(key, 0) + 1
    if any(c > 2 for c in edge_faces.values()):
        raise TubeError("%s has edges shared by more than two faces (non-manifold)" % name)
    used = set(v for poly in polygons for v in poly)
    nbrs = {}
    for a, b in edge_faces:
        nbrs.setdefault(a, set()).add(b)
        nbrs.setdefault(b, set()).add(a)
    tubes = []
    shells = [s for s in _shells(len(points), list(edge_faces)) if s[0] in used]
    for index, shell in enumerate(shells):
        label = name if len(shells) == 1 else "%s_%d" % (name, index + 1)
        members = set(shell)
        try:
            boundary = [e for e, c in edge_faces.items() if c == 1 and e[0] in members]
            loops = _boundary_loops(boundary)
            if not loops:
                raise TubeError("it is closed (capped): delete its cap faces so its ends are open")
            if len(loops) > 2:
                raise TubeError("it has %d open edge loops; a tube has one at each end (or one and a pointed tip)"
                                % len(loops))
            ends = [set(l) for l in loops[1:]]
            rings, pointed = _walk(loops[0], nbrs, ends, len(shell))
            if len(loops) == 2 and pointed:
                raise TubeError("it has an open end and a pointed tip besides its other open end")
            if len(rings) < 2:
                raise TubeError("it is a single edge loop")
        except TubeError as e:
            raise TubeError("%s: %s" % (label, e))
        tubes.append(Tube([[tuple(points[v]) for v in r] for r in rings], name=label, pointed=pointed,
                          smooth_rings=[[tuple(smooth_points[v]) for v in r] for r in rings] if smooth else None))
    return tubes


def orient(tube, root_distance=None):
    """Put the tube's root first: the end nearer the scalp (`root_distance(point)`: a point's
    distance to it), else the wider end (an open end before a pointed one)."""
    if tube.pointed and root_distance is None:
        return tube
    if root_distance is not None:
        a, b = section_center(tube.rings[0]), section_center(tube.rings[-1])
        if root_distance(b) < root_distance(a):
            tube.reverse()
        return tube
    root, tip = tube.end_sizes()
    if tip > root:
        tube.reverse()
    return tube


# ---------------------------------------------------------------------------
# Fitting GroomLab sections (tube.h): a section puts shape point (u, v) at
# center + rotate(twist) * (u * width, v * width * aspect) in the section's axes.


def signed_area(points2d):
    n = len(points2d)
    return 0.5 * sum(points2d[i][0] * points2d[(i + 1) % n][1] - points2d[(i + 1) % n][0] * points2d[i][1]
                     for i in range(n))


def _regress(xs, ys):
    """Least squares y = slope * x + offset (slope 0 or more); (slope, offset, squared error)."""
    n = float(len(xs))
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = max(sxy / sxx, 0.0) if sxx > 1e-18 else 0.0
    offset = my - slope * mx
    return slope, offset, sum((slope * x + offset - y) ** 2 for x, y in zip(xs, ys))


class _Fit(object):
    """What fitting a shape to points at any turn needs, summed once: per axis the shape's
    centered coordinates and their square sum, and the points' sums (so each turn is a few
    sums: _fit_at)."""

    def __init__(self, points2d, shape):
        n = float(len(points2d))
        self.n = n
        mu = sum(p[0] for p in shape) / n
        mv = sum(p[1] for p in shape) / n
        self.du = [p[0] - mu for p in shape]
        self.dv = [p[1] - mv for p in shape]
        self.suu = sum(d * d for d in self.du)
        self.svv = sum(d * d for d in self.dv)
        self.mu, self.mv = mu, mv
        self.points = points2d
        self.sx = sum(p[0] for p in points2d)
        self.sy = sum(p[1] for p in points2d)
        self.sq = sum(p[0] * p[0] + p[1] * p[1] for p in points2d)  # (a turn keeps it)
        # The points' sums against the centered shape, for the turn's terms.
        self.xu = sum(p[0] * d for p, d in zip(points2d, self.du))
        self.yu = sum(p[1] * d for p, d in zip(points2d, self.du))
        self.xv = sum(p[0] * d for p, d in zip(points2d, self.dv))
        self.yv = sum(p[1] * d for p, d in zip(points2d, self.dv))
        self.sxx = sum(p[0] * p[0] for p in points2d)
        self.syy = sum(p[1] * p[1] for p in points2d)
        self.sxy = sum(p[0] * p[1] for p in points2d)


def _fit_at(angle, fit):
    """The points turned by -angle, each axis a least squares line on the shape's (_regress, the
    same numbers): (squared error, width, height, offset x, offset y)."""
    c, s = math.cos(-angle), math.sin(-angle)
    n = fit.n
    mqx = (fit.sx * c - fit.sy * s) / n
    mqy = (fit.sx * s + fit.sy * c) / n
    # sum((q - mean q)^2) per axis, from the turned second moments.
    qxx = c * c * fit.sxx - 2.0 * c * s * fit.sxy + s * s * fit.syy - n * mqx * mqx
    qyy = s * s * fit.sxx + 2.0 * c * s * fit.sxy + c * c * fit.syy - n * mqy * mqy
    cxu = c * fit.xu - s * fit.yu  # sum(du * qx)
    cyv = s * fit.xv + c * fit.yv  # sum(dv * qy)
    w = max(cxu / fit.suu, 0.0) if fit.suu > 1e-18 else 0.0
    h = max(cyv / fit.svv, 0.0) if fit.svv > 1e-18 else 0.0
    ex = max(qxx - w * cxu, 0.0)
    ey = max(qyy - h * cyv, 0.0)
    return ex + ey, w, h, mqx - w * fit.mu, mqy - h * fit.mv


def _closest_on_outline(q, placed, outline):
    """The point of closed polygon `placed` nearest q, as the matching point of `outline` (the
    same polygon in shape coordinates), and its distance."""
    best = None
    n = len(placed)
    for j in range(n):
        a, b = placed[j], placed[(j + 1) % n]
        ex, ey = b[0] - a[0], b[1] - a[1]
        length = ex * ex + ey * ey
        f = max(0.0, min(1.0, ((q[0] - a[0]) * ex + (q[1] - a[1]) * ey) / length)) if length > 1e-24 else 0.0
        d = math.hypot(a[0] + ex * f - q[0], a[1] + ey * f - q[1])
        if best is None or d < best[0]:
            oa, ob = outline[j], outline[(j + 1) % n]
            best = (d, (oa[0] + (ob[0] - oa[0]) * f, oa[1] + (ob[1] - oa[1]) * f))
    return best[1], best[0]


def plane_section(columns, origin, normal):
    """Where a tube's surface crosses a plane (through `origin`, square to `normal`): for each of
    its `columns` (point lists along the tube, root to tip), the crossing nearest `origin` (the
    column's point nearest the plane when it does not cross it). The section the tube draws there,
    one point per column, in the columns' order."""
    out = []
    for col in columns:
        ds = [_dot(_sub(p, origin), normal) for p in col]
        best = None
        for k in range(len(col) - 1):
            a, b = ds[k], ds[k + 1]
            if (a <= 0.0 <= b) or (b <= 0.0 <= a):
                f = a / (a - b) if a != b else 0.0
                q = lerp(col[k], col[k + 1], f)
                d = _dist(q, origin)
                if best is None or d < best[0]:
                    best = (d, q)
        if best is None:
            k = min(range(len(col)), key=lambda i: abs(ds[i]))
            best = (0.0, col[k])
        out.append(best[1])
    return out


def ray_reach(polygon, angle):
    """How far a closed polygon (2D, round the origin) reaches from the origin in direction `angle`:
    the farthest crossing of that ray with it (None when the ray misses it)."""
    dx, dy = math.cos(angle), math.sin(angle)
    best = None
    n = len(polygon)
    for i in range(n):
        ax, ay = polygon[i]
        bx, by = polygon[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        den = dx * ey - dy * ex
        if abs(den) < 1e-15:
            continue
        s = (ax * ey - ay * ex) / den  # along the ray
        f = (ax * dy - ay * dx) / den  # along the edge
        if s > 0.0 and -1e-9 <= f <= 1.0 + 1e-9 and (best is None or s > best):
            best = s
    return best


COVER_DIRECTIONS = 64


def cover_scale(section, outline, uncovered=0.05, limits=(1.0, 1.35)):
    """How much wider (both ways) `outline` must be drawn so that at most `uncovered` of `section`'s
    area lies outside it: both closed polygons round the origin, in the same plane. Measured
    direction by direction (COVER_DIRECTIONS, both star-shaped from the origin): the section's
    area outside the outline drawn k times as large is sum(max(0, r^2 - k^2)) over sum(r^2), r
    the section's reach over the outline's. Within `limits` (never smaller: the volume holds; a
    shape very unlike the section is not blown up past the upper one)."""
    ratios = []
    for j in range(COVER_DIRECTIONS):
        a = 2.0 * math.pi * j / COVER_DIRECTIONS
        there, drawn = ray_reach(section, a), ray_reach(outline, a)
        if there and drawn:
            ratios.append(there / drawn)
    if not ratios:
        return limits[0]
    total = sum(r * r for r in ratios)
    lo, hi = limits
    if sum(max(0.0, r * r - lo * lo) for r in ratios) <= uncovered * total:
        return lo
    for _ in range(30):
        k = 0.5 * (lo + hi)
        if sum(max(0.0, r * r - k * k) for r in ratios) > uncovered * total:
            lo = k
        else:
            hi = k
    return hi


def section_box(points2d, twist):
    """The section turned `twist` that just holds `points2d` (a loop as drawn, in the section's
    axes): (width, height, center), each half the loop's extent along the turned axes, the
    center that extent's middle. A shape fills its -1..1 box, so the section spans the loop."""
    c, s = math.cos(twist), math.sin(twist)
    xs = [x * c + y * s for x, y in points2d]
    ys = [-x * s + y * c for x, y in points2d]
    mx, my = 0.5 * (max(xs) + min(xs)), 0.5 * (max(ys) + min(ys))
    return 0.5 * (max(xs) - min(xs)), 0.5 * (max(ys) - min(ys)), (mx * c - my * s, mx * s + my * c)


def place_outline(outline, twist, width, height, center):
    c, s = math.cos(twist), math.sin(twist)
    return [(center[0] + u * width * c - v * height * s, center[1] + u * width * s + v * height * c) for u, v in outline]


def fit_section(points2d, shape, near=None):
    """The section that best puts `shape`'s points (u, v) on `points2d` (each its own, in order),
    points in the section's axes at twist 0. Returns (twist, width, height, (cx, cy)): its center
    off the axes' origin (in the axes). `near`: the twist to stay closest to (the last section's),
    for the same fit a turn away. A section with no size (a pointed tip) keeps `near`."""
    spread = max(math.hypot(x, y) for x, y in points2d) if points2d else 0.0
    mx = sum(p[0] for p in points2d) / float(len(points2d))
    my = sum(p[1] for p in points2d) / float(len(points2d))
    if max(math.hypot(x - mx, y - my) for x, y in points2d) < 1e-9 * max(spread, 1.0):
        return (near or 0.0), 0.0, 0.0, (mx, my)
    fit = _Fit(points2d, shape)
    steps = 120
    step = 2.0 * math.pi / steps
    angles = [step * i for i in range(steps)]
    best = None
    for a in angles:
        f = _fit_at(a, fit)
        if best is None or f[0] < best[1][0]:
            best = (a, f)
    lo, hi = best[0] - step, best[0] + step
    golden = 0.5 * (math.sqrt(5.0) - 1.0)
    for _ in range(30):  # golden section search round the best step
        a1, a2 = hi - golden * (hi - lo), lo + golden * (hi - lo)
        if _fit_at(a1, fit)[0] < _fit_at(a2, fit)[0]:
            hi = a2
        else:
            lo = a1
    angle = 0.5 * (lo + hi)
    _err, w, h, ox, oy = _fit_at(angle, fit)
    if near is not None:
        angle += 2.0 * math.pi * round((near - angle) / (2.0 * math.pi))
    c, s = math.cos(angle), math.sin(angle)
    return angle, w, h, (ox * c - oy * s, ox * s + oy * c)


def shape_points(points2d, limit=MAX_SHAPE_POINTS):
    """A section's points as a tube shape's control points: about their center, counter-clockwise,
    at most `limit` (evenly picked). Returns (points, indices: which of points2d each is)."""
    n = len(points2d)
    indices = list(range(n))
    if n > limit:
        indices = sorted(set(int(round(i * n / float(limit))) % n for i in range(limit)))
    pts = [points2d[i] for i in indices]
    if signed_area(pts) < 0.0:
        indices.reverse()
        pts.reverse()
    return pts, indices


def match_fitted(given, fitted):
    """How GroomLab fitted a shape's points (given) into its square (fitted, maybe reordered):
    for each given point, its fitted point. None when they do not match point for point."""
    n = len(given)
    if len(fitted) != n:
        return None
    best = None
    for order in (fitted, list(reversed(fitted))):
        for shift in range(n):
            cand = order[shift:] + order[:shift]
            err = 0.0
            for axis in (0, 1):
                _slope, _off, e = _regress([p[axis] for p in given], [p[axis] for p in cand])
                err += e
            if best is None or err < best[0]:
                best = (err, cand)
    return best[1] if best and best[0] < 1e-4 * n else None


def resample(ts, values, count):
    """`values` at params `ts` (rising, 0 to 1) as `count` values evenly spaced 0 to 1 (linear)."""
    out = []
    for j in range(count):
        t = j / float(count - 1) if count > 1 else 0.0
        if t <= ts[0]:
            out.append(values[0])
            continue
        if t >= ts[-1]:
            out.append(values[-1])
            continue
        k = 0
        while ts[k + 1] < t:
            k += 1
        span = ts[k + 1] - ts[k]
        f = (t - ts[k]) / span if span > 1e-12 else 0.0
        out.append(values[k] + (values[k + 1] - values[k]) * f)
    return out


def polyline_param(points, p):
    """Where on a polyline (points evenly spaced) p lies nearest: its param 0 to 1 by point index."""
    n = len(points)
    best, param = None, 0.0
    for i in range(n - 1):
        a, b = points[i], points[i + 1]
        ab = _sub(b, a)
        span = _dot(ab, ab)
        f = min(max(_dot(_sub(p, a), ab) / span, 0.0), 1.0) if span > 1e-18 else 0.0
        d = _dist(p, _add(a, _mul(ab, f)))
        if best is None or d < best:
            best, param = d, (i + f) / float(n - 1)
    return param


def frame_at(frames, t):
    """A guide's section axes (tubeFrames: points, u, v) at param t: (center, u, v), u and v unit
    and square."""
    pts = frames["points"]
    n = len(pts)
    x = min(max(t, 0.0), 1.0) * (n - 1)
    i = min(int(x), n - 2)
    f = x - i
    center = lerp(pts[i], pts[i + 1], f)
    u = _unit(lerp(frames["u"][i], frames["u"][i + 1], f))
    v = lerp(frames["v"][i], frames["v"][i + 1], f)
    v = _unit(_sub(v, _mul(u, _dot(v, u))))
    return center, u, v


def to_plane(point, center, u, v):
    d = _sub(point, center)
    return (_dot(d, u), _dot(d, v))


def from_plane(xy, center, u, v):
    return _add(center, _add(_mul(u, xy[0]), _mul(v, xy[1])))
