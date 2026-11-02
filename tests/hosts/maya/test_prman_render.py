# SPDX-License-Identifier: Apache-2.0
"""RenderMan renders from Maya (groomlab.prman_render): the studio's scene as RIB (the groom through
GroomLab's RenderMan procedural, the meshes' archives, the studio camera and its Arnold lights as
RenderMan lights, light links, a turntable's frames, the render overrides), and, with prman and the
procedural, Nika's groom in the portrait studio rendered small: an image that is not black, with the
hair in the middle of it and colored (the render without the groom differs from it there, in a
warm color), and a two-frame sequence.

Usage: mayapy test_prman_render.py <GroomLab.mll> <maya/scripts dir> <extensions dir> <work dir>
       [procedural dir] [prman]
"""

import os
import re
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
EXTENSIONS_DIR = os.path.abspath(sys.argv[3])
WORK_DIR = os.path.abspath(sys.argv[4])
PROCEDURALS = sys.argv[5] if len(sys.argv) > 5 and os.path.isdir(sys.argv[5]) else ""
PRMAN = sys.argv[6] if len(sys.argv) > 6 and os.path.isfile(sys.argv[6]) else ""
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
os.environ["MTOA_EXTENSIONS_PATH"] = os.pathsep.join(
    p for p in [EXTENSIONS_DIR, os.environ.get("MTOA_EXTENSIONS_PATH", "")] if p)
if PROCEDURALS:
    os.environ["GROOMLAB_RENDERMAN_PROCEDURAL"] = PROCEDURALS

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402
from groomlab import prman_render, prman_scene, studio  # noqa: E402


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


def pixel(picture, x, y):
    p = picture.pixel(x, y)
    return ((p >> 16) & 255) / 255.0, ((p >> 8) & 255) / 255.0, (p & 255) / 255.0


