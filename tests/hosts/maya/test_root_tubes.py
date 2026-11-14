# SPDX-License-Identifier: Apache-2.0
"""Tonic-style tubes in the Tube tool (issues 61 and 62): tubes drawn root first (the root's outline
its tube's root section, the guide rooted at its middle, straight off the scalp or along a drawn
path; each tube a region of its own on the tubeRoots map; a root's CVs edited later, snapped onto a
neighbour's) and the tube hierarchy (each level splits the tubes above it in two, side by side,
following them; levels shown one at a time, the deepest removed).

Usage: mayapy test_root_tubes.py <GroomLab.mll> <maya/scripts dir>
"""

import json
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

import groomlab  # noqa: E402
from groomlab import root_tubes  # noqa: E402
from groomlab import tube_mesh as tm  # noqa: E402


def on_sphere(p, radius=1.0):
    n = math.sqrt(sum(c * c for c in p))
    return tuple(c * radius / n for c in p)


def ring(center, radius, count=5, turn=0.0):
    """`count` CVs round `center` (a point on the unit sphere's top half) on the sphere."""
    up = on_sphere(center)
    a = tm._unit(tm._cross(up, (0.0, 0.0, 1.0)))
    b = tm._cross(up, a)
    return [on_sphere(tm._add(up, tm._add(tm._mul(a, radius * math.cos(turn + 2 * math.pi * k / count)),
                                          tm._mul(b, radius * math.sin(turn + 2 * math.pi * k / count)))))
            for k in range(count)]


def nearest(p, points):
    return min(tm._dist(p, q) for q in points)


