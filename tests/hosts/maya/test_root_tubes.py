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
        back = root_tubes.root_outline(self.groom, guide)
        self.assertEqual(len(back), len(outline))
        for p in outline:
            self.assertLess(nearest(p, back), 0.01, "a CV off the tube's root section")
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
        for p in root_tubes.root_outline(self.groom, guide):
            self.assertLess(nearest(p, outline), 0.015)

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
            back = root_tubes.root_outline(self.groom, guide)
            self.assertEqual(len(back), len(cvs))
            for p in cvs:
                self.assertLess(nearest(p, back), 0.002, "edit %d: a CV off where it was put" % step)

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
