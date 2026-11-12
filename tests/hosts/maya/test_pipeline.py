# SPDX-License-Identifier: Apache-2.0
"""M4 pipeline in Maya: rest mesh, USD export, groom files evaluated by the CLI.

Usage: mayapy test_pipeline.py <GroomLab.mll> <maya/scripts dir> <groomlab-cli> <work dir>
"""

import json
import os
import re
import shutil
import subprocess
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
CLI = os.path.abspath(sys.argv[3])
WORK_DIR = os.path.abspath(sys.argv[4])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402
from groomlab import presets  # noqa: E402
from groomlab import sim  # noqa: E402

try:
    from pxr import Usd, UsdGeom  # MayaUSD's USD build
except ImportError:
    Usd = None


def cli(*args):
    result = subprocess.run([CLI] + [str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True)
    if result.returncode != 0:
        raise AssertionError("groomlab-cli %s failed:\n%s" % (" ".join(map(str, args)), result.stdout))
    return result.stdout


class PipelineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)
        shutil.rmtree(WORK_DIR, ignore_errors=True)
        os.makedirs(WORK_DIR)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")
        cmds.currentTime(1)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]

    def squash(self):
        """Animate a squash deformer on the scalp: every face changes area by frame 10."""
        deformer = cmds.nonLinear(self.scalp, type="squash")[0]
        cmds.setKeyframe(deformer, attribute="factor", time=1, value=0.0)
        cmds.setKeyframe(deformer, attribute="factor", time=10, value=0.8)
        return deformer

    def ids_at(self, groom, frame):
        cmds.currentTime(frame)
        return cmds.getAttr(groom + ".outStrandCount")

    def test_rest_mesh_keeps_strands_on_a_deforming_scalp(self):
        self.squash()
        groom = groomlab.create_groom(self.scalp)  # finds the deformer's Orig shape
        cmds.setAttr(groom + ".previewDensity", 1.0)
        rest = cmds.listConnections(groom + ".restMesh", shapes=True)
        self.assertTrue(rest and cmds.getAttr(rest[0] + ".intermediateObject"))
        self.assertEqual(self.ids_at(groom, 1), self.ids_at(groom, 10))

        groomlab.clear_rest_mesh(groom)
        self.assertNotEqual(self.ids_at(groom, 1), self.ids_at(groom, 10))

        # Without a deformer, a hidden copy of the scalp serves as rest.
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere()[0]
        groom = groomlab.create_groom(scalp)
        self.assertFalse(cmds.listConnections(groom + ".restMesh"))
        copy = groomlab.set_rest_mesh(groom)
        self.assertFalse(cmds.getAttr(cmds.listRelatives(copy, parent=True)[0] + ".visibility"))

    def test_exports_over_frames_match_each_frame_alone(self):
        # An export keeps what it converted on one frame for the next (the scalp, curves, an open
        # guide cache) while it reads the same: on a deforming scalp every frame must still be
        # what exporting that frame alone gives, bit for bit.
        self.squash()
        groom = groomlab.create_groom(self.scalp)
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)])
        groomlab.guides_from_curves(groom, [curve])
        groomlab.generate_children(groom, "primary", count=5)

        def hashes(path):
            return [line.split("hash=")[1] for line in cli("hash", path).splitlines()[:-1]]

        frames = [1, 4, 7, 10]
        for kind, flags in (("strands", {"density": 0.3}), ("guides", {"guideGroup": "primary"})):
            together = os.path.join(WORK_DIR, "frames_%s.abc" % kind)
            cmds.groomLabExportAlembic(groom, file=together, startFrame=1, endFrame=10, step=3, **flags)
            each = []
            for frame in frames:
                alone = os.path.join(WORK_DIR, "frame_%s_%d.abc" % (kind, frame))
                cmds.groomLabExportAlembic(groom, file=alone, startFrame=frame, endFrame=frame, **flags)
                each += hashes(alone)
            self.assertEqual(hashes(together), each, kind)
            self.assertNotEqual(each[0], each[-1], kind)  # the scalp does move them

    def test_export_usd(self):
        groom = groomlab.create_groom(self.scalp)
        cmds.setAttr(groom + ".previewDensity", 0.1)  # ignored: export density is its own
        path = os.path.join(WORK_DIR, "groom.usda")
        self.assertEqual(groomlab.export_usd(groom, path, start=1, end=3, density=0.25), 3)
        with open(path) as f:
            text = f.read()
        self.assertIn('defaultPrim = "groomLab1"', text)
        self.assertIn("timeCodesPerSecond = 24", text)
        self.assertIn("texCoord2f[] primvars:st", text)  # the sphere has UVs

        if Usd is None:
            self.skipTest("pxr not importable in mayapy; checked the text only")
        stage = Usd.Stage.Open(path)
        self.assertEqual(stage.GetStartTimeCode(), 1)
        self.assertEqual(stage.GetEndTimeCode(), 3)
        curves = UsdGeom.BasisCurves(stage.GetPrimAtPath("/groomLab1/strands"))
        self.assertTrue(curves)
        counts = curves.GetCurveVertexCountsAttr().Get(2)
        points = curves.GetPointsAttr().Get(2)
        self.assertGreater(len(counts), 200)
        self.assertEqual(sum(counts), len(points))
        self.assertEqual(len(curves.GetWidthsAttr().Get(2)), len(points))
        self.assertEqual(curves.GetWidthsInterpolation(), UsdGeom.Tokens.vertex)
        ids = UsdGeom.PrimvarsAPI(curves).GetPrimvar("strandId")
        self.assertEqual(ids.GetInterpolation(), UsdGeom.Tokens.uniform)
        self.assertEqual(len(ids.Get(2)), len(counts))
        extent = curves.GetExtentAttr().Get(2)
        self.assertLess(extent[0][1], -0.9)
        self.assertGreater(extent[1][1], 0.9)

    def test_groom_file_evaluates_the_same_in_the_cli(self):
        self.squash()
        groom = groomlab.create_groom(self.scalp)
        cmds.polyColorPerVertex(self.scalp, rgb=(1, 1, 1), colorDisplayOption=True)  # a color set rides along
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)])
        groomlab.guides_from_curves(groom, [curve])
        groomlab.generate_children(groom, "primary", count=5)
        presets.apply_preset(groom, "hair_basics", fit=False)  # its numbers as they are: light on this sphere

        path = os.path.join(WORK_DIR, "shot.groom")
        # Cache subframes too: between samples the CLI blends linearly, Maya's keys are splines.
        written = groomlab.export_groom(groom, path, scalp_cache=True, start=1, end=6, step=0.5)
        self.assertEqual([os.path.basename(p) for p in written], ["shot.groom", "shot.scalp.abc"])

        info = cli("info", path)
        self.assertIn("scalp cache: ", info)
        self.assertIn("rest pose", info)
        self.assertIn("hierarchy:   6 guides in 2 groups", info)

        from_cli = os.path.join(WORK_DIR, "cli.abc")
        from_maya = os.path.join(WORK_DIR, "maya.abc")
        out = cli("eval", path, "-o", from_cli, "-f", "1-6", "--step", "0.5", "--density", "0.3")
        self.assertIn("wrote 11 frame(s)", out)
        groomlab.export_alembic(groom, from_maya, start=1, end=6, step=0.5, density=0.3)

        # Same strands, same bounds on every frame, subframes included (6 digits).
        cli_frames = cli("abc-info", from_cli).splitlines()
        maya_frames = cli("abc-info", from_maya).splitlines()
        self.assertEqual(len(cli_frames), 12)
        self.assertEqual(cli_frames, maya_frames)
        self.assertNotEqual(cli_frames[1].split("bbox")[1], cli_frames[-1].split("bbox")[1])  # it does move
        # And exactly: the IDs and every point's float bits hash the same (`hash`, one line per frame + all),
        # the strands' root UVs on the scalp too (the CLI writes them as Maya does).
        cli_hashes = cli("hash", from_cli).splitlines()
        self.assertEqual(len(cli_hashes), 12)
        for line in cli_hashes[:-1]:
            self.assertNotIn(" uvs=0 ", line)
        self.assertEqual(cli_hashes, cli("hash", from_maya).splitlines())
        self.assertNotEqual(cli_hashes[0].split("hash=")[1], cli_hashes[-2].split("hash=")[1])

        # And to USD.
        usd = os.path.join(WORK_DIR, "cli.usda")
        cli("eval", path, "-o", usd, "-f", "3", "--density", "0.1")
        self.assertTrue(os.path.getsize(usd) > 10000)

    def test_the_scene_linear_unit_does_not_change_the_groom(self):
        # Maya's API works in centimeters whatever the UI's linear unit: the same physical head
        # made in a cm, m or mm scene grows the same strands (positions in cm) with the same
        # widths, and its .groom says "cm".
        from array import array

        def build(unit, scale):
            cmds.file(new=True, force=True)
            cmds.currentUnit(linear=unit, time="film")
            scalp = cmds.polySphere(radius=1.0 * scale, subdivisionsX=30, subdivisionsY=16)[0]
            groom = groomlab.create_groom(scalp)
            curve = cmds.curve(degree=1, point=[(0, 1.0 * scale, 0), (0.2 * scale, 1.6 * scale, 0.1 * scale),
                                                (0.5 * scale, 2.0 * scale, 0.2 * scale)])
            groomlab.guides_from_curves(groom, [curve])
            groomlab.generate_children(groom, "primary", count=4)
            cmds.setAttr(groom + ".previewDensity", 0.2)
            cmds.setAttr(groom + ".previewPointBudget", 0)
            count = cmds.getAttr(groom + ".outStrandCount")
            points = array("f", cmds.groomLabGraphStats(groom, points=True) or [])
            path = os.path.join(WORK_DIR, "units_%s.groom" % unit)
            groomlab.export_groom(groom, path)
            with open(path) as f:
                data = json.load(f)
            graph = groomlab.get_graph(groom, expand_default=True)
            widths = [(n["name"], n.get("params", {}).get("width")) for n in graph["nodes"] if n.get("type") == "Width"]
            abc = os.path.join(WORK_DIR, "units_%s.abc" % unit)
            groomlab.export_alembic(groom, abc, start=1, end=1, density=0.2)
            cmds.currentUnit(linear="cm")
            return count, points, data, widths, abc

        count_cm, points_cm, data_cm, widths_cm, abc_cm = build("cm", 1.0)
        self.assertGreater(count_cm, 0)
        self.assertTrue(widths_cm)
        for unit, scale in (("m", 0.01), ("mm", 10.0)):
            count, points, data, widths, abc = build(unit, scale)
            self.assertEqual(count, count_cm, unit)
            self.assertEqual(data["units"], "cm", unit)
            self.assertEqual(widths, widths_cm, unit + ": the Width node's widths (stored in cm)")
            self.assertEqual(len(points), len(points_cm), unit)
            worst = max(abs(a - b) for a, b in zip(points, points_cm))
            self.assertLess(worst, 1e-4, unit + ": strand points in cm")  # the m->cm conversion's rounding
            # The exported curves (positions and widths) are the cm scene's.
            lines = cli("hash", abc).splitlines()[:-1]
            lines_cm = cli("hash", abc_cm).splitlines()[:-1]
            self.assertEqual(lines, lines_cm, unit)

    def test_every_preset_is_the_same_from_its_groom_file(self):
        # Determinism across hosts: every shipped preset (fitted to this scalp, as the menu does)
        # in Maya against its groom file outside Maya, bit for bit. The export's strands (Maya's
        # NodeEvaluator) against groomlab-cli's (`hash`: IDs, every point's and width's float
        # bits, root UVs), and the node's own viewport strands against groomlab_core's GroomScene
        # (every point's float bits: the viewport's doubles are its floats exactly). Wind and
        # Jiggle over a squashed scalp's cache too, frame by frame.
        from array import array

        import groomlab_core as gc  # the build's (the plugin's Python imports it too)

        groom = groomlab.create_groom(self.scalp)
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)])
        groomlab.guides_from_curves(groom, [curve])
        groomlab.mirror_group(groom, "primary", "x")
        groomlab.generate_children(groom, "primary", count=4)
        cmds.setAttr(groom + ".previewDensity", 0.1)
        cmds.setAttr(groom + ".previewPointBudget", 0)

        def hashes(path):
            return [line.split("hash=")[1] for line in cli("hash", path).splitlines()[:-1]]

        compared = 0
        for name in presets.list_presets():
            presets.apply_preset(groom, name)
            if cmds.getAttr(groom + ".outStrandCount") == 0:
                continue  # nothing made on this scalp (an error the node shows)
            path = os.path.join(WORK_DIR, "preset_%s.groom" % name)
            groomlab.export_groom(groom, path)
            from_cli = os.path.join(WORK_DIR, "preset_%s_cli.abc" % name)
            from_maya = os.path.join(WORK_DIR, "preset_%s_maya.abc" % name)
            cli("eval", path, "-o", from_cli, "-f", "1", "--density", "0.1")
            groomlab.export_alembic(groom, from_maya, start=1, end=1, density=0.1)
            self.assertEqual(hashes(from_maya), hashes(from_cli), name)
            scene = gc.GroomScene(path)
            strands = scene.evaluate(1.0 / scene.frames_per_second, 0.1)
            viewport = array("f", cmds.groomLabGraphStats(groom, points=True) or []).tobytes()
            self.assertGreater(len(viewport), 0, name)
            self.assertEqual(viewport, bytes(strands.positions_view()), name + ": the viewport's strands")
            compared += 1
        self.assertGreater(compared, 30)

        # Time: Wind and Jiggle over the squashed scalp, its cache in the groom file.
        self.squash()
        presets.apply_preset(groom, "wind_and_jiggle")
        path = os.path.join(WORK_DIR, "preset_motion.groom")
        groomlab.export_groom(groom, path, scalp_cache=True, start=1, end=8)
        from_cli = os.path.join(WORK_DIR, "preset_motion_cli.abc")
        from_maya = os.path.join(WORK_DIR, "preset_motion_maya.abc")
        cli("eval", path, "-o", from_cli, "-f", "1-8", "--density", "0.1")
        groomlab.export_alembic(groom, from_maya, start=1, end=8, density=0.1)
        played = hashes(from_cli)
        self.assertEqual(hashes(from_maya), played)
        self.assertNotEqual(played[0], played[-1])  # it does move
        # A frame alone (a render of it) is the frame played to.
        alone = os.path.join(WORK_DIR, "preset_motion_alone.abc")
        cli("eval", path, "-o", alone, "-f", "6", "--density", "0.1")
        self.assertEqual(hashes(alone), played[5:6])

    def guided_groom(self):
        """A groom on the scalp with a few guides (mirrored, children), as the preset comparisons use."""
        groom = groomlab.create_groom(self.scalp)
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)])
        groomlab.guides_from_curves(groom, [curve])
        groomlab.mirror_group(groom, "primary", "x")
        groomlab.generate_children(groom, "primary", count=4)
        cmds.setAttr(groom + ".previewDensity", 0.1)
        cmds.setAttr(groom + ".previewPointBudget", 0)
        return groom

    def test_rest_mesh_stays_put_while_the_transform_moves(self):
        # The scalp under a group whose transform is keyed (a character moved by its rig; no
        # deformer), its rest mesh set at frame 5 (away from the identity): the rest pose is the
        # mesh's own points placed as they are then (a value, through a transformGeometry node), so
        # it stays put while the group moves: the same strands (IDs) on every frame, and Collide
        # against the moving scalp asks the rest pose's distance field, made once. Connected in world
        # space (as it was), the rest copy moves with the group: a field made on every frame.
        import groomlab_core as gc
        from groomlab import graph as gm
        rig = cmds.group(self.scalp, name="rig")
        for attr, a, b in (("translateX", 0.0, 3.0), ("translateY", 0.0, 1.5), ("rotateY", 0.0, 50.0), ("rotateZ", 0.0, 25.0)):
            cmds.setKeyframe(rig, attribute=attr, time=1, value=a)
            cmds.setKeyframe(rig, attribute=attr, time=10, value=b)
        groom = groomlab.create_groom(self.scalp)
        self.assertFalse(cmds.listConnections(groom + ".restMesh"))  # no deformer: none of its own
        cmds.setAttr(groom + ".previewDensity", 0.3)
        model = gm.GraphModel.from_groom(groom)
        collide = model.add_node("Collide", name="collide", params={"offset": 0.05})
        model.insert_before("output", collide)
        model.connect(next(n for n in model.order if model.node_type(n) == "Scalp"), collide, "scalp")
        model.save(groom)
        cmds.currentTime(5)
        rest = groomlab.set_rest_mesh(groom)
        self.assertFalse(cmds.getAttr(cmds.listRelatives(rest, parent=True, fullPath=True)[0] + ".visibility"))  # a hidden copy
        [place] = cmds.listConnections(groom + ".restMesh", source=True, destination=False)
        self.assertEqual(cmds.nodeType(place), "transformGeometry")
        self.assertEqual(cmds.ls(groomlab.rest_mesh(groom), long=True), cmds.ls(rest, long=True))
        self.assertEqual(groomlab.errors(groom), [])

        def fields_made_per_frame():
            made = []
            for frame in range(5, 11):
                cmds.currentTime(frame)
                cmds.getAttr(groom + ".outStrandCount")  # evaluated at this frame
                made.append(json.loads(cmds.groomLabGraphStats(groom))["distanceFieldsMade"])
            return [b - a for a, b in zip(made, made[1:])]

        self.assertEqual(fields_made_per_frame()[1:], [0, 0, 0, 0])
        path = os.path.join(WORK_DIR, "rest_transform.abc")
        groomlab.export_alembic(groom, path, start=5, end=10, density=0.3)
        frames = gc.read_alembic_curves(path)
        self.assertEqual(len(frames), 6)
        self.assertGreater(len(frames[0].strands.ids), 100)
        for frame in frames[1:]:
            self.assertEqual(frame.strands.ids, frames[0].strands.ids)

        # Cleared: the placement goes with it. The copy's world mesh instead moves with the rig.
        groomlab.clear_rest_mesh(groom)
        self.assertFalse(cmds.ls(place))
        cmds.connectAttr(rest + ".worldMesh[0]", groom + ".restMesh")
        self.assertTrue(all(made >= 1 for made in fields_made_per_frame()[1:]))

        # A deformed scalp at the identity keeps its original shape's points as they were: no placement.
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]
        cmds.cluster(scalp)
        groom = groomlab.create_groom(scalp)
        [orig] = cmds.listConnections(groom + ".restMesh", source=True, destination=False, shapes=True)
        self.assertTrue(cmds.getAttr(orig + ".intermediateObject"))
        self.assertFalse(cmds.ls(type="transformGeometry"))

    def test_a_collider_is_the_same_from_its_groom_file(self):
        # A preset colliding with a collider mesh of the scene (add_collider: a ColliderMesh node and
        # a Collide; a deformer on it, so its original shape is its rest pose): the groom file holds
        # the collider, so the CLI and groomlab_core collide as Maya does, bit for bit. Without it in
        # the file they would not.
        from array import array

        import groomlab_core as gc

        groom = self.guided_groom()
        presets.apply_preset(groom, "wind_and_jiggle")
        ball = cmds.polySphere(radius=0.6, subdivisionsX=16, subdivisionsY=10, name="ball")[0]
        cmds.xform(ball, translation=(0.0, 0.4, 1.2))
        cmds.cluster(ball)
        groomlab.add_collider(groom, ball)
        self.assertTrue(groomlab.colliders(groom)[0]["rest"])
        self.assertEqual(groomlab.errors(groom), [])

        def hashes(path):
            return [line.split("hash=")[1] for line in cli("hash", path).splitlines()[:-1]]

        path = os.path.join(WORK_DIR, "collider.groom")
        groomlab.export_groom(groom, path)
        with open(path) as f:
            stored = json.load(f)
        self.assertEqual(sorted(stored["colliders"]), ["ball"])
        self.assertIn("restPoints", stored["colliders"]["ball"])
        from_cli = os.path.join(WORK_DIR, "collider_cli.abc")
        from_maya = os.path.join(WORK_DIR, "collider_maya.abc")
        cli("eval", path, "-o", from_cli, "-f", "1", "--density", "0.1")
        groomlab.export_alembic(groom, from_maya, start=1, end=1, density=0.1)
        self.assertEqual(hashes(from_maya), hashes(from_cli))
        scene = gc.GroomScene(path)
        strands = scene.evaluate(gc.frame_to_seconds(1.0, scene.frames_per_second), 0.1)
        viewport = array("f", cmds.groomLabGraphStats(groom, points=True) or []).tobytes()
        self.assertEqual(viewport, bytes(strands.positions_view()))

        # The ball does move the hair: a file without it gives other strands.
        del stored["colliders"]
        bare = os.path.join(WORK_DIR, "collider_bare.groom")
        with open(bare, "w") as f:
            json.dump(stored, f)
        bare_cli = os.path.join(WORK_DIR, "collider_bare_cli.abc")
        cli("eval", bare, "-o", bare_cli, "-f", "1", "--density", "0.1")
        self.assertNotEqual(hashes(bare_cli), hashes(from_cli))

    def test_several_scalps_on_one_groom(self):
        # Another mesh the hair grows on (add_scalp: extraScalps, a Scalp node naming it, its own
        # Scatter and Interpolate merged after the groom's own): more strands, the groom file holds
        # it, the CLI and groomlab_core evaluate it as Maya does, bit for bit; it moves the hair on it
        # when it moves; removing it takes its branch away and the groom is as it was.
        from array import array

        import groomlab_core as gc
        from groomlab import graph as gm
        from groomlab.ui import ae

        groom = self.guided_groom()
        alone = cmds.getAttr(groom + ".outStrandCount")
        ear = cmds.polyPlane(width=0.8, height=0.8, subdivisionsX=6, subdivisionsY=6, name="ear")[0]
        cmds.xform(ear, translation=(1.6, 0.2, 0.0), rotation=(0, 0, 90))
        self.assertEqual(groomlab.add_scalp(groom, ear), "ear")
        self.assertEqual([s["name"] for s in groomlab.scalps(groom)], ["ear"])
        self.assertEqual([s["name"] for s in groomlab.inputs_summary(groom)["scalps"]], ["ear"])
        model = gm.GraphModel.from_groom(groom)
        reader = next(n for n in model.order if model.node_type(n) == "Scalp" and model.param(n, "input") == "ear")
        self.assertTrue(any(model.node_type(c) == "Scatter" for c, _ in model.downstream_of(reader)))
        self.assertEqual(groomlab.errors(groom), [])
        both = cmds.getAttr(groom + ".outStrandCount")
        self.assertGreater(both, alone)
        with self.assertRaises(ValueError):
            groomlab.add_scalp(groom, self.scalp)  # the groom's own scalp
        with self.assertRaises(ValueError):
            groomlab.add_scalp(groom, cmds.polyCube()[0], name="scalp")

        def hashes(path):
            return [line.split("hash=")[1] for line in cli("hash", path).splitlines()[:-1]]

        path = os.path.join(WORK_DIR, "scalps.groom")
        groomlab.export_groom(groom, path)
        with open(path) as f:
            stored = json.load(f)
        self.assertEqual([s["name"] for s in stored["scalps"]], ["ear"])
        from_cli = os.path.join(WORK_DIR, "scalps_cli.abc")
        from_maya = os.path.join(WORK_DIR, "scalps_maya.abc")
        cli("eval", path, "-o", from_cli, "-f", "1", "--density", "0.1")
        groomlab.export_alembic(groom, from_maya, start=1, end=1, density=0.1)
        self.assertEqual(hashes(from_maya), hashes(from_cli))
        scene = gc.GroomScene(path)
        strands = scene.evaluate(gc.frame_to_seconds(1.0, scene.frames_per_second), 0.1)
        viewport = array("f", cmds.groomLabGraphStats(groom, points=True) or []).tobytes()
        self.assertEqual(viewport, bytes(strands.positions_view()))

        # The ear moved: its hair goes with it (the same strands), the head's stays.
        before = array("f", cmds.groomLabGraphStats(groom, points=True) or [])
        cmds.xform(ear, translation=(1.6, 0.7, 0.0))
        after = array("f", cmds.groomLabGraphStats(groom, points=True) or [])
        self.assertEqual(len(before), len(after))
        moved = sum(1 for i in range(1, len(before), 3) if abs(after[i] - before[i] - 0.5) < 1e-3)
        self.assertGreater(moved, 0)
        self.assertLess(moved, len(before) // 3)

        # The Attribute Editor's Pick with a mesh selected along with the groom adds it too.
        brow = cmds.polyPlane(width=0.4, height=0.2, name="brow")[0]
        cmds.xform(brow, translation=(0.0, 0.9, 0.9), rotation=(90, 0, 0))
        cmds.select([groom, brow])
        self.assertIn("brow", ae.pick_extra_scalp(groom))
        self.assertEqual([s["name"] for s in groomlab.scalps(groom)], ["ear", "brow"])

        self.assertTrue(groomlab.remove_scalp(groom, "brow"))
        self.assertTrue(groomlab.remove_scalp(groom, "ear"))
        self.assertFalse(groomlab.remove_scalp(groom, "ear"))
        model = gm.GraphModel.from_groom(groom)
        self.assertFalse([n for n in model.order if model.node_type(n) in ("Scalp", "Merge") and n != "scalp"])
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), alone)

    def test_reference_node_brings_in_a_groom_file(self):
        # A library look (a groom saved as a .groom file) brought into another groom by a Reference
        # node, bound onto its scalp by closest point and merged with its own strands: the CLI
        # evaluates the host's groom file as Maya does, bit for bit, reading the library through
        # the path the host stores.
        from groomlab import graph as gm

        library = self.guided_groom()
        lib_path = os.path.join(WORK_DIR, "library_brows.groom").replace("\\", "/")
        groomlab.export_groom(library, lib_path)
        lib_count = cmds.getAttr(library + ".outStrandCount")
        cmds.delete(cmds.listRelatives(library, parent=True))

        host = groomlab.create_groom(self.scalp)
        cmds.setAttr(host + ".previewDensity", 0.1)
        own = cmds.getAttr(host + ".outStrandCount")
        model = gm.GraphModel.from_groom(host)
        ref = model.add_node("Reference", params={"path": lib_path, "bind": "closestPoint"})
        merge = model.add_node("Merge")
        model.insert_before("output", merge)
        model.connect(ref, merge, "b")
        model.save(host)
        self.assertEqual(groomlab.errors(host), [])
        self.assertEqual(cmds.getAttr(host + ".outStrandCount"), own + lib_count)

        def hashes(path):
            return [line.split("hash=")[1] for line in cli("hash", path).splitlines()[:-1]]

        path = os.path.join(WORK_DIR, "referencing.groom")
        groomlab.export_groom(host, path)
        from_cli = os.path.join(WORK_DIR, "referencing_cli.abc")
        from_maya = os.path.join(WORK_DIR, "referencing_maya.abc")
        cli("eval", path, "-o", from_cli, "-f", "1", "--density", "0.1")
        groomlab.export_alembic(host, from_maya, start=1, end=1, density=0.1)
        self.assertEqual(hashes(from_maya), hashes(from_cli))

        # A missing library is an error on the groom, naming the file.
        model = gm.GraphModel.from_groom(host)
        model.set_param(ref, "path", "nowhere/brows.groom")
        model.save(host)
        self.assertTrue(any("nowhere/brows.groom" in e for e in groomlab.errors(host)), groomlab.errors(host))

    def test_ntsc_rates_are_the_same_in_every_host(self):
        # At 23.976 and 29.97 fps a frame's seconds are one rule's (frame_time.h) in Maya and the
        # CLI, so Wind (which an ulp of time moves) gives the same strands frame by frame.
        groom = self.guided_groom()
        presets.apply_preset(groom, "wind_and_jiggle")

        def hashes(path):
            return [line.split("hash=")[1] for line in cli("hash", path).splitlines()[:-1]]

        for unit, fps in (("23.976fps", 24000.0 / 1001.0), ("29.97fps", 30000.0 / 1001.0)):
            cmds.currentUnit(time=unit)
            cmds.currentTime(1)
            name = unit.replace(".", "_")
            path = os.path.join(WORK_DIR, "ntsc_%s.groom" % name)
            groomlab.export_groom(groom, path)
            with open(path) as f:
                self.assertEqual(json.load(f)["framesPerSecond"], fps)
            from_cli = os.path.join(WORK_DIR, "ntsc_%s_cli.abc" % name)
            from_maya = os.path.join(WORK_DIR, "ntsc_%s_maya.abc" % name)
            cli("eval", path, "-o", from_cli, "-f", "1-6", "--density", "0.1")
            groomlab.export_alembic(groom, from_maya, start=1, end=6, density=0.1)
            played = hashes(from_cli)
            self.assertEqual(hashes(from_maya), played, unit)
            self.assertNotEqual(played[0], played[-1], unit)  # the wind does blow

    def test_cli_reports_errors(self):
        bad = os.path.join(WORK_DIR, "bad.groom")
        with open(bad, "w") as f:
            f.write('{"format": "something else"}')
        result = subprocess.run([CLI, "eval", bad, "-o", os.path.join(WORK_DIR, "x.abc")], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, universal_newlines=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not a GroomLab groom file", result.stdout)
        result = subprocess.run([CLI, "eval", bad], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True)
        self.assertEqual(result.returncode, 2)

        groom = groomlab.create_groom()
        with self.assertRaises(RuntimeError):
            groomlab.export_groom(groom, os.path.join(WORK_DIR, "none.groom"))


# Scales on a body: a light graph of them (a few hundred plates on a unit sphere).
SCALES_GRAPH = {
    "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 40.0, "length": 0.25}},
        {"name": "width", "type": "Width", "params": {"width": 0.12}},
        {"name": "color", "type": "HairColor", "params": {"melaninRandom": 0.3}},
        {"name": "scales", "type": "Scales", "params": {"prototype": "groomlab:scale"}},
        {"name": "output", "type": "Output"},
    ],
    "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "width", "input": "strands"},
        {"from": "width", "to": "color", "input": "strands"},
        {"from": "color", "to": "scales", "input": "strands"},
        {"from": "scales", "to": "output", "input": "strands"},
    ],
    "output": "output",
}