def luminance(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def read(path):
    with open(path) as f:
        return f.read()


@unittest.skipUnless(PROCEDURALS, "no RenderMan procedural (groomlab_renderman) built")
class PrmanSceneTest(unittest.TestCase):
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
        # The portrait studio: the groom through the procedural, the head and backdrop as archives,
        # the studio camera, every area light a PxrRectLight and the dome a PxrDomeLight.
        rig = studio.studio(self.groom, preset="portrait")
        path = os.path.join(WORK_DIR, "studio_scene", "scene.rib")
        info = prman_render.write_prman_scene(path, width=320, height=180)
        text = read(path)
        self.assertEqual(len(info["grooms"]), 1)
        groom_file = list(info["grooms"].values())[0]
        self.assertTrue(os.path.isfile(groom_file))
        self.assertIn('"string filename" [%s]' % prman_scene.path_text(groom_file), text)
        self.assertIn('Option "searchpath" "string procedural" ["%s:@"]' % PROCEDURALS.replace("\\", "/"), text)
        self.assertEqual(info["camera"].split("|")[-1], studio.CAMERA)
        self.assertEqual(text.count('Light "PxrRectLight"'), len(rig["lights"]) - 1)
        self.assertEqual(text.count('Light "PxrDomeLight"'), 1)
        self.assertIn("Format 320 180 1", text)
        folder = os.path.dirname(path)
        for mesh in ("mesh_%sShape" % studio.BACKDROP, "mesh_headShape"):
            self.assertTrue(os.path.isfile(os.path.join(folder, mesh + ".rib")), mesh)
            self.assertIn("%s.rib" % mesh, text)
        # The backdrop casts no shadow (the studio turns it off).
        backdrop = re.search(r'\["mesh_%sShape"\]\n(.*?)AttributeEnd' % studio.BACKDROP, text, re.S).group(1)
        self.assertIn('"int transmission" [0]', backdrop)
        # The camera: PxrPerspective's fov across the shorter side (the height here).
        shape = cmds.listRelatives(studio.CAMERA, shapes=True, fullPath=True)[0]
        horizontal = cmds.camera(shape, query=True, horizontalFieldOfView=True)
        fov = float(re.search(r'"PxrPerspective" "float fov" \[([0-9.e-]+)\]', text).group(1))
        self.assertAlmostEqual(fov, prman_scene.shorter_side_fov(horizontal * 3.14159265 / 180.0, 320, 180), 2)
        # An Arnold area light: the key's radiance 2^exposure (not normalized) times its spread's gain.
        key = re.search(r'Light "PxrRectLight" "groomlab_studio_keyShape" (.*?)\n', text).group(1)
        exposure = cmds.getAttr(studio.GROUP + "_keyShape.aiExposure")
        focus, gain = prman_scene.spread_to_focus(cmds.getAttr(studio.GROUP + "_keyShape.aiSpread"))
        self.assertAlmostEqual(float(re.search(r'"float intensity" \[([0-9.e-]+)\]', key).group(1)),
                               2.0 ** exposure * gain, 3)
        self.assertAlmostEqual(float(re.search(r'"float emissionFocus" \[([0-9.e-]+)\]', key).group(1)), focus, 4)
        # The hair network, its tilt the groom's.
        self.assertIn('Bxdf "PxrMarschnerHair"', text)

    def test_rim_drama_light_links(self):
        # Rim Drama's rims leave the backdrop unlit (Maya light links): excluded light groups.
        studio.studio(self.groom, preset="rim_drama")
        path = os.path.join(WORK_DIR, "rim_scene", "scene.rib")
        prman_render.write_prman_scene(path, width=64, height=36)
        text = read(path)
        excluded = re.search(r'\["mesh_%sShape"\]\n.*?"string excludesubset" \["(.*?)"\]' % studio.BACKDROP, text,
                             re.S).group(1).split()
        self.assertEqual(sorted(excluded), ["groomlab_studio_rim2Shape", "groomlab_studio_rimShape"])
        self.assertIn('Attribute "grouping" "string membership" ["groomlab_studio_rimShape"]', text)

    def test_turntable_frames(self):
        # A turntable: a frame block a frame, the camera turning; the head's archive written once.
        studio.studio(self.groom, preset="turntable", frames=8)
        path = os.path.join(WORK_DIR, "turntable_scene", "scene.rib")
        info = prman_render.write_prman_scene(path, start=1, end=3, width=64, height=36)
        self.assertEqual(info["frames"], [1, 2, 3])
        text = read(path)
        self.assertEqual(re.findall(r"FrameBegin (\d+)", text), ["1", "2", "3"])
        cameras = re.findall(r"Scale 1 1 -1\nConcatTransform \[(.*?)\]", text)
        self.assertEqual(len(set(cameras)), 3)
        self.assertEqual(sorted(re.findall(r'"float frame" \[(\d+)\]', text)), ["1", "2", "3"])
        self.assertFalse(os.path.isfile(os.path.join(os.path.dirname(path), "mesh_headShape.0001.rib")))

    def test_overrides_and_default_lights(self):
        cmds.setAttr("defaultResolution.width", 64)
        cmds.setAttr("defaultResolution.height", 36)
        cmds.setAttr(self.groom + ".renderDensity", 0.5)
        cmds.setAttr(self.groom + ".renderWidthScale", 2.0)
        path = os.path.join(WORK_DIR, "plain_scene", "scene.rib")
        info = prman_render.write_prman_scene(path, camera="persp", density=0.5, motion_blur=True)
        self.assertEqual(info["lights"], ["groomlab_default_dome", "groomlab_default_key"])
        self.assertEqual((info["width"], info["height"]), (64, 36))
        text = read(path)
        self.assertIn('"float density" [0.25]', text)
        self.assertIn('"float widthScale" [2]', text)
        self.assertIn('"int motionKeys" [2]', text)
        self.assertIn("Shutter 0 0.5", text)
        self.assertIn('Light "PxrDistantLight"', text)

    def test_menu_and_path_setting(self):
        from groomlab.ui import menu
        self.assertIn("render.prman", menu.ACTIONS)
        # The Render dialog's renderers (one dialog for V-Ray Standalone and RenderMan).
        specs = menu._standalone_renderers()
        self.assertEqual(sorted(specs), ["hw2", "prman", "vray"])
        self.assertIs(specs["prman"]["render"], groomlab.render_prman)
        self.assertTrue(specs["prman"]["motion_blur"])
        self.assertIn("non-commercial", specs["prman"]["not_found"])
        with self.assertRaises(ValueError):
            prman_render.set_prman_path(os.path.join(WORK_DIR, "no_such_prman.exe"))
        if PRMAN:
            self.assertEqual(os.path.normcase(prman_render.set_prman_path(os.path.dirname(PRMAN))),
                             os.path.normcase(PRMAN))
            self.assertEqual(os.path.normcase(prman_render.prman_path()), os.path.normcase(PRMAN))
            prman_render.set_prman_path(None)

    @unittest.skipUnless(PRMAN, "no prman")
    def test_render_nika_studio(self):
        # Nika's groom in the portrait studio, small and quick: not black; the hair in the middle
        # (the render without the groom differs from it there, the difference centered) and colored.
        cmds.file(new=True, force=True)
        groom = groomlab.create_groom(groomlab.add_sample("nika"), fit=True)
        studio.studio(groom, preset="portrait")
        os.environ[prman_scene.ENV_VAR] = PRMAN
        width, height = 160, 90
        with_hair = groomlab.render_prman(width=width, height=height, quality="draft",
                                          folder=os.path.join(WORK_DIR, "render_hair"))
        without = groomlab.render_prman(grooms=[], width=width, height=height, quality="draft",
                                        folder=os.path.join(WORK_DIR, "render_bald"))
        for job in (with_hair, without):
            job.wait(600)
            self.assertTrue(job.ok(), "prman failed: %s (%s)" % (job.errors(), job.log))
        from groomlab.ui.qt import QtGui
        hair = QtGui.QImage(with_hair.images()[0])
        bald = QtGui.QImage(without.images()[0])
        self.assertEqual((hair.width(), hair.height()), (width, height))
        values = [luminance(pixel(hair, x, y)) for y in range(0, height, 2) for x in range(0, width, 2)]
        mean = sum(values) / len(values)
        print("RenderMan studio portrait: %s, mean %.3f" % (with_hair.images()[0], mean))
        self.assertTrue(0.05 < mean < 0.9, "the render's mean brightness %.3f" % mean)
        changed, sx, sy, warm, warm_bald = 0, 0.0, 0.0, 0.0, 0.0
        for y in range(height):
            for x in range(width):
                a, b = pixel(hair, x, y), pixel(bald, x, y)
                if abs(luminance(a) - luminance(b)) > 0.04:
                    changed += 1
                    sx += x
                    sy += y
                    warm += a[0] - a[2]
                    warm_bald += b[0] - b[2]
        self.assertGreater(changed, 0.01 * width * height, "the hair shows")
        cx, cy = sx / changed / width, sy / changed / height
        warm, warm_bald = warm / changed, warm_bald / changed
        print("hair: %d pixels, centered at (%.2f, %.2f), red over blue %.3f (%.3f bald)" % (changed, cx, cy, warm,
                                                                                             warm_bald))
        self.assertTrue(0.3 < cx < 0.7 and 0.2 < cy < 0.8, "the hair is in the middle: (%.2f, %.2f)" % (cx, cy))
        # Nika's default hair is a warm blonde: where it shows the picture turns warmer than the grey
        # head and backdrop there (the pigment reached prman: not a grey or a white hair).
        self.assertGreater(warm - warm_bald, 0.03, "the hair is colored")

    @unittest.skipUnless(PRMAN, "no prman")
    def test_render_sequence(self):
        # Two turntable frames: one image a frame, named by frame; cancelling a finished job is harmless.
        studio.studio(self.groom, preset="turntable", frames=8)
        os.environ[prman_scene.ENV_VAR] = PRMAN
        job = groomlab.render_prman(start=1, end=2, width=48, height=27, quality="draft",
                                    folder=os.path.join(WORK_DIR, "render_sequence"))
        done, total, finished, _ = job.wait(600)
        self.assertTrue(job.ok(), "prman failed: %s (%s)" % (job.errors(), job.log))
        self.assertEqual((done, total, finished), (2, 2, True))
        self.assertEqual([os.path.basename(p) for p in job.images()], ["prman.0001.png", "prman.0002.png"])
        job.cancel()
        self.assertFalse(job.ok())  # cancelled: no longer counted a success


if __name__ == "__main__":
    unittest.main(verbosity=2)
