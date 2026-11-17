# SPDX-License-Identifier: Apache-2.0
"""Maya integration tests: load the plugin in mayapy standalone and groom a sphere.

Usage: mayapy test_plugin.py <path/to/GroomLab.mll> <path/to/maya/scripts>
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

import groomlab  # noqa: E402


# The default graph before it took a look (Noise, HairColor, uneven Length): the cases that measure
# the shape the guides alone give, or edit that chain by its node names, start from it.
CLASSIC_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
              {"name": "scatter", "type": "Scatter"}, {"name": "interpolate", "type": "Interpolate"},
              {"name": "length", "type": "Length"}, {"name": "width", "type": "Width"},
              {"name": "resample", "type": "Resample"}, {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "interpolate", "input": "strands"},
                    {"from": "guides", "to": "interpolate", "input": "guides"},
                    {"from": "scalp", "to": "interpolate", "input": "scalp"},
                    {"from": "interpolate", "to": "length", "input": "strands"},
                    {"from": "length", "to": "width", "input": "strands"},
                    {"from": "width", "to": "resample", "input": "strands"},
                    {"from": "resample", "to": "output", "input": "strands"}],
}

SPHERE_AREA = 4.0 * math.pi  # radius 1; the polygon sphere is slightly smaller


def dist(a, b):  # math.dist needs Python 3.8; mayapy 2022 is 3.7
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


class PluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]

    def test_version_command(self):
        self.assertRegex(cmds.groomLabVersion(), r"^\d+\.\d+\.\d+$")

    def test_default_graph_scatters_on_scalp(self):
        shape = groomlab.create_groom(self.scalp)
        self.assertEqual(cmds.nodeType(shape), "groomLabShape")
        self.assertTrue(cmds.isConnected("time1.outTime", shape + ".time"))

        preview = cmds.getAttr(shape + ".outStrandCount")
        cmds.setAttr(shape + ".previewDensity", 1.0)
        full = cmds.getAttr(shape + ".outStrandCount")

        # Default density is 80 strands per unit area (light enough to preview every strand).
        self.assertGreater(full, 0.9 * 80 * SPHERE_AREA)
        self.assertLess(full, 1.02 * 80 * SPHERE_AREA)
        self.assertEqual(preview, full)  # the preview shows every strand by default

    def test_no_scalp_means_no_strands(self):
        shape = groomlab.create_groom()
        self.assertEqual(cmds.getAttr(shape + ".outStrandCount"), 0)

    def test_guide_curve_shapes_strands(self):
        shape = groomlab.create_groom(self.scalp)
        groomlab.set_graph(shape, CLASSIC_GRAPH)
        # Without guides strands stand along the normals, length 1.
        xmin, ymin, zmin, xmax, ymax, zmax = cmds.exactWorldBoundingBox(shape)
        self.assertAlmostEqual(xmax, 2.0, delta=0.1)

        # One guide from the top of the sphere, combed +X by 1 and up by 0.4.
        # Rooted with the tip end first: the plugin must flip it.
        curve = cmds.curve(degree=1, point=[(1.0, 1.4, 0.0), (0.5, 1.2, 0.0), (0.0, 1.0, 0.0)])
        groomlab.add_guides(shape, [curve])

        # The guide's offset (1, 0.4, 0) is rotated from the top normal onto
        # each strand's normal. On the equator at angle phi the tip lands at
        # x = 1.4 cos(phi) + sin(phi) and y = 1.4 sin(phi) - cos(phi); both
        # peak at sqrt(1.4^2 + 1) = 1.72. (World-space copying, as in M1, gave
        # xmax = 2 and ymax = 1.4.)
        peak = math.sqrt(1.4 ** 2 + 1)
        xmin, ymin, zmin, xmax, ymax, zmax = cmds.exactWorldBoundingBox(shape)
        self.assertAlmostEqual(xmax, peak, delta=0.05)
        self.assertAlmostEqual(ymax, peak, delta=0.05)

    def test_graph_attribute_drives_evaluation(self):
        shape = groomlab.create_groom(self.scalp)
        cmds.setAttr(shape + ".previewDensity", 1.0)
        base = cmds.getAttr(shape + ".outStrandCount")

        graph = groomlab.get_graph(shape)
        self.assertEqual(graph, {})
        graph = {
            "nodes": [
                {"name": "scalp", "type": "Scalp"},
                {"name": "scatter", "type": "Scatter", "params": {"density": 40}},
                {"name": "output", "type": "Output"},
            ],
            "connections": [
                {"from": "scalp", "to": "scatter", "input": "scalp"},
                {"from": "scatter", "to": "output", "input": "strands"},
            ],
            "output": "output",
        }
        groomlab.set_graph(shape, graph)
        self.assertAlmostEqual(cmds.getAttr(shape + ".outStrandCount") / float(base), 0.5, delta=0.05)

    def test_invalid_graph_reports_and_recovers(self):
        shape = groomlab.create_groom(self.scalp)
        cmds.setAttr(shape + ".graph", '{"nodes": []}', type="string")  # no output node
        self.assertEqual(cmds.getAttr(shape + ".outStrandCount"), 0)
        cmds.setAttr(shape + ".graph", "{}", type="string")
        self.assertGreater(cmds.getAttr(shape + ".outStrandCount"), 0)

    def test_alembic_export_round_trips_through_abcimport(self):
        if not hasattr(cmds, "groomLabExportAlembic"):  # checked after the plugin loads
            self.skipTest("built without Alembic")
        shape = groomlab.create_groom(self.scalp)
        groomlab.set_graph(shape, {
            "nodes": [
                {"name": "scalp", "type": "Scalp"},
                {"name": "scatter", "type": "Scatter", "params": {"density": 10}},
                {"name": "resample", "type": "Resample", "params": {"segments": 4, "mode": "even"}},
                {"name": "output", "type": "Output"},
            ],
            "connections": [
                {"from": "scalp", "to": "scatter", "input": "scalp"},
                {"from": "scatter", "to": "resample", "input": "strands"},
                {"from": "resample", "to": "output", "input": "strands"},
            ],
            "output": "output",
        })
        # Scalp moves +2 in X from frame 1 to 3; strands must follow. Pin the
        # time unit: a new scene may default to a different fps than this one.
        cmds.currentUnit(time="film")
        cmds.setKeyframe(self.scalp, attribute="translateX", time=1, value=0)
        cmds.setKeyframe(self.scalp, attribute="translateX", time=3, value=2)
        cmds.keyTangent(self.scalp, attribute="translateX", inTangentType="linear", outTangentType="linear")
        cmds.currentTime(1)
        cmds.setAttr(shape + ".previewDensity", 0.1)

        path = os.path.join(cmds.internalVar(userTmpDir=True), "groomlab_test.abc")
        frames = groomlab.export_alembic(shape, path, start=1, end=3)
        self.assertEqual(frames, 3)
        self.assertEqual(cmds.currentTime(query=True), 1)  # export must not move scene time

        expected = 10 * SPHERE_AREA  # density 1.0, not the 0.1 preview
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")
        cmds.loadPlugin("AbcImport", quiet=True)
        cmds.AbcImport(path, mode="import")
        curves = cmds.ls(type="nurbsCurve")
        self.assertGreater(len(curves), 0.85 * expected)
        self.assertLess(len(curves), 1.1 * expected)
        self.assertEqual(cmds.getAttr(curves[0] + ".degree"), 1)
        self.assertEqual(cmds.getAttr(curves[0] + ".spans"), 4)

        xs = []
        for frame in (3, 1, 2):
            cmds.currentTime(frame)
            xs.append(cmds.pointPosition(curves[0] + ".cv[0]")[0])
        x3, x1, x2 = xs
        self.assertAlmostEqual(x2 - x1, 1.0, delta=0.05)
        self.assertAlmostEqual(x3 - x1, 2.0, delta=0.05)

    def test_alembic_guides_match_connected_curves(self):
        if not hasattr(cmds, "groomLabExportAlembic"):
            self.skipTest("built without Alembic")
        cmds.currentUnit(time="film")
        cmds.loadPlugin("AbcExport", quiet=True)

        # Guides under a scaled group: the import must apply the xform.
        group = cmds.group(empty=True, name="guidesGrp")
        c1 = cmds.curve(degree=1, point=[(0, 1, 0), (0.5, 1.2, 0), (1.0, 1.3, 0.2)])
        c2 = cmds.curve(degree=1, point=[(-1.3, -1.4, 0), (-0.5, -1.2, 0), (0, -1, 0)])  # tip first
        cmds.parent([c1, c2], group)
        cmds.setAttr(group + ".scaleX", 1.5)

        direct = groomlab.create_groom(self.scalp, [c1, c2], name="directGroom")
        path = os.path.join(cmds.internalVar(userTmpDir=True), "groomlab_guides.abc").replace("\\", "/")
        cmds.AbcExport(j="-frameRange 1 1 -root |guidesGrp -file " + path)

        imported = groomlab.create_groom(self.scalp, name="importGroom")
        groomlab.import_guides_from_alembic(imported, path)
        graph = groomlab.get_graph(imported)
        self.assertIn("ImportCurves", [n["type"] for n in graph["nodes"]])

        expected = cmds.exactWorldBoundingBox(direct)
        actual = cmds.exactWorldBoundingBox(imported)
        for e, a in zip(expected, actual):
            self.assertAlmostEqual(e, a, delta=1e-3)
        self.assertEqual(cmds.getAttr(direct + ".outStrandCount"), cmds.getAttr(imported + ".outStrandCount"))

    # -- Guide hierarchy ---------------------------------------------------

    def _hierarchy_groom(self):
        """Six primaries around the top of the sphere, 8 children each, 4 grandchildren each."""
        curves = []
        for i in range(6):
            a = 2 * math.pi * i / 6
            x, z = 0.5 * math.cos(a), 0.5 * math.sin(a)
            y = math.sqrt(1 - 0.25)
            pts = [(x, y, z), (1.4 * x, y + 0.3, 1.4 * z), (2.0 * x, y + 0.2, 2.0 * z)]
            if i % 2:
                pts.reverse()  # drawn tip first
            curves.append(cmds.curve(degree=2, point=pts))
        shape = groomlab.create_groom(self.scalp)
        cmds.setAttr(shape + ".previewDensity", 1.0)
        ids = groomlab.guides_from_curves(shape, curves, tube_radius=0.15)
        self.assertEqual(len(ids), 6)
        self.assertEqual(groomlab.generate_children(shape, "primary", name="children", count=8), 48)
        # Last, so undo in the tests below takes back exactly this step.
        self.assertEqual(groomlab.generate_children(shape, "children", name="grandchildren", count=4), 192)
        return shape, curves

    def test_three_level_hierarchy(self):
        shape, curves = self._hierarchy_groom()
        groups = {g["name"]: g for g in groomlab.list_guide_groups(shape)}
        self.assertEqual(groups["primary"]["depth"], 0)
        self.assertEqual(groups["children"]["parent"], "primary")
        self.assertEqual(groups["grandchildren"]["depth"], 2)
        self.assertEqual(groups["grandchildren"]["count"], 192)
        self.assertEqual(len(groomlab.guide_ids(shape, "children")), 48)
        self.assertTrue(cmds.getAttr(shape + ".displayGuides"))

        # The curves were copied: deleting them changes nothing.
        before = cmds.exactWorldBoundingBox(shape)
        cmds.delete(curves)
        for a, b in zip(before, cmds.exactWorldBoundingBox(shape)):
            self.assertAlmostEqual(a, b, places=4)
        self.assertGreater(cmds.getAttr(shape + ".outStrandCount"), 1000)

        with self.assertRaises(RuntimeError):
            groomlab.generate_children(shape, "missing")
        with self.assertRaises(RuntimeError):
            groomlab.generate_children(shape, "children", name="primary")  # a group under another parent
        # The same child group again: every parent gets a fresh set (4 each now), nothing is added twice.
        groomlab.generate_children(shape, "primary", name="children", count=4)
        self.assertEqual(len(groomlab.guide_ids(shape, "children")), 24)

    def test_hierarchy_edits_undo_and_redo(self):
        cmds.undoInfo(state=True, infinity=True)
        shape, _ = self._hierarchy_groom()
        self.assertEqual(len(groomlab.list_guide_groups(shape)), 3)
        cmds.undo()  # grandchildren
        self.assertEqual([g["name"] for g in groomlab.list_guide_groups(shape)], ["primary", "children"])
        cmds.redo()
        self.assertEqual(len(groomlab.list_guide_groups(shape)), 3)

        groomlab.remove_guide_group(shape, "children")  # takes grandchildren with it
        self.assertEqual(len(groomlab.list_guide_groups(shape)), 1)
        cmds.undo()
        self.assertEqual(len(groomlab.list_guide_groups(shape)), 3)

    def test_lock_and_unlock(self):
        shape, _ = self._hierarchy_groom()
        child = groomlab.guide_ids(shape, "children")[0]
        groomlab.lock_guides(shape, [child])
        locked = [g for g in groomlab.get_hierarchy(shape)["guides"] if str(g["id"]) == child][0]
        self.assertTrue(locked.get("locked"))
        groomlab.unlock_guides(shape, [child])
        unlocked = [g for g in groomlab.get_hierarchy(shape)["guides"] if str(g["id"]) == child][0]
        self.assertFalse(unlocked.get("locked", False))
        with self.assertRaises(RuntimeError):
            groomlab.lock_guides(shape, [groomlab.guide_ids(shape, "primary")[0]])  # primaries do not lock

    def test_hierarchy_follows_moving_scalp_and_saves(self):
        shape, _ = self._hierarchy_groom()
        before = cmds.exactWorldBoundingBox(shape)
        cmds.setAttr(self.scalp + ".translate", 1.0, 2.0, 3.0)
        after = cmds.exactWorldBoundingBox(shape)
        for a, b, d in zip(before, after, (1, 2, 3, 1, 2, 3)):
            self.assertAlmostEqual(b - a, d, delta=1e-3)

        count = cmds.getAttr(shape + ".outStrandCount")
        path = os.path.join(cmds.internalVar(userTmpDir=True), "groomlab_hierarchy.mb")
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaBinary", force=True)
        cmds.file(new=True, force=True)
        cmds.file(path, open=True, force=True)
        self.assertEqual(len(groomlab.list_guide_groups("groomLab1Shape")), 3)
        self.assertEqual(cmds.getAttr("groomLab1Shape.outStrandCount"), count)
        for a, b in zip(after, cmds.exactWorldBoundingBox("groomLab1Shape")):
            self.assertAlmostEqual(a, b, delta=1e-3)

    def test_render_path_uses_hierarchy(self):
        if not hasattr(cmds, "groomLabExportAlembic"):
            self.skipTest("built without Alembic")
        cmds.currentUnit(time="film")
        shape, _ = self._hierarchy_groom()
        live = cmds.exactWorldBoundingBox(shape)
        path = os.path.join(cmds.internalVar(userTmpDir=True), "groomlab_hierarchy.abc")
        groomlab.export_alembic(shape, path)

        cmds.file(new=True, force=True)
        cmds.loadPlugin("AbcImport", quiet=True)
        cmds.AbcImport(path, mode="import")
        exported = cmds.exactWorldBoundingBox(cmds.ls(type="nurbsCurve"))
        for a, b in zip(live, exported):
            self.assertAlmostEqual(a, b, delta=1e-3)

    def test_xgen_cache_for_renderman_xpu(self):
        # Issue 82: RenderMan XPU renders no procedural, so GroomLab hands it an XGen Interactive
        # Groom fed by -xgen's spline cache, and one look for the whole groom (XGen's curves carry
        # no primvars).
        if not hasattr(cmds, "groomLabExportAlembic"):
            self.skipTest("built without Alembic")
        import json
        shape, _ = self._hierarchy_groom()
        path = os.path.join(cmds.internalVar(userTmpDir=True), "groomlab_xpu.abc").replace("\\", "/")
        result = json.loads(cmds.groomLabExportAlembic(shape, file=path, xgen=True, renderOverrides=True))
        self.assertGreater(result["splines"], 100)
        look = result["look"]
        for key in ("melanin", "redness", "dye", "color", "rgbShare", "specularGainR", "coneAngleR", "coneAngleTT",
                    "coneAngleTRT", "specularOffset"):
            self.assertIn(key, look)
        self.assertEqual(len(look["dye"]), 3)
        self.assertGreater(look["coneAngleR"], 0.0)
        with self.assertRaises(RuntimeError):  # one frame
            cmds.groomLabExportAlembic(shape, file=path, xgen=True, startFrame=1, endFrame=3)
        with self.assertRaises(RuntimeError):  # XGen's layout or an engine's, not both
            cmds.groomLabExportAlembic(shape, file=path, xgen=True, unreal=True)
        try:
            cmds.loadPlugin("xgenToolkit", quiet=True)
        except RuntimeError:
            self.skipTest("XGen is not installed")
        import maya.mel
        maya.mel.eval('xgmSplineCache -import -file "%s"' % path)
        descriptions = cmds.ls(type="xgmSplineDescription")
        self.assertEqual(len(descriptions), 1)
        self.assertEqual(cmds.xgmSplineQuery(descriptions[0], splineCount=True), result["splines"])

    # -- Brushes -----------------------------------------------------------

    def _guide_points(self, shape, group):
        """{id: [(x, y, z), ...]} for a group, from the stored hierarchy (world points)."""
        h = groomlab.get_hierarchy(shape)
        gid = [g["id"] for g in h["groups"] if g["name"] == group][0]
        out = {}
        for g in h["guides"]:
            if g["group"] == gid and "points" in g:
                p = g["points"]
                out[str(g["id"])] = [tuple(p[i:i + 3]) for i in range(0, len(p), 3)]
        return out

    def test_comb_stroke_is_one_undo_step(self):
        cmds.undoInfo(state=True, infinity=True)
        shape, _ = self._hierarchy_groom()
        before_data = cmds.getAttr(shape + ".guideData")
        before = self._guide_points(shape, "primary")
        bbox_before = cmds.exactWorldBoundingBox(shape)

        # Comb every primary toward +X, sweeping across the top of the head.
        groomlab.brush_stroke(shape, "comb", [(0, 1.3, 0), (0.6, 1.3, 0)], radius=1.5)
        after = self._guide_points(shape, "primary")
        moved = [gid for gid in before if after[gid][-1][0] > before[gid][-1][0] + 0.05]
        self.assertGreaterEqual(len(moved), 3)
        for gid in before:  # roots stay put
            self.assertAlmostEqual(dist(before[gid][0], after[gid][0]), 0.0, places=4)
        self.assertNotEqual(cmds.exactWorldBoundingBox(shape), bbox_before)  # strands followed

        cmds.undo()
        self.assertEqual(cmds.getAttr(shape + ".guideData"), before_data)
        cmds.redo()
        self.assertNotEqual(cmds.getAttr(shape + ".guideData"), before_data)

    def test_brush_levels_and_modes(self):
        shape, _ = self._hierarchy_groom()
        primaries = self._guide_points(shape, "primary")

        # Children only: primaries do not move.
        groomlab.brush_stroke(shape, "comb", [(0, 1.3, 0), (0, 1.3, 0.6)], radius=1.5, group="children")
        self.assertEqual(self._guide_points(shape, "primary"), primaries)

        # Cut every primary by 20%.
        def length(pts):
            return sum(dist(a, b) for a, b in zip(pts, pts[1:]))
        lengths = {gid: length(p) for gid, p in primaries.items()}
        groomlab.brush_stroke(shape, "length", [(0, 1.0, 0)], radius=100.0, strength=-4.0)  # huge radius: weight ~1
        for gid, p in self._guide_points(shape, "primary").items():
            self.assertAlmostEqual(length(p), 0.8 * lengths[gid], delta=1e-3)

        # Place two new primaries.
        groomlab.brush_stroke(shape, "place", [(0, -1.2, 0), (1.2, 0, 0)], length=0.4)
        self.assertEqual(len(groomlab.guide_ids(shape, "primary")), 8)

        # The groom's transform works as well as its shape (regression: the
        # transform was rejected, which also broke the interactive tool).
        transform = cmds.listRelatives(shape, parent=True)[0]
        groomlab.brush_stroke(transform, "place", [(1.2, 0, 0.3)], length=0.4)
        self.assertEqual(len(groomlab.guide_ids(shape, "primary")), 9)

        with self.assertRaises(ValueError):
            groomlab.brush_stroke(shape, "bogus", [(0, 1, 0)])
        with self.assertRaises(RuntimeError):
            cmds.groomLabBrushStroke(shape, mode="comb", interactive=True)

    def test_brush_context_settings(self):
        ctx = cmds.groomLabBrushCtx("groomLabTestCtx")
        if not ctx:  # standalone Maya creates no contexts
            self.skipTest("contexts need the Maya UI")
        cmds.groomLabBrushCtx(ctx, edit=True, mode="smooth", radius=80, strength=0.5, level=1, propagate=True)
        self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, mode=True), "smooth")
        self.assertAlmostEqual(cmds.groomLabBrushCtx(ctx, query=True, radius=True), 80.0)
        self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, level=True), 1)
        self.assertTrue(cmds.groomLabBrushCtx(ctx, query=True, propagate=True))
        with self.assertRaises(RuntimeError):
            cmds.groomLabBrushCtx(ctx, edit=True, mode="paint")

    def test_look_operators_in_maya(self):
        shape, _ = self._hierarchy_groom()
        graph = groomlab.get_graph(shape, expand_default=True)
        nodes = {n["name"]: n for n in graph["nodes"]}
        graph["nodes"] += [
            {"name": "clump", "type": "Clump", "params": {"count": 60, "strength": 0.8, "profile": [[0, 0], [1, 1]]}},
            {"name": "curl", "type": "Curl", "params": {"radius": 0.02}},
            {"name": "wiggle", "type": "Noise", "params": {"amplitude": 0.02}},
            {"name": "melanin", "type": "Attribute", "params": {"name": "melanin"}},
        ]
        self.assertIn("interpolate", nodes)
        # interpolate -> clump (+ centers) -> curl -> noise -> melanin -> length ...
        graph["connections"] = [c for c in graph["connections"] if not (c["from"] == "interpolate" and c["to"] == "length")]
        graph["connections"] += [
            {"from": "interpolate", "to": "clump", "input": "strands"},
            {"from": "clump", "to": "curl", "input": "strands"},
            {"from": "curl", "to": "wiggle", "input": "strands"},
            {"from": "wiggle", "to": "melanin", "input": "strands"},
            {"from": "melanin", "to": "length", "input": "strands"},
        ]
        count = cmds.getAttr(shape + ".outStrandCount")
        before = cmds.exactWorldBoundingBox(shape)
        groomlab.set_graph(shape, graph)
        self.assertEqual(cmds.getAttr(shape + ".outStrandCount"), count)  # same strands, new shapes
        self.assertNotEqual(cmds.exactWorldBoundingBox(shape), before)

    # -- Maps --------------------------------------------------------------

    def test_painted_color_set_drives_density(self):
        cmds.polyColorSet(self.scalp, create=True, colorSet="density")
        cmds.polyColorSet(self.scalp, currentColorSet=True, colorSet="density")
        cmds.polyColorPerVertex(self.scalp + ".vtx[*]", rgb=(0, 0, 0))
        verts = cmds.ls(self.scalp + ".vtx[*]", flatten=True)
        right = [v for v in verts if cmds.pointPosition(v, world=True)[0] > 0.0]
        cmds.polyColorPerVertex(right, rgb=(1, 1, 1))

        shape = groomlab.create_groom(self.scalp)
        groomlab.set_graph(shape, CLASSIC_GRAPH)  # roots where painted, no noise moving tips across
        cmds.setAttr(shape + ".previewDensity", 1.0)
        full = cmds.getAttr(shape + ".outStrandCount")
        groomlab.set_param(shape, "scatter", "density", groomlab.color_set_map("density", max=80))
        self.assertAlmostEqual(cmds.getAttr(shape + ".outStrandCount") / float(full), 0.5, delta=0.08)
        self.assertGreater(cmds.exactWorldBoundingBox(shape)[0], -0.3)  # nothing on the left

    def test_texture_drives_density(self):
        # White top half: the sphere's UVs run v = 0 (bottom) to 1 (top).
        path = os.path.join(cmds.internalVar(userTmpDir=True), "groomlab_density.pgm")
        with open(path, "wb") as f:
            f.write(b"P5\n32 32\n255\n" + bytes([255 if y < 16 else 0 for y in range(32) for _ in range(32)]))
        shape = groomlab.create_groom(self.scalp)
        cmds.setAttr(shape + ".previewDensity", 1.0)
        groomlab.set_param(shape, "scatter", "density", groomlab.texture_map(path, max=100))
        count = cmds.getAttr(shape + ".outStrandCount")
        self.assertAlmostEqual(count / (100 * SPHERE_AREA), 0.5, delta=0.08)
        self.assertGreater(cmds.exactWorldBoundingBox(shape)[1], -0.3)  # nothing below the equator

        with self.assertRaises(ValueError):
            groomlab.set_param(shape, "nope", "density", 1.0)

    def test_moving_scalp_moves_strands(self):
        # worldMesh data carries its transform separately from the points.
        shape = groomlab.create_groom(self.scalp)
        x0 = cmds.exactWorldBoundingBox(shape)[0]
        cmds.setAttr(self.scalp + ".translateX", 3.0)
        self.assertAlmostEqual(cmds.exactWorldBoundingBox(shape)[0] - x0, 3.0, delta=0.01)

        # Guide transforms count too. Translation is undone by rooting, so
        # scale the guide: its offset doubles from 1 to 2 units.
        curve = cmds.curve(degree=1, point=[(3, 1, 0), (3, 2, 0)])
        groomlab.add_guides(shape, [curve])
        ymax = cmds.exactWorldBoundingBox(shape)[4]
        cmds.setAttr(curve + ".scaleY", 2.0)
        self.assertAlmostEqual(cmds.exactWorldBoundingBox(shape)[4] - ymax, 1.0, delta=0.05)

    def test_graph_is_saved_with_scene(self):
        shape = groomlab.create_groom(self.scalp)
        groomlab.set_graph(shape, {"version": 1, "nodes": [{"name": "o", "type": "Output"}], "output": "o"})
        path = os.path.join(cmds.internalVar(userTmpDir=True), "groomlab_test.ma")
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        cmds.file(new=True, force=True)
        cmds.file(path, open=True, force=True)
        self.assertEqual(groomlab.get_graph("groomLab1Shape")["output"], "o")


@unittest.skipUnless(sys.platform == "win32", "the crash log's exception filter is Windows only")
class CrashLogTest(unittest.TestCase):
    """The crash log (crash_log.cpp) in a mayapy that crashes on purpose (crash_log_child.py)."""

    def crash(self, mode):
        import shutil
        import subprocess
        import tempfile
        work = tempfile.mkdtemp(prefix="groomlab_crash_")
        self.addCleanup(shutil.rmtree, work, True)
        env = dict(os.environ, TMP=work, TEMP=work, MAYA_DEBUG_ENABLE_CRASH_REPORTING="0")
        child = os.path.join(os.path.dirname(os.path.abspath(__file__)), "crash_log_child.py")
        result = subprocess.run([sys.executable, child, PLUGIN_PATH, work, mode], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, universal_newlines=True, timeout=300)
        markers = set(name for name in os.listdir(work) if "." not in name)
        logs = [name for name in os.listdir(work) if name.startswith("GroomLabCrash_")]
        text = open(os.path.join(work, logs[0])).read() if logs else ""
        return result, markers, logs, text

    def test_a_fault_is_logged_with_its_module_and_versions_then_handed_on(self):
        # A tester's crash in Arnold (ai.dll+0x78d60c on a render thread) read as GroomLab's: the log
        # now names the faulting module on its own line, the versions, and the faulting thread's
        # stack from the fault, and Maya's handler (here a stand-in) still runs after it.
        result, markers, logs, text = self.crash("fault")
        self.assertNotEqual(result.returncode, 0, result.stdout[-2000:])
        self.assertIn("faulting", markers, result.stdout[-2000:])
        self.assertNotIn("survived", markers)
        self.assertIn("maya_handler", markers, "the filter before the plugin's did not run")
        self.assertEqual(len(logs), 1, logs)
        self.assertIn("access violation 0xc0000005", text)
        self.assertIn("(reading 0x0)", text)
        self.assertRegex(text, r"faulting module: ucrtbase\.dll\+0x[0-9a-f]+ \(")
        self.assertRegex(text, r"GroomLab \d+\.\d+")
        self.assertRegex(text, r"running in .*20\d\d")
        if "mtoa" in markers:  # MtoA loaded in the child: its version and Arnold's
            self.assertRegex(text, r"\nMtoA \d+\.\d+")
            self.assertRegex(text, r"\nArnold \(ai\.dll\) \d+\.\d+")
        self.assertIn("stack of the faulting thread, from the fault:", text)
        self.assertRegex(text, r"\n 0  ucrtbase\.dll\+0x")
        self.assertRegex(text, r"\n 1  KERNEL32\.DLL\+0x|\n 1  kernel32\.dll\+0x", text)

    def test_an_unloaded_plugin_still_hands_on(self):
        # A filter set after the plugin's (Arnold's) keeps calling it after the plugin unloads: it
        # must still be there, handing on to the one before it, and write no log of its own.
        result, markers, logs, text = self.crash("unloaded")
        self.assertIn("unloaded", markers, result.stdout[-2000:])
        self.assertIn("later_handler", markers, result.stdout[-2000:])
        self.assertIn("maya_handler", markers, "the plugin's filter did not hand on after an unload")
        self.assertEqual(logs, [])


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
