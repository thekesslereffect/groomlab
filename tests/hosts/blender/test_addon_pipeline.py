# SPDX-License-Identifier: Apache-2.0
"""The add-on's pipeline client (groomlab/pipeline.py) in a headless Blender: game cards and the scales
written as FBX with Blender's exporter from the meshes the helper answers, strands as a mesh object,
colliders and other scalps from Blender objects, a transfer onto another mesh undone and redone, and the
sample meshes added at their real size. Then the operators the GroomLab menu and the Output strip run
(pipeline_ops.py): the engine exports with their file and folder browsers' properties, To Mesh,
Bake Scale Maps, Simulate (Add, Range, Bake, Clear), Add (Cards, a Collider kept posed, Feathers, Braid on
curve objects, samples), Transfer Groom to Scalp with the new mesh owning the groom (GroomLab's undo and
Blender's both taking it back), the strand count HUD, a turntable, Find Missing Files and Copy Graph."""

import importlib
import json
import os
import sys

import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402
import unittest  # noqa: E402

pkg, m = bu.enable()
groom, process = m["groom"], m["process"]
pipeline = importlib.import_module(bu.MODULE + ".pipeline")
pipeline_ops = importlib.import_module(bu.MODULE + ".pipeline_ops")
tools = importlib.import_module(bu.MODULE + ".tools")


def with_cards(scalp):
    rt = groom.ensure(scalp)
    c = process.client()
    g = c.call("get_graph", session=rt.session)["graph"]
    into = next(x for x in g["connections"] if x["to"] == g["output"] and x["input"] == "strands")
    g["nodes"].append({"name": "cards", "type": "Cards", "params": {"maxCards": 30}})
    g["connections"].append({"from": into["from"], "to": "cards", "input": "strands"})
    into["from"] = "cards"
    c.call("set_graph", session=rt.session, graph=g)


