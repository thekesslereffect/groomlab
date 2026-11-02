# SPDX-License-Identifier: Apache-2.0
"""Studio lighting (groomlab.studio): every preset builds one rig that scales with the head and
frames it, relights in place, undoes in one step, and remove_studio puts the head's material and the
render settings back. With --render, a small Arnold render of the portrait preset (kick) instead.

Usage: mayapy test_studio.py <GroomLab.mll> <maya/scripts dir> <extensions dir> <work dir> [--render]
The extensions dir holds groomLabTranslator (put on MTOA_EXTENSIONS_PATH before MtoA loads).
"""

import math
import os
import subprocess
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
EXTENSIONS_DIR = os.path.abspath(sys.argv[3])
WORK_DIR = os.path.abspath(sys.argv[4])
RENDER = "--render" in sys.argv[5:]
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
os.environ["MTOA_EXTENSIONS_PATH"] = os.pathsep.join(
    p for p in [EXTENSIONS_DIR, os.environ.get("MTOA_EXTENSIONS_PATH", "")] if p)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402
from groomlab import studio  # noqa: E402


def make_head(scale=1.0):
    """A head-sized sphere (10 cm radius times `scale`) with a lambert of its own and a groom of short
    hair from four guides. Returns (groom, head transform, lambert's shading group)."""
    head = cmds.polySphere(radius=10.0 * scale, subdivisionsX=40, subdivisionsY=20, name="head")[0]
    skin = cmds.shadingNode("lambert", asShader=True, name="skin")
    group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="skinSG")
    cmds.connectAttr(skin + ".outColor", group + ".surfaceShader")
    cmds.sets(head, edit=True, forceElement=group)
    s = 10.0 * scale
    guides = [cmds.curve(degree=1, point=[(x * s, y * s, z * s), (x * 1.3 * s, y * 1.3 * s - 0.3 * s, z * 1.3 * s)])
              for x, y, z in ((0, 1, 0), (0.7, 0.7, 0), (-0.7, 0.7, 0), (0, 0.7, -0.7))]
    groom = groomlab.create_groom(head, guides, fit=True)
    graph = {
        "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "guides", "type": "Guides"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 2.0 / (scale * scale)}},
            {"name": "interpolate", "type": "Interpolate"},
            {"name": "output", "type": "Output"},
        ],
        "connections": [
            {"from": "scalp", "to": "scatter", "input": "scalp"},
            {"from": "scatter", "to": "interpolate", "input": "strands"},
            {"from": "guides", "to": "interpolate", "input": "guides"},
            {"from": "interpolate", "to": "output", "input": "strands"},
        ],
        "output": "output",
    }
    groomlab.set_graph(groom, graph)
    return groom, head, group


def set_up_axis(axis):
    if cmds.upAxis(query=True, axis=True) != axis:
        cmds.upAxis(axis=axis)


def assignment(mesh):
    shape = cmds.listRelatives(mesh, shapes=True, fullPath=True)[0]
    return sorted(set(cmds.listConnections(shape, type="shadingEngine") or []))


def project(camera, point):
    """A world point in the camera's frame: (x, y) from -1 to 1 across the film gate."""
    shape = cmds.listRelatives(camera, shapes=True, fullPath=True)[0]
    m = cmds.xform(camera, query=True, worldSpace=True, matrix=True)
    axes = [m[0:3], m[4:7], m[8:11]]
    origin = m[12:15]
    d = [point[i] - origin[i] for i in range(3)]
    local = [sum(d[i] * axes[k][i] for i in range(3)) / sum(a * a for a in axes[k]) for k in range(3)]
    focal = cmds.getAttr(shape + ".focalLength")
    half_w = cmds.getAttr(shape + ".horizontalFilmAperture") * 25.4 * 0.5
    half_h = cmds.getAttr(shape + ".verticalFilmAperture") * 25.4 * 0.5
    depth = -local[2]
    return (local[0] / depth * focal / half_w, local[1] / depth * focal / half_h)


def lights_of_rig():
    return sorted(cmds.listRelatives(studio.LIGHTS, children=True, type="transform") or [])


