# SPDX-License-Identifier: Apache-2.0
"""V-Ray Standalone renders from Maya (groomlab.vray_render): the studio's scene as a .vrscene (the
groom included, the meshes, the studio camera and its Arnold lights as V-Ray lights, a turntable's
keys), and, with vray.exe installed, a small frame rendered: an image that is not black with the
hair in the middle of it (the render without the groom differs from it around the picture's
middle), and a two-frame sequence. The V-Ray for Maya hook (groomlab.vray_hook) says "V-Ray for
Maya 2024+ required" here and changes nothing.

Usage: mayapy test_vray_render.py <GroomLab.mll> <maya/scripts dir> <extensions dir> <work dir> [vray.exe]
"""

import os
import re
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
EXTENSIONS_DIR = os.path.abspath(sys.argv[3])
WORK_DIR = os.path.abspath(sys.argv[4])
VRAY = sys.argv[5] if len(sys.argv) > 5 and os.path.isfile(sys.argv[5]) else ""
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
os.environ["MTOA_EXTENSIONS_PATH"] = os.pathsep.join(
    p for p in [EXTENSIONS_DIR, os.environ.get("MTOA_EXTENSIONS_PATH", "")] if p)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402
from groomlab import studio, vray_hook, vray_render, vray_scene  # noqa: E402


def make_head():
    """A head-sized sphere (10 cm radius) with a groom of short hair from four guides (test_studio.py's)."""
    head = cmds.polySphere(radius=10.0, subdivisionsX=40, subdivisionsY=20, name="head")[0]
    guides = [cmds.curve(degree=1, point=[(x * 10, y * 10, z * 10), (x * 13, y * 13 - 3, z * 13)])
              for x, y, z in ((0, 1, 0), (0.7, 0.7, 0), (-0.7, 0.7, 0), (0, 0.7, -0.7))]
    groom = groomlab.create_groom(head, guides, fit=True)
    groomlab.set_graph(groom, {
        "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
                  {"name": "scatter", "type": "Scatter", "params": {"density": 2.0}},
                  {"name": "interpolate", "type": "Interpolate"}, {"name": "output", "type": "Output"}],
        "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                        {"from": "scatter", "to": "interpolate", "input": "strands"},
                        {"from": "guides", "to": "interpolate", "input": "guides"},
                        {"from": "interpolate", "to": "output", "input": "strands"}],
        "output": "output"})
    return groom, head


def luminance(picture, x, y):
    p = picture.pixel(x, y)
    return (0.2126 * ((p >> 16) & 255) + 0.7152 * ((p >> 8) & 255) + 0.0722 * (p & 255)) / 255.0


def plugins(text, kind):
    return re.findall(r"\n%s (\S+) \{" % kind, text)


class VRayRenderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin("mtoa")
        cmds.loadPlugin(PLUGIN_PATH)
        if not os.path.isdir(WORK_DIR):
            os.makedirs(WORK_DIR)

    def setUp(self):
        cmds.file(new=True, force=True)
        self.groom, self.head = make_head()

    def test_studio_scene(self):
        # The portrait studio: the groom included, the head and backdrop as meshes, the studio camera,
        # every area light a LightRectangle and the dome a LightDome; the resolution the studio's.
        rig = studio.studio(self.groom, preset="portrait")
        path = os.path.join(WORK_DIR, "studio_scene", "scene.vrscene")
        info = vray_render.write_vray_scene(path, width=320, height=180)
        with open(path) as f:
            text = f.read()
        self.assertEqual(len(info["grooms"]), 1)
        groom_file = list(info["grooms"].values())[0]
        self.assertTrue(os.path.isfile(groom_file))
        self.assertIn('#include "%s"' % vray_scene.scene_path(groom_file), text)
        self.assertEqual(info["camera"].split("|")[-1], studio.CAMERA)
        self.assertEqual(len(plugins(text, "LightRectangle")), len(rig["lights"]) - 1)
        self.assertEqual(len(plugins(text, "LightDome")), 1)
        self.assertEqual(len(plugins(text, "RenderView")), 1)
        meshes = plugins(text, "GeomStaticMesh")
        self.assertIn("mesh_%sShape@mesh" % studio.BACKDROP, meshes)
        self.assertIn("mesh_headShape@mesh", meshes)
        self.assertIn("img_width=320;", text)
        # The backdrop casts no shadow (the studio turns it off): a light linker lists it.
        self.assertIn("ignored_shadow_lights", text)
        # The camera: the studio camera's matrix (unit axes), its horizontal field of view.
        m = vray_render._matrix(studio.CAMERA)
        self.assertIn("transform=" + vray_scene.transform(vray_scene.unscaled(m)[0]), text)
        shape = cmds.listRelatives(studio.CAMERA, shapes=True, fullPath=True)[0]
        fov = cmds.camera(shape, query=True, horizontalFieldOfView=True)
        self.assertAlmostEqual(float(re.search(r"\n  fov=([0-9.e-]+);", text).group(1)), fov * 3.14159265 / 180.0, 3)
        # An Arnold area light: the key's radiance 2^exposure (not normalized), half sizes its scales.
        key = re.search(r"LightRectangle groomlab_studio_keyShape \{(.*?)\n\}", text, re.S).group(1)
        exposure = cmds.getAttr(studio.GROUP + "_keyShape.aiExposure")
        self.assertAlmostEqual(float(re.search(r"intensity=([0-9.e-]+);", key).group(1)), 2.0 ** exposure, 4)
        sx = cmds.xform(studio.GROUP + "_key", query=True, worldSpace=True, scale=True)[0]
        self.assertAlmostEqual(float(re.search(r"u_size=([0-9.e-]+);", key).group(1)), sx, 3)
        self.assertIn("invisible=1;", key)

    def test_rim_drama_light_links(self):
        # Rim Drama's rims leave the backdrop unlit (Maya light links): V-Ray ignores them there too.
        studio.studio(self.groom, preset="rim_drama")
        path = os.path.join(WORK_DIR, "rim_scene", "scene.vrscene")
        vray_render.write_vray_scene(path, width=64, height=36)
        with open(path) as f:
            text = f.read()
        ignored = re.search(r"ignored_lights=List\((.*?)\);", text).group(1)
        self.assertIn("groomlab_studio_rimShape, mesh_%sShape@node" % studio.BACKDROP, ignored)
        self.assertNotIn("groomlab_studio_keyShape", ignored)

    def test_turntable_keys(self):
        # A turntable: the camera and lights orbit, so their transforms are keyed per frame; the
        # head stays (one value).
        studio.studio(self.groom, preset="turntable", frames=8)
        path = os.path.join(WORK_DIR, "turntable_scene", "scene.vrscene")
        info = vray_render.write_vray_scene(path, start=1, end=3, width=64, height=36)
        self.assertEqual(info["frames"], [1, 2, 3])
        with open(path) as f:
            text = f.read()
        camera = re.search(r"RenderView groomlab_camera \{\n  transform=(.*?);\n", text).group(1)
        self.assertTrue(camera.startswith("interpolate((1, Transform("), camera[:60])
        self.assertEqual(camera.count("Transform("), 3)
        head = re.search(r"Node mesh_headShape@node \{\n  transform=(.*?);\n", text).group(1)
        self.assertTrue(head.startswith("Transform("), head[:40])

    def test_no_lights_gets_a_default_rig(self):
        cmds.setAttr("defaultResolution.width", 64)
        cmds.setAttr("defaultResolution.height", 36)
        path = os.path.join(WORK_DIR, "plain_scene", "scene.vrscene")
        info = vray_render.write_vray_scene(path, camera="persp")
        self.assertEqual(info["lights"], ["groomlab_default_dome", "groomlab_default_key"])
        self.assertEqual((info["width"], info["height"]), (64, 36))

    def test_vray_for_maya_is_required(self):
        # Maya 2022 (no V-Ray for Maya): every entry point says so and nothing changes.
        if vray_hook.maya_version() >= 2024 and vray_hook.vray_loaded():
            self.skipTest("V-Ray for Maya is here")
        self.transforms_before = cmds.ls(type="transform", long=True)
        for call in (vray_hook.enable_hook, lambda: vray_hook.load_vrayscene(__file__),
                     lambda: vray_hook.export_and_load(self.groom)):
            with self.assertRaises(vray_hook.VRayRequired) as caught:
                call()
            self.assertIn(vray_hook.REQUIRED, str(caught.exception))
        self.assertFalse(cmds.objExists(vray_hook.SETTINGS))
        self.assertEqual(vray_hook.post_translate(), [])  # no vray.utils: a warning, nothing made
        self.assertEqual(cmds.ls(type="transform", long=True), self.transforms_before)

    def test_vray_path_setting(self):
        with self.assertRaises(ValueError):
            vray_render.set_vray_path(os.path.join(WORK_DIR, "no_such_vray.exe"))
        if VRAY:
            self.assertEqual(os.path.normcase(vray_render.set_vray_path(os.path.dirname(VRAY))), os.path.normcase(VRAY))
            self.assertEqual(os.path.normcase(vray_render.vray_path()), os.path.normcase(VRAY))
            vray_render.set_vray_path(None)

    @unittest.skipUnless(VRAY, "no vray.exe")
    def test_render_studio(self):
        # The portrait studio rendered small: not black, and the hair is in the middle (the render
        # without the groom differs from it there, and the difference is centered).
        studio.studio(self.groom, preset="portrait")
        os.environ[vray_scene.ENV_VAR] = VRAY
        width, height = 160, 90
        with_hair = vray_render.render_vray(width=width, height=height, quality="draft",
                                            folder=os.path.join(WORK_DIR, "render_hair"))
        without = vray_render.render_vray(grooms=[], width=width, height=height, quality="draft",
                                          folder=os.path.join(WORK_DIR, "render_bald"))
        for job in (with_hair, without):
            job.wait(600)
            self.assertTrue(job.ok(), "vray failed: %s (%s)" % (job.errors(), job.log))
        from groomlab.ui.qt import QtGui
        hair = QtGui.QImage(with_hair.images()[0])
        bald = QtGui.QImage(without.images()[0])
        self.assertEqual((hair.width(), hair.height()), (width, height))
        values = [luminance(hair, x, y) for y in range(0, height, 2) for x in range(0, width, 2)]
        mean = sum(values) / len(values)
        print("V-Ray studio portrait: %s, mean %.3f" % (with_hair.images()[0], mean))
        self.assertTrue(0.03 < mean < 0.9, "the render's mean brightness %.3f" % mean)
        changed, sx, sy = 0, 0.0, 0.0
        for y in range(height):
            for x in range(width):
                if abs(luminance(hair, x, y) - luminance(bald, x, y)) > 0.04:
                    changed += 1
                    sx += x
                    sy += y
        self.assertGreater(changed, 0.01 * width * height, "the hair shows")
        cx, cy = sx / changed / width, sy / changed / height
        print("hair: %d pixels, centered at (%.2f, %.2f) of the frame" % (changed, cx, cy))
        self.assertTrue(0.3 < cx < 0.7 and 0.2 < cy < 0.8, "the hair is in the middle: (%.2f, %.2f)" % (cx, cy))

    @unittest.skipUnless(VRAY, "no vray.exe")
    def test_render_sequence(self):
        # Two turntable frames: one image a frame, named by frame; cancelling a finished job is harmless.
        studio.studio(self.groom, preset="turntable", frames=8)
        os.environ[vray_scene.ENV_VAR] = VRAY
        job = vray_render.render_vray(start=1, end=2, width=48, height=27, quality="draft",
                                      folder=os.path.join(WORK_DIR, "render_sequence"))
        done, total, finished, _ = job.wait(600)
        self.assertTrue(job.ok(), "vray failed: %s (%s)" % (job.errors(), job.log))
        self.assertEqual((done, total, finished), (2, 2, True))
        self.assertEqual([os.path.basename(p) for p in job.images()], ["vray.0001.png", "vray.0002.png"])
        job.cancel()


if __name__ == "__main__":
    unittest.main(verbosity=2)
