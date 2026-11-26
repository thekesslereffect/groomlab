# SPDX-License-Identifier: Apache-2.0
"""Root-first tubes in the Tube tool (issues 61 and 62): tubes drawn root first (the root's outline
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
        """Combing, retwisting and resizing the guide's tube leave its root CVs as drawn: only
        editing the root moves them."""
        outline = ring((0.0, 1.0, 0.0), 0.15)
        guide = root_tubes.make_root_tube(self.groom, outline, length=0.8)
        before = root_tubes.root_outline(self.groom, guide)
        groomlab.brush_stroke(self.groom, "comb", [(-0.3, 1.3, 0.0), (0.0, 1.3, 0.1), (0.4, 1.3, 0.3)], radius=0.6)
        groomlab.set_tube(self.groom, guides=[guide], twist=1.0, width=0.3)
        self.assertGreater(max(nearest(p, root_tubes.section_outline(self.groom, guide)) for p in before), 0.02,
                           "the tube's own section moved (else this tests nothing)")
        after = root_tubes.root_outline(self.groom, guide)
        self.assertEqual(len(after), len(before))
        for a, b in zip(before, after):
            self.assertLess(tm._dist(a, b), 1e-6)
        # Editing the root moves them, and puts the tube back on them.
        moved = [on_sphere(tm._add(p, (0.05, 0.0, 0.0))) for p in after]
        root_tubes.edit_root_tube(self.groom, guide, moved)
        for p in moved:
            self.assertLess(nearest(p, root_tubes.root_outline(self.groom, guide)), 0.003)
            self.assertLess(nearest(p, root_tubes.section_outline(self.groom, guide)), 0.01)

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

    def test_a_deleted_root_tubes_shape_goes_with_it(self):
        guide = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.15), length=0.5, regions=False)
        shape = groomlab.get_tube(self.groom, guide)["shape"]
        names = lambda: [s["name"] for s in groomlab.tube_shapes(self.groom, owned=True)]  # noqa: E731
        self.assertIn(shape, names())
        groomlab.brush_stroke(self.groom, "erase", [self.points(guide)[0]], radius=0.1)
        self.assertEqual(root_tubes.root_tubes(self.groom), [])
        self.assertNotIn(shape, names())
        cmds.undo()
        self.assertIn(shape, names())
        self.assertEqual(root_tubes.root_tubes(self.groom), [int(guide)])

    def test_the_footprint_is_the_outline_however_the_guide_leans(self):
        """Scatter and fillTubes go by the root footprint (fixed on the scalp:
        TubeSpine::setRootOnScalp): it is the outline drawn, upright or leaning far over, so
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
            # (GroomLab's footprint: fixed on the scalp, along its frame there, whatever the guide's
            # lean: TubeSpine::setRootOnScalp)
            fixed = frames["footprint"]
            foot = [tm._add(c, tm.from_plane(q, (0.0, 0.0, 0.0), tuple(fixed["u"]), tuple(fixed["v"]))) for q in placed]

            def flat(q):
                d = tm._sub(q, c)
                return tm._sub(d, tm._mul(normal, tm._dot(d, normal)))
            k = 1.0 + root_tubes.ROOT_OVERLAP  # (the footprint a little larger: sealed neighbours overlap)
            for q in root_tubes.root_outline(self.groom, guide):
                grown = tm._add(c, tm._mul(flat(q), k))
                self.assertLess(min(tm._dist(flat(grown), flat(f)) for f in foot), 0.002, "a CV off the footprint")

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
        # (The drawn roots: + cut their roots too, each half a root of its own at its level.)
        data = {int(x["id"]): x for x in groomlab.get_hierarchy(self.groom)["guides"]}
        drawn = [g for g in root_tubes.root_tubes(self.groom) if data[g].get("parent") in (None, -1)]
        self.assertEqual(drawn, sorted([int(a), int(b)]))
        o1, o2 = root_tubes.root_outline(self.groom, a), root_tubes.root_outline(self.groom, b)
        sides = lambda o: [tm._dist(x, y) for x, y in zip(o, o[1:] + o[:1])]  # noqa: E731
        for x, y in zip(sides(o1), sides(o2)):
            self.assertAlmostEqual(x, y, delta=0.002)  # the same shape
        self.assertLess(tm._dist(on_sphere(root_tubes._centroid(o2)), on_sphere((0.5, 0.85, 0.1))), 0.005)  # set down there
        for q in root_tubes.section_outline(self.groom, b):
            self.assertLess(nearest(q, o2), 0.004)  # its tube on its root (the footprint 1.5% larger, the sphere curving away)
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
        # (each root's CVs are kept with its guide, saved with the groom: the deleted root's went with it)
        # (a's halves keep theirs too: + cut its root)
        kept = [int(g["id"]) for g in groomlab.get_hierarchy(self.groom)["guides"]
                if g.get("rootOutline") and g.get("parent") in (None, -1)]
        self.assertEqual(kept, [int(a)])
        self.assertNotIn(shape, [x["name"] for x in groomlab.tube_shapes(self.groom, owned=True)])
        cmds.undo()
        self.assertEqual(count(), 14)
        levels = root_tubes.tube_levels(self.groom, picked=root_tubes.ROOT_GROUP)["levels"]
        self.assertEqual(len(levels), 3)
        groups = lambda: sorted(x["name"] for x in groomlab.list_guide_groups(self.groom))  # noqa: E731
        # Erased (the brush, groomLabGuides -removeGuides): the hierarchy under them goes, and the
        # levels left empty with it; the other root keeps its levels until it goes too.
        cmds.groomLabGuides(self.groom, removeGuides=True, guide=[str(b)])
        self.assertEqual(count(), 7)
        self.assertEqual(groups(), sorted(levels))
        cmds.groomLabGuides(self.groom, removeGuides=True, guide=[str(a)])
        self.assertEqual(count(), 0)
        self.assertEqual(groups(), [root_tubes.ROOT_GROUP])  # (the base stays: the graph reads it)
        cmds.undo()
        self.assertEqual(groups(), sorted(levels))
        self.assertEqual(count(), 7)

    def test_combing_the_guide_leaves_the_root_area_where_it_was_drawn(self):
        """A root tube's footprint is fixed on the scalp: combed hard over, its guide leans away,
        but the area its strands root in (and fill) stays the outline as drawn, with no region map."""
        outline = ring((0.0, 1.0, 0.0), 0.15)
        guide = root_tubes.make_root_tube(self.groom, outline, length=0.8)
        self.assertLess(root_tubes.footprint_error(self.groom, guide), 0.004)
        groomlab.brush_stroke(self.groom, "comb", [(-0.3, 1.3, 0.0), (0.0, 1.3, 0.1), (0.4, 1.3, 0.3)], radius=0.6)
        groomlab.brush_stroke(self.groom, "comb", [(0.4, 1.3, -0.3), (0.0, 1.3, 0.0), (-0.4, 1.3, 0.3)], radius=0.6)
        self.assertLess(root_tubes.footprint_error(self.groom, guide), 0.004, "the root area stayed as drawn")
        self.assertNotIn("tubeRoots", groomlab.list_maps(self.groom))

    def _pair(self):
        """Two roots sealed along an edge (a's CVs 0 and 1 are b's 1 and 0)."""
        a = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.12, count=4), regions=False)
        ca = root_tubes.root_outline(self.groom, a)
        out = tm._sub(tm.lerp(ca[0], ca[1], 0.5), root_tubes._centroid(ca))
        b = root_tubes.make_root_tube(self.groom, [ca[1], ca[0], on_sphere(tm._add(ca[0], out)),
                                                   on_sphere(tm._add(ca[1], out))])
        return int(a), int(b)

    def test_a_drawn_guide_eases_over_the_scalp_never_through_it(self):
        """A path drawn down over the head, dipping into it (as a stroke on the view's plane does):
        the guide keeps off the scalp, more so away from its root, and bends smoothly where it
        rides over it (no corner where it was pushed out)."""
        outline = ring((0.0, 1.0, 0.0), 0.08, count=6)
        size = max(tm._dist(on_sphere((0.0, 1.0, 0.0)), p) for p in outline)
        path = [on_sphere((0.0, 1.0, 0.0))]
        for k in range(1, 60):
            a = math.radians(1.2 * k)  # down the sphere's side, 3% inside it
            path.append((0.0, 0.97 * math.cos(a), 0.97 * math.sin(a)))
        guide = root_tubes.make_root_tube(self.groom, outline, path=path, regions=False)
        pts = self.points(guide)
        along = 0.0
        for a, b in zip(pts, pts[1:]):
            along += tm._dist(a, b)
            if along > 2.0 * root_tubes._CLEAR_RAMP * size:
                self.assertGreater(tm._length(b) - 1.0, 0.6 * root_tubes.CLEARANCE * size, "off the scalp")
        for i in range(1, len(pts) - 1):
            u, v = tm._sub(pts[i], pts[i - 1]), tm._sub(pts[i + 1], pts[i])
            turn = math.degrees(math.acos(max(-1.0, min(1.0, tm._dot(tm._unit(u), tm._unit(v))))))
            # (at the root: its seat, at most MAX_LEAN off the normal, turning into a path drawn flat
            # along the scalp; beyond, the path's own gentle bends, no corner where it was lifted)
            self.assertLess(turn, 65.0 if i <= 2 else 20.0, "no corner at point %d" % i)

    def test_a_drawn_guide_clear_of_the_scalp_keeps_its_course(self):
        """A path drawn just over the head (no dip into it) is left as drawn by the clearance; one
        dipping in only where it crosses the head is lifted there, its course elsewhere kept."""
        outline = ring((0.0, 1.0, 0.0), 0.08, count=6)
        size = max(tm._dist(on_sphere((0.0, 1.0, 0.0)), p) for p in outline)

        def path(height, dip=0.0):
            out = [on_sphere((0.0, 1.0, 0.0))]
            for k in range(1, 60):
                a = math.radians(1.2 * k)
                r = 1.0 + height - dip * math.exp(-((k - 35) / 6.0) ** 2)  # (a dip about k = 35)
                out.append((0.0, r * math.cos(a), r * math.sin(a)))
            return out

        def off(pts, drawn):
            def seg(q, a, b):
                ab = tm._sub(b, a)
                t = max(0.0, min(1.0, tm._dot(tm._sub(q, a), ab) / max(tm._dot(ab, ab), 1e-12)))
                return tm._dist(q, tm.lerp(a, b, t))
            return [min(seg(q, a, b) for a, b in zip(drawn, drawn[1:])) for q in pts]

        high = path(0.6 * size)
        guide = root_tubes.make_root_tube(self.groom, outline, path=high, regions=False)
        pts = self.points(guide)
        self.assertLess(max(off(pts[3:], high)), 0.25 * size, "kept where it never dipped")
        dipped = path(0.6 * size, dip=0.9 * size)
        cmds.undo()
        guide = root_tubes.make_root_tube(self.groom, outline, path=dipped, regions=False)
        pts = self.points(guide)
        self.assertGreater(min(tm._length(q) for q in pts[len(pts) // 3:]) - 1.0, 0.0, "out of the scalp")
        far = [q for q in pts if q[2] < math.sin(math.radians(1.2 * 20))]  # (its first part, far from the dip)
        self.assertLess(max(off(far[3:], dipped)), 0.25 * size, "its course kept away from the dip")

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
        # One level shown at a time: the level view (the groom's mixedView), the groups' eyes left
        # as they were; solo (Mixed view off) no tubes of the levels above.
        group = root_tubes.show_tube_level(self.groom, root_tubes.ROOT_GROUP, 2)
        self.assertEqual(group, root_tubes.ROOT_GROUP + "_L2")
        self.assertEqual(cmds.getAttr(self.groom + ".mixedView"), root_tubes.ROOT_GROUP + ":2")
        self.assertEqual(groomlab.hidden_guide_groups(self.groom), [])
        root_tubes.show_tube_level(self.groom, root_tubes.ROOT_GROUP, 3, solo=True)
        self.assertEqual(cmds.getAttr(self.groom + ".mixedView"), root_tubes.ROOT_GROUP + ":3:solo")
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

    def test_a_root_subdivided_by_its_painted_density(self):
        """Issue 85: Subdivide cuts a root into cells in the level below, its children (one undo
        step), by the one density map (made at 0.5); Apply cuts again by the map as painted, and the
        multiplier makes a cut denser."""
        from groomlab import painting
        guide = root_tubes.make_root_tube(self.groom, ring((0.0, 1.0, 0.0), 0.35, count=12), length=0.8)
        before = len(groomlab.get_hierarchy(self.groom)["guides"])
        name = root_tubes.ensure_density_map(self.groom, root_tubes.DENSITY_MAP)
        self.assertEqual(name, "tubeSubdivisionDensity")
        self.assertEqual(painting.map_kind(self.groom, name), "vertices")
        cut = root_tubes.subdivide(self.groom, [guide], name)
        self.assertGreaterEqual(len(cut["cells"]), 4)
        # The root kept, its cells a level below it: roots of their own too.
        self.assertEqual(sorted(root_tubes.root_tubes(self.groom)), sorted([guide] + cut["cells"]))
        self.assertEqual(len(root_tubes.tube_levels(self.groom)["levels"]), 2)
        cmds.undo()
        self.assertEqual(len(groomlab.get_hierarchy(self.groom)["guides"]), before)
        self.assertEqual(root_tubes.root_tubes(self.groom), [guide])
        cmds.redo()
        self.assertEqual(sorted(root_tubes.root_tubes(self.groom)), sorted([guide] + cut["cells"]))
        # Nothing painted: nothing cut again; white everywhere and Apply: more, smaller roots.
        self.assertEqual(root_tubes.apply_density(self.groom, [], name)["cells"], [])
        painting.fill_map(self.groom, name, value=1.0)
        again = root_tubes.apply_density(self.groom, [], name)
        self.assertGreater(len(again["cells"]), len(cut["cells"]))
        self.assertGreater(again["removed"], 0)
        # Apply forced at a higher multiplier: denser still.
        denser = root_tubes.apply_density(self.groom, [], name, multiplier=3.0, force=True)
        self.assertGreater(len(denser["cells"]), 2 * len(again["cells"]))


HERE = os.path.dirname(os.path.abspath(__file__))


def uneven(center, radius):
    """An irregular root outline (no symmetry: it fits its tube shape one way only)."""
    up = on_sphere(center)
    a = tm._unit(tm._cross(up, (0.0, 0.0, 1.0)))
    b = tm._cross(up, a)
    scale = (1.0, 0.7, 1.25, 0.85, 1.1, 0.6)
    return [on_sphere(tm._add(up, tm._add(tm._mul(a, radius * s * math.cos(2 * math.pi * k / 6.0 + 0.3)),
                                          tm._mul(b, radius * s * math.sin(2 * math.pi * k / 6.0 + 0.3)))))
            for k, s in enumerate(scale)]


class MatchesTheFirstMayaVersion(unittest.TestCase):
    """The core's root tubes (root_tubes.h, what groomlab.root_tubes now calls) against the first
    version, written in Maya's Python (kept as tests/hosts/maya/root_tubes_reference.py): the same
    outlines, paths and edits give the same guides, tubes and hierarchies."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)
        sys.path.insert(0, HERE)
        import root_tubes_reference
        cls.ref = root_tubes_reference

    def scene(self):
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=40, name="head")[0]
        return groomlab.create_groom(scalp)

    def points(self, groom, guide):
        return json.loads(cmds.groomLabGuides(groom, tubeFrames=str(guide), evaluated=True))["points"]

    def both(self, work):
        """`work(module, groom)` run on a groom of its own by each version: (theirs, ours)."""
        out = []
        for module in (self.ref, root_tubes):
            groom = self.scene()
            out.append((groom, work(module, groom)))
        return out

    def assert_same_tube(self, a, b, guide_a, guide_b, points=0.01):
        (ga, ma), (gb, mb) = a, b
        pa, pb = self.points(ga, guide_a), self.points(gb, guide_b)
        n = 40
        da = root_tubes._resample([tuple(p) for p in pa], n)
        db = root_tubes._resample([tuple(p) for p in pb], n)
        self.assertLess(max(tm._dist(x, y) for x, y in zip(da, db)), points, "the guide's course")
        ta, tb = groomlab.get_tube(ga, guide_a), groomlab.get_tube(gb, guide_b)
        self.assertAlmostEqual(ta["width"][0], tb["width"][0], delta=0.02 * ta["width"][0])
        self.assertAlmostEqual(ta["width"][-1], tb["width"][-1], delta=0.02 * ta["width"][0])
        self.assertAlmostEqual(ta["aspect"][0], tb["aspect"][0], delta=0.03)
        # The tube's root section on its outline, in both.
        for groom, guide, mod in ((ga, guide_a, self.ref), (gb, guide_b, root_tubes)):
            outline = mod.root_outline(groom, guide)
            self.assertLess(max(nearest(q, outline) for q in mod.section_outline(groom, guide)), 0.005)
        oa, ob = self.ref.root_outline(ga, guide_a), root_tubes.root_outline(gb, guide_b)
        # (the first version took CVs onto Maya's polygons, the core onto the triangles the hair roots
        # on: a sphere's quads are not flat, so the two lie up to a facet's sag apart)
        self.assertLess(max(tm._dist(x, y) for x, y in zip(oa, ob)), 5e-3, "the CVs kept")

    def test_a_straight_root_tube(self):
        outline = uneven((0.1, 1.0, 0.05), 0.12)
        a, b = self.both(lambda m, g: m.make_root_tube(g, outline, length=0.8, segments=10, taper=0.4))
        self.assert_same_tube(a, b, a[1], b[1])

    def test_a_drawn_root_tube(self):
        outline = uneven((0.0, 1.0, 0.0), 0.1)
        root = on_sphere((0.0, 1.0, 0.0))
        path = [root]
        for k in range(1, 50):  # a curl rising off the head, drawn a little shakily
            t = k / 49.0
            path.append((0.25 * math.sin(6.0 * t), 1.05 + 0.6 * t, 0.25 * (1.0 - math.cos(6.0 * t)) + 0.002 * (k % 3)))
        a, b = self.both(lambda m, g: m.make_root_tube(g, outline, path=path, density=1.5))
        self.assert_same_tube(a, b, a[1], b[1], points=0.02)

    def test_an_edited_root(self):
        outline = uneven((0.0, 1.0, 0.1), 0.1)

        def work(m, g):
            guide = m.make_root_tube(g, outline, length=0.6)
            cvs = m.root_outline(g, guide)
            c = m._centroid(cvs)
            cvs[2] = on_sphere(tm._add(c, tm._mul(tm._sub(cvs[2], c), 1.4)))
            m.edit_root_tube(g, guide, cvs)
            return guide
        a, b = self.both(work)
        self.assert_same_tube(a, b, a[1], b[1])

    def test_outlines_drawn_with_0_49_are_kept(self):
        """A root tube drawn by the first version (0.49.0: its CVs kept on the scene, not with its
        guide) keeps its CVs: read into its guide once, the scene's attribute dropped."""
        groom = self.scene()
        outline = uneven((0.0, 1.0, 0.0), 0.12)
        guide = self.ref.make_root_tube(groom, outline, length=0.6)
        drawn = self.ref.root_outline(groom, guide)
        self.assertTrue(cmds.attributeQuery("groomLabRootOutlines", node=groom, exists=True))
        kept = root_tubes.root_outline(groom, guide)
        self.assertLess(max(tm._dist(a, b) for a, b in zip(drawn, kept)), 5e-3, "the CVs as drawn")
        self.assertFalse(cmds.attributeQuery("groomLabRootOutlines", node=groom, exists=True))
        data = groomlab.get_hierarchy(groom)
        self.assertTrue(next(g for g in data["guides"] if int(g["id"]) == int(guide)).get("rootOutline"))

    def test_a_hierarchy_split_duplicated_and_trimmed(self):
        outline = uneven((0.0, 1.0, 0.0), 0.15)

        def work(m, g):
            guide = m.make_root_tube(g, outline, length=0.7)
            m.add_tube_level(g)
            m.add_tube_level(g)
            copy = m.duplicate_root_tube(g, guide, on_sphere((0.45, 0.85, 0.15)))
            data = groomlab.get_hierarchy(g)
            leaf = max(int(x["id"]) for x in data["guides"])
            m.remove_tube_level(g, m.ROOT_GROUP, [leaf])
            return guide, copy, m.tube_levels(g, picked=m.ROOT_GROUP)["levels"], len(groomlab.get_hierarchy(g)["guides"])
        a, b = self.both(work)
        (ga, (sa, ca, la, na)), (gb, (sb, cb, lb, nb)) = a, b
        self.assertEqual(la, lb, "the levels")
        self.assertEqual(na, nb, "the guides left")
        self.assert_same_tube((ga, None), (gb, None), sa, sb)
        # The copy: its outline the source's shape set down there, its tube on it, in both.
        oa, ob = self.ref.root_outline(ga, ca), root_tubes.root_outline(gb, cb)
        self.assertLess(max(tm._dist(x, y) for x, y in zip(oa, ob)), 5e-3, "the copy's CVs")
        for groom, guide, mod in ((ga, ca, self.ref), (gb, cb, root_tubes)):
            outline = mod.root_outline(groom, guide)
            self.assertLess(max(nearest(q, outline) for q in mod.section_outline(groom, guide)), 0.005)


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    maya.standalone.uninitialize()
    os._exit(0 if result.wasSuccessful() else 1)