@unittest.skipIf(RENDER, "the rig tests run without --render")
class StudioTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin("mtoa")
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        set_up_axis("y")
        self.groom, self.head, self.skin = make_head()

    def tearDown(self):
        set_up_axis("y")

    def check_framed(self, info):
        box = info["box"]
        center = [(box[i] + box[i + 3]) * 0.5 for i in range(3)]
        up = [0.0, 0.0, 1.0] if cmds.upAxis(query=True, axis=True) == "z" else [0.0, 1.0, 0.0]
        half = 0.5 * sum((box[i + 3] - box[i]) * up[i] for i in range(3))
        x, y = project(studio.CAMERA, center)
        self.assertLess(abs(x), 0.3, "the subject is centered across")
        top = project(studio.CAMERA, [center[i] + up[i] * half for i in range(3)])
        bottom = project(studio.CAMERA, [center[i] - up[i] * half for i in range(3)])
        self.assertTrue(0.6 < top[1] < 1.0, "headroom above the hair: %r" % (top,))
        self.assertTrue(-1.0 < bottom[1] < -0.4, "the bottom in the frame: %r" % (bottom,))

    def test_every_preset_builds_its_rig_and_frames_the_head(self):
        for name in studio.preset_names():
            rig = studio.studio(self.groom, preset=name)
            look = studio.get_preset(name)
            self.assertEqual(rig["preset"], name)
            self.assertEqual(studio.current_preset(), name)
            for node in (studio.GROUP, studio.CAMERA, studio.BACKDROP, studio.DOME):
                self.assertTrue(cmds.objExists(node), "%s: %s" % (name, node))
            wanted = sorted([studio.GROUP + "_" + light["name"] for light in look["lights"]] + [studio.DOME])
            self.assertEqual(lights_of_rig(), wanted, name)
            for light in look["lights"]:
                shape = cmds.listRelatives(studio.GROUP + "_" + light["name"], shapes=True)[0]
                self.assertEqual(cmds.nodeType(shape), "aiAreaLight")
                self.assertAlmostEqual(cmds.getAttr(shape + ".aiExposure"), light["exposure"], places=4)
                lit = cmds.lightlink(query=True, light=shape, shapes=False) or []
                self.assertEqual(studio.BACKDROP in lit, light.get("backdrop", True), "%s %s" % (name, light["name"]))
            self.assertEqual(assignment(self.head), [studio.MANNEQUIN_MATERIAL + "SG"])
            self.check_framed(studio.subject(self.groom))
            # The backdrop is behind the head from the camera and under it.
            backdrop = cmds.exactWorldBoundingBox(studio.BACKDROP)
            head = cmds.exactWorldBoundingBox(self.head)
            self.assertLess(backdrop[1], head[1])
            self.assertTrue(cmds.getAttr(cmds.listRelatives(studio.CAMERA, shapes=True)[0] + ".renderable"))
            self.assertFalse(cmds.getAttr("perspShape.renderable"))
        self.assertEqual(len(cmds.ls(studio.GROUP)), 1)

    def test_the_rig_scales_with_the_head(self):
        def measure():
            studio.studio(self.groom, preset="portrait")
            hair = studio.subject(self.groom)["hair"]
            key = cmds.xform(studio.GROUP + "_key", query=True, worldSpace=True, translation=True)
            camera = cmds.xform(studio.CAMERA, query=True, worldSpace=True, translation=True)
            backdrop = cmds.exactWorldBoundingBox(studio.BACKDROP)
            key_scale = cmds.xform(studio.GROUP + "_key", query=True, worldSpace=True, scale=True)
            return (math.sqrt(sum((key[i] - hair[i]) ** 2 for i in range(3))),
                    math.sqrt(sum((camera[i] - hair[i]) ** 2 for i in range(3))),
                    backdrop[4] - backdrop[1], key_scale[0])
        small = measure()
        cmds.file(new=True, force=True)
        self.groom, self.head, self.skin = make_head(scale=2.0)
        big = measure()
        for a, b in zip(small, big):
            self.assertAlmostEqual(b / a, 2.0, delta=0.15)
        self.check_framed(studio.subject(self.groom))

    def test_a_z_up_scene_gets_the_studio_turned(self):
        cmds.file(new=True, force=True)
        set_up_axis("z")
        self.groom, self.head, self.skin = make_head()
        studio.studio(self.groom)
        hair = studio.subject(self.groom)["hair"]
        key = cmds.xform(studio.GROUP + "_key", query=True, worldSpace=True, translation=True)
        self.assertGreater(key[2], hair[2], "the key is above the head")
        backdrop = cmds.exactWorldBoundingBox(studio.BACKDROP)
        self.assertLess(backdrop[2], cmds.exactWorldBoundingBox(self.head)[2], "the floor is under the head")
        self.check_framed(studio.subject(self.groom))

    def test_undo_removes_the_studio_in_one_step(self):
        cmds.undoInfo(state=True, infinity=True)
        cmds.flushUndo()
        studio.studio(self.groom)
        self.assertTrue(studio.exists())
        cmds.undo()
        self.assertFalse(studio.exists())
        self.assertFalse(cmds.objExists(studio.CAMERA))
        self.assertEqual(assignment(self.head), [self.skin])

    def test_remove_studio_restores_the_material_and_render_settings(self):
        import mtoa.core
        mtoa.core.createOptions()
        cmds.setAttr("defaultArnoldRenderOptions.AASamples", 3)
        cmds.setAttr("defaultResolution.width", 640)
        cmds.setAttr("defaultResolution.height", 480)
        cmds.playbackOptions(minTime=1, maxTime=24)
        studio.studio(self.groom, preset="turntable")
        self.assertEqual(cmds.getAttr("defaultArnoldRenderOptions.AASamples"), 5)
        self.assertEqual(cmds.getAttr("defaultResolution.width"), 1920)
        self.assertEqual(cmds.playbackOptions(query=True, maxTime=True), 120)
        self.assertTrue(cmds.keyframe(studio.ORBIT, query=True, keyframeCount=True))
        # Switching presets keeps one rig and what was there before the first.
        studio.studio(self.groom, preset="rim_drama")
        studio.studio(self.groom, preset="beauty")
        self.assertEqual(len(cmds.ls(studio.GROUP + "*", type="transform")),
                         len(cmds.ls([studio.GROUP, studio.ORBIT, studio.LIGHTS, studio.CAMERA, studio.BACKDROP]))
                         + len(studio.get_preset("beauty")["lights"]) + 1)
        self.assertFalse(cmds.keyframe(studio.ORBIT, query=True, keyframeCount=True))
        self.assertEqual(cmds.playbackOptions(query=True, maxTime=True), 24)
        self.assertTrue(studio.remove_studio())
        self.assertFalse(studio.exists())
        self.assertEqual(assignment(self.head), [self.skin])
        self.assertEqual(cmds.getAttr("defaultArnoldRenderOptions.AASamples"), 3)
        self.assertEqual((cmds.getAttr("defaultResolution.width"), cmds.getAttr("defaultResolution.height")),
                         (640, 480))
        self.assertTrue(cmds.getAttr("perspShape.renderable"))
        for node in (studio.MANNEQUIN_MATERIAL, studio.BACKDROP_MATERIAL):
            self.assertFalse(cmds.objExists(node))
        self.assertFalse(studio.remove_studio())

    def test_options_off_leave_the_scene_alone(self):
        import mtoa.core
        mtoa.core.createOptions()
        cmds.setAttr("defaultArnoldRenderOptions.AASamples", 3)
        rig = studio.studio(self.groom, mannequin=False, camera=False, render_settings=False)
        self.assertIsNone(rig["camera"])
        self.assertFalse(cmds.objExists(studio.CAMERA))
        self.assertEqual(assignment(self.head), [self.skin])
        self.assertEqual(cmds.getAttr("defaultArnoldRenderOptions.AASamples"), 3)
        self.assertTrue(cmds.getAttr("perspShape.renderable"))
        # Turned on later, then off again: each part goes back.
        studio.studio(self.groom)
        self.assertEqual(assignment(self.head), [studio.MANNEQUIN_MATERIAL + "SG"])
        studio.studio(self.groom, mannequin=False, render_settings=False)
        self.assertEqual(assignment(self.head), [self.skin])
        self.assertEqual(cmds.getAttr("defaultArnoldRenderOptions.AASamples"), 3)

    def test_without_a_groom_it_says_so(self):
        cmds.select(clear=True)
        with self.assertRaises(ValueError):
            studio.studio()
        with self.assertRaises(ValueError):
            studio.studio(self.groom, preset="no_such_preset")


