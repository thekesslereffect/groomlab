# SPDX-License-Identifier: Apache-2.0
"""Catmull-Clark on the scalp in Maya (engine/core/include/groomlab/subdivision.h): match render
from the mesh's Arnold subdivision or smooth preview, the level in groom files, our refined
surface against Maya's own smooth mesh (MFnMesh.generateSmoothMesh, OpenSubdiv) at the same level,
and parts and flow staying where they were drawn as the Scalp node's level changes.

Usage: mayapy test_subdivision.py <GroomLab.mll> <maya/scripts dir>
"""

import bisect
import json
import math
import os
import sys
import tempfile
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402
import maya.api.OpenMaya as om  # noqa: E402
import maya.OpenMaya as om1  # noqa: E402

import groomlab  # noqa: E402
import groomlab_core  # noqa: E402

RADIUS = 10.0

GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 1.0}},
              {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "output", "input": "strands"}],
}


# Short fur laid along the flow, a few points a strand.
FLOW_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp", "params": {"subdivision": "off"}},
              {"name": "scatter", "type": "Scatter", "params": {"density": 20.0, "length": 0.6}},
              {"name": "flow", "type": "Flow", "params": {"lift": 10.0, "bend": 0.0, "rollRandom": 0.0}},
              {"name": "resample", "type": "Resample", "params": {"segments": 3}},
              {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "flow", "input": "strands"},
                    {"from": "flow", "to": "resample", "input": "strands"},
                    {"from": "resample", "to": "output", "input": "strands"}],
}


# Scatter -> a Clump picking 30 centers of its own -> another picking 20; the Scalp node at match
# render (its default), so Maya's smooth mesh preview refines it.
CLUMP_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"},
              {"name": "scatter", "type": "Scatter", "params": {"density": 0.5, "randomness": 0.5}},
              {"name": "clump", "type": "Clump", "params": {"count": 30, "clumpChannel": "clumpId"}},
              {"name": "own", "type": "Clump", "params": {"clumpBy": "count", "count": 20, "clumpChannel": "ownClump"}},
              {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "clump", "input": "strands"},
                    {"from": "clump", "to": "own", "input": "strands"},
                    {"from": "own", "to": "output", "input": "strands"}],
}


def on_head(theta, phi):
    """A point over the head at polar angle theta from the crown, phi round it (degrees)."""
    t, f = math.radians(theta), math.radians(phi)
    return (1.3 * RADIUS * math.sin(t) * math.cos(f), 1.3 * RADIUS * math.cos(t), 1.3 * RADIUS * math.sin(t) * math.sin(f))


def overlays(groom):
    """The parts and flow the viewport draws (groomLabGraphStats -overlays)."""
    return json.loads(cmds.groomLabGraphStats(groom, overlays=True))


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def degrees_between(a, b):
    la, lb = math.sqrt(sum(x * x for x in a)), math.sqrt(sum(x * x for x in b))
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b)) / (la * lb)))))


def mesh_fn(node):
    sel = om.MSelectionList()
    sel.add(node)
    return om.MFnMesh(sel.getDagPath(0))


class MayaSmooth(object):
    """Maya's own smooth mesh (MFnMesh.generateSmoothMesh: OpenSubdiv Catmull-Clark, uniform,
    sharp edges and corners at the border, UVs smoothed with `uv_boundary`): its points, UVs and
    the distance from a point to it. The API 1 classes: API 2's options lack the OpenSubdiv rules."""

    def __init__(self, node, levels, uv_boundary):
        sel = om1.MSelectionList()
        sel.add(node)
        dag = om1.MDagPath()
        sel.getDagPath(0, dag)
        opts = om1.MMeshSmoothOptions()
        opts.setDivisions(levels)
        opts.setSubdivisionType(om1.MMeshSmoothOptions.kOpenSubdivCatmullClarkUniform)
        opts.setOpenSubdivVertexBoundary(om1.MMeshSmoothOptions.kSharpEdgesAndCorners)
        opts.setOpenSubdivFaceVaryingBoundary(uv_boundary)
        opts.setSmoothUVs(True)
        self.data = om1.MFnMeshData().create()
        om1.MFnMesh(dag).generateSmoothMesh(self.data, opts)
        self.mesh = om1.MFnMesh(self.data)
        self.intersector = om1.MMeshIntersector()
        self.intersector.create(self.data, om1.MMatrix())
        points = om1.MPointArray()
        self.mesh.getPoints(points)
        self.points = [(points[i].x, points[i].y, points[i].z) for i in range(points.length())]
        us, vs = om1.MFloatArray(), om1.MFloatArray()
        self.mesh.getUVs(us, vs)
        self.uvs = [(us[i], vs[i]) for i in range(us.length())]

    def distance(self, p):
        hit = om1.MPointOnMesh()
        self.intersector.getClosestPoint(om1.MPoint(p[0], p[1], p[2]), hit)
        q = om1.MPoint(hit.getPoint())
        return math.sqrt((q.x - p[0]) ** 2 + (q.y - p[1]) ** 2 + (q.z - p[2]) ** 2)