class RootTubes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=40, name="head")[0]
        self.groom = groomlab.create_groom(self.scalp)

    def points(self, guide):
        return json.loads(cmds.groomLabGuides(self.groom, tubeFrames=str(guide), evaluated=True))["points"]

    def test_root_outline_is_the_tubes_root_section(self):
        outline = ring((0.0, 1.0, 0.0), 0.15)
        guide = root_tubes.make_root_tube(self.groom, outline, length=0.8, segments=8)
        tube = groomlab.get_tube(self.groom, guide)
        self.assertTrue(tube["shape"].startswith(root_tubes.SHAPE_PREFIX))
        self.assertIs(tube.get("rollOnScalp"), False)  # swept from the root: no roll on the scalp
        self.assertAlmostEqual(tube["width"][-1] / tube["width"][0], 0.5, places=3)  # the tip's taper
        back = root_tubes.section_outline(self.groom, guide)
        self.assertEqual(len(back), len(outline))
        for p in outline:
            self.assertLess(nearest(p, back), 0.01, "a CV off the tube's root section")
        kept = root_tubes.root_outline(self.groom, guide)  # (the CVs as drawn, kept on the scalp)
        for p in outline:
            self.assertLess(nearest(p, kept), 0.003)
        pts = self.points(guide)
        self.assertLess(tm._dist(pts[0], on_sphere((0.0, 1.0, 0.0))), 0.02)  # rooted at the middle
        self.assertAlmostEqual(sum(tm._dist(a, b) for a, b in zip(pts, pts[1:])), 0.8, delta=0.05)
        self.assertGreater(pts[-1][1], 1.7)  # straight off the scalp
        self.assertEqual(root_tubes.root_tubes(self.groom), [int(guide)])

    def test_a_drawn_path_shapes_the_tube(self):
        outline = ring((0.3, 1.0, 0.0), 0.12, count=4, turn=0.3)
        root = on_sphere(root_tubes._centroid(outline))
        path = [root, tm._add(root, (0.1, 0.4, 0.0)), tm._add(root, (0.5, 0.6, 0.0)), tm._add(root, (0.9, 0.5, 0.2))]
        guide = root_tubes.make_root_tube(self.groom, outline, path=path, segments=10)
        pts = self.points(guide)
        self.assertLess(tm._dist(pts[-1], path[-1]), 0.05, "the tip where the stroke ended")
        for p in root_tubes.section_outline(self.groom, guide):
            self.assertLess(nearest(p, outline), 0.015)

    def test_a_hand_drawn_path_is_smoothed_and_evenly_spaced(self):
        """A shaky stroke (a smooth arc plus the hand's jitter, many points unevenly spaced): the
        guide follows the arc, its jitter gone, its points evenly spaced, turning one way only."""
        root = on_sphere((0.0, 1.0, 0.0))
        clean, shaky = [], []
        for i in range(90):
            t = (i / 89.0) ** 1.4  # (bunched up at the start, as a slow start of a stroke)
            a = 1.2 * t
            c = (0.0, 1.0 + 0.6 * math.sin(a), 0.6 * (1.0 - math.cos(a)) + 0.4 * t)
            clean.append(c)
            jitter = 0.012 * math.sin(i * 2.7) + 0.008 * math.cos(i * 4.1)
            shaky.append(tm._add(c, (jitter, 0.6 * jitter, -0.5 * jitter)))
        shaky[0] = clean[0] = root
        guide = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.12), path=shaky, regions=False)
        pts = self.points(guide)
        # Its points by its length and turns: a root 0.12 across, a stroke ~1.2 long turning ~70
        # degrees: about one a root size, the turn under 15 degrees a span.
        self.assertTrue(8 <= len(pts) - 1 <= 16, "%d spans" % (len(pts) - 1))
        spans = [tm._dist(a, b) for a, b in zip(pts, pts[1:])]
        mean = sum(spans) / len(spans)
        self.assertLess(max(abs(x - mean) for x in spans) / mean, 0.05, "evenly spaced")
        # Smooth: the jitter gone, each span turning a little and the same way (no wiggle).
        turns = []
        for a, b, c in zip(pts, pts[1:], pts[2:]):
            t1, t2 = tm._unit(tm._sub(b, a)), tm._unit(tm._sub(c, b))
            turns.append((math.degrees(math.acos(max(-1.0, min(1.0, tm._dot(t1, t2))))), tm._cross(t1, t2)[0]))
        join = 1
        for angle, _side in turns[join:]:
            self.assertLess(angle, 15.0, "jitter left in the guide")
        # (in the stroke's plane, y-z: its way turning on and on, never back)
        ways = [math.degrees(math.atan2(b[2] - a[2], b[1] - a[1])) for a, b in zip(pts, pts[1:])]
        for a, b in zip(ways, ways[1:]):
            self.assertGreater(b, a - 1.5, "the guide wiggles")  # (a degree's give: the tip's own shake)
        # On the arc drawn (the jitter averaged out), leaving the root its way.
        dense = root_tubes._resample(clean, 400)
        for p in pts[1:]:
            self.assertLess(nearest(p, dense), 0.03, "off the drawn curve")  # (within the shake's size, 0.02)
        # A stroke laid flat along the scalp: the guide leaves the root at most MAX_LEAN off its
        # normal, bending into the stroke in one turn.
        flat = [root] + [(0.6 * k / 20.0, 1.0 + 0.02 * k / 20.0, 0.0) for k in range(1, 21)]
        pts = root_tubes._guide_path(root, (0.0, 1.0, 0.0), flat, 1.0, 12, 0.12)
        lean = math.degrees(root_tubes._lean(tm._sub(pts[1], pts[0]), (0.0, 1.0, 0.0)))
        self.assertLessEqual(lean, math.degrees(root_tubes.MAX_LEAN) + 0.01)

    def test_a_drawn_curl_keeps_its_shape_and_a_short_stroke_few_points(self):
        """A curl drawn shakily (a helix of three turns, each a few roots wide, rising off the
        scalp): the guide keeps its turns (the smoothing goes by the root's size, not the stroke's
        length), the jitter gone; a short stroke gets few points, a long one more."""
        root = on_sphere((0.0, 1.0, 0.0))
        size = 0.06
        spiral, shaky = [root], [root]
        for i in range(1, 600):
            t = i / 599.0
            a = 3 * 2 * math.pi * t
            r = 0.15
            q = (r * math.cos(a) - r, 1.0 + 2.4 * t, r * math.sin(a))
            spiral.append(q)
            j = 0.004 * math.sin(i * 2.9)
            shaky.append(tm._add(q, (j, 0.5 * j, -j)))
        pts = root_tubes._guide_path(root, (0.0, 1.0, 0.0), shaky, 1.0, 12, size)
        drawn = root_tubes._turning(root_tubes._resample(spiral, 200))
        made = root_tubes._turning(pts)
        self.assertGreater(made, 0.8 * drawn, "the curls kept (%.0f of %.0f degrees)" % (math.degrees(made),
                                                                                         math.degrees(drawn)))
        dense = root_tubes._resample(spiral, 2000)
        for p in pts[3:]:
            self.assertLess(nearest(p, dense), 0.02, "off the drawn spiral")
        spans = [tm._dist(a, b) for a, b in zip(pts, pts[1:])]
        self.assertLess(max(spans) / min(spans), 1.1, "evenly spaced")
        # Few points on a short straight stroke, more on a long one; Point density doubles them.
        short = [root, tm._add(root, (0.0, 0.1, 0.0)), tm._add(root, (0.0, 0.2, 0.02))]
        long = [root, tm._add(root, (0.0, 1.0, 0.0)), tm._add(root, (0.0, 2.0, 0.2))]
        n_short = len(root_tubes._guide_path(root, (0.0, 1.0, 0.0), short, 1.0, 12, 0.1)) - 1
        n_long = len(root_tubes._guide_path(root, (0.0, 1.0, 0.0), long, 1.0, 12, 0.1)) - 1
        n_dense = len(root_tubes._guide_path(root, (0.0, 1.0, 0.0), long, 1.0, 12, 0.1, density=2.0)) - 1
        self.assertLessEqual(n_short, 3)
        self.assertGreater(n_long, 3 * n_short)
        self.assertAlmostEqual(n_dense, 2 * n_long, delta=2)

    def test_the_root_is_kept_whatever_happens_to_its_guide(self):
        """Combing, retwisting and resizing the guide's tube leave its root CVs (and its region)
        as drawn: only editing the root moves them."""
        outline = ring((0.0, 1.0, 0.0), 0.15)
        guide = root_tubes.make_root_tube(self.groom, outline, length=0.8)
        before = root_tubes.root_outline(self.groom, guide)
        regions = json.loads(cmds.getAttr(self.groom + ".scalpMaps"))[root_tubes.REGION_MAP]["values"]
        groomlab.brush_stroke(self.groom, "comb", [(-0.3, 1.3, 0.0), (0.0, 1.3, 0.1), (0.4, 1.3, 0.3)], radius=0.6)
        groomlab.set_tube(self.groom, guides=[guide], twist=1.0, width=0.3)
        self.assertGreater(max(nearest(p, root_tubes.section_outline(self.groom, guide)) for p in before), 0.02,
                           "the tube's own section moved (else this tests nothing)")
        after = root_tubes.root_outline(self.groom, guide)
        self.assertEqual(len(after), len(before))
        for a, b in zip(before, after):
            self.assertLess(tm._dist(a, b), 1e-6)
        root_tubes.update_regions(self.groom)
        again = json.loads(cmds.getAttr(self.groom + ".scalpMaps"))[root_tubes.REGION_MAP]["values"]
        self.assertEqual(again, regions)
        # Editing the root moves them, and puts the tube back on them.
        moved = [on_sphere(tm._add(p, (0.05, 0.0, 0.0))) for p in after]
        root_tubes.edit_root_tube(self.groom, guide, moved)
        for p in moved:
            self.assertLess(nearest(p, root_tubes.root_outline(self.groom, guide)), 0.003)
            self.assertLess(nearest(p, root_tubes.section_outline(self.groom, guide)), 0.01)

    def test_each_tube_is_a_region_of_its_own(self):
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.25), 0.18, count=6), length=0.5)
        b = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, -0.25), 0.18, count=6), length=0.5)
        maps = json.loads(cmds.getAttr(self.groom + ".scalpMaps"))
        entry = maps[root_tubes.REGION_MAP]
        self.assertEqual(entry.get("channels"), 4)
        points, _fns = root_tubes._scalp_points(self.groom)
        color = lambda i: tuple(entry["values"][4 * i:4 * i + 3])  # noqa: E731

        def vertex_near(p):
            return min(range(len(points)), key=lambda i: tm._dist(points[i], p))
        ca = color(vertex_near(self.points(a)[0]))
        cb = color(vertex_near(self.points(b)[0]))
        far = color(vertex_near((0.0, -1.0, 0.0)))
        self.assertNotEqual(ca, (0.0, 0.0, 0.0))
        self.assertNotEqual(cb, (0.0, 0.0, 0.0))
        self.assertGreater(math.sqrt(sum((x - y) ** 2 for x, y in zip(ca, cb))), 0.1, "the two regions differ")
        self.assertEqual(far, (0.0, 0.0, 0.0))
        # Interpolate reads it.
        graph = groomlab.get_graph(self.groom, expand_default=True)
        interpolates = [n for n in graph.get("nodes", []) if n.get("type") == "Interpolate"]
        for node in interpolates:
            self.assertEqual(node.get("params", {}).get("regionMap"), root_tubes.REGION_MAP)

    def test_cvs_snap_onto_a_neighbour_and_edits_follow(self):
        first = ring((0.0, 1.0, 0.0), 0.15, count=4)
        a = root_tubes.make_root_tube(self.groom, first, length=0.5)
        neighbours = root_tubes.snap_targets(self.groom)
        to_screen = lambda p: (p[0] * 1000.0, p[2] * 1000.0)  # noqa: E731 (a top view, 1000 px a unit)
        shared = root_tubes.root_outline(self.groom, a)[0]
        near = tm._add(shared, (0.004, 0.0, 0.004))  # ~6 px away
        snapped, how = root_tubes.snap_point(near, neighbours, to_screen)
        self.assertEqual(how, "cv")
        self.assertLess(tm._dist(snapped, shared), 1e-6)
        far = tm._add(shared, (0.2, 0.0, 0.2))
        self.assertEqual(root_tubes.snap_point(far, neighbours, to_screen)[1], None)

        # Edit: the outline moved over the scalp; the guide follows to its middle.
        moved = [on_sphere(tm._add(p, (0.15, 0.0, 0.0))) for p in root_tubes.root_outline(self.groom, a)]
        root_tubes.edit_root_tube(self.groom, a, moved)
        back = root_tubes.root_outline(self.groom, a)
        for p in moved:
            self.assertLess(nearest(p, back), 0.01)
        self.assertLess(tm._dist(self.points(a)[0], on_sphere(root_tubes._centroid(moved))), 0.03)
        cmds.undo()  # one undo step
        for p in first:
            self.assertLess(nearest(p, root_tubes.root_outline(self.groom, a)), 0.01)

    def test_edits_land_where_the_cvs_were_put(self):
        """Finish edit gives the tube the outline as edited (a CV pulled out, one added on an edge),
        edit after edit with no drift, on a tube drawn along the scalp."""
        root = on_sphere((0.0, 1.0, 0.0))
        path = [root, (0.0, 1.1, 0.1), (0.0, 1.1, 0.4), (0.0, 0.95, 0.7), (0.0, 0.7, 0.95)]
        guide = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.15), path=path, regions=False)
        for step in range(3):
            cvs = root_tubes.root_outline(self.groom, guide)
            c = root_tubes._centroid(cvs)
            cvs[1] = tm._add(c, tm._mul(tm._sub(cvs[1], c), 1.3))
            if step == 1:
                cvs.insert(3, tm._add(c, tm._mul(tm._sub(tm.lerp(cvs[2], cvs[3], 0.5), c), 1.4)))
            cvs = root_tubes._project(self.groom, cvs)
            root_tubes.edit_root_tube(self.groom, guide, cvs, regions=False)
            back = root_tubes.section_outline(self.groom, guide)
            self.assertEqual(len(back), len(cvs))
            for p in cvs:
                # (its tube's root a little larger than the outline: ROOT_OVERLAP)
                self.assertLess(nearest(p, back), 0.002 + root_tubes.ROOT_OVERLAP * 0.2, "edit %d: a CV off" % step)

    def test_a_ctrl_click_adds_a_cv_on_every_outline_sharing_the_edge(self):
        from groomlab.ui import root_tube_tool
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.12, count=4), regions=False)
        ca = root_tubes.root_outline(self.groom, a)
        c = root_tubes._centroid(ca)
        mid = tm.lerp(ca[0], ca[1], 0.5)
        out = tm._sub(mid, c)
        b = root_tubes.make_root_tube(self.groom, [ca[1], ca[0], on_sphere(tm._add(ca[0], out)),
                                                   on_sphere(tm._add(ca[1], out))], regions=False)
        tool = root_tube_tool.TOOL
        tool.to_screen = lambda p: (p[0] * 1000.0, p[2] * 1000.0)  # (a top view, 1000 px a unit)
        try:
            tool.start(self.groom, "edit")
            self.assertEqual(sorted(tool.edits), sorted([int(a), int(b)]))
            x, y = tool.to_screen(tm.lerp(tool.edits[int(a)][0], tool.edits[int(a)][1], 0.5))
            tool.insert_cv(x, y + 3.0)
            self.assertEqual(len(tool.edits[int(a)]), 5)
            self.assertEqual(len(tool.edits[int(b)]), 5, "the sealed neighbour gets it too")
            self.assertEqual(len(tool.drag), 2)
            self.assertEqual(tool.finish_edit(), 2)
            self.assertEqual(len(root_tubes.root_outline(self.groom, a)), 5)
            self.assertEqual(len(root_tubes.root_outline(self.groom, b)), 5)
        finally:
            tool.leave()
            del tool.to_screen

    def test_undo_brings_the_outline_back_to_draw_its_path_again(self):
        from groomlab.ui import root_tube_tool
        tool = root_tube_tool.TOOL
        try:
            tool.start(self.groom, "place")
            tool.cvs = ring((0.0, 1.0, 0.0), 0.15)
            tool.settings["draw"] = True
            tool.close()
            self.assertEqual(tool.mode, "draw")
            root = tool.center
            guide = tool.make([root, tm._add(root, (0.0, 0.3, 0.1)), tm._add(root, (0.0, 0.5, 0.5))])
            self.assertEqual(tool.mode, "place")
            cmds.undo()
            tool.undone()  # (Maya's Undo event calls it in a session; again: nothing more)
            tool.undone()
            self.assertNotIn(int(guide), root_tubes.root_tubes(self.groom))
            self.assertEqual(tool.mode, "draw")
            self.assertEqual(len(tool.cvs), 5)
            self.assertLess(tm._dist(tool.center, root), 1e-6)
        finally:
            tool.leave()

    def test_a_deleted_root_tubes_shape_goes_with_it(self):
        guide = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.15), length=0.5, regions=False)
        shape = groomlab.get_tube(self.groom, guide)["shape"]
        names = lambda: [s["name"] for s in groomlab.tube_shapes(self.groom)]  # noqa: E731
        self.assertIn(shape, names())
        groomlab.brush_stroke(self.groom, "erase", [self.points(guide)[0]], radius=0.1)
        self.assertEqual(root_tubes.root_tubes(self.groom), [])
        self.assertNotIn(shape, names())
        cmds.undo()
        self.assertIn(shape, names())
        self.assertEqual(root_tubes.root_tubes(self.groom), [int(guide)])

    def test_the_footprint_is_the_outline_however_the_guide_leans(self):
        """Scatter and fillTubes go by the root footprint (the root section turned flat onto the
        scalp: TubeSpine::setRootSurface): it is the outline drawn, upright or leaning far over, so
        the hair rooted inside the outline is the tube's (none left between, blending any guides)."""
        for way in (None, (0.0, 0.0, 1.0)):
            outline = ring((0.0, 1.0, 0.0), 0.14, count=6)
            root = on_sphere(root_tubes._centroid(outline))
            path = None
            if way:
                path = [root] + [tm._mul(on_sphere(tm._add(root, tm._mul(way, 0.06 * k))), 1.04 + 0.01 * k)
                                 for k in range(1, 16)]
            guide = root_tubes.make_root_tube(self.groom, outline, path=path, segments=12, regions=False)
            frames = json.loads(cmds.groomLabGuides(self.groom, tubeFrames=str(guide), evaluated=True))
            c, u, v = tm.frame_at(frames, 0.0)
            way_out = tm._unit(tm._cross(u, v))
            normal = root_tubes._smooth_normal(self.groom, c)
            if way:
                self.assertGreater(math.degrees(math.acos(tm._dot(way_out, normal))), 30.0, "it leans")
            tube = groomlab.get_tube(self.groom, guide)
            shape = root_tubes._library_shape(self.groom, tube["shape"])
            w = tube["width"][0]
            placed = tm.place_outline(shape["points"], tube["twist"][0], w, w * tube["aspect"][0], (0.0, 0.0))
            # (GroomLab's footprint: the section turned flat, by the least rotation from the
            # guide's root way onto the scalp's normal)
            foot = [tm._add(c, root_tubes._turn(tm.from_plane(q, (0.0, 0.0, 0.0), u, v), way_out, normal))
                    for q in placed]

            def flat(q):
                d = tm._sub(q, c)
                return tm._sub(d, tm._mul(normal, tm._dot(d, normal)))
            k = 1.0 + root_tubes.ROOT_OVERLAP  # (the footprint a little larger: sealed neighbours overlap)
            for q in root_tubes.root_outline(self.groom, guide):
                grown = tm._add(c, tm._mul(flat(q), k))
                self.assertLess(min(tm._dist(flat(grown), flat(f)) for f in foot), 0.002, "a CV off the footprint")

    def test_the_region_reaches_just_past_the_outline(self):
        """A face the outline crosses blends the tube's color with its own, not with black: a band
        about a face wide outside the outline is the tube's too; far off, black."""
        guide = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.15, count=6), length=0.5)
        entry = json.loads(cmds.getAttr(self.groom + ".scalpMaps"))[root_tubes.REGION_MAP]
        points, _fns = root_tubes._scalp_points(self.groom)
        outline = root_tubes.root_outline(self.groom, guide)
        c, u, v, n = root_tubes._plane(outline)
        flat = [tm.to_plane(q, c, u, v) for q in outline]
        color = lambda i: tuple(entry["values"][4 * i:4 * i + 3])  # noqa: E731
        ours = color(min(range(len(points)), key=lambda i: tm._dist(points[i], c)))
        edge = 2.0 * math.pi / 60.0  # (the sphere's faces: about this wide)
        just, far = 0, 0
        for i, q in enumerate(points):
            d = tm._sub(q, c)
            if tm._dot(d, n) < -0.5:
                continue
            x, y = tm._dot(d, u), tm._dot(d, v)
            if root_tubes._inside(flat, x, y):
                continue
            off = root_tubes._edge_distance(flat, x, y)
            if off < 0.5 * edge:
                just += 1
                self.assertEqual(color(i), ours, "a vertex just outside the outline")
            elif off > 3.0 * edge:
                far += 1
                self.assertEqual(color(i), (0.0, 0.0, 0.0))
        self.assertGreater(just, 0)
        self.assertGreater(far, 0)

    def test_a_root_duplicated_with_its_hierarchy_and_deleted(self):
        root = on_sphere((0.0, 1.0, 0.0))
        path = [root] + [tm._mul(on_sphere(tm._add(root, (0.0, 0.0, 0.06 * k))), 1.05 + 0.01 * k) for k in range(1, 14)]
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.15), path=path)
        root_tubes.add_tube_level(self.groom)
        root_tubes.add_tube_level(self.groom)
        count = lambda: len(groomlab.get_hierarchy(self.groom)["guides"])  # noqa: E731
        self.assertEqual(count(), 7)
        b = root_tubes.duplicate_root_tube(self.groom, a, on_sphere((0.5, 0.85, 0.1)))
        self.assertEqual(count(), 14)
        self.assertEqual(root_tubes.root_tubes(self.groom), sorted([int(a), int(b)]))
        o1, o2 = root_tubes.root_outline(self.groom, a), root_tubes.root_outline(self.groom, b)
        sides = lambda o: [tm._dist(x, y) for x, y in zip(o, o[1:] + o[:1])]  # noqa: E731
        for x, y in zip(sides(o1), sides(o2)):
            self.assertAlmostEqual(x, y, delta=0.002)  # the same shape
        self.assertLess(tm._dist(on_sphere(root_tubes._centroid(o2)), on_sphere((0.5, 0.85, 0.1))), 0.005)  # set down there
        for q in root_tubes.section_outline(self.groom, b):
            self.assertLess(nearest(q, o2), 0.003)  # its tube on its root
        data = groomlab.get_hierarchy(self.groom)
        kids = lambda gid: sorted(int(x["id"]) for x in data["guides"]  # noqa: E731
                                  if x.get("parent") and int(x["parent"]) == int(gid))
        self.assertEqual(len(kids(b)), 2)
        self.assertEqual([len(kids(k)) for k in kids(b)], [2, 2])
        length = lambda gid: sum(tm._dist(x, y) for x, y in zip(self.points(gid), self.points(gid)[1:]))  # noqa: E731
        for k1, k2 in zip(kids(a), kids(b)):
            self.assertAlmostEqual(length(k1), length(k2), delta=0.01)  # each child its counterpart's shape
        cmds.undo()  # one undo step
        self.assertEqual(count(), 7)
        cmds.redo()
        self.assertEqual(count(), 14)
        shape = groomlab.get_tube(self.groom, b)["shape"]
        root_tubes.delete_root_tubes(self.groom, [b])
        self.assertEqual(count(), 7)
        self.assertEqual(sorted(root_tubes._stored_outlines(self.groom)), [str(a)])
        self.assertNotIn(shape, [x["name"] for x in groomlab.tube_shapes(self.groom)])
        cmds.undo()
        self.assertEqual(count(), 14)

    def _tool(self):
        """The root tool, its view a top view (1000 px a unit) onto the sphere's top."""
        from groomlab.ui import root_tube_tool
        tool = root_tube_tool.TOOL
        tool.to_screen = lambda q: (q[0] * 1000.0, q[2] * 1000.0)

        def hit(x, y):
            q = on_sphere((x / 1000.0, 1.0, y / 1000.0))
            return q, q
        tool.hit = hit
        self.addCleanup(self._untool, tool)
        return tool

    def _untool(self, tool):
        tool.leave()
        for name in ("to_screen", "hit", "press_point", "view_point"):
            tool.__dict__.pop(name, None)

    def test_select_mode_carries_a_root_its_sealed_cvs_held(self):
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.12, count=4))
        ca = root_tubes.root_outline(self.groom, a)
        c = root_tubes._centroid(ca)
        out = tm._sub(tm.lerp(ca[0], ca[1], 0.5), c)
        b = root_tubes.make_root_tube(self.groom, [ca[1], ca[0], on_sphere(tm._add(ca[0], out)),
                                                   on_sphere(tm._add(ca[1], out))])
        tool = self._tool()
        tool.start(self.groom, "select")
        self.assertEqual(sorted(tool.roots), sorted([int(a), int(b)]))
        middle = tool.middles[int(b)][0]
        x, y = tool.to_screen(middle)
        tool.press_point = lambda: (x + 2.0, y)
        tool.press()
        self.assertEqual(tool.selected, int(b))
        self.assertEqual(len(tool.move["held"][int(b)]), 2, "its two CVs on a's edge held")
        step = tm._mul(tm._unit(out), 0.05)
        tool.view_point = lambda: (x + 2.0 + step[0] * 1000.0, y + step[2] * 1000.0)
        tool.drag_event()
        tool.release()
        after = root_tubes.root_outline(self.groom, b)
        before = [ca[1], ca[0]]
        for q in before:
            self.assertLess(nearest(q, after), 1e-3, "a sealed CV moved")
        self.assertGreater(tm._dist(root_tubes._centroid(after), c), tm._dist(root_tubes._centroid(
            [ca[1], ca[0], on_sphere(tm._add(ca[0], out)), on_sphere(tm._add(ca[1], out))]), c) + 0.01)
        for q in root_tubes.root_outline(self.groom, a):
            self.assertLess(nearest(q, ca), 1e-6, "the neighbour stays")
        cmds.undo()  # one step
        self.assertLess(max(nearest(q, root_tubes.root_outline(self.groom, b)) for q in before), 1e-3)
        # Duplicate: the copy follows the cursor; the click (release) sets it down.
        tool.start(self.groom, "select")
        tool.press_point = lambda: (x, y)
        tool.press()
        tool.release()
        self.assertTrue(tool.begin_duplicate())
        target = on_sphere((-0.45, 0.85, 0.1))
        tool.follow(target[0] * 1000.0, target[2] * 1000.0)
        self.assertIsNotNone(tool.ghost)
        new = tool.place_copy()
        self.assertEqual(tool.mode, "select")
        self.assertEqual(tool.selected, int(new))
        self.assertEqual(len(root_tubes.root_tubes(self.groom)), 3)
        self.assertEqual(tool.delete_selected(), 1)
        self.assertEqual(len(root_tubes.root_tubes(self.groom)), 2)
        self.assertIsNone(tool.selected)

    def test_shift_selects_several_roots_carried_and_copied_as_one(self):
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.12, count=4), regions=False)
        ca = root_tubes.root_outline(self.groom, a)
        c = root_tubes._centroid(ca)
        out = tm._sub(tm.lerp(ca[0], ca[1], 0.5), c)
        b = root_tubes.make_root_tube(self.groom, [ca[1], ca[0], on_sphere(tm._add(ca[0], out)),
                                                   on_sphere(tm._add(ca[1], out))], regions=False)
        other = tm._mul(out, -1.0)
        d = root_tubes.make_root_tube(self.groom, [ca[3], ca[2], on_sphere(tm._add(ca[2], other)),
                                                   on_sphere(tm._add(ca[3], other))], regions=False)
        tool = self._tool()
        tool.start(self.groom, "select")
        keys = {"mod": ""}
        tool.modifier = lambda: keys["mod"]

        def click(g):
            x, y = tool.to_screen(tool.middles[int(g)][0])
            tool.press_point = lambda: (x, y)
            tool.press()
            return x, y
        click(a)
        keys["mod"] = "shift"
        click(b)
        self.assertEqual(tool.picked, [int(a), int(b)])
        click(b)  # Shift+click a selected one: out
        self.assertEqual(tool.picked, [int(a)])
        x, y = click(b)
        self.assertEqual(sorted(tool.picked), sorted([int(a), int(b)]))
        # a's CVs sealed onto d (not selected) held; a and b's shared CVs carried together.
        self.assertEqual(len(tool.move["held"][int(a)]), 2)
        self.assertEqual(len(tool.move["held"][int(b)]), 0)
        keys["mod"] = ""
        step = tm._mul(tm._unit(out), 0.04)
        tool.view_point = lambda: (x + step[0] * 1000.0, y + step[2] * 1000.0)
        tool.drag_event()
        tool.release()
        oa, ob = root_tubes.root_outline(self.groom, a), root_tubes.root_outline(self.groom, b)
        for q in ob[:2]:
            self.assertLess(nearest(q, oa), 1e-3, "the seam between the two carried roots kept")
        od = root_tubes.root_outline(self.groom, d)
        for q in od[:2]:
            self.assertLess(nearest(q, oa), 1e-3, "a's CVs sealed onto d stay")
        self.assertGreater(nearest(ob[2], [ca[0], ca[1], ca[2], ca[3]]), 0.03, "b moved")
        cmds.undo()  # one step for both
        for q in [ca[1], ca[0]]:
            self.assertLess(nearest(q, root_tubes.root_outline(self.groom, b)), 1e-3)
        # Both copied at once, as one group, then both deleted.
        tool.start(self.groom, "select")
        tool.modifier = lambda: keys["mod"]
        keys["mod"] = ""
        click(a)
        keys["mod"] = "shift"
        click(b)
        tool.release()
        self.assertTrue(tool.begin_duplicate())
        target = on_sphere((-0.45, 0.85, 0.1))
        tool.follow(target[0] * 1000.0, target[2] * 1000.0)
        self.assertEqual(len(tool.ghost[1]), 2)
        made = tool.place_copy()
        self.assertEqual(len(made), 2)
        self.assertEqual(len(root_tubes.root_tubes(self.groom)), 5)
        na, nb = (root_tubes.root_outline(self.groom, g) for g in made)
        self.assertLess(min(nearest(q, na) for q in nb[:2]), 1e-3, "the copies sealed as their sources are")
        self.assertEqual(tool.delete_selected(), 2)
        self.assertEqual(len(root_tubes.root_tubes(self.groom)), 3)

    def test_update_regions_refits_a_tube_off_its_root(self):
        """A tube whose root section drifted off its kept outline (fitted by an earlier GroomLab, or
        resized) is fitted to it again by Update regions (refresh_root_tubes); a second refresh
        changes nothing."""
        guide = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.15), length=0.6)
        self.assertLess(root_tubes.footprint_error(self.groom, guide), 0.004)
        tube = groomlab.get_tube(self.groom, guide)
        groomlab.set_tube(self.groom, guides=[guide], width=[w * 0.8 for w in tube["width"]], twist=0.4)
        self.assertGreater(root_tubes.footprint_error(self.groom, guide), 0.05)
        out = root_tubes.refresh_root_tubes(self.groom)
        self.assertEqual(out["refitted"], [int(guide)])
        self.assertLess(root_tubes.footprint_error(self.groom, guide), 0.004)
        self.assertEqual(root_tubes.refresh_root_tubes(self.groom)["refitted"], [])

    def test_redo_puts_the_drawn_outline_away(self):
        tool = self._tool()
        tool.start(self.groom, "place")
        tool.cvs = ring((0.0, 1.0, 0.0), 0.15)
        tool.settings["draw"] = True
        tool.close()
        root = tool.center
        guide = tool.make([root, tm._add(root, (0.0, 0.3, 0.1)), tm._add(root, (0.0, 0.5, 0.5))])
        cmds.undo()
        tool.undone()
        self.assertEqual(tool.mode, "draw")
        cmds.redo()
        tool.redone()
        self.assertEqual(tool.mode, "place")
        self.assertEqual(tool.cvs, [])
        self.assertIn(int(guide), root_tubes.root_tubes(self.groom))

    def test_edit_mode_greens_the_root_touched(self):
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.12, count=4), regions=False)
        root_tubes.make_root_tube(self.groom, ring((0.5, 0.85, 0.0), 0.12, count=4), regions=False)
        tool = self._tool()
        tool.start(self.groom, "edit")
        self.assertEqual(tool.touched, set())
        x, y = tool.to_screen(tool.edits[int(a)][0])
        tool.modifier = lambda: ""
        self.addCleanup(lambda: tool.__dict__.pop("modifier", None))
        tool.press_edit(x, y)
        self.assertEqual(tool.touched, {int(a)})

    def _pair(self):
        """Two roots sealed along an edge (a's CVs 0 and 1 are b's 1 and 0)."""
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.12, count=4), regions=False)
        ca = root_tubes.root_outline(self.groom, a)
        out = tm._sub(tm.lerp(ca[0], ca[1], 0.5), root_tubes._centroid(ca))
        b = root_tubes.make_root_tube(self.groom, [ca[1], ca[0], on_sphere(tm._add(ca[0], out)),
                                                   on_sphere(tm._add(ca[1], out))])
        return int(a), int(b)

    def test_the_preview_draws_each_root_in_its_regions_color(self):
        from groomlab.ui import root_tube_tool as rt
        a, b = self._pair()
        tool = self._tool()
        tool.start(self.groom, "edit")
        shape = rt.PREVIEW + "Shape"
        self.assertEqual(cmds.nodeType(shape), rt.OUTLINES_NODE)
        counts = list(cmds.getAttr(shape + ".counts"))
        styles = list(cmds.getAttr(shape + ".styles"))
        colors = [tuple(c) for c in cmds.getAttr(shape + ".colors")]
        self.assertEqual(counts, [4, 4])
        want = root_tubes.root_colors(self.groom)
        self.assertEqual(len(set(want.values())), 2, "each root a color of its own")
        for got, g in zip(colors, [a, b]):
            for x, y in zip(got, want[g]):
                self.assertAlmostEqual(x, y, places=5)
        for st in styles:
            self.assertEqual(st & (rt.CLOSED | rt.FILLED | rt.DOTS), rt.CLOSED | rt.FILLED | rt.DOTS)
            self.assertFalse(st & rt.HIGHLIGHT)
        # The region map paints them the same colors.
        maps = __import__("json").loads(cmds.getAttr(self.groom + ".scalpMaps"))
        values = maps[root_tubes.REGION_MAP]["values"]
        seen = set(tuple(round(v, 2) for v in values[4 * i:4 * i + 3]) for i in range(len(values) // 4))
        for g in (a, b):
            self.assertIn(tuple(round(v, 2) for v in want[g]), seen)
        tool.modifier = lambda: ""
        self.addCleanup(lambda: tool.__dict__.pop("modifier", None))
        x, y = tool.to_screen(tool.edits[a][2])
        tool.press_edit(x, y)
        styles = list(cmds.getAttr(shape + ".styles"))
        self.assertTrue(styles[0] & rt.HIGHLIGHT and not styles[1] & rt.HIGHLIGHT, "the root touched edged white")
        tool.leave()
        self.assertFalse(cmds.objExists(rt.PREVIEW))

    def test_a_ctrl_click_on_a_sealed_cv_takes_it_out_of_the_root_meant_only(self):
        a, b = self._pair()
        tool = self._tool()
        tool.start(self.groom, "edit")
        keys = {"mod": ""}
        tool.modifier = lambda: keys["mod"]
        self.addCleanup(lambda: tool.__dict__.pop("modifier", None))
        # b first touched (its far CV), then Ctrl+click on the CV it shares with a.
        x, y = tool.to_screen(tool.edits[b][2])
        tool.press_edit(x, y)
        tool.release()
        tool.edits[a].insert(2, on_sphere(tm.lerp(tool.edits[a][1], tool.edits[a][2], 0.5)))  # (a has 5: room to lose one)
        tool.edits[b].insert(3, on_sphere(tm.lerp(tool.edits[b][2], tool.edits[b][3], 0.5)))
        shared = tool.edits[b][0]
        keys["mod"] = "ctrl"
        x, y = tool.to_screen(shared)
        tool.press_edit(x, y)
        self.assertEqual(len(tool.edits[b]), 4, "taken out of b, the root touched")
        self.assertEqual(len(tool.edits[a]), 5, "a keeps its own")
        self.assertLess(nearest(shared, tool.edits[a]), 1e-5)
        tool.undo_cv()
        self.assertEqual(len(tool.edits[b]), 5, "Undo CV puts it back")
        # A root down to 3 CVs keeps them.
        for _ in range(2):
            tool.touched = {b}
            x, y = tool.to_screen(tool.edits[b][-1])
            tool.press_edit(x, y)
        self.assertEqual(len(tool.edits[b]), 3)
        x, y = tool.to_screen(tool.edits[b][-1])
        tool.press_edit(x, y)
        self.assertEqual(len(tool.edits[b]), 3)
        self.assertEqual(tool.finish_edit(), 2)
        self.assertEqual(len(root_tubes.root_outline(self.groom, b)), 3)
        self.assertEqual(len(root_tubes.root_outline(self.groom, a)), 5)

    def test_a_cv_never_snaps_onto_its_own_root(self):
        a, b = self._pair()
        tool = self._tool()
        tool.settings["snap"] = True
        tool.start(self.groom, "edit")
        tool.modifier = lambda: ""
        self.addCleanup(lambda: tool.__dict__.pop("modifier", None))
        # b's far CV dragged to 5 px from its own neighbouring CV (b's 3): it stays where it is put.
        own = tool.edits[b][3]
        x, y = tool.to_screen(tool.edits[b][2])
        tool.press_edit(x, y)
        tx, ty = tool.to_screen(own)
        tool.view_point = lambda: (tx + 5.0, ty)
        self.addCleanup(lambda: tool.__dict__.pop("view_point", None))
        tool.drag_event()
        tool.release()
        self.assertGreater(tm._dist(tool.edits[b][2], own), 1e-3, "not snapped onto its own CV")
        self.assertEqual(len(tool.drag), 0)
        # ... and a CV of b dragged near a's free CV snaps onto it (another root).
        x, y = tool.to_screen(tool.edits[b][2])
        tool.press_edit(x, y)
        ax, ay = tool.to_screen(tool.edits[a][2])
        tool.view_point = lambda: (ax + 4.0, ay)
        tool.drag_event()
        self.assertLess(tm._dist(tool.edits[b][2], tool.edits[a][2]), 1e-6)

    def test_leaving_the_tool_while_editing_keeps_the_edits(self):
        a, _b = self._pair()
        tool = self._tool()
        tool.start(self.groom, "edit")
        c = root_tubes._centroid(tool.edits[a])
        moved = on_sphere(tm._add(c, tm._mul(tm._sub(tool.edits[a][2], c), 1.3)))
        tool.edits[a][2] = moved
        tool.start(self.groom, "select")  # (another mode: the edit finished first)
        self.assertLess(nearest(moved, root_tubes.root_outline(self.groom, a)), 1e-3)
        tool.start(self.groom, "edit")
        moved2 = on_sphere(tm._add(c, tm._mul(tm._sub(tool.edits[a][3], c), 1.3)))
        tool.edits[a][3] = moved2
        tool.leave()  # (another tool)
        self.assertEqual(tool.mode, "place")
        self.assertLess(nearest(moved2, root_tubes.root_outline(self.groom, a)), 1e-3)

    def test_finish_never_strands_the_edit(self):
        """A root whose CV was dragged onto its neighbour, and Maya's Undo taking away a root being
        edited: Finish still finishes, edit mode left."""
        a, b = self._pair()
        tool = self._tool()
        tool.start(self.groom, "edit")
        tool.edits[a][2] = tool.edits[a][3]  # (two CVs one)
        cmds.undo()  # Maya's Undo: b (made last) taken away while editing
        tool.undone()
        self.assertNotIn(b, tool.edits)
        self.assertEqual(tool.finish_edit(), 1)
        self.assertEqual(tool.mode, "place")
        self.assertEqual(len(root_tubes.root_outline(self.groom, a)), 3)

    def test_hierarchy_levels_split_tubes_in_two(self):
        root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.2, count=6), length=0.8, regions=False)
        root_tubes.make_root_tube(self.groom, ring((0.5, 0.8, 0.0), 0.15, count=5), length=0.8, regions=False)
        self.assertEqual(root_tubes.tube_levels(self.groom)["levels"], [root_tubes.ROOT_GROUP])
        info = root_tubes.add_tube_level(self.groom)
        self.assertEqual(info["split"], 2)
        self.assertEqual(info["levels"], [root_tubes.ROOT_GROUP, root_tubes.ROOT_GROUP + "_L2"])
        self.assertEqual(len(groomlab.guide_ids(self.groom, root_tubes.ROOT_GROUP + "_L2")), 4)
        info = root_tubes.add_tube_level(self.groom)
        self.assertEqual(len(info["levels"]), 3)
        self.assertEqual(len(groomlab.guide_ids(self.groom, root_tubes.ROOT_GROUP + "_L3")), 8)
        # The children of one tube lie side by side inside it, and share its root shape.
        data = groomlab.get_hierarchy(self.groom)
        kids = {}
        for g in data["guides"]:
            if g.get("parent"):
                kids.setdefault(g["parent"], []).append(g)
        for pair in kids.values():
            self.assertEqual(len(pair), 2)
            self.assertTrue(all(g["tube"]["shape"].startswith(root_tubes.SHAPE_PREFIX) for g in pair))
        # Only the selected tube splits further.
        one = groomlab.guide_ids(self.groom, root_tubes.ROOT_GROUP)[0]
        info = root_tubes.add_tube_level(self.groom, guides=[one])
        self.assertEqual(info["split"], 4)  # its four leaves
        self.assertEqual(len(groomlab.guide_ids(self.groom, root_tubes.ROOT_GROUP + "_L4")), 8)
        # One level shown at a time.
        group = root_tubes.show_tube_level(self.groom, root_tubes.ROOT_GROUP, 2)
        self.assertEqual(group, root_tubes.ROOT_GROUP + "_L2")
        hidden = groomlab.hidden_guide_groups(self.groom)
        self.assertNotIn(group, hidden)
        self.assertIn(root_tubes.ROOT_GROUP, hidden)
        self.assertIn(root_tubes.ROOT_GROUP + "_L3", hidden)
        info = root_tubes.remove_tube_level(self.groom, root_tubes.ROOT_GROUP)
        self.assertEqual(len(info["levels"]), 3)

    def test_round_tubes_split_too(self):
        curve = cmds.curve(point=[(0.0, 1.0, 0.0), (0.0, 1.5, 0.0), (0.0, 2.0, 0.0)], degree=1)
        ids = groomlab.guides_from_curves(self.groom, [curve], group="primary", tube_radius=0.1)
        info = root_tubes.add_tube_level(self.groom, picked="primary")
        self.assertEqual(info["levels"], ["primary", "primary_L2"])
        kids = groomlab.guide_ids(self.groom, "primary_L2")
        self.assertEqual(len(kids), 2)
        a, b = (self.points(k)[0] for k in kids)
        self.assertAlmostEqual(tm._dist(a, b), 0.1, delta=0.02)  # half the width apart each side
        self.assertTrue(ids)


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    maya.standalone.uninitialize()
    os._exit(0 if result.wasSuccessful() else 1)