class ScalesExportTest(unittest.TestCase):
    """Scales out of Maya: Alembic and USD meshes, the USD instancer, FBX, baked maps, groom files
    with the artist's own plates (and the CLI reading them)."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)
        if not os.path.isdir(WORK_DIR):
            os.makedirs(WORK_DIR)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")
        cmds.currentTime(1)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]
        deformer = cmds.nonLinear(self.scalp, type="squash")[0]
        cmds.setKeyframe(deformer, attribute="factor", time=1, value=0.0)
        cmds.setKeyframe(deformer, attribute="factor", time=10, value=0.8)
        self.groom = groomlab.create_groom(self.scalp)
        groomlab.set_graph(self.groom, SCALES_GRAPH)
        self.assertTrue(groomlab.has_scales(self.groom))

    def test_alembic_and_usd_carry_the_scales(self):
        abc = os.path.join(WORK_DIR, "scales.abc")
        self.assertEqual(groomlab.export_alembic(self.groom, abc, start=1, end=3), 3)
        groomlab.export_alembic(self.groom, os.path.join(WORK_DIR, "no_scales.abc"), start=1, end=3, scales="none")
        self.assertLess(os.path.getsize(os.path.join(WORK_DIR, "no_scales.abc")), os.path.getsize(abc) / 2)
        cmds.loadPlugin("AbcImport", quiet=True)
        before = set(cmds.ls(type="mesh"))
        cmds.AbcImport(abc, mode="import")
        meshes = [m for m in cmds.ls(type="mesh") if m not in before]
        self.assertEqual(len(meshes), 1, meshes)
        faces = cmds.polyEvaluate(meshes[0], face=True)
        self.assertGreater(faces, 5000)  # a few hundred plates of ~100 triangles
        self.assertIn("scales", cmds.listRelatives(meshes[0], parent=True)[0])
        cmds.delete(cmds.ls(assemblies=True)[-1])  # (the import: its curves share the groom's name)

        usd = os.path.join(WORK_DIR, "scales.usda")
        groomlab.export_usd(self.groom, usd, start=1, end=2, scales="both")
        with open(usd) as f:
            text = f.read()
        self.assertIn('def Mesh "scales"', text)
        self.assertIn('def PointInstancer "scaleInstances"', text)
        self.assertIn('def Mesh "groomlab_scale"', text)  # the plate itself, for an engine to instance
        self.assertIn("int[] primvars:scaleVariant", text)
        self.assertIn('token visibility = "invisible"', text)  # the strands only lay them out
        if Usd is None:
            return
        stage = Usd.Stage.Open(usd)
        mesh = UsdGeom.Mesh(stage.GetPrimAtPath("/groomLab1/scales"))
        self.assertEqual(len(mesh.GetFaceVertexCountsAttr().Get(1)), faces)
        instancer = UsdGeom.PointInstancer(stage.GetPrimAtPath("/groomLab1/scaleInstances"))
        count = len(instancer.GetPositionsAttr().Get(1))
        self.assertGreater(count, 50)
        self.assertEqual(len(instancer.GetOrientationsAttr().Get(1)), count)
        self.assertEqual(len(instancer.GetPrototypesRel().GetTargets()), 1)
        self.assertNotEqual(instancer.GetPositionsAttr().Get(1)[0], instancer.GetPositionsAttr().Get(2)[0])  # squashed

    def test_scales_mesh_and_fbx_for_unreal(self):
        mesh = groomlab.scale_mesh(self.groom)
        shape = cmds.listRelatives(mesh, shapes=True, fullPath=True)[0]
        self.assertEqual(set(cmds.polyUVSet(shape, query=True, allUVSets=True)), {"map1", "scaleUV", "scaleData"})
        self.assertEqual(set(cmds.polyColorSet(shape, query=True, allColorSets=True)), {"scaleColor", "scaleRootColor"})
        self.assertGreater(cmds.polyEvaluate(shape, face=True), 5000)
        cmds.undo()
        self.assertFalse(cmds.objExists(mesh))
        fbx = os.path.join(WORK_DIR, "scales.fbx")
        self.assertEqual(groomlab.export_scales(self.groom, fbx), fbx.replace("\\", "/"))
        self.assertGreater(os.path.getsize(fbx), 100000)
        self.assertFalse(cmds.ls(mesh))  # the mesh made for it is gone
        # The Unreal export writes them beside the groom (Unreal's groom importer takes curves only).
        groomlab.export_unreal_groom(self.groom, os.path.join(WORK_DIR, "dragon_unreal.abc"))
        self.assertTrue(os.path.exists(os.path.join(WORK_DIR, "dragon_unreal_scales.fbx")))

    def test_bake_scale_maps(self):
        maps = groomlab.bake_scale_maps(self.groom, directory=WORK_DIR, name="body", resolution=256)
        for m in groomlab.SCALE_MAPS:
            self.assertEqual(maps[m], [os.path.join(WORK_DIR, "body_%s.png" % m).replace("\\", "/")])
            self.assertTrue(os.path.exists(maps[m][0]))
        self.assertGreater(maps["max_height"], 0.0)
        self.assertLess(maps["max_height"], 0.1)
        self.assertEqual(maps["pattern"]["ScaleHeight"], maps["ScaleHeight"][0])

    def test_bake_follicle_map(self):
        baked = groomlab.bake_follicle_map(self.groom, directory=WORK_DIR, name="body", resolution=128)
        path = os.path.join(WORK_DIR, "body_FollicleMap.png").replace("\\", "/")
        self.assertEqual(baked["files"], [path])
        self.assertTrue(os.path.exists(path))
        self.assertEqual(baked["pattern"], path)
        self.assertGreater(baked["radius"], 0.0)
        self.assertAlmostEqual(groomlab.follicle_auto_radius(self.groom), baked["radius"], places=5)
        given = groomlab.bake_follicle_map(self.groom, directory=WORK_DIR, name="body", resolution=64, radius=0.02)
        self.assertAlmostEqual(given["radius"], 0.02, places=5)
        # By default into the scene's exports folder.
        default = groomlab.bake_follicle_map(self.groom, resolution=32)
        self.assertTrue(default["files"][0].startswith(groomlab.asset_folder("exports").replace("\\", "/")))

    def test_groom_files_carry_custom_plates(self):
        plate = cmds.polyPlane(name="myPlate", width=0.5, height=1, subdivisionsX=2, subdivisionsY=3)[0]
        # Its base at the origin, +Y its length, its face toward +Z (off the skin).
        cmds.rotate(90, 0, 0, plate + ".vtx[*]", pivot=(0, 0, 0), relative=True)
        cmds.move(0, 0.5, 0, plate + ".vtx[*]", relative=True)
        groomlab.set_param(self.groom, "scales", "prototype", "myPlate")
        path = os.path.join(WORK_DIR, "plates.groom")
        groomlab.export_groom(self.groom, path)
        import json
        with open(path) as f:
            stored = json.load(f)["prototypes"]
        self.assertEqual(list(stored), ["myPlate"])
        # The CLI (as the procedural, a farm) builds the plates from the file: the same mesh as Maya's.
        from_cli = os.path.join(WORK_DIR, "plates_cli.usda")
        from_maya = os.path.join(WORK_DIR, "plates_maya.usda")
        cli("eval", path, "-o", from_cli, "-f", "1")
        groomlab.export_usd(self.groom, from_maya, start=1, end=1)

        def scale_points(usd):
            with open(usd) as f:
                text = f.read()
            body = text[text.index('def Mesh "scales"'):]
            return re.search(r"point3f\[\] points\.timeSamples = \{\s*1: \[([^\]]*)\]", body).group(1).count("(")
        count = scale_points(from_maya)
        self.assertEqual(scale_points(from_cli), count)
        plate_points = len(stored["myPlate"]["points"]) // 3
        self.assertGreater(plate_points, 4)
        self.assertEqual(count % plate_points, 0)  # the plate's vertices each, not quads
        self.assertGreater(count, 50 * plate_points)
        out = cli("bake-scales", path, "-o", os.path.join(WORK_DIR, "plates_"), "--resolution", "128")
        self.assertIn("plates_ScaleHeight.png", out)


def bbox(node):
    return cmds.exactWorldBoundingBox(node)


class SimulationTest(unittest.TestCase):
    """Guide poses: sim curves, nHair, guide caches, and groom files that carry them."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")
        cmds.playbackOptions(minTime=1, maxTime=24)
        cmds.currentTime(1)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]
        self.groom = groomlab.create_groom(self.scalp)
        cmds.setAttr(self.groom + ".previewDensity", 0.3)
        curves = [cmds.curve(degree=1, point=[(0, 1, 0), (0.1, 1.7, 0.0), (0.2, 2.4, 0.0)]),
                  cmds.curve(degree=1, point=[(0, -1, 0), (0.1, -1.7, 0.0), (0.2, -2.4, 0.0)])]
        groomlab.guides_from_curves(self.groom, curves)
        groomlab.generate_children(self.groom, "primary", name="children", count=6)
        self.top = groomlab.guide_ids(self.groom, "primary")[0]

    def guides_bbox(self, group, frame=None):
        path = os.path.join(WORK_DIR, "guides_%s.abc" % group)
        kwargs = {"start": frame, "end": frame} if frame is not None else {}
        sim.export_guides(self.groom, path, group, **kwargs)
        text = cli("abc-info", path).splitlines()[1].split("bbox=")[1]
        return [float(v) for v in re.findall(r"-?[\d.]+(?:e-?\d+)?", text)]

    def tilt(self, curve, degrees, frame=None):
        """Rotate a sim curve about its root (a real pose change, not a translation)."""
        cmds.xform(curve, worldSpace=True, pivots=(0, 1, 0))
        if frame is None:
            cmds.setAttr(curve + ".rotateZ", degrees)
        else:
            cmds.setKeyframe(curve, attribute="rotateZ", time=frame, value=degrees)

    def test_sim_curves_pose_guides_and_children(self):
        before_children = self.guides_bbox("children")
        before = bbox(self.groom)
        made = sim.create_sim_curves(self.groom, "primary", dynamic=False)
        self.assertEqual(len(made["curves"]), 2)
        self.assertTrue(sim.has_poses(self.groom))
        for a, b in zip(self.guides_bbox("children"), before_children):  # the curves start on the guides
            self.assertAlmostEqual(a, b, delta=0.02)

        self.tilt(made["curves"][int(self.top)], -40)
        self.assertGreater(self.guides_bbox("children")[3], before_children[3] + 0.5)  # children followed
        self.assertNotEqual(bbox(self.groom), before)  # and the strands

        sim.remove_sim(self.groom, made)
        self.assertFalse(sim.has_poses(self.groom))
        self.assertFalse(cmds.objExists(made["group"]))
        self.assertEqual([round(v, 4) for v in bbox(self.groom)], [round(v, 4) for v in before])

    def test_guides_attached_to_a_driver_curve_follow_it_from_their_bind(self):
        """On a flat scalp, one guide (its children with it) attached to a rigged driver curve
        beside it (keyed here) and bound at frame 1 while the timeline sits elsewhere: as groomed at
        frame 1, swung over at frame 10; detached, as groomed again."""
        plane = cmds.polyPlane(width=2, height=2, subdivisionsX=10, subdivisionsY=10)[0]
        groom = groomlab.create_groom(plane)
        cmds.setAttr(groom + ".previewDensity", 0.3)
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 0, 0), (0, 0.7, 0), (0, 1.4, 0)])])
        groomlab.generate_children(groom, "primary", name="children", count=6)
        guide = int(groomlab.guide_ids(groom, "primary")[0])

        def strands_bbox(frame):  # the strands alone (the groom's bounds hold the groomed guides too)
            path = os.path.join(WORK_DIR, "driven_%d.abc" % frame)
            groomlab.export_alembic(groom, path, start=frame, end=frame, density=0.3)
            text = cli("abc-info", path).splitlines()[1].split("bbox=")[1]
            return [float(v) for v in re.findall(r"-?[\d.]+(?:e-?\d+)?", text)]

        before = strands_bbox(10)
        driver = cmds.curve(degree=1, point=[(0.03, 0, 0), (0.03, 0.7, 0), (0.03, 1.4, 0)])
        cmds.xform(driver, worldSpace=True, pivots=(0.03, 0, 0))
        cmds.setKeyframe(driver, attribute="rotateZ", time=1, value=0.0)
        cmds.setKeyframe(driver, attribute="rotateZ", time=10, value=40.0)
        cmds.currentTime(10)
        self.assertEqual(groomlab.attach_guides(groom, driver, guides=[guide], frame=1), [guide])
        self.assertEqual(groomlab.attached_guides(groom), {guide: [0]})
        self.assertEqual(groomlab.bind_frame(groom), 1)
        self.assertEqual(len(groomlab.driver_curves(groom)), 1)
        swung = strands_bbox(10)
        self.assertLess(swung[0], before[0] - 0.5)  # swung over toward -x with the driver, children too
        self.assertLess(swung[4], before[4] - 0.15)  # and down
        self.assertEqual(groomlab.bind_drivers(groom, frame=1), 1)
        self.assertEqual(groomlab.attached_guides(groom), {guide: [0]})  # binding again keeps them
        for a, b in zip(strands_bbox(1), before):
            self.assertAlmostEqual(a, b, delta=1e-3)  # the bind pose: as groomed
        self.assertEqual(groomlab.detach_guides(groom, guides=[guide]), 1)
        for a, b in zip(strands_bbox(10), before):
            self.assertAlmostEqual(a, b, delta=1e-3)  # detached: as groomed
        self.assertEqual(groomlab.remove_drivers(groom), 1)
        groomlab.clear_bind(groom)
        self.assertIsNone(groomlab.bind_frame(groom))

    def test_driver_curves_made_from_guides_drive_them(self):
        """create_drivers: a curve down the middle of two guides (one driver, both attached), or one
        on each guide exactly; the curves made at the bind frame while the timeline sits elsewhere.
        Rotating a made curve swings its guides; unmoved, they stay as groomed."""
        plane = cmds.polyPlane(width=2, height=2, subdivisionsX=10, subdivisionsY=10)[0]
        groom = groomlab.create_groom(plane)
        cmds.setAttr(groom + ".previewDensity", 0.3)
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(x, 0, 0), (x, 0.7, 0), (x, 1.4, 0)])
                                            for x in (-0.2, 0.2)])
        ids = [int(g) for g in groomlab.guide_ids(groom, "primary")]

        def strands_bbox(frame, name):
            path = os.path.join(WORK_DIR, "made_%s_%d.abc" % (name, frame))
            groomlab.export_alembic(groom, path, start=frame, end=frame, density=0.3)
            text = cli("abc-info", path).splitlines()[1].split("bbox=")[1]
            return [float(v) for v in re.findall(r"-?[\d.]+(?:e-?\d+)?", text)]

        before = strands_bbox(10, "before")
        cmds.currentTime(10)
        made = groomlab.create_drivers(groom, guides=ids)
        self.assertEqual(len(made), 1)
        self.assertEqual(groomlab.attached_guides(groom), {g: [0] for g in ids})
        self.assertEqual(groomlab.bind_frame(groom), 0)
        root = cmds.pointPosition(made[0] + ".cv[0]", world=True)
        tip = cmds.pointPosition(made[0] + ".cv[%d]" % (cmds.getAttr(made[0] + ".spans") +
                                                         cmds.getAttr(made[0] + ".degree") - 1), world=True)
        for a, b in zip(root, (0, 0, 0)):
            self.assertAlmostEqual(a, b, delta=1e-3)  # down the middle of the two
        for a, b in zip(tip, (0, 1.4, 0)):
            self.assertAlmostEqual(a, b, delta=1e-3)
        for a, b in zip(strands_bbox(10, "unmoved"), before):
            self.assertAlmostEqual(a, b, delta=1e-3)  # made on them: as groomed
        cmds.setKeyframe(made[0], attribute="rotateZ", time=1, value=0.0)
        cmds.setKeyframe(made[0], attribute="rotateZ", time=10, value=40.0)
        cmds.currentTime(1)
        cmds.currentTime(10)
        swung = strands_bbox(10, "swung")
        self.assertLess(swung[4], before[4] - 0.15)  # both swung over and down

        groomlab.remove_drivers(groom)
        groomlab.clear_bind(groom)
        each = groomlab.create_drivers(groom, guides=ids, per_guide=True)
        self.assertEqual(len(each), 2)
        attached = groomlab.attached_guides(groom)
        self.assertEqual(sorted(attached), ids)
        self.assertEqual(len(set(d[0] for d in attached.values())), 2)  # a curve each
        self.assertTrue(all(len(d) == 1 for d in attached.values()))
        for a, b in zip(strands_bbox(10, "each"), before):
            self.assertAlmostEqual(a, b, delta=1e-3)
        first = each[0]
        cmds.setKeyframe(first, attribute="rotateZ", time=1, value=0.0)
        cmds.setKeyframe(first, attribute="rotateZ", time=10, value=40.0)
        cmds.currentTime(1)
        cmds.currentTime(10)
        one = strands_bbox(10, "each_swung")  # the hair round that guide swung toward -x
        self.assertTrue(one[0] < before[0] - 0.3 or one[3] < before[3] - 0.3, (one, before))

    def test_nhair_simulation_drives_the_groom(self):
        made = sim.create_sim_curves(self.groom, "primary")
        self.assertTrue(made["hairSystem"] and made["nucleus"])
        self.assertEqual(len(made["outputs"]), 2)
        start = self.guides_bbox("children", frame=1)
        for frame in range(1, 25):  # nucleus needs every frame in order
            cmds.currentTime(frame)
        self.assertNotEqual(self.guides_bbox("children", frame=24), start)  # gravity took the guides down

        # The groom file carries the poses: the CLI matches Maya on the simulated frame.
        path = os.path.join(WORK_DIR, "sim.groom")
        written = groomlab.export_groom(self.groom, path, start=24, end=24)
        self.assertEqual([os.path.basename(p) for p in written], ["sim.groom", "sim.guides.abc"])
        self.assertIn("guide cache: ", cli("info", path))
        from_cli = os.path.join(WORK_DIR, "sim_cli.abc")
        from_maya = os.path.join(WORK_DIR, "sim_maya.abc")
        cli("eval", path, "-o", from_cli, "-f", "24", "--density", "0.3")
        groomlab.export_alembic(self.groom, from_maya, start=24, end=24, density=0.3)
        self.assertEqual(cli("abc-info", from_cli), cli("abc-info", from_maya))
        self.assertEqual(cli("hash", from_cli), cli("hash", from_maya))  # bit-exact
        sim.remove_sim(self.groom, made)

    def test_sim_presets_collision_and_bake(self):
        cmds.playbackOptions(minTime=1, maxTime=12)
        made = sim.create_sim_curves(self.groom, "primary", preset="long", collide=True, preroll=5,
                                     wind=((1, 0, 0), 2.0, 0.1))
        hair, nucleus = made["hairSystem"], made["nucleus"]
        self.assertAlmostEqual(cmds.getAttr(hair + ".bendResistance"), 0.5)
        self.assertEqual(len(cmds.getAttr(hair + ".attractionScale", multiIndices=True)), 3)
        self.assertTrue(made["collider"] and cmds.getAttr(hair + ".collide"))
        self.assertEqual(cmds.getAttr(nucleus + ".startFrame"), -4)
        self.assertEqual(cmds.getAttr(nucleus + ".windSpeed"), 2.0)
        with self.assertRaises(ValueError):
            sim.apply_sim_preset(hair, "nope")

        cache = os.path.join(WORK_DIR, "baked.guides.abc")
        self.assertEqual(sim.bake_sim(self.groom, cache, made), cache)
        self.assertTrue(os.path.exists(cache))
        self.assertFalse(cmds.objExists(hair))           # the live sim is gone
        self.assertFalse(cmds.objExists(made["collider"]))
        self.assertTrue(sim.has_poses(self.groom))        # the cache poses the groom
        self.assertNotEqual(self.guides_bbox("children", frame=12), self.guides_bbox("children", frame=1))
        sim.clear_guide_cache(self.groom)

    def test_guide_cache_round_trip(self):
        # "Simulate elsewhere": animate a sim curve, export the posed guides, then drive the groom from the file.
        made = sim.create_sim_curves(self.groom, "primary", dynamic=False)
        curve = made["curves"][int(self.top)]
        self.tilt(curve, 0, frame=1)
        self.tilt(curve, -40, frame=10)
        cache = os.path.join(WORK_DIR, "external_sim.abc")
        sim.export_guides(self.groom, cache, "primary", start=1, end=10)
        cmds.currentTime(10)
        posed = bbox(self.groom)
        sim.remove_sim(self.groom, made)
        self.assertNotEqual(bbox(self.groom), posed)

        sim.set_guide_cache(self.groom, cache)
        self.assertTrue(sim.has_poses(self.groom))
        self.assertEqual([round(v, 4) for v in bbox(self.groom)], [round(v, 4) for v in posed])
        tips = [self.guides_bbox("children", frame=f)[3] for f in (1, 5.5, 10)]  # 5.5: between samples
        self.assertLess(tips[0], tips[1])
        self.assertLess(tips[1], tips[2])
        sim.clear_guide_cache(self.groom)
        self.assertFalse(sim.has_poses(self.groom))