class PipelineTest(unittest.TestCase):
    def setUp(self):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        self.head = bu.head_object()

    def tmp(self, name):
        return os.path.join(bu.TMP, name)

    def test_cards_and_scales_as_fbx(self):
        groom.create_groom(bpy.context, self.head, preset="hair_basics", guide_count=30, guides="flat")
        groom.flush()
        with_cards(self.head)
        before = set(bpy.data.objects)
        r = pipeline.export(self.head, "unrealCards", directory=self.tmp("unreal"), name="head", resolution=32)
        self.assertEqual(sorted(os.path.basename(p) for p in r["fbxWritten"]), ["head_cards.fbx", "head_scalpCap.fbx"])
        for path in r["fbxWritten"]:
            self.assertGreater(os.path.getsize(path), 1000)
        self.assertEqual(set(bpy.data.objects), before, "the FBX's objects are gone after")
        with open(r["manifest"]) as f:
            self.assertEqual(json.load(f)["mesh"], "head_cards.fbx")
        # The FBX read back: the cards with their UVs and tint.
        bpy.ops.import_scene.fbx(filepath=r["fbx"])
        cards = [o for o in bpy.data.objects if o not in before]
        self.assertEqual(len(cards), 1)
        mesh = cards[0].data
        self.assertTrue(all(len(p.vertices) == 4 for p in mesh.polygons))
        self.assertIn("groomLabTint", mesh.color_attributes)
        for o in cards:
            bpy.data.objects.remove(o)
        r = pipeline.export(self.head, "unityCards", directory=self.tmp("unity"), name="head", resolution=32)
        self.assertEqual(len(r["fbxWritten"]), 2)
        self.assertNotIn("head_cards_mat", bpy.data.materials)
        glb = self.tmp("head.glb")
        r = pipeline.export(self.head, "gltfCards", path=glb, name="head", resolution=32)
        self.assertTrue(os.path.isfile(glb))
        # To Mesh: an object under the scalp.
        obj, r = pipeline.to_mesh(self.head, make="strands", sides=3, max_strands=25, density=0.2)
        self.assertEqual(obj.parent, self.head)
        self.assertEqual(r["strands"], 25)
        self.assertGreater(len(obj.data.polygons), 25)

    def test_scales_fbx_and_strand_exports(self):
        groom.create_groom(bpy.context, self.head, preset="dragon_scales")
        groom.flush()
        r = pipeline.export(self.head, "scalesFbx", path=self.tmp("scales.fbx"))
        self.assertEqual(r["fbxWritten"], [self.tmp("scales.fbx")])
        obj = pipeline.scale_mesh(self.head)
        self.assertEqual({l.name for l in obj.data.uv_layers}, {"UVMap", "scaleUV", "scaleData"})
        self.assertIn("scaleRootColor", obj.data.color_attributes)
        maps = pipeline.bake_scale_maps(self.head, bu.TMP, name="scales", resolution=32)
        self.assertEqual(len(maps["files"]), 3)
        r = pipeline.export(self.head, "unrealGroom", path=self.tmp("unreal.abc"), density=0.2)
        self.assertEqual(r["frames"], 1)
        r = pipeline.export(self.head, "scalesUsd", path=self.tmp("scales.usda"), density=0.2)
        with open(self.tmp("scales.usda")) as f:
            self.assertIn("PointInstancer", f.read())

    def test_colliders_scalps_transfer_and_samples(self):
        groom.create_groom(bpy.context, self.head, preset="hair_basics")
        groom.flush()
        bpy.ops.mesh.primitive_plane_add(size=0.4, location=(0.0, 0.0, -0.05))
        plane = bpy.context.active_object
        info = pipeline.set_colliders(self.head, {"collar": plane})
        self.assertEqual([c["name"] for c in info["colliders"]], ["collar"])
        info = pipeline.set_other_scalps(self.head, {"patch": plane})
        self.assertEqual([(s["name"], s["points"]) for s in info["scalps"]], [("patch", 4)])
        self.assertEqual(pipeline.set_other_scalps(self.head, [])["scalps"], [])
        # Place a few guides, then move the groom onto a copy of the head 5 cm along X.
        rt = groom.ensure(self.head)
        c = process.client()
        pts = bu.th.to_blender(bu.th.HEAD[0])
        tops = np.argsort(pts[:, 2])[-5:]
        curves = np.concatenate([np.stack([pts[i], pts[i] + [0.0, 0.0, 0.05]]) for i in tops])
        c.call("set_guides", session=rt.session, target="hierarchy", **c.put(points=curves, counts=np.full(5, 2, np.int32)))
        before = c.array(c.call("get_guides", session=rt.session, source="hierarchy")["arrays"]["points"])
        copy = self.head.copy()
        copy.data = self.head.data.copy()
        bpy.context.scene.collection.objects.link(copy)
        copy.location.x += 0.05
        bpy.context.view_layer.update()
        r = pipeline.transfer(self.head, copy, method="bounds")
        self.assertEqual(r["undo"]["undoLabel"], "transfer")
        after = c.array(c.call("get_guides", session=rt.session, source="hierarchy")["arrays"]["points"])
        self.assertTrue(np.allclose(after[::2, 0] - before[::2, 0], 0.05, atol=1e-4))  # the roots (2 points a guide)
        self.assertTrue(np.allclose(after[:, 0] - before[:, 0], 0.05, atol=5e-3))
        self.assertTrue(c.call("undo", session=rt.session)["scalpChanged"])
        markers = [(pts[i], pts[i] + [0.05, 0.0, 0.0]) for i in tops[:4]]
        self.assertEqual(len(pipeline.snap_markers(self.head, copy, markers)), 4)
        self.assertEqual(len(pipeline.transfer_markers(self.head, markers)), 4)
        self.assertEqual(len(pipeline.transfer_markers(self.head)), 4)
        # The sample meshes: their real size in the scene's units, the part to groom selected.
        ids = [s["id"] for s in pipeline.sample_meshes()]
        self.assertEqual(ids, ["nika", "manny", "wolf", "buddy"])
        manny = pipeline.add_sample(bpy.context, "manny")
        self.assertEqual(manny.name, "manny_head")
        size = max(manny.dimensions)
        self.assertAlmostEqual(size, 0.3421, delta=0.002)
        self.assertGreater(manny.dimensions.z, manny.dimensions.y * 0.5)  # up is Z

    def test_the_buddy_rig_poses_and_a_groom_follows(self):
        # Buddy, a rig: an armature (63 bones) with its meshes skinned to it (an Armature modifier
        # each), its real size, Z up, facing -Y, the body selected and active. A bone posed moves
        # the body's evaluated vertices; a groom on the body follows the head (Follow Scalp).
        scene = bpy.context.scene
        body = pipeline.add_sample(bpy.context, "buddy")
        self.assertEqual(body.name, "buddy_body")
        self.assertIs(bpy.context.view_layer.objects.active, body)
        self.assertEqual(bpy.context.selected_objects, [body])
        rig = body.parent
        self.assertEqual((rig.type, rig.name), ("ARMATURE", "buddy_rig"))
        self.assertEqual(len(rig.data.bones), 63)
        self.assertEqual(sorted(c.name for c in rig.children),
                         ["buddy_body", "buddy_eye_l", "buddy_eye_r", "buddy_teeth", "buddy_tongue"])
        for part in rig.children:
            self.assertTrue(any(md.type == "ARMATURE" and md.object is rig for md in part.modifiers), part.name)

        def world(o):
            e = o.evaluated_get(bpy.context.evaluated_depsgraph_get())
            m = np.array(e.matrix_world)
            co = np.array([v.co for v in e.data.vertices])
            return co @ m[:3, :3].T + m[:3, 3]

        pts = world(body)
        lo, hi = pts.min(axis=0), pts.max(axis=0)
        self.assertAlmostEqual(max(hi - lo), 0.9665, delta=0.005)  # metres
        self.assertAlmostEqual(lo[2], 0.0, delta=0.002)  # Z up, the feet on the ground
        self.assertAlmostEqual(hi[2], 0.5707, delta=0.005)
        self.assertLess(world(bpy.data.objects["buddy_eye_l"]).mean(axis=0)[1], -0.05)  # facing -Y
        # The groom, then the head bent 30 degrees (about its Z) at frame 10.
        groom.create_groom(bpy.context, body, preset="hair_basics", guide_count=30, guides="flat")
        groom.flush()
        rest, counts, _, ids = bu.hair_arrays(body.groomlab.hair)
        self.assertGreater(len(counts), 1000)
        pb = rig.pose.bones["head"]
        pb.rotation_mode = "XYZ"
        pb.rotation_euler = (0.0, 0.0, 0.0)
        pb.keyframe_insert("rotation_euler", frame=1)
        pb.rotation_euler = (0.0, 0.0, np.radians(30.0))
        pb.keyframe_insert("rotation_euler", frame=10)
        scene.frame_set(10)
        posed = world(body)
        crown = int(np.argmax(pts[:, 2]))
        self.assertGreater(np.linalg.norm(posed[crown] - pts[crown]), 0.05)  # the skin moved with the head
        pos, counts10, _, ids10 = bu.hair_arrays(body.groomlab.hair)
        np.testing.assert_array_equal(ids10, ids)
        np.testing.assert_array_equal(counts10, counts)
        # The strands rooted on the crown (the head bone's alone) turned with it: the bone's motion,
        # in the armature's space, which is the body's (and its hair's).
        deform = pb.matrix @ pb.bone.matrix_local.inverted()
        rot, move = np.array(deform.to_3x3()), np.array(deform.translation)
        local = np.array([v.co for v in body.data.vertices])
        starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
        on_crown = [s for s in starts if rest[s, 1] > local[:, 1].max() - 6.0]  # cm, Y up in the rig's space
        self.assertGreater(len(on_crown), 10)
        # Each keeps its shape, times the groom's scale (the bend stretches the neck: the skin's area
        # at the frame, so a few percent), the same for all.
        spans = [(s, counts[list(starts).index(s)]) for s in on_crown]
        turned = rest.astype(np.float64) @ rot.T + move
        k = np.median([np.linalg.norm(pos[s:s + n] - pos[s]) / np.linalg.norm(turned[s:s + n] - turned[s]) for s, n in spans])
        self.assertAlmostEqual(k, 1.0, delta=0.03)
        for s, n in spans:
            np.testing.assert_allclose(pos[s], turned[s], atol=1e-3)  # the root, on the bent skin
            np.testing.assert_allclose(pos[s:s + n], turned[s] + (turned[s:s + n] - turned[s]) * k, atol=0.05)  # cm
        self.assertTrue(groom.runtime(body).last["following"])
        scene.frame_set(1)


