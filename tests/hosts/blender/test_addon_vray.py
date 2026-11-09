# SPDX-License-Identifier: Apache-2.0
"""Final renders in V-Ray for Blender (the add-on's vray.py), when V-Ray for Blender is installed
for this Blender (its bootstrap in the user's scripts/addons/vray_blender: the throwaway profile
gets that scripts folder as a script directory, read only, for this run alone; else skipped).

- With V-Ray as the engine, a final render writes the groom's .vrscene (GroomLab's V-Ray hair:
  GeomMayaHair, BRDFHair4) and renders it through the groom's V-Ray Scene object instead of the
  hair Curves; after the render both are as they were (the Curves render, the V-Ray Scene object
  does not).
- The hair is in the image where Cycles puts it (the V-Ray Scene object's transform: groom space,
  Y up in centimeters, to the scalp's Z-up meters) and colored as the groom (brown: red over blue).
- Cycles renders as before (no V-Ray Scene object involved).
GROOMLAB_RENDER_KEEP, when set: the renders are kept there (V-Ray's, Cycles', V-Ray's own
conversion of the Curves for comparison)."""

import math
import os
import shutil
import sys
import unittest

import addon_utils
import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

KEEP = os.environ.get("GROOMLAB_RENDER_KEEP", "")
if sys.platform == "darwin":  # the user's Blender folder on macOS
    _USER_BLENDER = os.path.expanduser(os.path.join("~", "Library", "Application Support", "Blender"))
else:
    _USER_BLENDER = os.path.join(os.environ.get("APPDATA", ""), "Blender Foundation", "Blender")
VRAY_SCRIPTS = os.environ.get("GROOMLAB_VRAY_BLENDER_SCRIPTS") or os.path.join(
    _USER_BLENDER, "%d.%d" % bpy.app.version[:2], "scripts")


def enable_vray():
    """V-Ray for Blender from the user's scripts folder (added as a script directory of this
    throwaway profile, never saved); None when it is not installed or does not load."""
    if not os.path.isfile(os.path.join(VRAY_SCRIPTS, "addons", "vray_blender", "__init__.py")):
        return None
    d = bpy.context.preferences.filepaths.script_directories.new()
    d.directory = VRAY_SCRIPTS
    bpy.utils.refresh_script_paths()
    addon_utils.modules_refresh()
    mod = addon_utils.enable("vray_blender", default_set=True, handle_error=None)
    if mod is None or "VRAY_RENDER_RT" not in {getattr(e, "bl_idname", "") for e in bpy.types.RenderEngine.__subclasses__()}:
        return None
    return mod


VRAY = enable_vray()
pkg, m = bu.enable()
groom, graph = m["groom"], m["graph"]
import importlib  # noqa: E402
vray = importlib.import_module(bu.MODULE + ".vray")


def pixels(path):
    img = bpy.data.images.load(path)
    try:
        return np.array(img.pixels[:], np.float32).reshape(img.size[1], img.size[0], 4)
    finally:
        bpy.data.images.remove(img)


def render(scene, name):
    path = os.path.join(KEEP or bu.TMP, name + ".png")
    if os.path.isfile(path):
        os.remove(path)
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    if not os.path.isfile(path):  # V-Ray in the background leaves the saving to us
        bpy.data.images["Render Result"].save_render(path)
    return pixels(path)


def setup(scene, engine):
    scene.render.engine = engine
    scene.render.resolution_x = scene.render.resolution_y = 160
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    if engine == "CYCLES":
        scene.cycles.device = "CPU"
        scene.cycles.samples = 8
        scene.cycles.use_denoising = False


def stage(scene):
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
    scene.collection.objects.link(cam)
    cam.location = (0.0, -0.6, 0.06)
    cam.rotation_euler = (math.radians(85.0), 0.0, 0.0)
    scene.camera = cam
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 4.0
    az, el = math.radians(-35.0), math.radians(35.0)
    toward = Vector((math.sin(az) * math.cos(el), -math.cos(az) * math.cos(el), math.sin(el)))
    sun.rotation_euler = toward.to_track_quat("Z", "Y").to_euler()
    scene.collection.objects.link(sun)
    world = scene.world or bpy.data.worlds.new("World")
    scene.world = world
    world.color = (0.25, 0.25, 0.25)


def hair_mask(a, b):
    return np.abs(a[..., :3] - b[..., :3]).max(axis=2) > 0.04


