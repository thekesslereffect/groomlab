# SPDX-License-Identifier: Apache-2.0
"""Modeled hair tubes to curves and to GroomLab tubes (GroomLab > Utilities, issue #41): open quad
tubes read loop by loop (a pointed tip, bevels averaged away, shells of one mesh apart, meshes that
are not tubes said), their shape and center curves in the scene or as guides in their own
groups, and tube guides whose sections fit the modeled tube (a twisted, tapering, flattened tube).

Usage: mayapy test_tube_mesh.py <GroomLab.mll> <maya/scripts dir>
"""

import math
import os
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402
from maya.api import OpenMaya as om  # noqa: E402

import groomlab  # noqa: E402
from groomlab import tube_mesh as tm  # noqa: E402
from groomlab import utils  # noqa: E402


def _norm(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def tube_rings(root=(0.0, 1.0, 0.0), length=1.2, loops=10, sides=8, half_width=0.15, aspect=0.5, taper=0.3,
               twist=math.pi / 2, bevel_loop=None, bevel_run=None):
    """A modeled tube's rings: from `root` along a gentle bend, an ellipse (half_width, aspect)
    tapering to `taper` of itself and turning `twist` root to tip. bevel_loop k: a second loop just
    past loop k; bevel_run i: a second run just past run i (a crease's bevel)."""
    up = _norm(root)
    side = _norm(_cross(up, (0.0, 0.0, 1.0)) if abs(up[2]) < 0.9 else _cross(up, (1.0, 0.0, 0.0)))
    angles = [2.0 * math.pi * i / sides for i in range(sides)]
    if bevel_run is not None:
        step = 2.0 * math.pi / sides
        angles.insert(bevel_run + 1, angles[bevel_run] + 0.08 * step)
    ts = [k / float(loops - 1) for k in range(loops)]
    if bevel_loop is not None:
        ts.insert(bevel_loop + 1, ts[bevel_loop] + 0.03 / (loops - 1))
    rings = []
    for t in ts:
        center = tuple(r + u * length * t + s * 0.25 * t * t for r, u, s in zip(root, up, side))
        tangent = _norm(tuple(u * length + s * 0.5 * t for u, s in zip(up, side)))
        a = _norm(_cross(tangent, _cross(side, tangent)))
        b = _cross(tangent, a)
        w = half_width * (1.0 + (taper - 1.0) * t)
        h = w * aspect
        turn = twist * t
        ring = []
        for ang in angles:
            x, y = w * math.cos(ang), h * math.sin(ang)
            x, y = x * math.cos(turn) - y * math.sin(turn), x * math.sin(turn) + y * math.cos(turn)
            ring.append(tuple(c + ax * x + bx * y for c, ax, bx in zip(center, a, b)))
        rings.append(ring)
    return rings


def grid(rings, pointed=False):
    """Points and quads of an open tube through `rings` (the last closing on its tip with `pointed`)."""
    sides = len(rings[0])
    points = [p for r in rings for p in r]
    polys = []
    for k in range(len(rings) - 1):
        for i in range(sides):
            j = (i + 1) % sides
            polys.append([k * sides + i, k * sides + j, (k + 1) * sides + j, (k + 1) * sides + i])
    if pointed:
        tip = tuple(sum(c) / sides for c in zip(*rings[-1]))
        points.append(tuple(c + 0.1 * d for c, d in zip(tip, _norm(tuple(a - b for a, b in zip(tip, tm.section_center(rings[0])))))))
        last = (len(rings) - 1) * sides
        for i in range(sides):
            polys.append([last + i, last + (i + 1) % sides, len(points) - 1])
    return points, polys


def _outward(p, tube):
    """How far out from the tube's middle `p` is, as a share of the way to its surface there
    (the section nearest along the center line; 1: on the surface, above 1: outside)."""
    centers = tube.centers()
    best = None
    for k in range(len(centers) - 1):
        a, b = centers[k], centers[k + 1]
        ab = tm._sub(b, a)
        span = tm._dot(ab, ab)
        f = max(0.0, min(1.0, tm._dot(tm._sub(p, a), ab) / span)) if span > 0 else 0.0
        d = tm._dist(p, tm.lerp(a, b, f))
        if best is None or d < best[0]:
            best = (d, k, f)
    _, k, f = best
    ring = [tm.lerp(x, y, f) for x, y in zip(tube.rings[k], tube.rings[k + 1])]
    c = tm.section_center(ring)
    v = tm._sub(p, c)
    reach = max(tm._dot(tm._sub(r, c), tm._unit(v)) for r in ring)
    return tm._length(v) / reach if reach > 1e-9 else 0.0


def make_mesh(name, points, polys):
    fn = om.MFnMesh()
    fn.create([om.MPoint(*p) for p in points], [len(p) for p in polys], [v for p in polys for v in p])
    transform = om.MFnDependencyNode(fn.parent(0)).name()
    cmds.sets(transform, edit=True, forceElement="initialShadingGroup")
    return cmds.rename(transform, name)


class TubeReadingTest(unittest.TestCase):
    """tube_mesh alone: plain points and polygons."""

    def test_open_tube(self):
        points, polys = grid(tube_rings(loops=6, sides=8))
        (tube,) = tm.read_tubes(points, polys, name="lock")
        self.assertEqual((len(tube.rings), tube.sides, tube.pointed), (6, 8, False))
        self.assertEqual(len(tube.columns()), 8)
        self.assertEqual(len(tube.columns()[0]), 6)

    def test_pointed_tip_and_root_at_the_wide_end(self):
        points, polys = grid(tube_rings(loops=6, sides=6), pointed=True)
        (tube,) = tm.read_tubes(points, polys)
        self.assertTrue(tube.pointed)
        self.assertEqual(len(tube.rings), 7)
        self.assertEqual(len(set(tube.rings[-1])), 1)
        tm.orient(tube)
        self.assertEqual(len(set(tube.rings[0])), 6)  # the open end stays the root

    def test_root_is_the_wider_end_without_a_scalp(self):
        rings = tube_rings(loops=5, taper=0.3)
        points, polys = grid(list(reversed(rings)))  # modeled tip first
        (tube,) = tm.read_tubes(points, polys)
        tm.orient(tube)
        root, tip = tube.end_sizes()
        self.assertGreater(root, tip)

    def test_bevels_average_away(self):
        points, polys = grid(tube_rings(loops=8, sides=8, bevel_loop=3, bevel_run=2))
        (tube,) = tm.read_tubes(points, polys)
        self.assertEqual((len(tube.rings), tube.sides), (9, 9))
        self.assertEqual(tube.merge_bevels(), (1, 1))
        self.assertEqual((len(tube.rings), tube.sides), (8, 8))
        # A tapering tube's loops drawing closer toward its tip are no bevel.
        points, polys = grid(tube_rings(loops=8, sides=8))
        (plain,) = tm.read_tubes(points, polys)
        self.assertEqual(plain.merge_bevels(), (0, 0))

    def test_shells_and_not_tubes(self):
        a, pa = grid(tube_rings(root=(0.0, 1.0, 0.0), loops=4))
        b, pb = grid(tube_rings(root=(1.0, 0.0, 0.0), loops=5))
        tubes = tm.read_tubes(a + b, pa + [[v + len(a) for v in p] for p in pb], name="locks")
        self.assertEqual([t.name for t in tubes], ["locks_1", "locks_2"])
        self.assertEqual(sorted(len(t.rings) for t in tubes), [4, 5])
        # A capped tube: said, with what to do.
        rings = tube_rings(loops=4, sides=4)
        points, polys = grid(rings)
        capped = polys + [[0, 1, 2, 3][::-1], [12, 13, 14, 15]]
        with self.assertRaisesRegex(tm.TubeError, "closed"):
            tm.read_tubes(points, capped, name="capped")

    def test_cover_scale(self):
        # A round section on a square outline of the same area pokes out at the square's sides: the
        # square drawn wider until at most 5% of the circle lies outside (about 1.10; all of it
        # inside takes 1.13, the circle's radius over the square's half side). A shape matching
        # the section: nothing to add.
        circle = [(math.cos(a), math.sin(a)) for a in [2 * math.pi * i / 96 for i in range(96)]]
        half = math.sqrt(math.pi) / 2.0
        square = [(-half, -half), (half, -half), (half, half), (-half, half)]
        k5 = tm.cover_scale(circle, square, 0.05)
        k0 = tm.cover_scale(circle, square, 0.0)
        self.assertGreater(k5, 1.03)
        self.assertLess(k5, k0)
        self.assertAlmostEqual(k0, 1.0 / half, delta=0.01)
        self.assertAlmostEqual(tm.cover_scale(circle, circle, 0.05), 1.0, places=6)
        self.assertEqual(tm.cover_scale(circle, [(x * 0.1, y * 0.1) for x, y in circle], 0.05), 1.35)  # (the cap)

    def test_section_fit(self):
        shape = [(math.cos(a), 0.5 * math.sin(a)) for a in [2 * math.pi * i / 8 for i in range(8)]]
        turn, w, h = 0.6, 0.3, 0.2
        pts = [(0.1 + w * u * math.cos(turn) - h * v * math.sin(turn), -0.05 + w * u * math.sin(turn) + h * v * math.cos(turn))
               for u, v in shape]
        twist, fw, fh, c = tm.fit_section(pts, shape, near=0.5)
        self.assertAlmostEqual(twist, turn, places=3)
        self.assertAlmostEqual(fw, w, places=4)
        self.assertAlmostEqual(fh, h, places=4)
        self.assertAlmostEqual(c[0], 0.1, places=4)
        self.assertAlmostEqual(c[1], -0.05, places=4)


class TubesInMayaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20, name="head")[0]
        self.rings = tube_rings(root=(0.0, 0.98, 0.0), bevel_loop=4)
        self.lock = make_mesh("lock", *grid(self.rings, pointed=True))
        side = tube_rings(root=(0.6, 0.78, 0.0), loops=7, sides=6, aspect=1.0, twist=0.0)
        self.side = make_mesh("sideLock", *grid(list(reversed(side))))  # modeled tip first

    def test_curves_in_the_scene(self):
        result = utils.tubes_to_curves([self.lock, self.side])
        self.assertEqual(result["tubes"], 2)
        self.assertEqual(len(result["shape"]), 8 + 6)
        self.assertEqual(len(result["centers"]), 2)
        self.assertTrue(cmds.objExists(result["group"]))
        # The center curves together in a group of their own.
        self.assertTrue(all("|tubeCenterCurves|" in c for c in result["centers"]))
        # A shape curve: a point per loop (the bevel's two as one, the tip's point), root first.
        lock_curves = [c for c in result["shape"] if "|lock_curves|" in c]
        ep = cmds.ls(lock_curves[0] + ".ep[*]", flatten=True)
        self.assertEqual(len(ep), len(self.rings) - 1 + 1)  # 11 loops, the bevel merged, the tip added
        first = cmds.pointPosition(ep[0])
        self.assertLess(abs(math.sqrt(sum(c * c for c in first)) - 1.0), 0.2)  # on the head
        side_center = [c for c in result["centers"] if "sideLock" in c][0]
        root = cmds.pointPosition(cmds.ls(side_center + ".ep[0]", flatten=True)[0])
        self.assertLess(math.sqrt(sum(c * c for c in root)), 1.05)  # the wider end first
        cmds.undo()
        self.assertFalse(cmds.objExists(result["group"]))

    def test_no_bevel_merge(self):
        result = utils.tubes_to_curves([self.lock], centers=False, ignore_bevels=False)
        self.assertEqual(result["centers"], [])
        ep = cmds.ls(result["shape"][0] + ".ep[*]", flatten=True)
        self.assertEqual(len(ep), len(self.rings) + 1)

    def test_curves_into_the_groom(self):
        groom = groomlab.create_groom(self.scalp)
        result = utils.tubes_to_curves([self.lock, self.side], groom=groom)
        self.assertIsNone(result["group"])
        self.assertFalse(cmds.ls("tubeCurves*"))

        def counts():
            return {g["name"]: g["count"] for g in groomlab.list_guide_groups(groom)}
        self.assertEqual(counts().get("tubesShape"), 14)
        self.assertEqual(counts().get("tubesCenter"), 2)
        # A second conversion: new groups, numbered after the first.
        again = utils.tubes_to_curves([self.lock], groom=groom)
        self.assertEqual(again["groups"], {"shape": "tubesShape2", "centers": "tubesCenter2"})
        # Into the existing groups: added to what they hold.
        more = utils.tubes_to_curves([self.lock], groom=groom, existing_groups=True)
        self.assertEqual(more["groups"], {"shape": "tubesShape", "centers": "tubesCenter"})
        self.assertEqual((counts()["tubesShape"], counts()["tubesCenter"]), (22, 3))
        # Centers not in a group of their own: with the shape curves.
        last = utils.tubes_to_curves([self.lock], groom=groom, centers_group=False)
        self.assertEqual(last["groups"], {"shape": "tubesShape3", "centers": "tubesShape3"})
        self.assertEqual(counts()["tubesShape3"], 8 + 1)
        self.assertNotIn("tubesCenter3", counts())
        # Center curves only: no shape guides, no new tubesShape group.
        before = set(counts())
        only = utils.tubes_to_curves([self.lock, self.side], groom=groom, shapes=False, existing_groups=True)
        self.assertEqual((only["shape"], len(only["centers"])), ([], 2))
        self.assertEqual(only["groups"], {"shape": None, "centers": "tubesCenter"})
        self.assertEqual(set(counts()) - before, set())
        self.assertEqual(counts()["tubesCenter"], 5)

    def test_each_tubes_hair_from_its_own_guides(self):
        # Into a groom, the tubes' guides get a guide map of a value a tube, which the Interpolate
        # takes as its regions: every strand in the region of the tube rooted nearest it.
        groom = groomlab.create_groom(self.scalp)
        result = utils.tubes_to_curves([self.lock, self.side], groom=groom)
        self.assertEqual(result["regionMap"], "tubes")
        self.assertEqual(groomlab.map_kind(groom, "tubes"), "guides")
        interpolates = [n["name"] for n in groomlab.get_graph(groom)["nodes"] if n["type"] == "Interpolate"]
        self.assertTrue(interpolates)
        for n in groomlab.get_graph(groom)["nodes"]:
            if n["type"] == "Interpolate":
                self.assertEqual(n["params"].get("regionMap"), "tubes")
        made = utils.strands_to_curves(groom, node=interpolates[0], max_curves=100000, attributes=True)
        regions = set()
        for t in made["curves"]:
            shape = cmds.listRelatives(t, shapes=True)[0]
            regions.add(cmds.getAttr(shape + ".glRegion"))
        cmds.delete(made["group"])
        self.assertEqual(len(regions), 2)  # one a tube, no strand without one
        self.assertNotIn(-1.0, regions)
        # Another conversion adds its tube to the map the Interpolate reads, at a value of its own
        # (issue #60: conversions add, never take the earlier tubes' regions).
        before = utils._region_values_in_use(groom, "tubes")
        again = utils.tubes_to_curves([self.lock], groom=groom)
        self.assertEqual(again["regionMap"], "tubes")
        self.assertNotIn("tubes2", groomlab.list_maps(groom))
        self.assertEqual(next(n for n in groomlab.get_graph(groom)["nodes"] if n["type"] == "Interpolate")["params"]["regionMap"],
                         "tubes")
        after = utils._region_values_in_use(groom, "tubes")
        self.assertEqual(len(after), 3)
        self.assertTrue(set(before) <= set(after))
        # Off: no map.
        self.assertIsNone(utils.tubes_to_curves([self.lock], groom=groom, region_map=False)["regionMap"])
        # The values: distinct, neighbouring tubes far apart.
        values = utils.tube_region_values(7)
        self.assertEqual(len(set(values)), 7)
        self.assertTrue(all(abs(a - b) > 0.2 for a, b in zip(values, values[1:])))

    def test_center_curves_only_in_the_scene(self):
        result = utils.tubes_to_curves([self.lock, self.side], shapes=False)
        self.assertEqual((result["shape"], len(result["centers"])), ([], 2))
        self.assertFalse(cmds.ls("*_curves", type="transform"))
        with self.assertRaises(ValueError):
            utils.tubes_to_curves([self.lock], shapes=False, centers=False)

    def test_center_volume(self):
        # The layout: the center curve plus count-1 others on rings, none past VOLUME_REACH.
        self.assertEqual(tm.volume_layout(1), [])
        self.assertEqual([round(t, 3) for _, t in tm.volume_layout(4)], [0.0, 0.333, 0.667])
        for count in (2, 4, 7, 10, 20, 50):
            layout = tm.volume_layout(count)
            self.assertEqual(len(layout), count - 1)
            self.assertEqual(len(set((round(r, 6), round(t % 1.0, 6)) for r, t in layout)), count - 1)
            self.assertTrue(all(0.0 < r <= tm.volume_reach(count) + 1e-9 for r, _ in layout))
        # A few curves stay near the center; more spread out, never past VOLUME_REACH.
        self.assertAlmostEqual(tm.volume_reach(4), 0.6)
        self.assertLess(tm.volume_reach(4), tm.volume_reach(10))
        self.assertEqual(tm.volume_reach(100), tm.VOLUME_REACH)
        # In the scene: each curve stays inside the tube, section by section.
        result = utils.tubes_to_curves([self.lock], shapes=False, center_count=10)
        self.assertEqual(len(result["centers"]), 10)
        self.assertTrue(result["centers"][1].endswith("lock_center2"))
        tube = utils._read_tubes([self.lock], None, True)[0][0]
        for curve in tube.volume_curves(10):
            for k, (p, ring) in enumerate(zip(curve, tube.rings)):
                c = tm.section_center(ring)
                self.assertLessEqual(tm._dist(p, c), tm.section_radius(ring) * 0.95 + 1e-9, (k, p))

    def test_center_volume_stays_inside_as_guides(self):
        # Made guides, every curve keeps its place in the tube past the root (none moved whole).
        tube = utils._read_tubes([self.lock], None, True)[0][0]
        groom = groomlab.create_groom(self.scalp)
        result = utils.tubes_to_curves([self.lock], groom=groom, center_count=10)
        self.assertEqual(len(result["centers"]), 10)
        for guide in result["centers"] + result["shape"]:
            points = [tuple(p) for p in utils._tube_frames(groom, guide)["points"]]
            # A point per vertex along the tube: 11 loops (the bevel's two as one) and the tip.
            self.assertEqual(len(points), len(self.rings) - 1 + 1, guide)
            self.assertLess(tm._dist(points[0], (0.0, 0.0, 0.0)), 1.01)  # rooted on the head
            if guide in result["shape"]:
                continue  # (on the surface: a smooth curve through the edge loops bows out at a pointed tip)
            past_root = points[len(points) // 3:-1]  # (the pointed tip: no width to be a share of)
            self.assertLess(max(_outward(p, tube) for p in past_root), 1.0, guide)  # inside

    def test_smoothed_surface(self):
        # Each vertex's limit point lies on Maya's own smoothing of the mesh (pointed tip included).
        shape = cmds.listRelatives(self.lock, shapes=True, fullPath=True)[0]
        fn = utils._om_mesh(shape)
        points = [(p.x, p.y, p.z) for p in fn.getPoints(om.MSpace.kWorld)]
        counts, connects = fn.getVertices()
        polygons, at = [], 0
        for c in counts:
            polygons.append(list(connects[at:at + c]))
            at += c
        self.assertFalse(utils.smooth_preview(shape))
        cmds.setAttr(shape + ".displaySmoothMesh", 2)  # (the 3 key)
        self.assertTrue(utils.smooth_preview(shape))
        options = fn.getSmoothMeshDisplayOptions()
        options.divisions = 5
        data = om.MFnMeshData().create()  # (kept: the smooth mesh lives in it)
        smooth = om.MFnMesh(fn.generateSmoothMesh(data, options))
        limit = tm.limit_points(points, polygons)
        for p in limit:
            q = om.MPoint(*p)
            self.assertLess(q.distanceTo(smooth.getClosestPoint(q)[0]), 0.001)
        self.assertGreater(max(tm._dist(a, b) for a, b in zip(points, limit)), 0.01)  # (it did move)
        # The option: on, the curves go through the limit points; off, through the vertices.
        on = utils.tubes_to_curves([self.lock], centers=False, ignore_bevels=False)
        off = utils.tubes_to_curves([self.lock], centers=False, ignore_bevels=False, smooth=False)
        first_on = cmds.pointPosition(cmds.ls(on["shape"][0] + ".ep[1]", flatten=True)[0])
        first_off = cmds.pointPosition(cmds.ls(off["shape"][0] + ".ep[1]", flatten=True)[0])
        self.assertLess(min(tm._dist(tuple(first_on), p) for p in limit), 1e-4)
        self.assertLess(min(tm._dist(tuple(first_off), p) for p in points), 1e-4)
        self.assertEqual(len(cmds.ls(on["shape"][0] + ".ep[*]", flatten=True)),
                         len(cmds.ls(off["shape"][0] + ".ep[*]", flatten=True)))  # still a point per vertex
        # Bevels are told apart as modeled: merged on the smooth surface too (11 loops, the tip).
        merged = utils.tubes_to_curves([self.lock], centers=False)
        self.assertEqual(len(cmds.ls(merged["shape"][0] + ".ep[*]", flatten=True)), len(self.rings) - 1 + 1)

    def test_center_curves_with_their_tube_in_the_scene(self):
        result = utils.tubes_to_curves([self.lock, self.side], centers_group=False)
        self.assertFalse(cmds.ls("tubeCenterCurves"))
        self.assertTrue(any(c.endswith("|lock_curves|lock_center") for c in result["centers"]))
        self.assertTrue(all(c.split("|")[-1].startswith(("lock_curve", "sideLock_curve")) for c in result["shape"]))

    def test_not_a_tube_is_said(self):
        cube = cmds.polyCube(name="box")[0]
        with self.assertRaisesRegex(ValueError, "box"):
            utils.tubes_to_curves([cube])
        result = utils.tubes_to_curves([cube, self.lock])
        self.assertEqual(result["tubes"], 1)
        self.assertIn("box", result["skipped"])

    def test_groomlab_tubes_padding_progress_and_cancel(self):
        groom = groomlab.create_groom(self.scalp)
        # Width compensation: never thinner than the volume, the root as modeled; this lock keeps
        # its ellipse all along (its sections agree with its shape), so it gains next to nothing.
        plain = utils.tubes_to_groomlab_tubes(groom, [self.lock], uncovered=None, group="plain")
        covered = utils.tubes_to_groomlab_tubes(groom, [self.lock], group="covered")
        a = groomlab.get_tube(groom, plain["guides"][0])["width"]
        b = groomlab.get_tube(groom, covered["guides"][0])["width"]
        self.assertEqual(len(a), len(b))
        for x, y in zip(a[:2], b[:2]):
            self.assertAlmostEqual(y, x, delta=1e-4 + 0.01 * x)  # (the root points)
        for x, y in zip(a[:-1], b[:-1]):
            self.assertGreaterEqual(y, x * 0.99)
            self.assertLess(y, x * 1.08)
        # Progress: steps of one count, rising to the total, each said.
        seen = []
        utils.tubes_to_groomlab_tubes(groom, [self.lock, self.side], group="watched",
                                      progress=lambda done, total, text: seen.append((done, total, text)))
        self.assertEqual(seen[0][0], 0)
        self.assertEqual(seen[-1][0], seen[-1][1])
        self.assertEqual(seen[-1][1], 1 + 2 * (2 + utils.FIT_PASSES))
        self.assertTrue(all(b2[0] == a2[0] + 1 for a2, b2 in zip(seen[1:], seen[2:])))
        self.assertTrue(all(text for _d, _t, text in seen))
        # Cancelled partway: Cancelled says something was made; one undo takes it back.
        before = [g["name"] for g in groomlab.list_guide_groups(groom)]
        with self.assertRaises(utils.Cancelled) as stopped:
            utils.tubes_to_groomlab_tubes(groom, [self.lock, self.side], group="stopped",
                                          progress=lambda done, total, text: done < 4)
        self.assertTrue(stopped.exception.made)
        cmds.undo()
        self.assertEqual([g["name"] for g in groomlab.list_guide_groups(groom)], before)
        with self.assertRaises(utils.Cancelled) as early:
            utils.tubes_to_groomlab_tubes(groom, [self.lock], group="early", progress=lambda *a: False)
        self.assertFalse(early.exception.made)

    def test_groomlab_tubes_fit_the_mesh(self):
        groom = groomlab.create_groom(self.scalp)
        result = utils.tubes_to_groomlab_tubes(groom, [self.lock, self.side], uncovered=None)  # (as fitted)
        self.assertEqual(result["group"], "tubes")
        self.assertEqual(len(result["guides"]), 2)
        self.assertEqual(set(result["shapes"]), {"lock", "sideLock"})
        # The lock: its tube's sections where its loops are, each within a few percent of its width.
        guide = result["guides"][0]
        tube = groomlab.get_tube(groom, guide)
        self.assertEqual(tube["shape"], "lock")
        frames = utils._tube_frames(groom, guide, evaluated=True)  # (as drawn: its roll on the head)
        shape = [s for s in groomlab.tube_shapes(groom) if s["name"] == "lock"][0]
        # By default as smooth mesh preview draws it: the shape the smooth curve round the root loop.
        self.assertTrue(shape["smooth"])
        self.assertEqual(len(shape["points"]), 8)  # a point per root vertex
        (modeled,) = tm.read_tubes(*grid(self.rings, pointed=True), smooth=True)
        modeled.merge_bevels()
        modeled.use_smooth()
        def along(values, t):  # a tube list at t (evenly spaced root to tip, linear between)
            values = values if len(values) > 1 else values * 2
            x = t * (len(values) - 1)
            i = min(int(x), len(values) - 2)
            return values[i] + (values[i + 1] - values[i]) * (x - i)

        worst = 0.0
        # Every loop before the pointed tip: each modeled point on the tube's outline there.
        for ring in modeled.rings[:-1]:
            t = tm.polyline_param(frames["points"], tm.section_center(ring))
            center, u, v = tm.frame_at(frames, t)
            w, a, tw = along(tube["width"], t), along(tube["aspect"], t), along(tube["twist"], t)
            placed = tm.place_outline(shape["points"], tw, w, w * a, (0.0, 0.0))
            size = tm.section_radius(ring)
            for p in ring:
                near = tm._closest_on_outline(tm.to_plane(p, center, u, v), placed, shape["points"])[1]
                worst = max(worst, near / size)
        self.assertLess(worst, 0.08)
        # The root: on the head, leaving it square (so its section lies on the head as the loop does).
        root_point, second = frames["points"][0], frames["points"][1]
        self.assertAlmostEqual(math.sqrt(sum(c * c for c in root_point)), 1.0, delta=0.005)
        out = tm._unit(tm._sub(tuple(second), tuple(root_point)))
        self.assertGreater(tm._dot(out, tm._unit(tuple(root_point))), 0.99)
        # Untwisted at the root (the shape is the root section as it lies on the head), then the
        # quarter turn the lock was modeled with (or the same outline a half turn on: an ellipse);
        # flat, as modeled (one side half the other).
        self.assertLess(abs(tube["twist"][0]), 0.05)
        turn = abs(tube["twist"][-1] - tube["twist"][0]) % math.pi
        self.assertAlmostEqual(min(turn, math.pi - turn), math.pi / 2, delta=0.1)
        root = (tube["width"][0], tube["width"][0] * tube["aspect"][0])
        # Its wide side (whichever way it lies): the modeled 0.15, a little in on the smooth surface;
        # half as high.
        self.assertTrue(0.12 < max(root) < 0.15, root)
        self.assertAlmostEqual(min(root) / max(root), 0.5, delta=0.05)
        # Not smooth: straight edges through the root loop's vertices, as modeled.
        cage = utils.tubes_to_groomlab_tubes(groom, [self.side], smooth=False, group="cageTubes")
        cage_shape = [s for s in groomlab.tube_shapes(groom) if s["name"] == cage["shapes"][0]][0]
        self.assertFalse(cage_shape["smooth"])
        self.assertEqual(len(cage_shape["points"]), 6)
        cmds.undo()
        # The round side lock: no twist, its own shape.
        side = groomlab.get_tube(groom, result["guides"][1])
        self.assertEqual(side["shape"], "sideLock")
        self.assertLess(max(abs(x) for x in side["twist"]), 0.05)
        cmds.undo()
        self.assertFalse(groomlab.guide_ids(groom, "tubes") if "tubes" in
                         [g["name"] for g in groomlab.list_guide_groups(groom)] else [])

    def test_menu_action(self):
        from groomlab.ui import menu
        groom = groomlab.create_groom(self.scalp)
        cmds.select([self.lock, self.side, groom])
        result = menu.tubes_to_groomlab_tubes_action()
        self.assertEqual(len(result["guides"]), 2)
        self.assertIn("2 tube guides in guide group tubes", menu.tubes_report(result))


def _guide_points(groom, guide):
    xyz = cmds.groomLabGuides(groom, guidePoints=str(guide)) or []
    return [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]


class TubeLiveLinkTest(unittest.TestCase):
    """Issue #60: guides made with the live link keep following their modeled tubes."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        from groomlab import tube_links
        self.links = tube_links
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20, name="head")[0]
        self.lock = make_mesh("lock", *grid(tube_rings(root=(0.0, 0.98, 0.0)), pointed=True))
        side = tube_rings(root=(0.6, 0.78, 0.0), loops=7, sides=6, aspect=1.0, twist=0.0)
        self.side = make_mesh("sideLock", *grid(list(reversed(side))))
        self.groom = groomlab.create_groom(self.scalp)

    def tearDown(self):
        self.links.stop()

    def link_of(self, mesh):
        uuid = cmds.ls(cmds.listRelatives(mesh, shapes=True, fullPath=True), uuid=True)[0]
        return next(l for l in self.links.links(self.groom) if l["mesh"] == uuid)

    def test_off_by_default(self):
        result = utils.tubes_to_curves([self.lock], groom=self.groom)
        self.assertFalse(result["liveLink"])
        self.assertEqual(self.links.links(self.groom), [])

    def test_guides_follow_moves_and_edits(self):
        result = utils.tubes_to_curves([self.lock, self.side], groom=self.groom, live_link=True)
        self.assertTrue(result["liveLink"])
        self.assertEqual(len(self.links.links(self.groom)), 2)
        link = self.link_of(self.lock)
        self.assertEqual(len(link["shape"]), 8)
        self.assertEqual(len(link["centers"]), 1)
        shape_guide, center_guide = link["shape"][0], link["centers"][0]
        tip = _guide_points(self.groom, shape_guide)[-1]
        center_tip = _guide_points(self.groom, center_guide)[-1]
        # Nothing changed: nothing to do.
        self.assertEqual(self.links.sync()["reshaped"], [])

        # The object moved (its transform): the tip goes with it, the root stays on the scalp.
        cmds.move(0.1, 0.0, 0.0, self.lock, relative=True)
        report = self.links.sync()
        self.assertIn(shape_guide, report["reshaped"])
        self.assertIn(center_guide, report["reshaped"])
        moved = _guide_points(self.groom, shape_guide)
        self.assertAlmostEqual(moved[-1][0] - tip[0], 0.1, places=3)
        self.assertLess(abs(math.sqrt(sum(c * c for c in moved[0])) - 1.0), 0.02)  # (rooted on the head)
        side = self.link_of(self.side)
        self.assertFalse(set(report["reshaped"]) & set(side["shape"] + side["centers"]))

        # A component edit: the tip's vertex moved up.
        shape = cmds.listRelatives(self.lock, shapes=True, fullPath=True)[0]
        tip_vertex = cmds.polyEvaluate(shape, vertex=True) - 1
        cmds.move(0.0, 0.2, 0.0, "%s.vtx[%d]" % (shape, tip_vertex), relative=True)
        self.links.sync()
        self.assertGreater(_guide_points(self.groom, center_guide)[-1][1] - center_tip[1], 0.1)

        # Undo the edit and the move: the guides follow the tube back (outside the undo queue).
        cmds.undo()
        cmds.undo()
        self.links.sync()
        back = _guide_points(self.groom, shape_guide)[-1]
        self.assertLess(max(abs(a - b) for a, b in zip(back, tip)), 1e-4)

    def test_watched_meshes_queue_a_sync(self):
        utils.tubes_to_curves([self.lock], groom=self.groom, live_link=True)
        self.links.start()
        uuid = self.link_of(self.lock)["mesh"]
        self.assertIn(uuid, self.links._mesh_jobs)
        self.links._pending.clear()
        cmds.move(0.1, 0.0, 0.0, self.lock, relative=True)
        self.assertIn(uuid, self.links._pending)
        self.links._flush()
        self.assertEqual(self.links._pending, set())

    def test_topology_change_remakes_shape_guides(self):
        utils.tubes_to_curves([self.lock], groom=self.groom, live_link=True)
        link = self.link_of(self.lock)
        old = list(link["shape"])
        value = link["regionValue"]
        self.assertIsNotNone(value)
        # The lock remodeled with 6 sides instead of 8: its shape guides made anew, 6 of them.
        other = make_mesh("remodel", *grid(tube_rings(root=(0.0, 0.98, 0.0), sides=6), pointed=True))
        sel = om.MSelectionList()
        sel.add(cmds.listRelatives(self.lock, shapes=True, fullPath=True)[0])
        sel.add(cmds.listRelatives(other, shapes=True, fullPath=True)[0])
        om.MFnMesh(sel.getDagPath(0)).copyInPlace(sel.getDependNode(1))
        cmds.delete(other)
        report = self.links.sync()
        self.assertEqual(sorted(report["removed"]), sorted(old))
        self.assertEqual(len(report["remade"]), 6)
        link = self.link_of(self.lock)
        self.assertEqual(link["shape"], report["remade"])
        self.assertEqual(link["columnCount"], 6)
        self.assertFalse(set(utils.all_guide_ids(self.groom)) & set(old))
        for guide in groomlab.get_hierarchy(self.groom)["guides"]:
            if str(guide["id"]) in link["shape"]:
                self.assertAlmostEqual(guide["attributes"]["tubes"], value, places=4)

    def test_conversions_add_links(self):
        utils.tubes_to_curves([self.lock], groom=self.groom, live_link=True)
        utils.tubes_to_curves([self.side], groom=self.groom, live_link=True, existing_groups=True)
        found = self.links.links(self.groom)
        self.assertEqual(len(found), 2)
        self.assertEqual(set(l["groups"]["shape"] for l in found), {"tubesShape"})
        self.assertEqual(set(l["regionMap"] for l in found), {"tubes"})
        self.assertEqual(len(set(l["regionValue"] for l in found)), 2)  # one map, a value a tube
        # Undoing the second conversion takes only its link.
        cmds.undo()
        self.assertEqual(len(self.links.links(self.groom)), 1)

    def test_saved_with_the_scene(self):
        utils.tubes_to_curves([self.lock], groom=self.groom, live_link=True)
        guide = self.link_of(self.lock)["shape"][0]
        path = os.path.join(os.environ.get("TEMP", "."), "groomlab_tube_live_link.ma")
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        cmds.file(new=True, force=True)
        cmds.file(path, open=True, force=True)
        try:
            from groomlab.base import GROOM_TYPE
            self.groom = cmds.ls(type=GROOM_TYPE, long=True)[0]
            self.assertEqual(len(self.links.links(self.groom)), 1)
            tip = _guide_points(self.groom, guide)[-1]
            cmds.move(0.0, 0.0, 0.1, "lock", relative=True)
            self.links.sync()
            moved = _guide_points(self.groom, guide)[-1]
            self.assertAlmostEqual(moved[2] - tip[2], 0.1, places=3)
            # Unlinked: the guides stay as they are.
            self.assertEqual(self.links.unlink(self.groom, "lock"), 1)
            cmds.move(0.0, 0.0, 0.1, "lock", relative=True)
            self.links.sync()
            self.assertLess(max(abs(a - b) for a, b in zip(_guide_points(self.groom, guide)[-1], moved)), 1e-5)
        finally:
            cmds.file(new=True, force=True)
            os.remove(path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