def select_only(*objs):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]


def groom_on(head, preset="hair_basics", guides=30):
    """A groom made as the menu makes it (GroomLab's tools: guides in groups)."""
    select_only(head)
    m["graph"].presets()  # the Create Groom dialog's list
    assert bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset=preset, guides=guides) == {"FINISHED"}
    groom.flush()
    return head


def graph_of(scalp):
    return process.call("get_graph", session=groom.ensure(scalp).session)["graph"]


def types_of(scalp):
    return [n["type"] for n in graph_of(scalp)["nodes"]]


def first(scalp, kind):
    return next(n["name"] for n in graph_of(scalp)["nodes"] if n["type"] == kind)


def link(scalp, to, input_name):
    return next((c["from"] for c in graph_of(scalp)["connections"] if c["to"] == to and c["input"] == input_name), None)


def param(scalp, node, name):
    return process.call("get_param", session=groom.ensure(scalp).session, node=node, param=name)["value"]


def curve_object(name, points):
    data = bpy.data.curves.new(name, "CURVE")
    spline = data.splines.new("POLY")
    spline.points.add(len(points) - 1)
    for p, co in zip(spline.points, points):
        p.co = (co[0], co[1], co[2], 1.0)
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    return obj


class PipelineOpsTest(unittest.TestCase):
    """The operators the menus bind (headless: their EXEC paths, the dialogs' values given)."""

    def setUp(self):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        self.head = bu.head_object()
        self.scene = bpy.context.scene

    def tmp(self, name):
        for folder in ("unreal", "unity", "maps", "found"):
            os.makedirs(os.path.join(bu.TMP, "ops", folder), exist_ok=True)
        return os.path.join(bu.TMP, "ops", name)

    def test_1_simulate_cards_collider_feathers_braid(self):
        head = groom_on(self.head)
        rt = groom.runtime(head)
        self.assertEqual(bpy.ops.groomlab.simulate_add(), {"FINISHED"})
        sim = first(head, "Simulate")
        self.assertEqual(link(head, first(head, "Interpolate"), "guides"), sim)
        self.assertEqual(next(n["type"] for n in graph_of(head)["nodes"] if n["name"] == link(head, sim, "guides")),
                         "Guides")
        self.scene.frame_start, self.scene.frame_end = 1, 4
        self.scene.frame_set(1)
        self.assertEqual(bpy.ops.groomlab.simulate_range(), {"FINISHED"})
        st = pipeline.simulation_status(head)
        self.assertFalse(st["running"])
        self.assertTrue(st["status"][0]["frames"])
        self.assertEqual(bpy.ops.groomlab.simulate_range(bake=True), {"FINISHED"})
        st = pipeline.simulation_status(head)
        self.assertEqual(len(st["bakes"]), 1)
        self.assertTrue(os.path.isfile(next(iter(st["bakes"].values()))["file"]))
        self.assertEqual(bpy.ops.groomlab.simulate_clear(), {"FINISHED"})
        self.assertEqual(pipeline.simulation_status(head)["bakes"], {})
        # Cards: one node before the output, set for each level of detail.
        self.assertEqual(bpy.ops.groomlab.add_cards(lod="hero"), {"FINISHED"})
        cards = first(head, "Cards")
        self.assertEqual(link(head, graph_of(head)["output"], "strands"), cards)
        hero = param(head, cards, "cardsPerClump")
        self.assertEqual(bpy.ops.groomlab.add_cards(lod="low"), {"FINISHED"})
        self.assertEqual(types_of(head).count("Cards"), 1)
        self.assertGreater(hero, param(head, cards, "cardsPerClump"))
        # A collider: its nodes before the output, the mesh sent again as it moves.
        bpy.ops.mesh.primitive_plane_add(size=0.4, location=(0.0, 0.0, -0.05))
        plane = bpy.context.active_object
        select_only(plane)
        self.assertEqual(bpy.ops.groomlab.add_collider(), {"FINISHED"})
        collide = first(head, "Collide")
        self.assertEqual(param(head, link(head, collide, "collider"), "name"), "Plane")
        self.assertEqual([c["name"] for c in pipeline.meshes_info(head)["colliders"]], ["Plane"])
        self.assertEqual(list(pipeline.kept_colliders(head)), ["Plane"])
        key = rt.keys.get("colliders")
        self.assertIsNotNone(key)
        plane.location.z = -0.1
        bpy.context.view_layer.update()
        groom.sync(head, evaluate_now=False)
        self.assertNotEqual(rt.keys.get("colliders"), key)
        # Feathers: two designs sharing scattered shafts, merged with the hair.
        select_only(head)
        self.assertEqual(bpy.ops.groomlab.add_feathers(design="contour, down", weights="3 1", source="scatter"),
                         {"FINISHED"})
        t = types_of(head)
        self.assertEqual(t.count("Split"), 1)
        self.assertGreaterEqual(t.count("Compound"), 2)
        self.assertGreaterEqual(t.count("Merge"), 2)
        self.assertEqual(process.call("node_status", session=rt.session).get("graphError", ""), "")
        # Braid on a curve object drawn from the scalp: the braids group, its Guides node, the Braid node.
        pts = bu.th.to_blender(bu.th.HEAD[0])
        root = pts[np.argmax(pts[:, 2])]
        path = curve_object("braidPath", [root, root + [0.0, -0.03, 0.02], root + [0.0, -0.06, -0.02],
                                          root + [0.0, -0.08, -0.08]])
        select_only(path)
        self.assertEqual(bpy.ops.groomlab.add_braid(style="three"), {"FINISHED"})
        groups = {g["name"]: g for g in tools.call(head, "guides_info")["groups"]}
        self.assertIn("braids", groups)
        braid = first(head, "Braid")
        self.assertEqual(param(head, link(head, braid, "braids"), "group"), "braids")
        hair_guides = [n["name"] for n in graph_of(head)["nodes"]
                       if n["type"] == "Guides" and not (n.get("params") or {}).get("group")]
        self.assertTrue(hair_guides)
        self.assertTrue(all("braids" in str(param(head, n, "exclude")) for n in hair_guides))
        self.assertEqual(process.call("node_status", session=rt.session).get("graphError", ""), "")
        # The feathers' choice in the shared words: scattered feathers, or one feather per guide.
        items = {i.identifier: i.name for i in
                 bpy.ops.groomlab.add_feathers.get_rna_type().properties["source"].enum_items}
        self.assertEqual(items, {"scatter": "Scattered feathers (shaped by the guides)",
                                 "guides": "One feather per guide (hand-placed)"})
        # One feather per guide in a guide group of its own: the selected curve objects join
        # flight_left, each a flight feather, left out of the hair's guides.
        wing = [curve_object("wing%d" % i, [root + [0.01 * i, 0.0, 0.0], root + [0.01 * i, -0.02, 0.03],
                                            root + [0.01 * i, -0.03, 0.07]]) for i in range(2)]
        select_only(wing[0])
        wing[1].select_set(True)
        self.assertEqual(bpy.ops.groomlab.add_feathers(design="flight", source="guides", group="flight_left"),
                         {"FINISHED"})
        groups = {g["name"]: g for g in tools.call(head, "guides_info")["groups"]}
        self.assertEqual(groups["flight_left"]["guides"], 2)
        self.assertEqual(pipeline.feather_guide_groups(head), ["flight_left"])
        self.assertTrue(all("flight_left" in str(param(head, n, "exclude")) for n in hair_guides))
        self.assertEqual(process.call("node_status", session=rt.session).get("graphError", ""), "")
        # The hand-placed flight feathers preset: its feathers group made, empty; the Draw tool
        # would draw into it (not in background mode).
        graph = importlib.import_module(bu.MODULE + ".graph")
        r = graph.apply_preset(head, "hand_placed_feathers")
        self.assertEqual((r["groupsMade"], r["drawGroup"]), (["feathers"], "feathers"))
        groups = {g["name"]: g for g in tools.call(head, "guides_info")["groups"]}
        self.assertEqual(groups["feathers"]["guides"], 0)
        self.assertEqual(groups["flight_left"]["guides"], 2)
        self.assertEqual(process.call("node_status", session=rt.session).get("graphError", ""), "")
        self.assertIn("feathers", pipeline.feather_guide_groups(head))
        self.assertFalse(graph.draw_into(bpy.context, head, "feathers"))
        self.assertEqual(graph.apply_preset(head, "hand_placed_feathers")["groupsMade"], [])  # made once

    def test_2_exports_mesh_and_maps(self):
        head = groom_on(self.head, guides=20)
        self.assertEqual(bpy.ops.groomlab.add_cards(lod="mid"), {"FINISHED"})
        process.call("set_param", session=groom.ensure(head).session, node=first(head, "Cards"), param="maxCards",
                     value=30)
        before = set(bpy.data.objects)
        for op, name in ((bpy.ops.groomlab.export_unreal_groom, "head_unreal.abc"),
                         (bpy.ops.groomlab.export_unity_strands, "head_unity.abc")):
            self.assertEqual(op(filepath=self.tmp(name)), {"FINISHED"})
            self.assertGreater(os.path.getsize(self.tmp(name)), 100)
        self.assertEqual(bpy.ops.groomlab.export_gltf_cards(filepath=self.tmp("head_cards.glb"), resolution=32),
                         {"FINISHED"})
        self.assertTrue(os.path.isfile(self.tmp("head_cards.glb")))
        self.assertEqual(bpy.ops.groomlab.export_unreal_cards(directory=self.tmp("unreal"), resolution=32), {"FINISHED"})
        self.assertTrue(os.path.isfile(os.path.join(self.tmp("unreal"), "Head_cards.fbx")))
        self.assertEqual(bpy.ops.groomlab.export_unity_cards(directory=self.tmp("unity"), resolution=32), {"FINISHED"})
        self.assertTrue(any(f.endswith(".fbx") for f in os.listdir(self.tmp("unity"))))
        self.assertEqual(set(bpy.data.objects), before)
        # To Mesh: a mesh under the scalp, selected.
        self.assertEqual(bpy.ops.groomlab.to_mesh(make="strands", sides=3, max_strands=20), {"FINISHED"})
        mesh = bpy.context.view_layer.objects.active
        self.assertEqual(mesh.parent, head)
        self.assertGreater(len(mesh.data.polygons), 20)
        bpy.data.objects.remove(mesh)
        # The scales: FBX, the USD instancer, the baked maps.
        lizard = groom_on(bu.head_object("Lizard", location=(1.0, 0.0, 0.0)), preset="dragon_scales", guides=0)
        self.assertEqual(self.scene.groomlab.groom, lizard)
        self.assertEqual(bpy.ops.groomlab.export_scales_fbx(filepath=self.tmp("lizard_scales.fbx")), {"FINISHED"})
        self.assertTrue(os.path.isfile(self.tmp("lizard_scales.fbx")))
        self.assertEqual(bpy.ops.groomlab.export_scales_usd(filepath=self.tmp("lizard_scales.usda")), {"FINISHED"})
        with open(self.tmp("lizard_scales.usda")) as f:
            self.assertIn("PointInstancer", f.read())
        self.assertEqual(bpy.ops.groomlab.bake_scale_maps(directory=self.tmp("maps"), resolution=32), {"FINISHED"})
        self.assertEqual(len([f for f in os.listdir(self.tmp("maps")) if f.endswith(".png")]), 3)

    def test_3_transfer_owned_and_undone(self):
        head = groom_on(self.head, guides=20)
        head["ctl_test"] = 0.5
        rt = groom.ensure(head)
        c = process.client()
        before = c.array(c.call("get_guides", session=rt.session, source="hierarchy")["arrays"]["points"]).copy()
        hair = head.groomlab.hair
        new = bu.head_object("NewHead", location=(0.3, 0.0, 0.0))
        select_only(head)
        self.assertEqual(bpy.ops.groomlab.transfer_groom(target=new.name, method="closest", own=True), {"FINISHED"})
        self.assertEqual(new.groomlab.role, "SCALP")
        self.assertEqual(head.groomlab.role, "NONE")
        self.assertEqual(hair.parent, new)
        self.assertEqual(hair.groomlab.scalp, new)
        self.assertEqual(self.scene.groomlab.groom, new)
        self.assertEqual(groom.scalp_of(hair), new)
        self.assertIn("ctl_test", new.keys())
        self.assertNotIn("ctl_test", head.keys())
        self.assertIs(groom.runtime(new), rt)
        after = c.array(c.call("get_guides", session=rt.session, source="hierarchy")["arrays"]["points"])
        self.assertTrue(np.allclose(after, before, atol=1e-4))  # the same shape in its own space
        groom.flush()
        self.assertEqual(rt.error, "")
        self.assertGreater(len(hair.data.curves), 0)
        # GroomLab's Undo takes the transfer back: the groom on its old scalp again; Redo moves it again.
        self.assertEqual(bpy.ops.groomlab.tool_undo(), {"FINISHED"})
        self.assertEqual(head.groomlab.role, "SCALP")
        self.assertEqual(new.groomlab.role, "NONE")
        self.assertEqual(hair.parent, head)
        self.assertEqual(bpy.ops.groomlab.tool_undo(redo=True), {"FINISHED"})
        self.assertEqual(new.groomlab.role, "SCALP")
        # Blender's undo restores the objects (here by hand): the helper's transfer is undone to match.
        pipeline.move_groom(new, head)
        pipeline.undo_moves()
        self.assertEqual(c.call("undo_state", session=rt.session).get("redoLabel"), "transfer")
        groom.flush()
        self.assertEqual(rt.error, "")
        self.assertGreater(len(hair.data.curves), 0)

    def test_4_review_utilities_samples(self):
        head = groom_on(self.head, guides=20)
        t = self.scene.groomlab_tools
        t.show_hud = True
        self.assertIn("strands", pipeline_ops.hud_text(bpy.context))
        self.assertIn("20 guides", pipeline_ops.hud_text(bpy.context))
        t.show_hud = False
        self.assertEqual(bpy.ops.groomlab.turntable(frames=48), {"FINISHED"})
        cam = self.scene.camera
        self.assertIsNotNone(cam)
        self.assertEqual((self.scene.frame_start, self.scene.frame_end), (1, 48))
        self.scene.frame_set(25)
        self.assertAlmostEqual(cam.parent.rotation_euler.z, np.pi, places=2)
        self.scene.frame_set(1)
        # Find Missing Files: a texture map whose image moved, found in another folder.
        img = bpy.data.images.new("spot", 8, 8)
        img.pixels = [0.5] * (8 * 8 * 4)
        a = self.tmp("spot.png")
        os.makedirs(os.path.dirname(a), exist_ok=True)
        img.filepath_raw = a
        img.file_format = "PNG"
        img.save()
        select_only(head)
        self.assertEqual(bpy.ops.groomlab.map_import(filepath=a), {"FINISHED"})
        moved = os.path.join(self.tmp("found"), "deep")
        os.makedirs(moved, exist_ok=True)
        os.replace(a, os.path.join(moved, "spot.png"))
        self.assertIn("spot", pipeline.missing_files(head)["maps"])
        self.assertEqual(bpy.ops.groomlab.find_missing_files(directory=self.tmp("found")), {"FINISHED"})
        self.assertEqual(pipeline.missing_files(head)["maps"], {})
        texture = next(m for m in tools.call(head, "maps_list")["maps"] if m["name"] == "spot")["texture"]
        self.assertTrue(texture.replace("\\", "/").endswith("deep/spot.png"), texture)
        # Copy Graph to Other Grooms: the selected grooms take the active one's graph.
        other = groom_on(bu.head_object("Other", location=(0.5, 0.0, 0.0)), preset="curly", guides=10)
        self.scene.groomlab.groom = head
        self.assertEqual(bpy.ops.groomlab.add_cards(lod="low"), {"FINISHED"})
        self.assertNotEqual(sorted(types_of(other)), sorted(types_of(head)))
        select_only(head, other)
        self.assertEqual(bpy.ops.groomlab.copy_graph_to_grooms("EXEC_DEFAULT"), {"FINISHED"})
        self.assertEqual(sorted(types_of(other)), sorted(types_of(head)))
        # A sample mesh from the menu: added, selected.
        self.assertEqual(bpy.ops.groomlab.add_sample(sample="manny"), {"FINISHED"})
        self.assertEqual(bpy.context.view_layer.objects.active.name, "manny_head")


if __name__ == "__main__":
    bu.run(PipelineTest, PipelineOpsTest)
