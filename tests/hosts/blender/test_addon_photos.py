# SPDX-License-Identifier: Apache-2.0
"""The Photos tab (groomlab/photos.py) and the last inputs (pipeline_ops.py) in a headless Blender, on
synthetic photos made here (never real people's): a photo imported as a camera with the photo as its
background image, aimed at the groom; point pairs picked on the photo and the mesh (the pick tool's
functions), Solve lining the camera up with a camera made up for the test (its position, rotation and
focal length back), the photo's pixels under the camera view agreeing with the solve's camera model,
pick again, delete, clear, lock and rename; the photos kept with the groom (its .groom file) and back
as cameras when the file is opened; Guides from Photos into fromReference; Alembic curves as guides
(the groom made on a selected mesh first), Bake Node to Guides, and a param painted on the guides."""

import importlib
import json
import math
import os
import sys
import types

import bpy
import numpy as np
from mathutils import Euler, Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402
import unittest  # noqa: E402

pkg, m = bu.enable()
groom, process = m["groom"], m["process"]
photos = importlib.import_module(bu.MODULE + ".photos")
pipeline = importlib.import_module(bu.MODULE + ".pipeline")
tools = importlib.import_module(bu.MODULE + ".tools")
graph_ui = importlib.import_module(bu.MODULE + ".graph_ui")


def write_photo(path, width, height, stripes=False):
    """A synthetic photo: plain gray, or (stripes) dark vertical strands on white over its middle."""
    rgb = np.full((height, width, 3), 0.55 if not stripes else 1.0, np.float32)
    if stripes:
        x0, x1, y0, y1 = int(width * 0.25), int(width * 0.75), int(height * 0.15), int(height * 0.85)
        rgb[y0:y1, x0:x1:3] = (0.25, 0.19, 0.12)
    rgba = np.concatenate([rgb, np.ones((height, width, 1), np.float32)], axis=2)[::-1]  # Blender's rows: bottom first
    image = bpy.data.images.new("synthetic_photo", width, height)
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)
    return path


def project(cam_matrix, focal, width, height, p):
    """Pixels (from the top-left, v down) of world point p through a camera (the solve's model)."""
    c = cam_matrix.inverted() @ Vector(p)
    return [0.5 * width + focal * c.x / -c.z, 0.5 * height - focal * c.y / -c.z]


class Layout(bu.FakeLayout):
    """A FakeLayout with the menus and grids the Groom Tools draw."""

    def _sub(self, *args, **kwargs):
        return Layout(self.log)

    row = column = box = split = grid_flow = _sub

    def menu(self, idname, **kwargs):
        self.assertion = getattr(bpy.types, idname)
        self.log.append(("menu", idname))


def hcall(scalp, cmd, **args):
    return process.call(cmd, session=groom.ensure(scalp).session, **args)


class PhotosTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.head = bu.head_object()
        assert bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset="hair_basics", guides=30) == {"FINISHED"}
        bpy.context.scene.groomlab.groom = cls.head
        tools.refresh_info(cls.head)

    def test_1_import_pick_solve(self):
        head, scene = self.head, bpy.context.scene
        path = write_photo(os.path.join(bu.TMP, "gray_front.png"), 400, 500)
        self.assertEqual(bpy.ops.groomlab.photo_import(filepath=path), {"FINISHED"})
        cam = photos.current(scene)
        self.assertIsNotNone(cam)
        self.assertEqual(cam.name, "gray_front_ref")
        self.assertIn(cam.name, bpy.data.collections[photos.COLLECTION].objects)
        p = cam.groomlab_photo
        self.assertEqual((p.width, p.height), (400, 500))
        bg = photos.background(cam)
        self.assertAlmostEqual(bg.alpha, photos.DEFAULT_OPACITY, places=6)
        self.assertEqual((bg.display_depth, bg.frame_method), ("FRONT", "FIT"))
        self.assertTrue(cam.data.show_background_images)
        # In front of the head (-Y), aimed at it.
        forward = cam.matrix_world.to_3x3() @ Vector((0.0, 0.0, -1.0))
        self.assertLess(cam.location.y, -0.1)
        self.assertGreater(forward.y, 0.99)
        # Kept with the groom, in the scalp's space.
        stored = hcall(head, "reference_photos")["photos"]
        self.assertEqual([(s["name"], s["width"], s["height"]) for s in stored], [(cam.name, 400, 500)])
        np.testing.assert_allclose(stored[0]["camera"]["position"], list(cam.location), atol=1e-5)
        self.assertAlmostEqual(stored[0]["camera"]["focalPx"], 35.0 / 36.0 * 400, places=4)
        # Look Through: the scene's camera, its render size the photo's.
        self.assertTrue(photos.look_through(bpy.context, cam))
        self.assertEqual(scene.camera, cam)
        self.assertEqual((scene.render.resolution_x, scene.render.resolution_y), (400, 500))

        # A camera made up for the test: off to the side, turned, a 50 mm lens. The pairs are scalp
        # vertices and where it sees them.
        truth = Matrix.LocRotScale(Vector((0.08, -0.42, 0.11)), Euler((math.radians(84), 0.0, math.radians(9))).to_quaternion(),
                                   Vector((1.0, 1.0, 1.0)))
        focal = 50.0 / 36.0 * 400
        verts = [head.data.vertices[i].co.copy() for i in np.linspace(0, len(head.data.vertices) - 1, 8).astype(int)]
        for v in verts:
            photos.add_pick(cam, "mesh", v)  # either order: pairs fill in turn
        for v in verts:
            photos.add_pick(cam, "image", project(truth, focal, 400, 500, head.matrix_world @ v))
        self.assertEqual(len(photos.complete_pairs(cam)), 8)
        self.assertEqual(bpy.ops.groomlab.pairs_solve(), {"FINISHED"})
        np.testing.assert_allclose(np.array(cam.matrix_world), np.array(truth), atol=1e-4)
        self.assertAlmostEqual(cam.data.lens, 50.0, places=2)
        self.assertLess(p.rms, 0.05)
        self.assertTrue(all(0.0 <= x.error < 0.05 for x in p.pairs))
        self.assertIn("lined up", scene.groomlab_photos.result)
        rec = hcall(head, "reference_photos")["photos"][0]
        self.assertAlmostEqual(rec["rms"], p.rms, places=6)
        self.assertAlmostEqual(rec["camera"]["focalPx"], focal, places=2)

        # The camera view's pixels are the solve's: a vertex Blender's own camera projection puts at a
        # region pixel (the region the photo's size, the frame filling it) is the photo pixel the
        # solve's model gives.
        depsgraph = bpy.context.evaluated_depsgraph_get()
        proj = cam.calc_matrix_camera(depsgraph, x=400, y=500)
        rv3d = types.SimpleNamespace(perspective_matrix=proj @ cam.matrix_world.inverted(), view_perspective="CAMERA",
                                     is_perspective=True)
        region = types.SimpleNamespace(width=400, height=500)
        from bpy_extras import view3d_utils
        for v in verts[:3]:
            w = head.matrix_world @ v
            q = view3d_utils.location_3d_to_region_2d(region, rv3d, w)
            uv = photos.photo_pixel(scene, region, rv3d, cam, q.x, q.y)
            np.testing.assert_allclose(uv, project(cam.matrix_world, focal, 400, 500, w), atol=1e-2)
            np.testing.assert_allclose(photos.region_of_pixel(scene, region, rv3d, cam, *uv), (q.x, q.y), atol=1e-3)

        # Morph to Photos: two pins' photo points moved off their vertices; the scalp's shape key takes
        # those vertices onto them (their depth kept: one photo), the mesh itself left alone.
        before = [x.image[:] for x in p.pairs]
        p.pairs[0].image = (p.pairs[0].image[0] + 12.0, p.pairs[0].image[1] - 6.0)
        p.pairs[3].image = (p.pairs[3].image[0] - 8.0, p.pairs[3].image[1] + 10.0)
        rest = [v.co.copy() for v in head.data.vertices]
        scene.groomlab_photos.morph_symmetry = False  # (the test's pins are one-sided: each lands as given)
        self.assertEqual(bpy.ops.groomlab.photos_morph(), {"FINISHED"})
        key = head.data.shape_keys.key_blocks[photos.MORPH_KEY]
        self.assertEqual(key.value, 1.0)
        self.assertEqual([v.co for v in head.data.vertices], rest)  # the mesh itself: as it was
        moved = key.data[int(np.linspace(0, len(head.data.vertices) - 1, 8).astype(int)[0])].co
        landed = project(cam.matrix_world, focal, 400, 500, head.matrix_world @ moved)
        np.testing.assert_allclose(landed, p.pairs[0].image, atol=0.05)
        self.assertIn("morphed to 8 pins", scene.groomlab_photos.result)
        head.shape_key_clear()
        for x, image in zip(p.pairs, before):
            x.image = image

        # Pick again, delete, keep the focal length with 4 pairs, clear.
        p.pair_index = 2
        self.assertEqual(bpy.ops.groomlab.pairs_repick(kind="mesh"), {"FINISHED"})
        self.assertFalse(p.pairs[2].has_mesh)
        self.assertEqual(photos.add_pick(cam, "mesh", verts[2]), 2)
        self.assertEqual(bpy.ops.groomlab.pairs_delete(), {"FINISHED"})
        self.assertEqual(len(p.pairs), 7)
        for pair in list(p.pairs)[4:]:
            pair.has_image = False
        with self.assertRaises(ValueError):
            photos.solve(cam, head, keep_focal=False)
        scene.groomlab_photos.keep_focal = True
        cam.matrix_world = truth @ Matrix.Translation((0.01, 0.0, 0.0))
        self.assertEqual(bpy.ops.groomlab.pairs_solve(), {"FINISHED"})
        np.testing.assert_allclose(np.array(cam.matrix_world), np.array(truth), atol=1e-3)
        scene.groomlab_photos.keep_focal = False
        # Locked: the solve leaves it alone.
        p.locked = True
        self.assertTrue(all(cam.lock_location) and all(cam.lock_rotation))
        with self.assertRaises(RuntimeError) as e:  # the operator's error report
            bpy.ops.groomlab.pairs_solve()
        self.assertIn("is locked", str(e.exception))
        p.locked = False
        self.assertEqual(bpy.ops.groomlab.pairs_clear(), {"FINISHED"})
        self.assertEqual((len(p.pairs), p.rms), (0, -1.0))
        self.assertEqual(hcall(head, "reference_photos")["photos"][0]["pairs"], [])
        # Rename: the groom's record follows.
        self.assertEqual(bpy.ops.groomlab.photo_rename(new_name="front"), {"FINISHED"})
        self.assertEqual(cam.name, "front")
        self.assertEqual(hcall(head, "reference_photos")["photos"][0]["name"], "front")
        # The Photos tab draws (the photos, the pairs, Guides from Photos), its menus built from the files.
        lay = Layout()
        photos.draw_tab(lay, bpy.context, 40)
        ops = [e[1] for e in lay.log if e[0] == "operator"]
        for idname in ("groomlab.photo_import", "groomlab.photo_rename", "groomlab.photo_remove",
                       "groomlab.photo_look_through", "groomlab.photo_pick", "groomlab.pairs_solve", "groomlab.pairs_clear",
                       "groomlab.pairs_delete", "groomlab.photos_guides"):
            self.assertIn(idname, ops)
        self.assertIn(("menu", "GROOMLAB_MT_photo_context"), lay.log)
        self.assertIn(("menu", "GROOMLAB_MT_pair_context"), lay.log)
        self.assertIn(("prop", "alpha"), lay.log)

    def test_2_photos_travel_with_the_groom_file(self):
        head, scene = self.head, bpy.context.scene
        cam = photos.list_photos(scene)[0]
        photos.add_pick(cam, "image", (10.0, 20.0))
        photos.add_pick(cam, "mesh", (0.01, 0.02, 0.03))
        photos.sync(head)
        path = os.path.join(bu.TMP, "photos.groom")
        self.assertEqual(bpy.ops.groomlab.save_groom(filepath=path), {"FINISHED"})
        with open(path) as f:
            stored = json.load(f)["referencePhotos"]
        self.assertEqual([s["name"] for s in stored], [cam.name])
        self.assertEqual(stored[0]["state"]["landmarks"][0]["image"], [10.0, 20.0])
        np.testing.assert_allclose(stored[0]["state"]["landmarks"][0]["mesh"], [1.0, 3.0, -2.0], atol=1e-4)  # Y up, cm
        where = cam.matrix_world.copy()
        lens = cam.data.lens
        self.assertEqual(bpy.ops.groomlab.photo_remove(), {"FINISHED"})
        self.assertEqual(photos.list_photos(scene), [])
        self.assertEqual(hcall(head, "reference_photos")["photos"], [])
        # Opened again: its photo comes back as a camera, where it was, with its pairs.
        bpy.context.view_layer.objects.active = head
        self.assertEqual(bpy.ops.groomlab.load_groom(filepath=path, use_file_scalp=False), {"FINISHED"})
        back = photos.list_photos(scene)
        self.assertEqual([c.name for c in back], ["front"])
        np.testing.assert_allclose(np.array(back[0].matrix_world), np.array(where), atol=1e-4)
        self.assertAlmostEqual(back[0].data.lens, lens, places=3)
        self.assertEqual(len(back[0].groomlab_photo.pairs), 1)
        self.assertEqual(tuple(back[0].groomlab_photo.pairs[0].image), (10.0, 20.0))
        self.assertIsNotNone(photos.background(back[0]).image)
        self.assertEqual(photos.current(scene), back[0])

    def test_3_guides_from_photos(self):
        head, scene = self.head, bpy.context.scene
        for c in photos.list_photos(scene):
            photos.remove_photo(c, scene)
        path = write_photo(os.path.join(bu.TMP, "strands_front.png"), 240, 300, stripes=True)
        self.assertEqual(bpy.ops.groomlab.photo_import(filepath=path), {"FINISHED"})
        self.assertEqual(bpy.ops.groomlab.photos_guides(guide_count=60, symmetry="none"), {"FINISHED"})
        info = hcall(head, "guides_info")
        self.assertIn(photos.GROUP, [g["name"] for g in info["groups"]])
        self.assertIn("guides in fromReference", scene.groomlab_photos.result)
        self.assertIn(photos.GROUP, [g.name for g in scene.groomlab_tools.groups])


