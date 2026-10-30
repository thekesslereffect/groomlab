# SPDX-License-Identifier: Apache-2.0
"""The add-on's Graph Editor (graph_tree.py, graph_editor.py) in a headless Blender: a groom's graph as
a "GroomLab Graph" node tree equal to the helper's (nodes, typed ports, links, positions); nodes
added, linked, unlinked, renamed, muted and deleted in the tree go to the helper and the hair; a
link the graph refuses goes away; params set on the nodes (scene units in, centimeters in the
helper) and from the 3D view's panel; ramps through Blender's curve widget; presets applied and
inserted (a Compound with its exposed params); positions kept through rebuilds and undo, and saved
in the .groom where Maya's editor reads them ("ui": {"x", "y"}, y down); the graph's pins as
reroutes and back (a bundle, a reroute put on a link, moved, fed from another output, refused, left
with no link out); View and Set as Output;
an outdated node upgraded (groomlab-helper-testhooks: TestCApiLift at version 2)."""

import importlib
import json
import os
import sys
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

pkg, m = bu.enable()
groom, graph, process, units = m["groom"], m["graph"], m["process"], m["units"]
gt = importlib.import_module(bu.MODULE + ".graph_tree")
ge = importlib.import_module(bu.MODULE + ".graph_editor")
gu = importlib.import_module(bu.MODULE + ".graph_ui")


class PanelLayout(bu.FakeLayout):
    """FakeLayout with what the Graph Editor's panel also draws."""

    scale_y = 1.0

    def _sub(self, *args, **kwargs):
        return PanelLayout(self.log)

    row = column = box = split = _sub

    def template_curve_mapping(self, data, name, **kwargs):
        self.log.append(("curve", data.name))

    def context_string_set(self, name, value):
        self.log.append(("context", name, value))

    def menu(self, idname, **kwargs):
        self.log.append(("menu", idname))

    def prop(self, data, name, **kwargs):
        if name.startswith('["'):  # a custom property (the groom's ctl_ controls)
            if name[2:-2] not in data.keys():
                raise AttributeError("%s has no custom property %s" % (data, name))
            self.log.append(("prop", name))
            return
        bu.FakeLayout.prop(self, data, name, **kwargs)


def helper_graph(scalp):
    return process.call("get_graph", session=groom.runtime(scalp).session)["graph"]


def node_of(tree, name):
    return next((n for n in tree.nodes if gt.ours(n) and n.gl_name == name), None)


def settle(scalp):
    """What Blender's event loop does after an edit: the tree rebuilt, the groom evaluated."""
    gt.flush()
    groom.flush()


class GraphEditorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.head = bu.head_object()
        graph.presets()
        assert bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset="hair_basics", guides=30) == {"FINISHED"}

    def tree(self):
        return gt.tree_of(self.head)

    def param(self, node, name):
        return process.call("get_param", session=groom.runtime(self.head).session, node=node, param=name)["value"]

    def assert_tree_is_graph(self, positions=True):
        data = helper_graph(self.head)
        tree = self.tree()
        nodes = {n.gl_name: n for n in tree.nodes if gt.ours(n)}
        self.assertEqual(len(nodes), len(tree.nodes))
        self.assertEqual(set(nodes), {n["name"] for n in data["nodes"]})
        for gnode in data["nodes"]:
            node = nodes[gnode["name"]]
            self.assertEqual(node.name, gnode["name"])
            self.assertEqual(node.gl_type, gnode["type"])
            self.assertEqual(node.bl_idname, gt.NODE_PREFIX + gnode["type"])
            self.assertEqual(node.gl_is_output, data.get("output") == gnode["name"])
            self.assertEqual(bool(node.mute), bool(gnode.get("bypass")))
            if positions:
                self.assertAlmostEqual(node.location.x, gnode["ui"]["x"], 3)
                self.assertAlmostEqual(node.location.y, -gnode["ui"]["y"], 3)
        links = {(l.from_node.name, l.to_node.name, l.to_socket.identifier) for l in tree.links}
        self.assertEqual(links, {(c["from"], c["to"], c["input"]) for c in data["connections"]})
        return data

    # ---- In order: each builds on the groom the one before left -------------------------------

    def test_1_tree_from_preset(self):
        tree = self.tree()
        self.assertIsNotNone(tree)
        self.assertEqual(tree.bl_idname, "GroomLabGraphTree")
        self.assertEqual(tree.gl_scalp, self.head)
        self.assertTrue(tree.use_fake_user)
        data = self.assert_tree_is_graph()
        self.assertEqual(len(data["nodes"]), len(self.head.groomlab.nodes))  # the 3D view's list, the same nodes
        # Ports typed by port type, in Maya's wire colors.
        scalp_node, interp = node_of(tree, "scalp"), node_of(tree, "interpolate")
        self.assertEqual(scalp_node.outputs[0].bl_idname, "GroomLabSocketMesh")
        self.assertEqual(interp.inputs["guides"].bl_idname, "GroomLabSocketGuides")
        self.assertEqual(interp.inputs["strands"].bl_idname, "GroomLabSocketStrands")
        color = type(interp.inputs["strands"]).draw_color_simple()
        self.assertEqual([round(c * 255) for c in color[:3]], [232, 168, 92])
        # The output node; nodes tinted by category; the type's params as properties with their ranges.
        self.assertTrue(node_of(tree, data["output"]).gl_is_output)
        self.assertTrue(interp.use_custom_color)
        clump = node_of(tree, "clump")
        rna = clump.bl_rna.properties
        self.assertEqual(rna["p_strength"].soft_min, -1.0)
        self.assertEqual(rna["p_strength"].soft_max, 1.0)
        self.assertEqual(rna["p_strengthRandom"].subtype, "FACTOR")
        self.assertIn("clump center", rna["p_strength"].description)
        self.assertAlmostEqual(clump.p_strength, self.param("clump", "strength"), 6)
        self.assertEqual(node_of(tree, "scatter").bl_rna.properties["p_length"].unit, "LENGTH")
        # The Add menu's categories: Maya's, with the helper's types (Compound and Input left out).
        cats = dict(gt.categories())
        self.assertIn("Clump", cats["Look"])
        self.assertIn("Scatter", cats["Grow"])
        offered = [item.label for c in ge._add_categories() for item in c.items(None)]
        self.assertIn("Clump", offered)
        self.assertNotIn("Compound", offered)
        self.assertIn("Soft Frizz", offered)  # the node library's compound nodes (Compound Nodes)
        self.assertIn("Wave", offered)  # and its script nodes (Script Nodes)
        self.assertNotIn("Script", offered)
        # The categories, labels and keys are the core's (node_catalog.h, shortcuts.h), not the add-on's.
        specs = graph.operator_specs()
        titles = [t for _, t in sorted({(s["categoryOrder"], s["categoryTitle"]) for s in specs.values()})]
        self.assertEqual([t for t in gt.CATEGORY_TITLES if t in titles], titles)
        self.assertIn("Linked Groom", offered)  # the Reference node's label
        self.assertIn("Gravity", cats["Deform"])
        tool_ops = importlib.import_module(bu.MODULE + ".tool_ops")
        self.assertTrue(process.running())
        self.assertEqual(tool_ops.shortcuts(), list(tool_ops.SHORTCUTS))  # the fallback matches the helper's
        self.assertGreater(groom.runtime(self.head).last["strandCount"], 100)

    def test_2_edits_in_the_tree(self):
        head, tree = self.head, self.tree()
        rt = groom.runtime(head)
        serial = rt.last["serial"]
        noise, color = node_of(tree, "noise"), node_of(tree, "color")
        # A Gravity node added (the Add menu), then wired between noise and color.
        gravity = tree.nodes.new(gt.NODE_PREFIX + "Gravity")
        gravity.location = (noise.location.x + 120, noise.location.y - 200)
        name = gravity.gl_name
        self.assertTrue(name)
        self.assertEqual(gravity.name, name)
        self.assertIn(name, [n["name"] for n in helper_graph(head)["nodes"]])
        settle(head)  # the tree made again from the graph: the node stays where it was dropped
        gravity = node_of(tree, name)
        self.assertAlmostEqual(gravity.location.x, noise.location.x + 120, 3)
        ui = next(n for n in helper_graph(head)["nodes"] if n["name"] == name)["ui"]
        self.assertAlmostEqual(ui["x"], noise.location.x + 120, 3)  # in the graph, Maya's y down
        self.assertAlmostEqual(ui["y"], -(noise.location.y - 200), 3)
        noise, color = node_of(tree, "noise"), node_of(tree, "color")
        link = next(l for l in tree.links if l.from_node == noise and l.to_node == color)
        tree.links.remove(link)
        tree.links.new(noise.outputs[0], gravity.inputs["strands"])
        tree.links.new(gravity.outputs[0], color.inputs["strands"])
        settle(head)
        conns = {(c["from"], c["to"], c["input"]) for c in helper_graph(head)["connections"]}
        self.assertIn(("noise", name, "strands"), conns)
        self.assertIn((name, "color", "strands"), conns)
        self.assertNotIn(("noise", "color", "strands"), conns)
        self.assertEqual(rt.errors, [])
        self.assertNotEqual(rt.last["serial"], serial)  # the hair changed
        self.assertIn(name, [n.name for n in head.groomlab.nodes])  # the 3D view's list follows
        self.assert_tree_is_graph()

        # A link the graph refuses (a mesh into strands): it goes away, the graph keeps its own.
        tree.links.new(node_of(tree, "scalp").outputs[0], node_of(tree, "color").inputs["strands"])
        self.assertTrue(gt.message(head))
        settle(head)
        self.assertIn((name, "color", "strands"), {(c["from"], c["to"], c["input"]) for c in helper_graph(head)["connections"]})
        self.assert_tree_is_graph()

        # Renamed (F2), muted (Bypass), deleted (X): the helper's graph follows.
        node_of(tree, name).name = "falling"
        settle(head)
        self.assertIn("falling", [n["name"] for n in helper_graph(head)["nodes"]])
        self.assertIn(("falling", "color", "strands"), {(c["from"], c["to"], c["input"]) for c in helper_graph(head)["connections"]})
        node_of(tree, "falling").mute = True
        settle(head)
        self.assertTrue(next(n for n in helper_graph(head)["nodes"] if n["name"] == "falling").get("bypass"))
        self.assertEqual(rt.errors, [])
        tree.nodes.remove(node_of(tree, "falling"))
        settle(head)
        self.assertNotIn("falling", [n["name"] for n in helper_graph(head)["nodes"]])
        tree.links.new(node_of(tree, "noise").outputs[0], node_of(tree, "color").inputs["strands"])  # closed again
        settle(head)
        self.assertEqual(rt.errors, [])
        self.assert_tree_is_graph()

        # A compound node from the library (the Add menu's Compound Nodes): a Compound holding its
        # copy, evaluated by the core.
        with gt.building():
            frizz = tree.nodes.new(gt.NODE_PREFIX + "Compound")
            frizz.gl_library = "node:groomlab.soft_frizz"
            frizz.location = (noise.location.x, noise.location.y - 400)
        gt.sync_from_tree(tree)
        settle(head)
        added = next(n for n in helper_graph(head)["nodes"] if n["name"] == "softFrizz")
        self.assertEqual(added["params"]["definition"]["id"], "groomlab.soft_frizz")
        self.assertEqual(rt.errors, [])
        tree.nodes.remove(node_of(tree, "softFrizz"))
        settle(head)
        self.assert_tree_is_graph()
        # A script node from the library (the Add menu's Script Nodes): a Script node holding its
        # file, its params the header's, evaluated by the core.
        with gt.building():
            wave = tree.nodes.new(gt.NODE_PREFIX + "Script")
            wave.gl_library = "node:groomlab.wave"
            wave.location = (noise.location.x, noise.location.y - 400)
        gt.sync_from_tree(tree)
        settle(head)
        added = next(n for n in helper_graph(head)["nodes"] if n["name"] == "wave")
        self.assertEqual((added["type"], added["params"]["definition"]["id"]), ("Script", "groomlab.wave"))
        self.assertIn("amplitude", [p["name"] for p in gt.node_param_specs(node_of(tree, "wave"))])
        self.assertEqual(rt.errors, [])
        tree.nodes.remove(node_of(tree, "wave"))
        settle(head)
        self.assert_tree_is_graph()

        # A duplicate (Shift+D): a node of its own with the copied node's params.
        clump = node_of(tree, "clump")
        clump.p_strength = 0.25
        with gt.building():
            copy = tree.nodes.new(gt.NODE_PREFIX + "Clump")
            copy.gl_copy_of = "clump"  # what Node.copy() leaves on a duplicate
            copy.location = (clump.location.x, clump.location.y - 300)
        gt.sync_from_tree(tree)
        settle(head)
        self.assertTrue(copy.gl_name and copy.gl_name != "clump")
        self.assertAlmostEqual(self.param(copy.gl_name, "strength"), 0.25, 6)
        tree.nodes.remove(node_of(tree, copy.gl_name))
        settle(head)
        self.assert_tree_is_graph()

    def test_3_params(self):
        head, tree = self.head, self.tree()
        scene = bpy.context.scene
        noise = node_of(tree, "noise")
        # A length in scene units (meters), centimeters in the helper; then a centimeter scene.
        noise.p_amplitude = 0.004
        self.assertAlmostEqual(self.param("noise", "amplitude"), 0.4, 5)
        scene.unit_settings.scale_length = 0.01
        noise.p_amplitude = 0.5
        self.assertAlmostEqual(self.param("noise", "amplitude"), 0.5, 5)
        scene.unit_settings.scale_length = 1.0
        gt.values_changed(head, "noise")
        self.assertAlmostEqual(noise.p_amplitude, 0.005, 6)
        # A choice, an int, a bool, a color.
        noise.p_space = "world"
        self.assertEqual(self.param("noise", "space"), "world")
        noise.p_octaves = 3
        self.assertEqual(self.param("noise", "octaves"), 3)
        # Numbers with a soft range draw as fill sliders (the name inside); a seed (no range) as a field.
        specs = noise.gl_param_specs
        for name, slider in (("amplitude", True), ("frequency", True), ("octaves", True), ("seed", False)):
            self.assertEqual(ge.fill_slider(noise, gt.prop_name(name), specs[name]), slider, name)
        clump = node_of(tree, "clump")
        clump.p_respectParts = False
        self.assertIs(self.param("clump", "respectParts"), False)
        color = node_of(tree, "color")
        color.p_dye = (1.0, 0.5, 0.25)
        self.assertEqual(self.param("color", "dye"), [1.0, 0.5, 0.25])
        self.assertIn("octaves", noise.gl_summary + json.dumps(gt.values(noise)))
        # From the 3D view's panel: the node shows it.
        g = head.groomlab
        g.node_index = [n.name for n in g.nodes].index("noise")
        frequency = {p.name: p for p in g.params}["frequency"]
        setattr(frequency, "value_" + frequency.widget, 7.0)
        spec = node_of(tree, "noise").gl_param_specs["frequency"]
        self.assertAlmostEqual(node_of(tree, "noise").p_frequency,
                               units.to_ui(spec.get("unit", ""), self.param("noise", "frequency"), scene), 5)
        self.assertAlmostEqual(node_of(tree, "noise").p_frequency, 7.0, 5)
        # A ramp through its curve widget (a Float Curve node): the helper's param follows.
        curve = gt.ramp_node(noise, "profile")
        self.assertIsNotNone(curve)
        points = curve.mapping.curves[0].points
        points.new(0.5, 0.2)
        curve.mapping.update()
        self.assertTrue(gt.ramp_changed(curve))
        self.assertTrue(gt.sync_ramps(tree))
        self.assertEqual(self.param("noise", "profile"), [[0.0, 0.0], [0.5, 0.2], [1.0, 1.0]])
        self.assertFalse(gt.ramp_changed(curve))
        groom.flush()
        self.assertEqual(groom.runtime(head).errors, [])

        # The sidebar panel: header, sections (Advanced folded), the curve widget, showWhen.
        def drawn(node):
            log = []
            ge.draw_node_panel(PanelLayout(log), bpy.context, node)
            return log, {entry[1] for entry in log if entry[0] == "prop"}

        clump = node_of(tree, "clump")
        tree.nodes.active = clump
        log, props = drawn(clump)
        self.assertIn("name", props)
        self.assertIn(gt.ADVANCED, [s for s, _ in gt.param_sections(gt.node_param_specs(clump))])
        self.assertTrue(ge.is_folded("Clump", gt.ADVANCED))
        advanced = {gt.prop_name(s["name"]) for s in gt.node_param_specs(clump) if s.get("advanced")} & set(clump.bl_rna.properties.keys())
        self.assertTrue(advanced)
        self.assertFalse(advanced & props)  # folded
        self.assertIn("p_strength", props)
        self.assertEqual(bpy.ops.groomlab.graph_fold(op_type="Clump", section=gt.ADVANCED), {"FINISHED"})
        self.assertTrue(advanced <= drawn(clump)[1])  # open, for every Clump this session
        bpy.ops.groomlab.graph_fold(op_type="Clump", section=gt.ADVANCED)
        log, _ = drawn(node_of(tree, "noise"))
        self.assertIn(("curve", gt.ramp_node(node_of(tree, "noise"), "profile").name), log)
        # showWhen: a param with a condition shows exactly when its condition holds (and the param
        # it names shows: Clump's count hides with clumpBy).
        checked = 0
        for node in [n for n in tree.nodes if gt.ours(n) and not n.gl_spec_json]:
            specs = {s["name"]: s for s in gt.node_param_specs(node)}
            for spec in specs.values():
                prop = gt.prop_name(spec["name"])
                if not spec.get("showWhen") or spec.get("advanced") or spec.get("group") or prop not in node.bl_rna.properties:
                    continue
                params = gt.values(node)
                holds = gt.shown_when(spec, lambda q: params.get(q, specs.get(q, {}).get("default")), specs.get)
                self.assertEqual(prop in drawn(node)[1], holds, (node.name, spec["name"]))
                checked += 1
        self.assertGreater(checked, 0)

        # Attribute params: Clump's clumpChannel writes, its regionChannel reads (a menu of the
        # attributes reaching the node; region by default may well be missing: no node note).
        def channels():
            ge.node_channels(head)
            ge._run_deferred()  # the helper's graph_channels, fetched outside drawing
            return ge.node_channels(head)

        info = channels()["clump"]
        self.assertTrue(any(w["param"] == "clumpChannel" for w in info["writes"]), info)
        bpy.ops.groomlab.graph_fold(op_type="Clump", section=gt.ADVANCED)
        log, _ = drawn(node_of(tree, "clump"))
        bpy.ops.groomlab.graph_fold(op_type="Clump", section=gt.ADVANCED)
        self.assertIn(("label", "Writes"), log)
        self.assertIn(("label", "Reads"), log)
        self.assertIn(("operator", "groomlab.graph_pick_attribute"), log)
        self.assertIn(("label", "no node upstream writes region: none applies"), log)
        self.assertNotIn("clump", ge.attribute_notes(head))
        # HairColor reading a name nothing writes: a note on the node; back to clumpId, none.
        color = node_of(tree, "color")
        color.p_clumpChannel = "nothing"
        settle(head)
        channels()
        self.assertIn("reads the attribute nothing", ge.node_notes(head).get("color", ""))
        color.p_clumpChannel = "clumpId"
        settle(head)
        channels()
        self.assertNotIn("color", ge.attribute_notes(head))

    def test_4_presets(self):
        head, tree = self.head, self.tree()
        self.assertEqual(bpy.ops.groomlab.graph_presets_refresh(), {"FINISHED"})
        self.assertIn("Hair", ge.preset_categories())
        log = []
        ge.GROOMLAB_MT_graph_presets.draw(type("M", (), {"layout": PanelLayout(log)}), bpy.context)
        self.assertIn(("menu", "GROOMLAB_MT_graph_submenu"), log)  # a submenu per category (graph_ui.json's presetMenu)
        self.assertIn(("operator", "groomlab.graph_save_preset"), log)
        hair = next(k for k, (label, _) in gu._submenus.items() if label == "Hair")
        first = gu._submenus[hair][1][0]  # a preset: Apply, Insert After Selected Node
        self.assertEqual(first[0], "menu")
        self.assertEqual([(e[1], e[2], e[3]["mode"]) for e in gu._submenus[first[2]][1]],
                         [("Apply", "groomlab.graph_preset", "APPLY"),
                          ("Insert After Selected Node", "groomlab.graph_preset", "INSERT")])
        tip = ge.GROOMLAB_OT_graph_preset.description(bpy.context, type("P", (), {"name": "frizz", "mode": "INSERT"}))
        self.assertIn("Compound", tip)
        self.assertTrue(tip.startswith((ge.preset_info("frizz").get("description") or "frizz").rstrip(".")))
        # Apply: the tree is the preset's graph, laid out as the preset is.
        self.assertEqual(bpy.ops.groomlab.graph_preset(name="curly", mode="APPLY"), {"FINISHED"})
        data = self.assert_tree_is_graph()
        self.assertIn("Curl", {n["type"] for n in data["nodes"]})
        # Insert after the selected node: a Compound with the look's exposed params.
        tree = self.tree()
        after = next(n["name"] for n in data["nodes"] if n["type"] == "Curl")
        for n in tree.nodes:
            n.select = n.name == after
        tree.nodes.active = node_of(tree, after)
        x_after = node_of(tree, after).location.x
        self.assertEqual(bpy.ops.groomlab.graph_preset(name="frizz", mode="INSERT"), {"FINISHED"})
        data = self.assert_tree_is_graph()
        look = next(n for n in tree.nodes if gt.ours(n) and n.gl_type == "Compound")
        self.assertEqual(tree.nodes.active, look)
        self.assertIn((after, look.gl_name, "strands"), {(c["from"], c["to"], c["input"]) for c in data["connections"]})
        self.assertAlmostEqual(look.location.x, x_after + gt.LAYOUT_DX, 3)
        exposed = {p.name: p for p in look.gl_params}
        self.assertIn("amount", exposed)
        self.assertEqual(exposed["amount"].widget, "length")
        exposed["amount"].value_length = 0.003  # meters
        self.assertAlmostEqual(self.param(look.gl_name, "amount"), 0.3, 5)
        log = []
        ge.draw_node_panel(PanelLayout(log), bpy.context, look)
        self.assertIn(("prop", "value_length"), log)
        # Its actions (graph_ui.json's nodeActions for a Compound) at the top, before any param.
        self.assertEqual(ge.draw_pinned_actions(PanelLayout([]), bpy.context, look)[:2],
                         ["compound.ungroup", "compound.saveAsNode"])
        ungroup = log.index(("operator", "groomlab.graph_ungroup"))
        self.assertLess(ungroup, log.index(("prop", "value_length")))
        self.assertEqual(ge.draw_pinned_actions(PanelLayout([]), bpy.context, node_of(tree, after)), [])  # a Curl: none
        settle(head)
        self.assertEqual(groom.runtime(head).errors, [])

    def test_5_positions(self):
        head, tree = self.head, self.tree()
        rt = groom.runtime(head)
        node = next(n for n in tree.nodes if gt.ours(n) and n.gl_type == "Curl")
        name = node.gl_name
        node.location = (node.location.x + 100.5, node.location.y - 50.25)
        moved = (node.location.x, node.location.y)
        self.assertFalse(gt.sync_from_tree(tree))  # a move is no edit
        # An edit from elsewhere (the 3D view's panel): the tree rebuilt, the move kept.
        made, error = graph.add_node(head, "Gravity", after=name)
        settle(head)
        node = node_of(tree, name)
        self.assertAlmostEqual(node.location.x, moved[0], 3)
        self.assertAlmostEqual(node.location.y, moved[1], 3)
        self.assert_tree_is_graph()
        # Undo (Blender restores the tree and the groom's graph text; the helper takes it back).
        before = head.groomlab.graph_json
        node_of(tree, made).location = (0.0, 400.0)
        tree.nodes.remove(node_of(tree, made))
        settle(head)
        self.assertNotIn(made, [n["name"] for n in helper_graph(head)["nodes"]])
        head.groomlab.graph_json = before  # what undo gives back
        with gt.building():
            again = tree.nodes.new(gt.NODE_PREFIX + "Gravity")
            again.name = again.gl_name = made
            again.location = (12.0, -34.0)  # the tree as undone (its own positions)
        gt.after_undo()
        settle(head)
        self.assertIn(made, [n["name"] for n in helper_graph(head)["nodes"]])
        self.assertAlmostEqual(node_of(tree, made).location.x, 12.0, 3)
        self.assertAlmostEqual(node_of(tree, made).location.y, -34.0, 3)
        self.assertAlmostEqual(node_of(tree, name).location.x, moved[0], 3)
        data = self.assert_tree_is_graph()
        self.assertEqual(rt.errors, [])
        # Tidy Layout: Maya's columns.
        self.assertEqual(bpy.ops.groomlab.graph_tidy(), {"FINISHED"})
        data = self.assert_tree_is_graph()
        self.assertEqual(next(n for n in data["nodes"] if n["name"] == "scalp")["ui"], {"x": 0.0, "y": 0.0})
        # Moved, then saved: the .groom holds the positions where Maya's editor reads them.
        node = node_of(tree, name)
        node.location = (777.0, -333.0)
        path = os.path.join(bu.TMP, "graph_positions.groom")
        groom.save_groom(head, path)
        with open(path, encoding="utf-8") as f:
            saved = json.load(f)
        ui = next(n for n in saved["graph"]["nodes"] if n["name"] == name)["ui"]
        self.assertEqual((ui["x"], ui["y"]), (777.0, 333.0))
        self.assertTrue(all(isinstance(n.get("ui"), dict) for n in saved["graph"]["nodes"]))
        # Opened again (as from Maya): the file's layout.
        node.location = (0.0, 0.0)
        self.assertEqual(bpy.ops.groomlab.load_groom(filepath=path), {"FINISHED"})
        self.assertAlmostEqual(node_of(self.tree(), name).location.x, 777.0, 3)
        self.assertAlmostEqual(node_of(self.tree(), name).location.y, -333.0, 3)

    def test_5b_pins_as_reroutes(self):
        head = self.head
        rt = groom.runtime(head)
        session = rt.session

        def call(cmd, **args):
            return process.call(cmd, session=session, **args)

        def pin_node(pin):
            return next((r for r in gt.reroutes(self.tree()) if gt.pin_of(r) == pin), None)

        def tree_links():
            return {(l.from_node.name, l.to_node.name, l.to_socket.identifier) for l in self.tree().links}

        def pins():
            return helper_graph(head).get("pins") or {"points": [], "routes": []}

        # Two Gravity nodes off interpolate (a branch nothing reads): one output into two inputs.
        src = "interpolate"
        other = next(to for (to, inp), (frm, _) in gt.connections_of(helper_graph(head)).items() if frm == src and inp == "strands")
        g1, g2 = (call("edit_graph", op="add_node", type="Gravity", x=600.0 + 200.0 * i, y=900.0)["name"] for i in range(2))
        for g in (g1, g2):
            call("edit_graph", op="connect", **{"from": src, "to": g, "input": "strands"})
        # The graph's pins (Maya's) as reroutes: the link into g1 through pin1 and pin2, into g2
        # through pin1 (a bundle: one reroute, two links out); a route whose link is not there is
        # left out, and the pin only it passes.
        gt.set_positions(head, {}, pins={
            "points": [{"id": "pin1", "x": 400.0, "y": 850.0}, {"id": "pin2", "x": 500.0, "y": 880.0},
                       {"id": "pin7", "x": 0.0, "y": 0.0}],
            "routes": [{"from": src, "to": g1, "input": "strands", "via": ["pin1", "pin2"]},
                       {"from": src, "to": g2, "input": "strands", "via": ["pin1"]},
                       {"from": "scalp", "to": other, "input": "strands", "via": ["pin7"]}]})
        gt.rebuild(head)
        settle(head)
        tree = self.tree()
        self.assertEqual(sorted(gt.pin_of(r) for r in gt.reroutes(tree)), ["pin1", "pin2"])
        r1, r2 = pin_node("pin1"), pin_node("pin2")
        pin_in = r1.inputs[0].identifier
        self.assertEqual(gt.get_location(r1), (400.0, -850.0))
        links = tree_links()
        self.assertTrue({(src, r1.name, pin_in), (r1.name, r2.name, pin_in), (r2.name, g1, "strands"),
                         (r1.name, g2, "strands")} <= links)
        self.assertFalse({(src, g1, "strands"), (src, g2, "strands")} & links)
        self.assertEqual(len([l for l in tree.links if l.from_node == r1]), 2)
        self.assertFalse(gt.sync_from_tree(tree))  # the tree is the graph
        self.assertEqual(rt.errors, [])
        serial, depth = rt.last["serial"], call("undo_state")["undoDepth"]

        # A reroute moved: layout, as a node's move (with the next edit, a save, a preset).
        gt.set_location(r2, (520.0, -900.0))
        self.assertFalse(gt.sync_from_tree(tree))
        gt.push_positions(head)
        self.assertEqual(pins()["points"], [{"id": "pin1", "x": 400.0, "y": 850.0}, {"id": "pin2", "x": 520.0, "y": 900.0}])
        self.assertEqual(len(pins()["routes"]), 2)

        # A reroute put on a link (Shift+right-drag): the same connection, now routed through it.
        connections = gt.connections_of(helper_graph(head))
        with gt.building():
            tree.links.remove(next(l for l in tree.links if l.from_node.name == src and l.to_node.name == other))
            r3 = tree.nodes.new("NodeReroute")
            gt.set_location(r3, (300.0, -100.0))
            tree.links.new(node_of(tree, src).outputs[0], r3.inputs[0])
            tree.links.new(r3.outputs[0], node_of(tree, other).inputs["strands"])
        self.assertTrue(gt.sync_from_tree(tree))
        self.assertEqual(gt.pin_of(r3), "pin3")
        self.assertEqual(gt.connections_of(helper_graph(head)), connections)
        self.assertIn({"from": src, "to": other, "input": "strands", "via": ["pin3"]}, pins()["routes"])
        self.assertIn({"id": "pin3", "x": 300.0, "y": 100.0}, pins()["points"])
        settle(head)
        self.assertEqual(rt.last["serial"], serial)  # nothing evaluated
        self.assertEqual(call("undo_state")["undoDepth"], depth)  # no undo step

        # Another output plugged into the bundle's reroute: both links through it come from there.
        tree = self.tree()
        with gt.building():
            tree.links.remove(next(l for l in tree.links if l.to_node == pin_node("pin1")))
            tree.links.new(node_of(tree, other).outputs[0], pin_node("pin1").inputs[0])
        self.assertTrue(gt.sync_from_tree(tree))
        connections = gt.connections_of(helper_graph(head))
        self.assertEqual((connections[(g1, "strands")], connections[(g2, "strands")]), ((other, ""), (other, "")))
        routes = {r["to"]: r for r in pins()["routes"]}
        self.assertEqual((routes[g1]["from"], routes[g1]["via"]), (other, ["pin1", "pin2"]))
        self.assertEqual((routes[g2]["from"], routes[g2]["via"]), (other, ["pin1"]))
        settle(head)
        self.assertEqual(rt.errors, [])
        self.assertIn((other, pin_node("pin1").name, pin_in), tree_links())

        # One its inputs cannot take (the scalp's mesh): refused, the reroute fed as it was.
        tree = self.tree()
        with gt.building():
            tree.links.remove(next(l for l in tree.links if l.to_node == pin_node("pin1")))
            tree.links.new(node_of(tree, "scalp").outputs[0], pin_node("pin1").inputs[0])
        gt.sync_from_tree(tree)
        self.assertTrue(gt.message(head))
        settle(head)
        self.assertEqual(gt.connections_of(helper_graph(head)), connections)
        self.assertIn((other, pin_node("pin1").name, pin_in), tree_links())
        self.assertNotIn(("scalp", pin_node("pin1").name, pin_in), tree_links())
        self.assertEqual(len(pins()["routes"]), 3)

        # A reroute left with no link out routes nothing: the link is gone, the reroute no pin, and it
        # stays as the user left it; a link drawn out of it again makes it a pin again.
        tree = self.tree()
        r2_name = pin_node("pin2").name
        with gt.building():
            tree.links.remove(next(l for l in tree.links if l.from_node.name == r2_name))
        self.assertTrue(gt.sync_from_tree(tree))
        self.assertNotIn((g1, "strands"), gt.connections_of(helper_graph(head)))
        self.assertNotIn("pin2", [p["id"] for p in pins()["points"]])
        settle(head)
        tree = self.tree()
        self.assertIn(r2_name, tree.nodes)
        self.assertEqual(gt.pin_of(tree.nodes[r2_name]), "")
        self.assertIn((pin_node("pin1").name, r2_name, pin_in), tree_links())
        with gt.building():
            tree.links.new(tree.nodes[r2_name].outputs[0], node_of(tree, g1).inputs["strands"])
        self.assertTrue(gt.sync_from_tree(tree))
        self.assertEqual(gt.connections_of(helper_graph(head))[(g1, "strands")], (other, ""))
        self.assertIn({"from": other, "to": g1, "input": "strands", "via": ["pin1", "pin4"]}, pins()["routes"])
        settle(head)
        # Saved in the .groom where Maya reads them.
        path = os.path.join(bu.TMP, "graph_pins.groom")
        groom.save_groom(head, path)
        with open(path, encoding="utf-8") as f:
            self.assertEqual(json.load(f)["graph"]["pins"], pins())

        # The reroutes and the branch deleted, the link closed again: no pins.
        tree = self.tree()
        with gt.building():
            for r in gt.reroutes(tree):
                tree.nodes.remove(r)
            for g in (g1, g2):
                tree.nodes.remove(node_of(tree, g))
            tree.links.new(node_of(tree, src).outputs[0], node_of(tree, other).inputs["strands"])
        self.assertTrue(gt.sync_from_tree(tree))
        settle(head)
        self.assertNotIn("pins", helper_graph(head))
        self.assertEqual(rt.errors, [])
        self.assert_tree_is_graph()

    def test_6_view_and_output(self):
        head, tree = self.head, self.tree()
        rt = groom.runtime(head)
        self.assertEqual(bpy.ops.groomlab.graph_view_node(node="interpolate"), {"FINISHED"})
        self.assertEqual(head.groomlab.view_node, "interpolate")
        self.assertTrue(node_of(tree, "interpolate").gl_viewed)
        groom.flush()
        h = process.call("evaluate", session=rt.session, viewNode="interpolate", **groom.preview_args(head))
        self.assertEqual(rt.last["strandCount"], h["strandCount"])
        pos, counts, _, _ = bu.hair_arrays(head.groomlab.hair)
        self.assertEqual(len(counts), h["strandCount"])
        self.assertEqual(bpy.ops.groomlab.graph_view_node(node="interpolate"), {"FINISHED"})  # again: off
        self.assertEqual(head.groomlab.view_node, "")
        # View is refused for a node that gives no strands.
        self.assertEqual(bpy.ops.groomlab.graph_view_node(node="scalp"), {"CANCELLED"})
        # Set as Output.
        output = helper_graph(head)["output"]
        self.assertEqual(bpy.ops.groomlab.graph_set_output(node="interpolate"), {"FINISHED"})
        self.assertEqual(helper_graph(head)["output"], "interpolate")
        self.assertTrue(node_of(self.tree(), "interpolate").gl_is_output)
        bpy.ops.groomlab.graph_set_output(node=output)
        self.assert_tree_is_graph()
        # Bypass (B).
        self.assertEqual(bpy.ops.groomlab.graph_bypass(node="interpolate"), {"FINISHED"})
        self.assertTrue(node_of(self.tree(), "interpolate").mute)
        bpy.ops.groomlab.graph_bypass(node="interpolate")
        self.assertFalse(node_of(self.tree(), "interpolate").mute)
        # Evaluate: render only (the preview skips it), then always again; kept in the graph.
        def evaluation():
            return next(n for n in helper_graph(head)["nodes"] if n["name"] == "interpolate").get("evaluation")
        self.assertEqual(bpy.ops.groomlab.graph_evaluation(node="interpolate", mode="renderOnly"), {"FINISHED"})
        self.assertEqual(evaluation(), "renderOnly")
        settle(head)
        self.assertEqual(node_of(self.tree(), "interpolate").gl_evaluation, "renderOnly")
        self.assertEqual(bpy.ops.groomlab.graph_evaluation(node="interpolate", mode="always"), {"FINISHED"})
        self.assertIsNone(evaluation())
        settle(head)
        self.assertEqual(node_of(self.tree(), "interpolate").gl_evaluation, "always")
        self.assertEqual(rt.errors, [])

    def test_7_upgrade(self):
        testhooks = os.environ.get("GROOMLAB_HELPER_TESTHOOKS", "")
        if not testhooks or not os.path.isfile(testhooks):
            self.skipTest("groomlab-helper-testhooks not built")
        head = self.head
        os.environ["GROOMLAB_HELPER"] = testhooks
        try:
            process.shutdown()
            gt.reset_for_another_helper()
            rt = groom.ensure(head)
            data = helper_graph(head)
            out = data["output"]
            feeder = next(c for c in data["connections"] if c["to"] == out)
            data["nodes"].append({"name": "lift", "type": "TestCApiLift", "version": 1, "params": {"lift": 1.0},
                                  "ui": {"x": 100.0, "y": 300.0}})
            data["connections"] = [c for c in data["connections"] if c is not feeder]
            data["connections"] += [{"from": feeder["from"], "to": "lift", "input": "strands"},
                                    {"from": "lift", "to": out, "input": feeder["input"]}]
            process.call("set_graph", session=rt.session, graph=data)
            graph.refresh(head)
            tree = self.tree()
            lift = node_of(tree, "lift")
            self.assertIsNotNone(lift)
            self.assertTrue(lift.gl_outdated)
            self.assertEqual(lift.gl_version, 1)
            log = []
            ge.draw_node_panel(PanelLayout(log), bpy.context, lift)
            self.assertIn(("operator", "groomlab.graph_upgrade_node"), log)
            self.assertEqual(bpy.ops.groomlab.graph_upgrade_node(node="lift"), {"FINISHED"})
            spec = process.call("node_spec", session=rt.session, node="lift")["spec"]
            self.assertFalse(spec["outdated"])
            self.assertEqual(next(n for n in helper_graph(head)["nodes"] if n["name"] == "lift")["version"], 2)
            lift = node_of(self.tree(), "lift")
            self.assertFalse(lift.gl_outdated)
            log = []
            ge.draw_node_panel(PanelLayout(log), bpy.context, lift)
            self.assertNotIn(("operator", "groomlab.graph_upgrade_node"), log)
            settle(head)
            self.assertEqual(groom.runtime(head).errors, [])
        finally:
            os.environ.pop("GROOMLAB_HELPER", None)
            process.shutdown()
            gt.reset_for_another_helper()

    def test_8_editor_ui(self):
        """The node editor's header, menus and sidebar draw (into checking stand-in layouts, as a
        node editor showing the graph would draw them): every property and operator they name exists."""
        tree = self.tree()
        if tree is None or not len(tree.nodes):
            graph.refresh(self.head)
            tree = self.tree()
        space = type("Space", (), {"type": "NODE_EDITOR", "tree_type": gt.TREE, "edit_tree": tree, "node_tree": tree})()
        ctx = type("Context", (), {"space_data": space, "scene": bpy.context.scene, "active_object": self.head,
                                   "window_manager": bpy.context.window_manager, "preferences": bpy.context.preferences,
                                   "region": None})()
        self.assertEqual(ge.editor_tree(ctx), tree)
        node = node_of(tree, "clump")
        for n in tree.nodes:
            n.select = n == node
        tree.nodes.active = node
        log = []

        def holder():
            return type("Holder", (), {"layout": PanelLayout(log)})()
        ge.draw_header(holder(), ctx)
        ge.draw_context_menu(holder(), ctx)
        for cls in (ge.GROOMLAB_MT_graph_edit, ge.GROOMLAB_MT_graph_presets,
                    ge.GROOMLAB_PT_graph_node, ge.GROOMLAB_PT_graph_groom, ge.GROOMLAB_PT_graph_presets):
            if hasattr(cls, "poll"):
                self.assertTrue(cls.poll(ctx), cls)
            cls.draw(holder(), ctx)
        for key in [k for k in gu._submenus if k.startswith(("node/", "presets/"))]:  # the submenus (Evaluate, presets)
            gu.draw_prepared(PanelLayout(log), gu._submenus[key][1])
        ops = {entry[1] for entry in log if entry[0] == "operator"}
        for op in ("groomlab.graph_set_output", "groomlab.graph_view_node", "groomlab.graph_bypass",
                   "groomlab.graph_evaluation", "groomlab.graph_duplicate", "groomlab.graph_tidy", "groomlab.graph_reload",
                   "groomlab.graph_preset", "groomlab.graph_group", "groomlab.graph_copy", "groomlab.graph_paste"):
            self.assertIn(op, ops)
        self.assertIn(("menu", "GROOMLAB_MT_graph_presets"), log)
        self.assertIn(("menu", "GROOMLAB_MT_graph_edit"), log)
        self.assertIn("p_strength", {entry[1] for entry in log if entry[0] == "prop"})
        # The 3D view's GroomLab tab still draws; presets and the graph are the editor's alone (no
        # Presets or Graph panel in the 3D view's sidebar, as Maya keeps them out of Groom Tools).
        self.assertIn(("operator", "groomlab.update"), bu.draw_panels())
        view3d = [c for c in bpy.types.Panel.__subclasses__()
                  if getattr(c, "bl_space_type", "") == "VIEW_3D" and getattr(c, "bl_category", "") == "GroomLab"]
        self.assertTrue(view3d)
        for c in view3d:
            self.assertNotIn(c.bl_label, ("Presets", "Graph"), c)
        self.assertFalse(hasattr(bpy.types, "VIEW3D_PT_groomlab_presets") or hasattr(bpy.types, "VIEW3D_PT_groomlab_graph"))
        self.assertEqual(bpy.ops.groomlab.graph_open(), {"FINISHED"})  # no interface here: the tree is made

    def test_8b_preset_makes_the_maps_it_reads(self):
        # A preset reading a painted map the groom lacks (flow_with_guides' guideAreas): the map is
        # made (black, a vertex map), so the groom evaluates at once; applied again, it is kept.
        head = self.head
        self.assertNotIn("guideAreas", {m["name"] for m in ge.painted_maps(head)})
        self.assertEqual(bpy.ops.groomlab.graph_preset(name="flow_with_guides", mode="APPLY"), {"FINISHED"})
        self.assertIn(("guideAreas", "vertices"), {(m["name"], m["kind"]) for m in ge.painted_maps(head)})
        settle(head)
        self.assertEqual(groom.runtime(head).errors, [])
        self.assertEqual(bpy.ops.groomlab.graph_preset(name="flow_with_guides", mode="APPLY"), {"FINISHED"})
        self.assertEqual([m["name"] for m in ge.painted_maps(head)].count("guideAreas"), 1)
        tree = self.tree()
        self.assertIn("blend", {n.gl_name for n in tree.nodes if gt.ours(n)})

    def test_9_remove_groom(self):
        self.assertEqual(bpy.ops.groomlab.remove_groom(), {"FINISHED"})
        self.assertIsNone(gt.tree_of(self.head))
        self.assertFalse([t for t in bpy.data.node_groups if t.bl_idname == gt.TREE])


if __name__ == "__main__":
    bu.run(GraphEditorTest)