def notes(groom):
    return json.loads(cmds.groomLabGraphStats(groom)).get("notes", {})


class SubdivisionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        # A head-like cap: an uneven sphere of quads with the pole's triangles, its lower faces cut
        # away (a border for the boundary rules), squashed and pushed out of round, frozen.
        self.scalp = cmds.polySphere(radius=RADIUS, subdivisionsX=24, subdivisionsY=16, name="head")[0]
        fn = mesh_fn(self.scalp)
        points = fn.getPoints()
        low = [f for f in range(fn.numPolygons)
               if sum(points[v].y for v in fn.getPolygonVertices(f)) / len(fn.getPolygonVertices(f)) < -0.35 * RADIUS]
        cmds.delete(["%s.f[%d]" % (self.scalp, f) for f in low])
        cmds.move(0, 1.5, 1.0, "%s.vtx[100:130]" % self.scalp, relative=True)
        cmds.scale(1.0, 1.15, 0.9, self.scalp)
        cmds.makeIdentity(self.scalp, apply=True, translate=True, rotate=True, scale=True)
        cmds.delete(self.scalp, constructionHistory=True)
        self.shape = cmds.listRelatives(self.scalp, shapes=True)[0]
        # MtoA's attributes on the mesh, as MtoA makes them (it is not loaded in mayapy).
        cmds.addAttr(self.shape, longName="aiSubdivType", attributeType="enum", enumName="none:catclark:linear")
        cmds.addAttr(self.shape, longName="aiSubdivIterations", attributeType="byte", defaultValue=1)
        self.groom = groomlab.create_groom(self.scalp)
        groomlab.set_graph(self.groom, GRAPH)

    def test_match_render_reads_arnold_then_the_smooth_preview(self):
        self.assertNotIn("scalp", notes(self.groom))  # neither: the mesh as it is
        cmds.setAttr(self.shape + ".aiSubdivType", 1)
        cmds.setAttr(self.shape + ".aiSubdivIterations", 2)
        note = notes(self.groom).get("scalp", "")
        self.assertIn("Catmull-Clark x2", note)
        self.assertIn("Arnold catclark, 2 iterations", note)
        # Arnold off: the smooth mesh preview, when it is on.
        cmds.setAttr(self.shape + ".aiSubdivType", 0)
        self.assertNotIn("scalp", notes(self.groom))
        cmds.setAttr(self.shape + ".displaySmoothMesh", 2)
        cmds.setAttr(self.shape + ".smoothLevel", 1)
        note = notes(self.groom).get("scalp", "")
        self.assertIn("Catmull-Clark x1", note)
        self.assertIn("smooth mesh preview, level 1", note)
        # Arnold's linear subdivision keeps the cage's surface whatever the preview shows.
        cmds.setAttr(self.shape + ".aiSubdivType", 2)
        self.assertNotIn("scalp", notes(self.groom))
        # The node's own level wins over match render.
        cmds.setAttr(self.shape + ".aiSubdivType", 1)
        groomlab.set_param(self.groom, "scalp", "subdivision", "off")
        self.assertNotIn("scalp", notes(self.groom))
        groomlab.set_param(self.groom, "scalp", "subdivision", "3")
        self.assertIn("Catmull-Clark x3", notes(self.groom).get("scalp", ""))

    def test_colliders_match_render(self):
        body = cmds.polySphere(radius=4, subdivisionsX=12, subdivisionsY=8, name="body")[0]
        cmds.setAttr(cmds.listRelatives(body, shapes=True)[0] + ".displaySmoothMesh", 2)
        groomlab.add_collider(self.groom, body)
        note = notes(self.groom).get("body", "")
        self.assertIn("Catmull-Clark x2", note)
        self.assertIn("smooth mesh preview, level 2", note)

    def test_our_surface_against_mayas_smooth_mesh(self):
        cmds.setAttr(self.shape + ".aiSubdivType", 1)
        folder = tempfile.mkdtemp()
        for levels in (1, 2):
            cmds.setAttr(self.shape + ".aiSubdivIterations", levels)
            path = os.path.join(folder, "head%d.groom" % levels)
            groomlab.export_groom(self.groom, path)
            # The groom file keeps the polygons and the level: what a farm render and the app read.
            groom_file = groomlab_core.read_groom_file(path)
            scalp = groom_file.data.scalp
            self.assertEqual(scalp.render_subdivision, levels)
            self.assertEqual(len(scalp.polygons), mesh_fn(self.scalp).numPolygons)
            session = groomlab_core.GroomSession(groom_file.data)
            self.assertEqual(session.scalp_subdivision, levels)
            ours = session.scalp
            self.assertEqual(ours.subdivision_level, levels)
            smooth = MayaSmooth(self.scalp, levels, om1.MMeshSmoothOptions.kSharpEdgesAndCorners)
            self.assertEqual(ours.point_count, smooth.mesh.numVertices())
            self.assertEqual(ours.triangle_count, 2 * smooth.mesh.numPolygons())
            # Every vertex of each on the other's surface.
            mine = [p.to_tuple() for p in ours.points]
            to_maya = [smooth.distance(p) for p in mine]
            to_ours = []
            for q in smooth.points:
                hit = ours.closest_point(groomlab_core.Vec3(*q))
                to_ours.append((hit.position - groomlab_core.Vec3(*q)).length())
            same_order = max((groomlab_core.Vec3(*q) - p).length() for q, p in zip(smooth.points, ours.points))
            worst = max(max(to_maya), max(to_ours))
            mean = (sum(to_maya) + sum(to_ours)) / (len(to_maya) + len(to_ours))
            # Roots: on Maya's smooth surface, and how far the cage is from it.
            strands = session.evaluate(1.0, 0.0)
            offsets = strands.offsets
            positions = strands.positions
            roots = [positions[offsets[s]].to_tuple() for s in range(strands.strand_count)]
            heights = [smooth.distance(r) for r in roots]
            cage = groomlab_core.GroomSession(groom_file.data)
            cage.set_graph_json(json.dumps(dict(GRAPH, nodes=[dict(GRAPH["nodes"][0], params={"subdivision": "off"})]
                                                + GRAPH["nodes"][1:])))
            cage_strands = cage.evaluate(1.0, 0.0)
            cage_heights = [smooth.distance(cage_strands.positions[cage_strands.offsets[s]].to_tuple())
                            for s in range(cage_strands.strand_count)]
            print("[ subdivision ] level %d: %d vertices; vertices to the other surface: max %.2e, mean %.2e "
                  "(radius %.0f; vertex by vertex in order: max %.2e); %d roots off Maya's surface: max %.2e, "
                  "mean %.2e (on the cage: max %.2e, mean %.2e)"
                  % (levels, len(mine), worst, mean, RADIUS, same_order, len(heights), max(heights),
                     sum(heights) / len(heights), max(cage_heights), sum(cage_heights) / len(cage_heights)))
            # The vertices agree to float rounding. Between them each refined quad is two triangles:
            # ours split (a, b, c) (a, c, d) on every frame, Maya's intersector across the shorter
            # diagonal, so a root sits within a quad's bend of Maya's surface (smaller each level),
            # far closer than the cage's.
            self.assertLess(worst, 1e-5 * RADIUS)
            self.assertLess(sum(heights) / len(heights), 1e-4 * RADIUS)
            self.assertLess(max(heights), 0.25 * max(cage_heights))
            self.assertGreater(sum(cage_heights) / len(cage_heights), 50 * sum(heights) / len(heights))
            # The UVs (pin corners: smoothed, the UV layout's corners kept), against Maya's with each
            # of its UV boundary rules: ours are among the rule's that matches.
            self.assertTrue(ours.has_uvs)
            ours_uvs = sorted(set(tuple(uv) for uv in ours.corner_uvs))
            best = None
            for rule in ("kNone", "kSharpEdgesAndCorners", "kSharpEdges", "kAlwaysSharp"):
                theirs = sorted(MayaSmooth(self.scalp, levels, getattr(om1.MMeshSmoothOptions, rule)).uvs)
                keys = [uv[0] for uv in theirs]
                missing = 0
                for u, v in ours_uvs:
                    at = bisect.bisect_left(keys, u - 1e-5)
                    found = False
                    while at < len(theirs) and theirs[at][0] <= u + 1e-5 and not found:
                        found = abs(theirs[at][1] - v) <= 1e-5
                        at += 1
                    missing += 0 if found else 1
                print("[ subdivision ] level %d: UVs against Maya's %s: %d of ours, %d of Maya's, %d of ours not "
                      "among Maya's (to 1e-5)" % (levels, rule, len(ours_uvs), len(theirs), missing))
                best = missing if best is None else min(best, missing)
            self.assertEqual(best, 0)

    def test_smooth_preview_keeps_the_strands_and_their_clumps(self):
        # Pressing 3 (smooth mesh preview) refines the scalp the hair grows on: the strands move onto
        # the smooth surface, but they are the same strands, with the same clump centers and each in
        # the same clump (placed and chosen on the cage).
        from groomlab import utils
        groomlab.set_graph(self.groom, CLUMP_GRAPH)
        cmds.setAttr(self.groom + ".previewDensity", 1.0)  # the viewport's centers are the render's

        def layout():
            made = utils.strands_to_curves(self.groom, max_curves=100000)
            strands = {}
            for transform in made["curves"]:
                shape = cmds.listRelatives(transform, shapes=True, fullPath=True)[0]
                strands[cmds.getAttr(shape + ".glStrandId")] = (cmds.getAttr(shape + ".glClumpId"),
                                                                cmds.getAttr(shape + ".glOwnClump"),
                                                                tuple(cmds.pointPosition(shape + ".cv[0]", world=True)))
            cmds.delete(made["group"])
            cmds.setAttr(self.groom + ".viewNode", "clump", type="string")
            centers = overlays(self.groom)["clumpCenters"]
            cmds.setAttr(self.groom + ".viewNode", "", type="string")
            return strands, centers

        cage, cage_centers = layout()
        self.assertGreater(len(cage), 200)
        self.assertGreater(len(cage_centers), 10)  # about the count asked (30), as many as the spacing lets in
        worst = 0.0
        for level in (1, 2):
            cmds.setAttr(self.shape + ".displaySmoothMesh", 2)
            cmds.setAttr(self.shape + ".smoothLevel", level)
            self.assertIn("Catmull-Clark x%d" % level, notes(self.groom).get("scalp", ""))
            smooth, smooth_centers = layout()
            self.assertEqual(sorted(smooth), sorted(cage))  # the same strands (IDs)
            self.assertEqual({k: v[:2] for k, v in smooth.items()}, {k: v[:2] for k, v in cage.items()})
            self.assertEqual(len(smooth_centers), len(cage_centers))
            for a, b in zip(smooth_centers, cage_centers):
                self.assertLess(distance(a, b), 0.1 * RADIUS)
            worst = max([worst] + [distance(smooth[k][2], cage[k][2]) for k in cage])
            self.assertLess(worst, 0.1 * RADIUS, "level %d" % level)
            cmds.setAttr(self.shape + ".displaySmoothMesh", 0)
            again, _ = layout()
            self.assertEqual(again, cage)  # the cage's strands again, to the bit
        print("[ subdivision ] smooth preview: %d strands, %d centers kept; roots moved up to %.3f (radius %.0f)"
              % (len(cage), len(cage_centers), worst, RADIUS))

    def test_parts_and_flow_stay_where_they_were_drawn(self):
        # Parts and flow are stored on the mesh as given (the cage) whatever the Scalp node's level,
        # and shown carried onto the refined scalp: changing the level leaves them where they were
        # drawn (the same places of the surface), and the fur the flow lays with them.
        groomlab.set_graph(self.groom, FLOW_GRAPH)
        triangles = sum(mesh_fn(self.scalp).getTriangles()[0])
        # A part from a curve (bound on the cage), one drawn and the arrows laid at level 2 (on the
        # scalp as shown), a whorl placed at level 0.
        curve = cmds.curve(degree=1, point=[on_head(15 + 3 * k, 30) for k in range(14)])
        groomlab.add_part(self.groom, [curve], name="crown", delete_curves=True)
        groomlab.set_param(self.groom, "scalp", "subdivision", "2")
        groomlab.brush_stroke(self.groom, "part", [on_head(20 + 3 * k, 150) for k in range(12)], part_name="side")
        groomlab.flow_stroke(self.groom, [on_head(25 + 4 * k, 260 + 2 * k) for k in range(10)], radius=1.5)
        groomlab.set_param(self.groom, "scalp", "subdivision", "off")
        groomlab.flow_sink(self.groom, on_head(40, 330), kind="whorl", radius=3.0)
        parts = json.loads(cmds.getAttr(self.groom + ".partData"))["parts"]
        flow = json.loads(cmds.getAttr(self.groom + ".flowData"))
        self.assertEqual(sorted(p["name"] for p in parts), ["crown", "side"])
        self.assertGreater(len(flow["pins"]), 4)
        self.assertEqual(len(flow["sinks"]), 1)
        faces = [pt[0] for p in parts for pt in p["points"]] + [x["face"] for x in flow["pins"] + flow["sinks"]]
        self.assertLess(max(faces), triangles)  # all on the cage's triangles

        def shown(level):
            groomlab.set_param(self.groom, "scalp", "subdivision", level)
            o = overlays(self.groom)
            xyz = groomlab.viewport_points(self.groom)
            o["strands"] = [xyz[i:i + 4] for i in range(0, len(xyz), 4)]  # 3 segments each
            return o

        off = shown("off")
        self.assertEqual(len(off["parts"]), 2)
        self.assertEqual(len(off["pins"]), len(flow["pins"]))
        self.assertEqual(len(off["sinks"]), 1)
        self.assertGreater(len(off["strands"]), 200)

        def heading_near(o, i):
            """The fur's mean heading (root to tip) within reach of pin i."""
            near = [s for s in o["strands"] if distance(s[0], o["pins"][i]) < 1.5]
            self.assertTrue(near)
            return [sum(s[-1][k] - s[0][k] for s in near) for k in range(3)]

        worst = {}
        for level in ("1", "2", "3"):
            on = shown(level)
            # A point keeps its place on the surface, not in space: the cage of this lumpy head lies
            # over half a unit off its smooth surface in places (as the test above measures), so a
            # cage point and where it refines to are up to ~0.7 apart (printed). Bound to the
            # refined triangles by the cage's numbers (the bug), they landed anywhere on the head.
            for a, b in zip(on["parts"], off["parts"]):
                self.assertEqual(len(a), len(b))
                worst[level] = max([worst.get(level, 0.0)] + [distance(p, q) for p, q in zip(a, b)])
            for key in ("pins", "sinks"):
                self.assertEqual(len(on[key]), len(off[key]))
                worst[level] = max([worst[level]] + [distance(p, q) for p, q in zip(on[key], off[key])])
            for i, (a, b) in enumerate(zip(on["directions"], off["directions"])):
                self.assertLess(degrees_between(a, b), 10.0, "level %s, pin %d" % (level, i))
                self.assertLess(degrees_between(heading_near(on, i), heading_near(off, i)), 15.0, "level %s, fur at pin %d"
                                % (level, i))
            self.assertLess(worst[level], 0.1 * RADIUS, "level %s" % level)
        print("[ subdivision ] parts and flow, farthest from where they are at level 0: %s (radius %.0f)"
              % (", ".join("level %s %.3f" % (k, v) for k, v in sorted(worst.items())), RADIUS))
        # Back to the cage: as at first, to the bit.
        again = shown("off")
        self.assertEqual(again["parts"], off["parts"])
        self.assertEqual(again["pins"], off["pins"])
        self.assertEqual(again["directions"], off["directions"])
        self.assertEqual(again["strands"], off["strands"])

    def test_guides_stay_put_through_select_and_deselect(self):
        # Issue #68: guides placed on the refined scalp (the brush roots there) moved when the
        # Select tool selected or deselected them: groomLabGuides (its solo, -freezeOthers) rooted
        # every guide again on the cage and wrote them back. It roots them as the brush does now.
        groomlab.set_param(self.groom, "scalp", "subdivision", "2")
        cmds.groomLabGraphStats(self.groom, points=True)  # evaluated at level 2, as the viewport has
        for k in range(12):
            groomlab.brush_stroke(self.groom, "place", [on_head(15 + 5 * (k % 4), 30 * k)], radius=0.5, length=3.0)
        ids = groomlab.guide_ids(self.groom, "primary")
        self.assertEqual(len(ids), 12)

        def stored():
            return {g["id"]: g["points"] for g in json.loads(cmds.getAttr(self.groom + ".guideData"))["guides"]}
        before = stored()
        for _ in range(3):
            cmds.groomLabGuides(self.groom, select=True, guide=[str(ids[0])], freezeOthers=True)
            cmds.groomLabGuides(self.groom, clearSelection=True, freezeOthers=True)
        after = stored()
        worst = max(max(abs(a - b) for a, b in zip(before[i], after[i])) for i in before)
        self.assertLess(worst, 1e-4 * RADIUS)  # (was 0.2 on a radius-10 head, each guide re-rooted)


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