class InputsTest(unittest.TestCase):
    """Import > Alembic Curves..., Bake Node to Guides and a param painted on the guides."""

    def setUp(self):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        bpy.context.scene.groomlab.groom = None

    def test_alembic_curves_bake_and_paint_on_guides(self):
        source = bu.head_object("Source")
        assert bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset="hair_basics", guides=30) == {"FINISHED"}
        abc = os.path.join(bu.TMP, "curves.abc")
        hcall(source, "export", format="alembic", path=abc, frames={"first": 1, "last": 1}, density=0.02)
        # A mesh selected and no groom: the groom is made on it first, its guides the file's curves.
        bpy.context.scene.groomlab.groom = None
        head = bu.head_object("Target", location=(0.5, 0.0, 0.0))
        self.assertIsNone(groom.scalp_of(head) if groom.is_groom(head) else None)
        self.assertEqual(bpy.ops.groomlab.import_alembic_curves(filepath=abc, samples=12), {"FINISHED"})
        self.assertTrue(groom.is_groom(head))
        g = json.loads(head.groomlab.graph_json)
        node = next(n for n in g["nodes"] if n["name"] == "guides")
        self.assertEqual((node["type"], node["params"]["samples"]), ("ImportCurves", 12))
        self.assertEqual(os.path.normcase(node["params"]["path"]), os.path.normcase(abc))
        groom.flush()
        self.assertGreater(len(head.groomlab.hair.data.curves), 0)
        # Bake Node to Guides: the output's strands into "baked" (the graph menu's Bake too).
        self.assertIn("node.bake", graph_ui.ACTIONS)
        self.assertEqual(bpy.ops.groomlab.bake_to_guides(node="", group="baked", density=0.05), {"FINISHED"})
        info = hcall(head, "guides_info")
        baked = next(x for x in info["groups"] if x["name"] == "baked")
        self.assertGreater(baked["guides"], 0)
        self.assertIn("scatter", pipeline.bakeable_nodes(head))
        # A param painted on the guides (the Graph Editor's Paint > On Guides): a guide layer.
        self.assertIn("param.paintGuides", graph_ui.ACTIONS)
        r = tools.map_for_param(head, "noise", "amplitude", kind="guides")
        self.assertEqual((r["name"], r["kind"]), ("noise_amplitude", "guides"))
        self.assertEqual(bpy.context.scene.groomlab_tools.paint_map, "noise_amplitude")


if __name__ == "__main__":
    bu.run(PhotosTest, InputsTest)
