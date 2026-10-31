# SPDX-License-Identifier: Apache-2.0
"""The live strand mesh (a ToMesh node, the groom's outMesh) in mayapy: on a deforming
scalp with follow scalp and a Simulate node its points move from frame to frame while its vertex
and face counts stay; it matches the snapshot (groomLabConvert -strandMesh) of the node at a frame;
Maya's Alembic export writes it as an animated mesh. Prints the ms per frame of a 10k-strand mesh.

Usage: mayapy test_to_mesh.py <GroomLab.mll> <maya/scripts dir> <work dir>
"""

import math
import os
import shutil
import sys
import time
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
WORK_DIR = os.path.abspath(sys.argv[3])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402
from maya.api import OpenMaya as om  # noqa: E402

import groomlab  # noqa: E402
from groomlab import graph as gm  # noqa: E402
from groomlab import sim  # noqa: E402
from groomlab import utils  # noqa: E402


def mesh_fn(node):
    shape = node if cmds.nodeType(node) == "mesh" else cmds.listRelatives(node, shapes=True, fullPath=True)[0]
    sel = om.MSelectionList()
    sel.add(shape)
    return om.MFnMesh(sel.getDagPath(0))


def points_of(node):
    return [(p.x, p.y, p.z) for p in mesh_fn(node).getPoints(om.MSpace.kObject)]


def farthest(a, b):
    return max(math.sqrt(sum((x - y) ** 2 for x, y in zip(p, q))) for p, q in zip(a, b))


class ToMeshTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)
        shutil.rmtree(WORK_DIR, ignore_errors=True)
        os.makedirs(WORK_DIR)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        cmds.currentUnit(time="film")
        cmds.playbackOptions(minTime=1, maxTime=6)
        cmds.currentTime(1)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]
        # A deforming scalp: a squash, keyed.
        deformer = cmds.nonLinear(self.scalp, type="squash")[0]
        cmds.setKeyframe(deformer, attribute="factor", time=1, value=0.0)
        cmds.setKeyframe(deformer, attribute="factor", time=6, value=0.6)
        self.groom = groomlab.create_groom(self.scalp)  # its rest mesh: the deformer's Orig shape
        cmds.setAttr(self.groom + ".previewDensity", 0.25)
        curves = []
        for angle in (0.35, 0.8, 1.2):
            n = (math.sin(angle), math.cos(angle), 0.2)
            curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.1 * c for c in n)]))
        groomlab.guides_from_curves(self.groom, curves)
        cmds.delete(curves)
        groomlab.generate_children(self.groom, "primary", name="children", count=4)
        groomlab.set_follow_scalp(self.groom, True)

    def set_scatter(self, density):
        model = gm.GraphModel.from_groom(self.groom)
        for name in model.order:
            if model.node_type(name) == "Scatter":
                model.set_param(name, "density", float(density))
        model.save(self.groom)

    def test_scales_make_their_plates_and_export(self):
        # A ToMesh node on scales: auto makes the plates (quads, nearly all); strands asked for,
        # tubes (quads); Export Mesh writes the node's mesh as an OBJ.
        model = gm.GraphModel.from_groom(self.groom)
        scales = model.add_node("Scales", name=model.unique_name("scales"))
        model.insert_before(model.output, scales)
        model.save(self.groom)
        node = groomlab.add_to_mesh(self.groom)
        self.assertEqual(gm.GraphModel.from_groom(self.groom).param(node, "make"), "auto")
        plates = groomlab.to_mesh(self.groom)["mesh"]
        faces = cmds.polyEvaluate(plates, face=True)
        self.assertGreater(faces, 0)
        self.assertLess(faces * 1.5, cmds.polyEvaluate(plates, triangle=True))  # plates: mostly quads
        tubes = groomlab.to_mesh(self.groom, make="strands", max_strands=20)["mesh"]
        self.assertLess(cmds.polyEvaluate(tubes, face=True), cmds.polyEvaluate(tubes, triangle=True))  # quads
        with self.assertRaises(ValueError):
            groomlab.to_mesh(self.groom, make="cards")
        path = os.path.join(WORK_DIR, "scales.obj").replace("\\", "/")
        self.assertEqual(groomlab.export_mesh(self.groom, path, node=node), path)
        with open(path) as f:
            text = f.read()
        self.assertEqual(sum(1 for line in text.splitlines() if line.startswith("f ")), faces)
        with self.assertRaises(ValueError):
            groomlab.export_mesh(self.groom, os.path.join(WORK_DIR, "scales.stl"))

    def test_live_mesh_follows_the_scalp_and_simulate_with_a_fixed_topology(self):
        simulate = sim.add_simulate(self.groom, preset="fur")
        node = groomlab.add_to_mesh(self.groom, sides=4, max_strands=150)
        model = gm.GraphModel.from_groom(self.groom)
        self.assertEqual(model.node_type(node), "ToMesh")
        self.assertEqual(model.source_of(model.output, "strands"), node)  # just before the output
        self.assertTrue(cmds.getAttr(self.groom + ".displayMesh"))  # the groom draws it at once
        # Mesh in Scene: one undo step; again, the same mesh.
        mesh = groomlab.to_mesh_shape(self.groom)
        cmds.undo()
        self.assertFalse(cmds.objExists(mesh))
        cmds.redo()
        self.assertTrue(cmds.objExists(mesh))
        self.assertEqual(groomlab.to_mesh_shape(self.groom), mesh)
        self.assertEqual(cmds.polyEvaluate(mesh, shell=True), 150)
        self.assertIn("initialShadingGroup", cmds.listConnections(cmds.listRelatives(mesh, shapes=True)[0],
                                                                  type="shadingEngine") or [])
        made = {"vertices": cmds.polyEvaluate(mesh, vertex=True)}
        # Again: the same node, the settings taken (one undo step).
        self.assertEqual(groomlab.add_to_mesh(self.groom, sides=5, max_strands=150), node)
        self.assertEqual(groomlab.graph.GraphModel.from_groom(self.groom).param(node, "sides"), 5)
        cmds.undo()
        self.assertEqual(groomlab.graph.GraphModel.from_groom(self.groom).param(node, "sides"), 4)

        counts, frames = None, []
        for frame in range(1, 7):  # in order: Simulate steps
            cmds.currentTime(frame)
            fn = mesh_fn(mesh)
            if counts is None:
                counts = (fn.numVertices, fn.numPolygons)
            self.assertEqual((fn.numVertices, fn.numPolygons), counts, "frame %d" % frame)
            frames.append(points_of(mesh))
        self.assertEqual(counts[0], made["vertices"])
        for a, b in zip(frames, frames[1:]):
            self.assertGreater(farthest(a, b), 1e-4)  # it moves every frame

        # The snapshot of the node at this frame: the same mesh.
        snapshot = utils.to_mesh(self.groom, node=node, sides=4, max_strands=150)
        self.assertEqual((snapshot["vertices"], snapshot["faces"]), counts)
        self.assertLess(farthest(points_of(snapshot["mesh"]), frames[-1]), 1e-4)
        cmds.delete(snapshot["mesh"])

        # Baked, the frames read the bake: the mesh still is the node's strands.
        sim.bake_simulation(self.groom, node=simulate, end=6, path=os.path.join(WORK_DIR, "sim.abc"))
        cmds.currentTime(3)
        snapshot = utils.to_mesh(self.groom, node=node, sides=4, max_strands=150)
        self.assertLess(farthest(points_of(snapshot["mesh"]), points_of(mesh)), 1e-4)
        self.assertEqual(mesh_fn(mesh).numVertices, counts[0])
        cmds.delete(snapshot["mesh"])
        baked = []  # what the groom shows now (the bake's frames)
        for frame in (1, 6):
            cmds.currentTime(frame)
            baked.append(points_of(mesh))

        # Alembic: an animated mesh, its vertex count on every frame.
        try:
            cmds.loadPlugin("AbcExport", quiet=True)
            cmds.loadPlugin("AbcImport", quiet=True)
        except RuntimeError:
            sys.stderr.write("\nAbcExport did not load: the Alembic check is skipped\n")
            return
        path = os.path.join(WORK_DIR, "strand_mesh.abc").replace("\\", "/")
        cmds.AbcExport(j="-frameRange 1 6 -uvWrite -root %s -file %s" % (cmds.ls(mesh, long=True)[0], path))
        self.assertTrue(os.path.exists(path))
        cmds.file(new=True, force=True)
        cmds.AbcImport(path, mode="import")
        imported = cmds.ls(type="mesh", long=True)
        self.assertEqual(len(imported), 1)
        read = []
        for frame in (1, 6):
            cmds.currentTime(frame)
            self.assertEqual(mesh_fn(imported[0]).numVertices, counts[0])
            read.append(points_of(imported[0]))
        self.assertGreater(farthest(read[0], read[1]), 1e-3)
        self.assertLess(farthest(read[0], baked[0]), 1e-4)
        self.assertLess(farthest(read[1], baked[1]), 1e-4)

    def test_strands_to_mesh_is_a_snapshot(self):
        # Utilities > To Mesh: a mesh of this frame, no node, nothing on outMesh.
        import inspect
        from groomlab.ui import menu
        self.assertNotIn("live", inspect.signature(utils.to_mesh).parameters)
        self.assertNotIn("live", inspect.signature(menu.to_mesh_dialog).parameters)
        made = menu.to_mesh_dialog(make="auto", node="(output)", style="tube", sides=4, width_scale=1.0, max_strands=20,
                                           strand_uvs=False, groom=self.groom)
        self.assertEqual(made["strands"], 20)
        self.assertEqual(utils.to_mesh_nodes(self.groom), [])
        self.assertFalse(cmds.listConnections(self.groom + ".outMesh", destination=True, source=False))

    def test_a_node_after_another_and_ms_per_frame(self):
        # After a node: what that node makes (Interpolate's strands); the output is as it was.
        model = gm.GraphModel.from_groom(self.groom)
        interpolate = next(n for n in model.order if model.node_type(n) == "Interpolate")
        before = cmds.getAttr(self.groom + ".outStrandCount")
        node = groomlab.add_to_mesh(self.groom, after=interpolate, style="ribbon", max_strands=50)
        model = gm.GraphModel.from_groom(self.groom)
        self.assertEqual(model.source_of(node, "strands"), interpolate)
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), before)
        with self.assertRaises(ValueError):
            groomlab.add_to_mesh(self.groom, after="nope")
        mesh = groomlab.to_mesh_shape(self.groom)
        fn = mesh_fn(mesh)
        self.assertEqual(cmds.polyEvaluate(mesh, shell=True), 50)
        self.assertEqual(fn.numPolygons * 2, fn.numVertices - 2 * 50)  # ribbons: (points - 1) quads, 2 vertices a point
        self.assertEqual(groomlab.to_mesh_shape(self.groom), mesh)  # reused

        # 10,000 strands of 16 points as 4-sided tubes: ms per frame of the scalp moving.
        model = gm.GraphModel.from_groom(self.groom)
        resample = model.add_node("Resample", params={"segments": 15})
        model.insert_before(node, resample)
        model.set_param(node, "style", "tube")
        model.set_param(node, "sides", 4)
        model.set_param(node, "maxStrands", 10000)
        model.save(self.groom)
        self.set_scatter(1500.0)
        plug = om.MSelectionList()
        plug.add(self.groom + ".outMesh")
        plug = plug.getPlug(0)
        cmds.currentTime(1)
        start = time.time()
        data = plug.asMObject()
        built = (time.time() - start) * 1000.0
        vertices = om.MFnMesh(data).numVertices
        self.assertEqual(vertices, 10000 * (16 * 4 + 1))
        took = {}
        for shown in (False, True):  # the groom drawing the mesh too (its view's strands), or not
            cmds.setAttr(self.groom + ".displayMesh", shown)
            took[shown] = []
            for frame in range(2, 7):
                cmds.currentTime(frame)
                start = time.time()
                data = plug.asMObject()
                took[shown].append((time.time() - start) * 1000.0)
                self.assertEqual(om.MFnMesh(data).numVertices, 10000 * (16 * 4 + 1))
        sys.stderr.write("\nlive strand mesh, 10,000 strands x 16 points, 4 sides (%d vertices): first frame %.0f ms, "
                         "then %s ms per frame (%s with displayMesh off; the groom's evaluation included)\n"
                         % (vertices, built, ", ".join("%.0f" % t for t in took[True]),
                            ", ".join("%.0f" % t for t in took[False])))


if __name__ == "__main__":
    unittest.main(verbosity=2)