class RenderManForMayaTest(unittest.TestCase):
    """groomlab.renderman, what runs without RenderMan for Maya (Maya 2022 has none): the data string,
    the plugin's path, the pre-shape script on a stand-in for RfM's RenderManProcedural node (a
    network node with its data, frame and preShapePythonScript), and nothing failing without RfM.
    The procedural's side of the data string: tests/hosts/renderman (prman)."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        from groomlab import renderman
        self.rman = renderman
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")
        cmds.currentTime(1)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=16)[0]
        self.groom = groomlab.create_groom(self.scalp)
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)])
        groomlab.guides_from_curves(self.groom, [curve])
        groomlab.generate_children(self.groom, "primary", count=5)
        presets.apply_preset(self.groom, "hair_basics", fit=False)
        # A scene saved in a folder with a space: the groom files go beside it, quoted in the data.
        self.folder = os.path.join(WORK_DIR, "render man")
        os.makedirs(self.folder, exist_ok=True)
        cmds.file(rename=os.path.join(self.folder, "shot.ma"))
        cmds.file(save=True, type="mayaAscii", force=True)

    def stand_in(self):
        """A RenderManProcedural's params on a network node (RfM's node type is not in Maya 2022)."""
        node = cmds.createNode("network", name="hair_rmanShape")
        cmds.addAttr(node, longName="data", dataType="string")
        cmds.addAttr(node, longName="preShapePythonScript", dataType="string")
        cmds.addAttr(node, longName="frame", attributeType="float")
        return node

    def test_data_string(self):
        rman = self.rman
        data = rman.build_data(file="C:\\shots\\a b\\hair.groom", frame=12.5, density=0.5, motion_start=-0.25,
                               motion_end=0.25, motion_keys=3, search_paths=["C:/maps", "D:/more maps"], name="hair",
                               debug=True)
        self.assertEqual(data, '-file "C:/shots/a b/hair.groom" -frame 12.5 -density 0.5 -motionStart -0.25 '
                               '-motionEnd 0.25 -motionKeys 3 -searchPaths "C:/maps;D:/more maps" -name hair -debug 1')
        self.assertEqual(rman.parse_data(data), [
            ("file", "C:/shots/a b/hair.groom"), ("frame", "12.5"), ("density", "0.5"), ("motionStart", "-0.25"),
            ("motionEnd", "0.25"), ("motionKeys", "3"), ("searchPaths", "C:/maps;D:/more maps"), ("name", "hair"),
            ("debug", "1")])
        # Updates keep the other keys (unknown ones too) where they are; None removes a key.
        updated = rman.update_data("-foo bar -density 0.25 -FILE 'old one.groom' -debug -Frame 1",
                                   file="new.groom", frame=3, motion_start=None, debug=False)
        self.assertEqual(updated, "-foo bar -density 0.25 -file new.groom -frame 3")
        self.assertEqual(rman.update_data(updated, frame=4, width_scale=2),
                         "-foo bar -density 0.25 -file new.groom -frame 4 -widthScale 2")
        self.assertEqual(rman.quote("it's"), '"it\'s"')
        self.assertEqual(rman.quote('say "hi"'), "'say \"hi\"'")
        self.assertEqual(rman.split_words("-file 'a \"b\" c' -x"), ["-file", 'a "b" c', "-x"])
        self.assertEqual([rman.format_number(v) for v in (1.0, 12.25, -0.5, 1e-9, 3)], ["1", "12.25", "-0.5", "0", "3"])
        self.assertEqual([rman.frame_tag(f) for f in (3, 12.5, -2)], ["0003", "0012_500", "-002"])
        with self.assertRaises(TypeError):
            rman.build_data(bogus=1)

    def test_plugin_path(self):
        root = os.path.join(WORK_DIR, "module")
        folder = os.path.join(root, "procedurals", "renderman")
        os.makedirs(folder, exist_ok=True)
        dso = os.path.join(folder, self.rman.DSO_FILE)
        open(dso, "w").close()
        old = os.environ.pop(self.rman.DSO_ENV, None)
        try:
            self.assertEqual(self.rman.dso_path(root), dso.replace("\\", "/"))
            self.assertIsNone(self.rman.dso_path(os.path.join(WORK_DIR, "nowhere")))
            # The environment over the module: the file, or its folder.
            other = os.path.join(WORK_DIR, "other")
            os.makedirs(other, exist_ok=True)
            open(os.path.join(other, self.rman.DSO_FILE), "w").close()
            os.environ[self.rman.DSO_ENV] = other
            self.assertEqual(self.rman.dso_path(root), os.path.join(other, self.rman.DSO_FILE).replace("\\", "/"))
            os.environ[self.rman.DSO_ENV] = os.path.join(other, self.rman.DSO_FILE)
            self.assertEqual(self.rman.dso_path(root), os.path.join(other, self.rman.DSO_FILE).replace("\\", "/"))
        finally:
            os.environ.pop(self.rman.DSO_ENV, None)
            if old is not None:
                os.environ[self.rman.DSO_ENV] = old
        # The installed module's layout: <module>/scripts/groomlab/renderman.py.
        self.assertEqual(os.path.normpath(self.rman.module_root()),
                         os.path.normpath(os.path.dirname(SCRIPTS_DIR)))

    def test_a_groom_takes_a_material_as_any_shape(self):
        # The groom shape is renderable: the Hypershade's Assign Material (sets -forceElement) puts
        # it in a shading group, which V-Ray's translator and RenderMan's procedural then render with.
        sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="anyMaterialSG")
        cmds.sets(self.groom, edit=True, forceElement=sg)
        self.assertEqual(cmds.sets(sg, query=True), [cmds.ls(self.groom)[0]])
        transform = cmds.listRelatives(self.groom, parent=True)[0]
        other = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="otherMaterialSG")
        cmds.sets(transform, edit=True, forceElement=other)  # assigned through its transform
        self.assertEqual(cmds.sets(other, query=True), [cmds.ls(self.groom)[0]])
        self.assertIsNone(cmds.sets(sg, query=True))
        # Not a RenderMan material: the procedural keeps GroomLab's network.
        self.assertIsNone(self.rman.groom_shading_group(self.groom))

    def test_shading_group_reaches_the_procedural(self):
        # RfM takes a RenderManProcedural's material from its Shading Group attribute, connected from
        # the shading group (set membership is ignored: the hair rendered grey in RfM 27.3).
        node = self.stand_in()
        sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="hair_rman_SG")
        self.rman.assign_shading_group(node, sg)
        plug = node + "." + self.rman.SHADING_GROUP_ATTR
        self.assertEqual(cmds.getAttr(plug, type=True), "string")
        self.assertEqual(cmds.listConnections(plug, plugs=True), [sg + ".message"])
        other = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="other_rman_SG")
        self.rman.assign_shading_group(node, other)  # reassigned: the new one replaces it
        self.assertEqual(cmds.listConnections(plug, plugs=True), [other + ".message"])

    def test_without_renderman_for_maya(self):
        # Maya 2022 has no RfM: nothing is made, nothing breaks, no dialog.
        self.assertFalse(self.rman.rfm_loaded())
        with self.assertRaises(RuntimeError) as raised:
            self.rman.add_renderman_procedural(self.groom)
        self.assertIn("RenderMan for Maya is not loaded", str(raised.exception))
        with self.assertRaises(RuntimeError):
            self.rman.create_hair_network("x")
        self.assertEqual(cmds.ls("*_rman*"), [])
        # The plugin's hook (scene open, a groom made) makes nothing without RfM either.
        self.assertEqual(self.rman.sync_renderman(), [])
        self.assertEqual(cmds.ls("*_rman*"), [])
        # A pre-shape script for a node that is gone: a warning, no exception.
        self.assertIsNone(self.rman.pre_shape("0BADF00D-0000-0000-0000-000000000000"))
        # The menu's item stays away (the check runs as the Export menu opens).
        from groomlab.ui import menu
        menu._sync_renderman_item("noSuchMenu")
        self.assertFalse(cmds.menuItem(menu.RENDERMAN_ITEM, exists=True))

    def test_xpu_warns_once_per_scene(self):
        # RenderMan XPU renders no procedurals: with RfM's renderer set to XPU (rmanGlobals'
        # renderVariant), pre_shape warns once per scene, and still writes the data string (RIS).
        globals_node = cmds.createNode("network", name="rmanGlobals")
        cmds.addAttr(globals_node, longName="renderVariant", dataType="string")
        cmds.setAttr(globals_node + ".renderVariant", "ris", type="string")
        self.assertFalse(self.rman.renderer_is_xpu())
        self.assertFalse(self.rman.warn_if_xpu())
        cmds.setAttr(globals_node + ".renderVariant", "xpu", type="string")
        self.assertTrue(self.rman.renderer_is_xpu())
        self.assertTrue(self.rman.warn_if_xpu())
        self.assertFalse(self.rman.warn_if_xpu())  # once per scene
        self.assertTrue(self.rman.warn_if_xpu(force=True))
        node = self.stand_in()
        self.rman.link_procedural(node, self.groom)
        self.assertIn("-file", self.rman.pre_shape(node) or "")
        cmds.setAttr(globals_node + ".renderVariant", "ris", type="string")
        self.assertFalse(self.rman.warn_if_xpu())
        cmds.delete(globals_node)

    def test_pre_shape_revision_and_a_file_in_use(self):
        # The groom's change count goes into the data string (-revision): a change makes it differ,
        # so RfM's interactive render renders the procedural again; an unchanged run writes nothing
        # (a write would make RfM translate the node again, endlessly).
        node = self.stand_in()
        self.rman.link_procedural(node, self.groom)
        data = self.rman.pre_shape(node)
        revision = dict(self.rman.parse_data(data))["revision"]
        self.assertEqual(self.rman.pre_shape(node), data)
        cmds.setAttr(self.groom + ".renderDensity", 0.5)  # a change a render shows
        self.assertNotEqual(dict(self.rman.parse_data(self.rman.pre_shape(node)))["revision"], revision)
        # The frame's file open (prman renders it during an interactive render; Windows refuses to
        # replace it): the groom goes to <name>.ipr1.groom beside it, and the data string names that.
        path = dict(self.rman.parse_data(cmds.getAttr(node + ".data")))["file"]
        with open(path, "rb"):
            written = self.rman.export_free(self.groom, path)
        self.assertTrue(written.endswith(".ipr1.groom"), written)
        self.assertTrue(os.path.isfile(written))
        # Free again: the frame's own file, and the .ipr1 file of the last update removed.
        self.assertEqual(self.rman.export_free(self.groom, path), path)
        self.assertFalse(os.path.exists(written))

    def test_pre_shape_script(self):
        node = self.stand_in()
        self.rman.link_procedural(node, self.groom, density=0.5)
        cmds.setAttr(node + ".data", "-foo bar " + cmds.getAttr(node + ".data"), type="string")
        script = cmds.getAttr(node + ".preShapePythonScript")
        self.assertIn(cmds.ls(node, uuid=True)[0], script)
        # Renamed, it still finds its node (by UUID), as RfM runs the script: exec'd before translating.
        node = cmds.rename(node, "renamed_rmanShape")
        cmds.currentTime(3)
        exec(compile(script, "preShapePythonScript", "exec"), {})
        pairs = dict(self.rman.parse_data(cmds.getAttr(node + ".data")))
        self.assertEqual(pairs["foo"], "bar")  # the user's keys stay
        self.assertEqual(pairs["frame"], "3")
        self.assertEqual(pairs["density"], "0.5")
        self.assertEqual(pairs["name"], cmds.listRelatives(self.groom, parent=True)[0])
        self.assertNotIn("motionKeys", pairs)  # no rmanGlobals: no motion blur
        path = pairs["file"]
        self.assertTrue(path.endswith(".0003.groom"), path)
        self.assertIn("/render man/", path)
        self.assertTrue(os.path.isfile(path), path)
        self.assertIn('"%s"' % path, cmds.getAttr(node + ".data"))  # quoted: the folder has a space
        # The file renders the groom: the CLI evaluates it at that frame and density.
        out = cli("eval", path, "-o", os.path.join(WORK_DIR, "rman.abc"), "-f", "3", "--density", "0.5", "--stats")
        stats = json.loads(re.search(r"^stats: (\{.*\})$", out, re.M).group(1))
        self.assertGreater(stats["strands"], 10)

        # RfM's shutter (3D motion blur, centred, three samples) as motion keys; off again, none.
        globals_node = cmds.createNode("network", name="rmanGlobals")
        for attr, value in (("motionBlur", 1), ("shutterAngle", 180), ("shutterTiming", 1), ("motionSamples", 3)):
            cmds.addAttr(globals_node, longName=attr, attributeType="long", defaultValue=value)
        self.assertEqual(self.rman.shutter(), (-0.25, 0.25, 3))
        cmds.currentTime(4)
        pairs = dict(self.rman.parse_data(self.rman.pre_shape(node)))
        self.assertEqual((pairs["frame"], pairs["motionStart"], pairs["motionEnd"], pairs["motionKeys"]),
                         ("4", "-0.25", "0.25", "3"))
        self.assertTrue(pairs["file"].endswith(".0004.groom"))
        cmds.setAttr(node + "." + self.rman.SHUTTER_ATTR, False)  # the user's own keys
        cmds.setAttr(node + ".data", self.rman.update_data(cmds.getAttr(node + ".data"), motion_keys=5),
                     type="string")
        self.assertEqual(dict(self.rman.parse_data(self.rman.pre_shape(node)))["motionKeys"], "5")
        cmds.setAttr(node + "." + self.rman.SHUTTER_ATTR, True)
        cmds.setAttr(globals_node + ".motionBlur", 0)
        self.assertNotIn("motionKeys", dict(self.rman.parse_data(self.rman.pre_shape(node))))

        # A groom rendering from its own groom file (its scalp cached): that file, nothing exported.
        linked = os.path.join(self.folder, "linked.groom")
        groomlab.render_from_groom_file(self.groom, linked, scalp_cache=False)
        cmds.currentTime(5)
        pairs = dict(self.rman.parse_data(self.rman.pre_shape(node)))
        self.assertEqual(os.path.normcase(os.path.abspath(pairs["file"])), os.path.normcase(os.path.abspath(linked)))
        self.assertFalse(os.path.exists(path.replace(".0003.", ".0005.")))
        groomlab.render_curves(self.groom)


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