@unittest.skipIf(not RENDER, "renders only with --render")
class StudioRenderTest(unittest.TestCase):
    """The portrait preset rendered small by kick: an image, neither black nor blown out."""

    def test_portrait_renders(self):
        cmds.loadPlugin("mtoa")
        cmds.loadPlugin(PLUGIN_PATH)
        cmds.file(new=True, force=True)
        groom, head, _skin = make_head()
        studio.studio(groom, preset="portrait")
        cmds.setAttr("defaultArnoldRenderOptions.AASamples", 2)
        cmds.setAttr("defaultArnoldDriver.ai_translator", "png", type="string")
        if not os.path.isdir(WORK_DIR):
            os.makedirs(WORK_DIR)
        scene = os.path.join(WORK_DIR, "studio_portrait.ass")
        image = os.path.join(WORK_DIR, "studio_portrait.png")
        for path in (scene, image):
            if os.path.exists(path):
                os.remove(path)
        cmds.arnoldExportAss(filename=scene, asciiAss=True, cam=studio.CAMERA)
        mtoa = os.path.dirname(os.path.dirname(cmds.pluginInfo("mtoa", query=True, path=True)))
        command = [os.path.join(mtoa, "bin", "kick"), "-i", scene, "-l", os.path.join(mtoa, "shaders"),
                   "-o", image, "-r", "160", "90", "-dw", "-dp", "-sl", "-v", "1"]
        for attempt in range(2):  # kick now and then stalls at startup (see test_arnold.py)
            try:
                result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, universal_newlines=True, timeout=180)
                break
            except subprocess.TimeoutExpired:
                if attempt == 1:
                    self.fail("kick stalled twice")
        self.assertEqual(result.returncode, 0, result.stdout[-3000:])
        from groomlab.ui.qt import QtGui
        picture = QtGui.QImage(image)
        self.assertEqual((picture.width(), picture.height()), (160, 90))
        values = []
        for y in range(0, 90, 3):
            for x in range(0, 160, 3):
                p = picture.pixel(x, y)
                values.append((0.2126 * ((p >> 16) & 255) + 0.7152 * ((p >> 8) & 255) + 0.0722 * (p & 255)) / 255.0)
        mean = sum(values) / len(values)
        print("studio portrait: %s, mean %.3f" % (image, mean))
        self.assertTrue(0.05 < mean < 0.9, "the render's mean brightness %.3f" % mean)
        # The head (the middle of the frame) is lit: brighter than the darkest of the backdrop.
        self.assertGreater(max(values), 0.4)


if __name__ == "__main__":
    unittest.main(verbosity=2)
