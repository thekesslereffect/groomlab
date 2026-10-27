# SPDX-License-Identifier: Apache-2.0
"""The GroomLab Blender add-on in a headless Blender (run_blender_test.py installs the extension in a
throwaway profile): a groom on the test head, the hair written into Blender equal to the helper's
evaluation and the saved .groom evaluating to the same bits in groomlab-cli; guides sculpted by
script (moved, drawn, deleted) and evaluated incrementally; presets, params in scene units, graph
edits; a deforming scalp (shape keys, an armature) the strands follow with stable IDs; the helper
restarted after a crash; the groom kept in a .blend; exports."""

import json
import math
import os
import sys

import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402
import test_helper as th  # noqa: E402
import unittest  # noqa: E402

pkg, m = bu.enable()
groom, graph, process, units = m["groom"], m["graph"], m["process"], m["units"]


def evaluate_host(scalp):
    """The helper's own answer for the groom (host space), copied."""
    rt = groom.runtime(scalp)
    c = process.client()
    r = c.call("evaluate", session=rt.session, **groom.preview_args(scalp))
    a = r["arrays"]
    return c.array(a["positions"]), c.array(a["pointCounts"]), c.array(a["widths"]), c.array(a["ids"]), r


def groom_hash(scalp, density):
    rt = groom.runtime(scalp)
    c = process.client()
    r = c.call("evaluate", session=rt.session, density=density, space="groom", attributes=["rootUV"])
    a = r["arrays"]
    frame = th.strands_hash(c.array(a["ids"]), c.array(a["offsets"]), c.array(a["positions"]), c.array(a["widths"]),
                            c.array(a["attributes"]["rootUV"]))
    return th.file_hash([frame])


def depsgraph_flush():
    """What Blender's event loop does after an edit: the depsgraph updates (our handler marks the
    groom) and the debounce timer fires (flush)."""
    bpy.context.view_layer.update()
    groom.flush()


class AddonTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.head = bu.head_object()

    def tmp(self, name):
        return os.path.join(bu.TMP, name)

    def assert_hair_is_the_helpers(self, scalp):
        pos, counts, radius, ids = bu.hair_arrays(scalp.groomlab.hair)
        h_pos, h_counts, h_widths, h_ids, r = evaluate_host(scalp)
        np.testing.assert_array_equal(counts, h_counts)
        np.testing.assert_array_equal(pos, h_pos)
        np.testing.assert_array_equal(radius, h_widths * np.float32(0.5))
        np.testing.assert_array_equal(ids, h_ids)
        return r

    def assert_saved_groom_is_cli(self, scalp, name):
        density = float(scalp.groomlab.preview_density)
        path = self.tmp(name + ".groom")
        r = groom.save_groom(scalp, path)
        self.assertEqual(r["upAxis"], "Y")
        abc = self.tmp(name + "_cli.abc")
        th.cli_eval(path, abc, density=density)
        self.assertEqual("%016x" % groom_hash(scalp, density), "%016x" % th.cli_hash(abc))
        return path

    # ---- The tests (in order: each builds on the groom the first made) ----------------------

    def test_1_add_groom(self):
        graph.presets()
        head = self.head
        # B2's guides as a Blender Curves object (grooms made before GroomLab's tools; test_addon_tools.py
        # has the hierarchy grooms Create Groom makes now).
        groom.create_groom(bpy.context, head, preset="hair_basics", guide_count=60, guides="flat")
        bpy.context.scene.groomlab.groom = head
        graph.refresh(head)
        g = head.groomlab
        self.assertEqual(g.role, "SCALP")
        self.assertEqual(bpy.context.scene.groomlab.groom, head)
        guides, hair = g.guides, g.hair
        self.assertEqual(guides.type, "CURVES")
        self.assertEqual(guides.data.surface, head)
        self.assertEqual(guides.data.surface_uv_map, "UVMap")
        self.assertEqual(guides.parent, head)
        self.assertTrue(hair.hide_select)
        gpos, gcounts, gids = bu.guide_arrays(guides)
        self.assertEqual(len(gcounts), 60)
        self.assertEqual(sorted(gids.tolist()), list(range(1, 61)))
        r = self.assert_hair_is_the_helpers(head)
        self.assertGreater(r["strandCount"], 1000)
        self.assertEqual(r["errors"], [])
        # Hair attributes for the shader, and its material.
        attrs = {a.name for a in hair.data.attributes}
        self.assertTrue({"radius", "melanin", "melaninRedness", "groomlab_color", "surface_uv_coordinate"} <= attrs, attrs)
        mat = hair.data.materials[0]
        bsdf = [n for n in mat.node_tree.nodes if n.bl_idname == "ShaderNodeBsdfHairPrincipled"][0]
        self.assertTrue(bsdf.inputs["Color"].is_linked or bsdf.parametrization == "MELANIN")  # (or the highlight)
        color = [n for n in mat.node_tree.nodes if n.bl_idname == "ShaderNodeGroup"][0]
        self.assertTrue(color.inputs["Color"].is_linked)  # Arnold's look from HairColor's RGB (shading.py)
        # The graph in the panel.
        names = [n.name for n in g.nodes]
        self.assertIn("scatter", names)
        self.assertEqual(g.nodes[0].name, "scatter")  # the main chain first, from the roots
        self.assert_saved_groom_is_cli(head, "added")

    def test_2_sculpt_guides(self):
        head = self.head
        guides = head.groomlab.guides
        rt = groom.runtime(head)
        # The viewport's own view evaluated last (the CLI comparison before evaluated without its
        # point budget): what a stroke edits incrementally.
        groom.evaluate(head, rt)
        before, _, _, _ = bu.hair_arrays(head.groomlab.hair)
        pos, counts, ids = bu.guide_arrays(guides)
        # A comb stroke over one guide: its points past the root move.
        moved = pos.copy()
        k = int(np.cumsum(counts)[4] - counts[4])
        moved[k + 1:k + counts[4]] += np.array([0.01, 0.0, -0.01], np.float32)
        guides.data.attributes["position"].data.foreach_set("vector", moved.ravel())
        guides.data.update_tag()
        depsgraph_flush()
        self.assertEqual(rt.error, "")
        self.assertTrue(rt.last["sameTopology"])
        self.assertGreater(rt.last["nodesIncremental"], 0, rt.last)
        after, _, _, _ = bu.hair_arrays(head.groomlab.hair)
        self.assertFalse(np.array_equal(before, after))
        self.assert_hair_is_the_helpers(head)
        # Nothing changed: nothing is sent, nothing evaluated.
        serial = rt.last["serial"]
        depsgraph_flush()
        self.assertEqual(rt.last["serial"], serial)

        # Add brush: a new curve (ID 0) floating a little off the scalp; the helper roots it and
        # gives it the next ID, written back to Blender.
        scalp_pts = np.array([v.co for v in head.data.vertices], np.float32)
        top = scalp_pts[np.argmax(scalp_pts[:, 2])]
        guides.data.add_curves([5])
        pos, counts, ids = bu.guide_arrays(guides)
        new = np.array([top + np.array([0.0, 0.0, 0.002 + 0.02 * i], np.float32) for i in range(5)], np.float32)
        pos[-5:] = new
        guides.data.attributes["position"].data.foreach_set("vector", pos.ravel())
        guides.data.update_tag()
        depsgraph_flush()
        pos, counts, ids = bu.guide_arrays(guides)
        self.assertEqual(ids[-1], 61)
        self.assertLess(np.linalg.norm(pos[-5] - top), 1e-4)  # rooted on the scalp (written back)
        # Delete brush: the first curve goes.
        guides.data.remove_curves(indices=[0])
        guides.data.update_tag()
        depsgraph_flush()
        info = process.call("groom_info", session=rt.session)
        self.assertEqual(info["flatGuides"], 60)
        self.assert_hair_is_the_helpers(head)
        self.assert_saved_groom_is_cli(head, "sculpted")

    def test_3_presets_params_and_units(self):
        head = self.head
        g = head.groomlab
        scene = bpy.context.scene
        names = [n.name for n in g.nodes]
        g.node_index = names.index("scatter")
        params = {p.name: p for p in g.params}
        length = params["length"]
        self.assertEqual(length.widget, "length")
        cm = process.call("get_param", session=groom.runtime(head).session, node="scatter", param="length")["value"]
        self.assertAlmostEqual(length.value_length, cm / 100.0, 6)  # meters (the scene's unit, scale 1)
        length.value_length = 0.2
        self.assertAlmostEqual(self.param("scatter", "length"), 20.0, 4)
        self.assertEqual(params["randomness"].widget, "factor")
        # A scene in centimeters (unit scale 0.01): the same 20 cm shows as 20 units.
        scene.unit_settings.scale_length = 0.01
        graph.refresh_params(head)
        self.assertAlmostEqual({p.name: p for p in g.params}["length"].value_length, 20.0, 4)
        {p.name: p for p in g.params}["length"].value_length = 25.0
        self.assertAlmostEqual(self.param("scatter", "length"), 25.0, 4)
        scene.unit_settings.scale_length = 1.0
        graph.refresh_params(head)
        self.assertAlmostEqual({p.name: p for p in g.params}["length"].value_length, 0.25, 6)
        # A choice.
        g.node_index = [n.name for n in g.nodes].index("noise")
        space = {p.name: p for p in g.params}["space"]
        self.assertEqual(space.widget, "enum")
        space.value_enum = "world"
        self.assertEqual(self.param("noise", "space"), "world")
        groom.flush()
        self.assert_hair_is_the_helpers(head)

        # Graph edits: a node added after noise, moved up, removed.
        made, error = graph.add_node(head, "Gravity", after="noise")
        self.assertEqual(error, "")
        chain = graph.main_chain(json.loads(g.graph_json))[0]
        self.assertEqual(chain[chain.index("noise") + 1], made)
        self.assertTrue(graph.move_node(head, made, -1))
        chain = graph.main_chain(json.loads(g.graph_json))[0]
        self.assertEqual(chain[chain.index("noise") - 1], made)
        self.assertFalse(graph.move_node(head, "scatter", -1))
        self.assertEqual(graph.remove_node(head, made), "")
        self.assertNotIn(made, [n.name for n in g.nodes])
        groom.flush()
        self.assertEqual(groom.runtime(head).errors, [])

        # Presets: applied (the graph replaced, fitted), inserted as a compound.
        graph.apply_preset(head, "curly")
        types = {n.type for n in g.nodes}
        self.assertIn("Curl", types)
        r = graph.insert_preset(head, "frizz", after="noise" if "noise" in [n.name for n in g.nodes] else None)
        self.assertIn(r["node"], [n.name for n in g.nodes])
        groom.flush()
        self.assertEqual(groom.runtime(head).errors, [])
        self.assert_hair_is_the_helpers(head)
        self.assert_saved_groom_is_cli(head, "presets")

    def param(self, node, name):
        return process.call("get_param", session=groom.runtime(self.head).session, node=node, param=name)["value"]

    def test_4_helper_restart(self):
        head = self.head
        before = bu.hair_arrays(head.groomlab.hair)
        generation = process.generation()
        process.kill_for_tests()
        groom.mark(head, "eval")
        groom.flush()
        self.assertEqual(groom.runtime(head).error, "")
        self.assertEqual(process.generation(), generation + 1)
        after = bu.hair_arrays(head.groomlab.hair)
        # The same strands; the guides sent again are rooted again (float rounding: a few microns at
        # the tips of curly strands, a little more with blend directions, which turns a segment where
        # offsets would shorten it).
        np.testing.assert_allclose(after[0], before[0], atol=4e-6)
        np.testing.assert_allclose(after[2], before[2], rtol=1e-4)
        np.testing.assert_array_equal(after[1], before[1])
        np.testing.assert_array_equal(after[3], before[3])

    def test_5_deforming_scalp(self):
        scene = bpy.context.scene
        head = bu.head_object("Deformed", location=(1.0, 0.0, 0.0))
        groom.create_groom(bpy.context, head, preset="hair_basics", guide_count=40, guides="flat")
        bpy.context.scene.groomlab.groom = head
        graph.refresh(head)
        head.groomlab.preview_density = 0.1
        scene.frame_set(1)
        groom.flush()
        rest, counts, _, ids = bu.hair_arrays(head.groomlab.hair)
        # Shape keys: the whole scalp lifted 2 cm at frame 20.
        head.shape_key_add(name="Basis")
        lift = head.shape_key_add(name="lift")
        for v in lift.data:
            v.co.z += 0.02
        lift.value = 0.0
        lift.keyframe_insert("value", frame=1)
        lift.value = 1.0
        lift.keyframe_insert("value", frame=20)
        scene.frame_set(20)
        pos, counts20, _, ids20 = bu.hair_arrays(head.groomlab.hair)
        np.testing.assert_array_equal(ids20, ids)
        np.testing.assert_array_equal(counts20, counts)
        np.testing.assert_allclose(pos, rest + np.array([0, 0, 0.02], np.float32), atol=2e-5)
        scene.frame_set(1)
        np.testing.assert_array_equal(bu.hair_arrays(head.groomlab.hair)[0], rest)

        # An armature turns the head 30 degrees about X at frame 30.
        arm_data = bpy.data.armatures.new("Rig")
        rig = bpy.data.objects.new("Rig", arm_data)
        scene.collection.objects.link(rig)
        rig.location = head.location
        bpy.context.view_layer.objects.active = rig
        bpy.ops.object.mode_set(mode="EDIT")
        bone = arm_data.edit_bones.new("neck")
        bone.head = (0.0, 0.0, 0.0)
        bone.tail = (0.0, 0.0, 0.1)
        bpy.ops.object.mode_set(mode="OBJECT")
        group = head.vertex_groups.new(name="neck")
        group.add(list(range(len(head.data.vertices))), 1.0, "REPLACE")
        mod = head.modifiers.new("Armature", "ARMATURE")
        mod.object = rig
        pb = rig.pose.bones["neck"]
        pb.rotation_mode = "XYZ"
        pb.rotation_euler = (0.0, 0.0, 0.0)
        pb.keyframe_insert("rotation_euler", frame=1)
        pb.rotation_euler = (math.radians(30.0), 0.0, 0.0)
        pb.keyframe_insert("rotation_euler", frame=30)
        scene.frame_set(1)
        np.testing.assert_allclose(bu.hair_arrays(head.groomlab.hair)[0], rest, atol=1e-6)
        scene.frame_set(30)
        pos, _, _, ids30 = bu.hair_arrays(head.groomlab.hair)
        np.testing.assert_array_equal(ids30, ids)
        deform = pb.matrix @ pb.bone.matrix_local.inverted()  # the bone's motion, in the head's space too
        rot, move = np.array(deform.to_3x3()), np.array(deform.translation)
        self.assertGreater(abs(rot[1, 2]), 0.4)  # it did turn
        lifted = rest.astype(np.float64) + np.array([0.0, 0.0, 0.02])  # the shape key is still on at frame 30
        np.testing.assert_allclose(pos, lifted @ rot.T + move, atol=3e-5)
        self.assertTrue(groom.runtime(head).last["following"])
        scene.frame_set(1)
        # An export over frames of the deforming scalp: Blender plays each frame for the helper.
        abc = self.tmp("deforming.abc")
        r = groom.export(head, scene, "alembic", abc, 28, 30, density=0.05)
        self.assertEqual(r["frames"], 3)
        self.assertGreater(os.path.getsize(abc), 1000)
        self.assertEqual(scene.frame_current, 1)

    def test_6_exports_and_files(self):
        head = self.head
        abc = self.tmp("export.abc")
        result = bpy.ops.groomlab.export_alembic(filepath=abc)
        self.assertEqual(result, {"FINISHED"})
        self.assertGreater(os.path.getsize(abc), 1000)
        usd = self.tmp("export.usda")
        self.assertEqual(bpy.ops.groomlab.export_usd(filepath=usd), {"FINISHED"})
        with open(usd) as f:
            self.assertIn("metersPerUnit = 0.01", f.read(4000))
        # Card maps: a Cards node, then the export as a preset packs the maps (no dialog: EXEC).
        cards, error = graph.add_node(head, "Cards")
        self.assertFalse(error)
        bpy.context.scene.groomlab.groom = head  # the operator works on the panel's groom
        presets = [p["name"] for p in groom.card_map_presets(refresh=True)]
        self.assertIn("Unity HDRP Hair", presets)
        folder = self.tmp("card_maps")
        result = bpy.ops.groomlab.bake_card_maps("EXEC_DEFAULT", preset="Unity HDRP Hair", resolution=64, file_format="png",
                                                 directory=folder, name="head")
        self.assertEqual(result, {"FINISHED"})
        self.assertEqual(sorted(os.listdir(folder)), sorted("head_%s.png" % m for m in
                                                            ("BaseColor", "Normal", "Mask", "Tangent", "Data")))
        graph.remove_node(head, cards)
        # A .groom opened with its own scalp: the same groom.
        path = self.assert_saved_groom_is_cli(head, "files")
        bpy.ops.groomlab.load_groom(filepath=path, use_file_scalp=True)
        loaded = bpy.context.scene.groomlab.groom
        self.assertNotEqual(loaded, head)
        self.assertEqual(len(loaded.groomlab.guides.data.curves), len(head.groomlab.guides.data.curves))
        a, b = bu.hair_arrays(head.groomlab.hair), bu.hair_arrays(loaded.groomlab.hair)
        np.testing.assert_array_equal(a[1], b[1])
        np.testing.assert_array_equal(a[3], b[3])
        np.testing.assert_allclose(a[0], b[0], atol=1e-6)

        # The groom kept in the .blend: opened again (another helper), the same hair.
        blend = self.tmp("groom.blend")
        bpy.ops.wm.save_as_mainfile(filepath=blend)
        before = bu.hair_arrays(head.groomlab.hair)
        self.assertTrue(head.groomlab.snapshot)
        process.shutdown()
        bpy.ops.wm.open_mainfile(filepath=blend)
        again = bpy.data.objects["Head"]
        self.assertEqual(again.groomlab.role, "SCALP")
        groom.sync(again, force_eval=True)
        after = bu.hair_arrays(again.groomlab.hair)
        for x, y in zip(before, after):
            np.testing.assert_array_equal(x, y)
        AddonTest.head = again


if __name__ == "__main__":
    bu.run(AddonTest)