@unittest.skipIf(VRAY is None, "V-Ray for Blender is not installed for this Blender")
class VRayRenderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.scene = scene = bpy.context.scene
        cls.head = bu.head_object()
        graph.presets()
        bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset="hair_basics", guides=60)
        cls.head.groomlab.preview_density = 0.2
        groom.flush()
        stage(scene)

    def test_vray_renders_the_groom_as_its_vray_scene(self):
        scene, head = self.scene, self.head
        hair = head.groomlab.hair
        setup(scene, "VRAY_RENDER_RT")
        image = render(scene, "vray_groom")
        obj = vray.find_object(head)
        self.assertIsNotNone(obj, "the groom's V-Ray Scene object")
        path = obj.data.vray.VRayScene["filepath"]
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for plugin in ("GeomMayaHair", "BRDFHair4", "TexHairSampler", "Node"):
            self.assertIn(plugin, text)
        # Back as they were: the Curves render (Cycles' and EEVEE's), the V-Ray Scene object does not.
        self.assertFalse(hair.hide_render)
        self.assertTrue(obj.hide_render)
        self.assertEqual(obj.vray.VRayAsset.assetType, vray.SCENE_ASSET)

        # The head alone (the hair hidden, GroomLab's V-Ray path off): where the hair is.
        hair.hide_render = True
        renders = vray.renders
        vray.renders = lambda s: False
        try:
            bald = render(scene, "vray_bald")
            if KEEP:  # V-Ray's own conversion of the hair Curves, for comparison
                hair.hide_render = False
                render(scene, "vray_curves_own")
        finally:
            vray.renders = renders
            hair.hide_render = False
        mask = hair_mask(image, bald)
        self.assertGreater(mask.mean(), 0.03, "the hair barely shows in V-Ray's render")
        rgb = image[..., :3][mask].mean(axis=0)
        self.assertGreater(rgb[0], rgb[2] * 1.2, "brown hair (red over blue): %s" % rgb)

        # Cycles: the hair where V-Ray has it (the V-Ray Scene object's transform), no V-Ray Scene object.
        setup(scene, "CYCLES")
        cycles = render(scene, "cycles_groom")
        hair.hide_render = True
        try:
            cycles_bald = render(scene, "cycles_bald")
        finally:
            hair.hide_render = False
        cycles_mask = hair_mask(cycles, cycles_bald)
        overlap = (mask & cycles_mask).sum() / max(1, (mask | cycles_mask).sum())
        self.assertGreater(overlap, 0.5, "V-Ray's hair and Cycles' overlap %.2f" % overlap)
        print("V-Ray hair %.3f of the image, mean %s; Cycles %.3f, mean %s; overlap %.2f"
              % (mask.mean(), rgb, cycles_mask.mean(), cycles[..., :3][cycles_mask].mean(axis=0), overlap))

    def test_frames_cover_motion_blur_and_animation(self):
        scene = self.scene
        scene.frame_current, scene.frame_start, scene.frame_end = 5, 1, 10
        scene.vray.SettingsMotionBlur.on = False
        scene.vray.Exporter.animation_mode = "FRAME"
        self.assertEqual(vray.frames(scene), (5.0, 5.0, 1.0))
        scene.vray.SettingsMotionBlur.on = True
        if scene.camera.data.vray.CameraPhysical.use:
            scene.camera.data.vray.CameraPhysical.use_moblur = True
        self.assertEqual(vray.frames(scene), (4.0, 6.0, 1.0))
        scene.vray.SettingsMotionBlur.on = False
        scene.vray.Exporter.animation_mode = "ANIMATION"
        self.assertEqual(vray.frames(scene), (1.0, 10.0, 1.0))
        scene.vray.Exporter.animation_mode = "FRAME"
        scene.frame_current = 1

    def test_the_vray_scene_object_turns_the_file_into_the_scalp(self):
        head = self.head
        obj = vray.ensure_object(head, self.scene)
        self.assertEqual(obj.parent, head)
        # The file is in meters, Y up: (0, 0.1, 0) is 0.1 m up (Z) in the scalp's space; (0, 0, 0.1)
        # toward the viewer (groom +Z) is Blender's -Y. No scale (V-Ray would not scale the widths).
        up = obj.matrix_basis @ Vector((0.0, 0.1, 0.0))
        front = obj.matrix_basis @ Vector((0.0, 0.0, 0.1))
        self.assertAlmostEqual((up - Vector((0.0, 0.0, 0.1))).length, 0.0, places=6)
        self.assertAlmostEqual((front - Vector((0.0, -0.1, 0.0))).length, 0.0, places=6)
        # A scaled scalp: the file carries its scale (the points and widths), the V-Ray Scene
        # object's world transform none (V-Ray would not scale the widths with it).
        head.scale = (2.0, 2.0, 2.0)
        bpy.context.view_layer.update()
        try:
            self.assertAlmostEqual(vray.scalp_scale(head), 2.0, places=6)
            obj = vray.ensure_object(head, self.scene)
            bpy.context.view_layer.update()
            for axis in obj.matrix_world.to_scale():
                self.assertAlmostEqual(axis, 1.0, places=5)
        finally:
            head.scale = (1.0, 1.0, 1.0)
            vray.ensure_object(head, self.scene)
            bpy.context.view_layer.update()


if __name__ == "__main__":
    bu.run(VRayRenderTest)
