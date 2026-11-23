# SPDX-License-Identifier: Apache-2.0
"""The add-on's Graph Editor authoring (graph_authoring.py, graph_ui.py) in a headless Blender, as
Maya's Graph Editor does it: the menus and colors from the shared UI definitions (engine/ui), Group
into Compound / Ungroup and a compound's inner params, params exposed as inputs, sliders and keyed
params (Blender keys on the groom's ctl_ properties), map sources and the expression's text block,
backdrops as Blender frames, the node library (Save as Node, Save to Library, Update Node, Import
Node), script nodes edited in text blocks with live errors, Save as Preset, Copy Graph / Paste
Graph, a node dropped on a link, Tab search's painted maps, and the tree after the helper's undo."""

import importlib
import json
import os
import shutil
import sys
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

# The node library and presets this test writes: its own folders (the helper reads them at start).
os.environ["GROOMLAB_USER_NODES"] = os.path.join(bu.TMP, "nodes")
os.environ["GROOMLAB_USER_PRESETS"] = os.path.join(bu.TMP, "presets")

pkg, m = bu.enable()
groom, graph, process = m["groom"], m["graph"], m["process"]
gt = importlib.import_module(bu.MODULE + ".graph_tree")
ge = importlib.import_module(bu.MODULE + ".graph_editor")
gu = importlib.import_module(bu.MODULE + ".graph_ui")
ga = importlib.import_module(bu.MODULE + ".graph_authoring")
tools = importlib.import_module(bu.MODULE + ".tools")

GRAPH_CONTAINERS = ("nodeMenu", "backdropMenu", "editMenu", "addMenu", "presetMenu", "presetItem", "keyMenu",
                    "paramMenu", "paintMenu")


class Layout(bu.FakeLayout):
    scale_y = 1.0

    def _sub(self, *args, **kwargs):
        return Layout(self.log)

    row = column = box = split = _sub

    def template_curve_mapping(self, data, name, **kwargs):
        self.log.append(("curve", data.name))

    def context_string_set(self, name, value):
        self.log.append(("context", name, value))

    def menu(self, idname, **kwargs):
        self.log.append(("menu", idname, kwargs.get("text", "")))

    def prop(self, data, name, **kwargs):
        if name.startswith('["'):
            if name[2:-2] not in data.keys():
                raise AttributeError("%s has no custom property %s" % (data, name))
            self.log.append(("prop", name))
            return
        bu.FakeLayout.prop(self, data, name, **kwargs)

    def operator(self, idname, **kwargs):
        props = bu.FakeLayout.operator(self, idname, **kwargs)
        self.log.append(("op_text", idname, kwargs.get("text", "")))
        return props


def helper_graph(scalp):
    return process.call("get_graph", session=groom.runtime(scalp).session)["graph"]


def gnode(scalp, name):
    return next((n for n in helper_graph(scalp)["nodes"] if n["name"] == name), None)


def links(scalp):
    return {(c["from"], c["to"], c["input"]) for c in helper_graph(scalp)["connections"]}


def node_of(tree, name):
    return next((n for n in tree.nodes if gt.ours(n) and n.gl_name == name), None)


def settle():
    gt.flush()
    groom.flush()


def texts_of(entries):
    return [e[1] for e in entries if e[0] in ("op", "menu")]


class GraphAuthoringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.head = bu.head_object()
        graph.presets()
        assert bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset="hair_basics", guides=30) == {"FINISHED"}
        gt.rebuild(cls.head)

    def tree(self):
        return gt.tree_of(self.head)

    def param(self, node, name):
        return process.call("get_param", session=groom.runtime(self.head).session, node=node, param=name)["value"]

    def assert_tree_is_graph(self):
        data = helper_graph(self.head)
        tree = self.tree()
        self.assertEqual({n.gl_name for n in tree.nodes if gt.ours(n)}, {n["name"] for n in data["nodes"]})
        tlinks = {(l.from_node.name, l.to_node.name, l.to_socket.identifier) for l in tree.links}
        self.assertEqual(tlinks, {(c["from"], c["to"], c["input"]) for c in data["connections"]})
        return data

    # ---- In order: each builds on the groom the one before left -------------------------------

    def test_1_definitions(self):
        """Menus, labels and colors are graph_ui.json's; every action they list is bound."""
        doc = gu.document()
        self.assertIn("graph", doc)
        self.assertEqual(gt.CATEGORY_COLORS["Look"], (0xAA, 0x78, 0xF0))
        self.assertEqual(gt.TYPE_COLORS["Strands"], (232, 168, 92))
        self.assertIn(("Scalp", "input"), gt.PLUMBING)
        self.assertEqual([c[0] for c in gu.backdrop_colors()], ["Blue", "Green", "Purple", "Amber", "Gray"])
        # A node whose path names a file (filePathFilters): Blender's file browser button.
        self.assertEqual(gt.node_class("Reference").bl_rna.properties["p_path"].subtype, "FILE_PATH")
        unbound = set()
        for name in GRAPH_CONTAINERS:
            for action_id in gu.listed_ids(gu.get("graph/" + name)):
                if action_id not in gu.ACTIONS:
                    unbound.add(action_id)
        self.assertEqual(unbound, gu.NOT_IN_BLENDER)
        for action_id, b in gu.ACTIONS.items():  # each binding runs an operator that exists
            module, op = b.idname.split(".")
            self.assertTrue(hasattr(getattr(bpy.ops, module), op), b.idname)
        # Tips are the actions': an operator's tooltip is its menu entry's.
        self.assertEqual(ga.GROOMLAB_OT_graph_group.description(bpy.context, type("P", (), {"action": ""})()),
                         gu.tip("graph.group"))
        # The node menu of a node: Maya's entries in Maya's order, with labels from actions.json.
        tree = self.tree()
        for n in tree.nodes:
            n.select = n.gl_name == "clump"
        tree.nodes.active = node_of(tree, "clump")
        log = []
        entries = ge.draw_node_menu(Layout(log), bpy.context, node_of(tree, "clump"))
        labels = texts_of(entries)
        self.assertEqual(labels[:3], [gu.label("node.setOutput"), gu.label("node.view"), gu.label("node.bypass")])
        self.assertIn("Evaluate", labels)
        self.assertIn("Expose Param as Input", labels)
        self.assertNotIn(gu.label("compound.ungroup"), labels)  # a compound's only
        self.assertEqual(labels[-3:], [gu.label("compound.groupSelection"), gu.label("backdrop.add"), gu.label("node.delete")])
        evaluate = next(e for e in entries if e[0] == "menu" and e[1] == "Evaluate")
        self.assertEqual([e[1] for e in gu._submenus[evaluate[2]][1]], [c["label"] for c in gu.choices("evaluation")])
        # The Edit menu: undo, framing, group, copy and paste the graph, reload (no GPU preview here).
        log = []
        ge.GROOMLAB_MT_graph_edit.draw(type("M", (), {"layout": Layout(log)}), bpy.context)
        edit = [e[2] for e in log if e[0] == "op_text"]
        self.assertEqual(edit[:2], ["Undo", "Redo"])
        for text in ("Group into Compound", "Ungroup Compound", "Copy Graph", "Paste Graph", "Reload from Groom"):
            self.assertIn(text, edit)
        self.assertNotIn(gu.label("graph.gpuPreview"), edit)
        # The Add menu: the categories, then the node library's entries (addMenu).
        space = type("Space", (), {"type": "NODE_EDITOR", "tree_type": gt.TREE, "edit_tree": tree, "node_tree": tree})()
        ctx = type("Context", (), {"space_data": space, "scene": bpy.context.scene, "active_object": self.head,
                                   "window_manager": bpy.context.window_manager, "preferences": bpy.context.preferences,
                                   "region": None})()
        log = []
        ge._draw_add_menu(ge._add_categories())(type("M", (), {"layout": Layout(log)}), ctx)
        menus = [e[1] for e in log if e[0] == "menu"]
        self.assertTrue(any(name.startswith("NODE_MT_category_GROOMLAB_") for name in menus))
        self.assertEqual([e[2] for e in log if e[0] == "op_text"][-3:],
                         ["New Script Node", "Import Node...", "Open Nodes Folder"])
        # Ctrl+G and Ctrl+Shift+G, only in GroomLab graphs.
        keys = {(kmi.idname, kmi.type, kmi.ctrl, kmi.shift) for _, kmi in ge._keymaps}
        self.assertIn(("groomlab.graph_group", "G", True, False), keys)
        self.assertIn(("groomlab.graph_ungroup", "G", True, True), keys)
        other = type("Context", (), {"space_data": type("S", (), {"type": "NODE_EDITOR", "tree_type": "ShaderNodeTree"})()})()
        self.assertFalse(ga.GROOMLAB_OT_graph_group.poll(other))

    def test_2_compounds(self):
        head = self.head
        before = {n["name"] for n in helper_graph(head)["nodes"]}
        self.assertEqual(bpy.ops.groomlab.graph_group(names="noise,color"), {"FINISHED"})
        tree = self.tree()
        compound = tree.nodes.active
        self.assertEqual(compound.gl_type, "Compound")
        self.assertIsNone(gnode(head, "noise"))
        self.assert_tree_is_graph()
        # The compound's menu: Ungroup, Show Inner Param (its inner nodes' params), Save as Node.
        entries = ge.draw_node_menu(Layout([]), bpy.context, compound)
        labels = texts_of(entries)
        self.assertIn(gu.label("compound.ungroup"), labels)
        self.assertIn(gu.label("compound.saveAsNode"), labels)
        self.assertNotIn(gu.label("compound.saveToLibrary"), labels)  # not a library node yet
        inner = next(e for e in entries if e[0] == "menu" and e[1] == "Show Inner Param")
        self.assertIn("noise", texts_of(gu._submenus[inner[2]][1]))
        # An inner param shown on the compound, then not.
        self.assertEqual(bpy.ops.groomlab.graph_show_inner(node=compound.gl_name, target_param="noise.amplitude"), {"FINISHED"})
        compound = node_of(self.tree(), compound.gl_name)
        self.assertIn("amplitude", [p["name"] for p in gt.node_param_specs(compound)])
        self.assertIn("noise.amplitude", ga.shown_inner(compound))
        bpy.ops.groomlab.graph_show_inner(node=compound.gl_name, target_param="noise.amplitude")
        self.assertNotIn("amplitude", [p["name"] for p in gt.node_param_specs(node_of(self.tree(), compound.gl_name))])
        # The helper's undo (Groom Tools' Ctrl+Z) answers graphChanged: the tree follows.
        tools.undo(head)  # (the unexpose)
        tools.undo(head)  # (the expose)
        tools.undo(head)  # the group
        settle()
        self.assertEqual({n["name"] for n in helper_graph(head)["nodes"]}, before)
        self.assertIsNotNone(node_of(self.tree(), "noise"))
        self.assert_tree_is_graph()
        tools.undo(head, redo=True)
        settle()
        self.assertIsNone(node_of(self.tree(), "noise"))
        self.assert_tree_is_graph()
        # Ungroup (Ctrl+Shift+G): the nodes back.
        name = next(n.gl_name for n in self.tree().nodes if gt.ours(n) and n.gl_type == "Compound")
        self.assertEqual(bpy.ops.groomlab.graph_ungroup(node=name), {"FINISHED"})
        self.assertEqual({n["name"] for n in helper_graph(head)["nodes"]}, before)
        self.assertEqual({n.gl_name for n in self.tree().nodes if n.select}, {"noise", "color"})
        self.assert_tree_is_graph()
        settle()
        self.assertEqual(groom.runtime(head).errors, [])

    def test_3_expose_and_wire_insertion(self):
        head, tree = self.head, self.tree()
        self.assertIn("width", ga.linkable(node_of(tree, "width")))
        self.assertEqual(bpy.ops.groomlab.graph_expose(node="width", param="width"), {"FINISHED"})
        self.assertEqual(gnode(head, "width").get("paramInputs"), ["width"])
        width = node_of(self.tree(), "width")
        self.assertEqual(width.inputs["width"].display_shape, "DIAMOND")
        entries = ge.draw_node_menu(Layout([]), bpy.context, width)
        expose = next(e for e in entries if e[0] == "menu" and e[1] == "Expose Param as Input")
        checked = next(e for e in gu._submenus[expose[2]][1] if e[1] == "width")
        self.assertEqual(checked[4], "CHECKBOX_HLT")
        bpy.ops.groomlab.graph_expose(node="width", param="width")
        self.assertNotIn("paramInputs", gnode(head, "width"))
        # The node's three-line button exposes every connectable param, then collapses them all.
        params = ga.linkable(node_of(self.tree(), "width"))
        self.assertGreater(len(params), 1)
        self.assertEqual(bpy.ops.groomlab.graph_toggle_inputs(node="width"), {"FINISHED"})
        self.assertEqual(gnode(head, "width")["paramInputs"], params)
        tree = self.tree()
        field = tree.nodes.new(gt.NODE_PREFIX + "FieldNoise")
        field_name = field.gl_name
        settle()
        tree = self.tree()
        field, width = node_of(tree, field_name), node_of(tree, "width")
        with gt.building():
            tree.links.new(field.outputs[0], width.inputs["width"])
        gt.sync_from_tree(tree)
        settle()
        tree = self.tree()
        link = next(link for link in tree.links if link.to_node.gl_name == "width" and link.to_socket.identifier == "width")
        with gt.building():
            tree.links.remove(link)
        gt.sync_from_tree(tree)
        settle()
        self.assertEqual(bpy.ops.groomlab.graph_toggle_inputs(node="width"), {"FINISHED"})
        self.assertNotIn("paramInputs", gnode(head, "width"))
        self.assertEqual(bpy.ops.groomlab.graph_toggle_inputs(node="width"), {"FINISHED"})
        self.assertEqual(gnode(head, "width")["paramInputs"], params)
        tree = self.tree()
        field, width = node_of(tree, field_name), node_of(tree, "width")
        with gt.building():
            tree.links.new(field.outputs[0], width.inputs["width"])
        gt.sync_from_tree(tree)
        settle()
        self.assertIn((field_name, "width", "width"), links(head))
        # Back as it was for what follows (a param fed by a port is not keyed): the field gone, the
        # inputs collapsed.
        self.assertEqual(bpy.ops.groomlab.graph_delete(node=field_name), {"FINISHED"})
        settle()
        self.assertEqual(bpy.ops.groomlab.graph_toggle_inputs(node="width"), {"FINISHED"})
        self.assertNotIn("paramInputs", gnode(head, "width"))
        # A node dropped on a link: Blender splices it in (the link a -> b goes, a -> node -> b come);
        # the graph's insert_on_link does it, one step on the helper's stack.
        tree = self.tree()
        extra = tree.nodes.new(gt.NODE_PREFIX + "Gravity")
        name = extra.gl_name
        settle()
        tree = self.tree()
        extra, color, widthn = node_of(tree, name), node_of(tree, "color"), node_of(tree, "width")
        with gt.building():
            tree.links.remove(next(l for l in tree.links if l.from_node == color and l.to_node == widthn))
            tree.links.new(color.outputs[0], extra.inputs["strands"])
            tree.links.new(extra.outputs[0], widthn.inputs["strands"])
        gt.sync_from_tree(tree)
        self.assertIn(("color", name, "strands"), links(head))
        self.assertIn((name, "width", "strands"), links(head))
        self.assertNotIn(("color", "width", "strands"), links(head))
        self.assertEqual(tools.call(head, "undo_state")["undoLabel"], "insert node")
        settle()
        self.assert_tree_is_graph()
        self.assertEqual(bpy.ops.groomlab.graph_delete(node=name), {"FINISHED"})
        tree = self.tree()
        with gt.building():
            tree.links.new(node_of(tree, "color").outputs[0], node_of(tree, "width").inputs["strands"])
        gt.sync_from_tree(tree)
        settle()
        self.assertIn(("color", "width", "strands"), links(head))
        self.assert_tree_is_graph()

    def test_4_sliders_and_keys(self):
        head = self.head
        scene = bpy.context.scene
        # Drive with Slider: a Control node, the groom's ctl_ property its value.
        amplitude = self.param("noise", "amplitude")
        self.assertEqual(bpy.ops.groomlab.graph_add_control(node="noise", param="amplitude"), {"FINISHED"})
        control = next(c for c in graph.controls(head) if c["kind"] == "control")
        key = ga.CONTROL_PREFIX + control["name"]
        self.assertIn(key, head.keys())
        self.assertAlmostEqual(head[key], amplitude, 5)
        tree = self.tree()
        driver = next(n for n in tree.nodes if gt.ours(n) and n.gl_type == "Control")
        self.assertEqual(ga.key_state(head, node_of(tree, "noise"), "amplitude"), "driven")
        log = []
        ge.draw_node_panel(Layout(log), bpy.context, driver)
        self.assertIn(("prop", '["%s"]' % key), log)
        head[key] = amplitude * 3.0
        self.assertTrue(ga.push_controls(head))
        value = next(c for c in graph.controls(head) if c["name"] == control["name"])["value"]
        self.assertAlmostEqual(value, amplitude * 3.0, 5)
        self.assertFalse(ga.push_controls(head))  # unchanged: nothing sent
        tools.undo(head)  # the control gone again
        settle()
        del head[key]
        # Set Key: the param reads a control of its own, keyed on the groom (Blender's keys).
        width = self.param("width", "width")
        scene.frame_set(1)
        self.assertEqual(bpy.ops.groomlab.graph_set_key(node="width", param="width"), {"FINISHED"})
        names = ga.control_names(node_of(self.tree(), "width"), "width")
        self.assertEqual(names, ["width_width"])
        self.assertEqual(gnode(head, "width")["paramControls"], {"width": "width_width"})
        self.assertEqual(ga.key_state(head, node_of(self.tree(), "width"), "width"), "keyed")
        scene.frame_set(10)
        head["ctl_width_width"] = width * 2.0
        bpy.ops.groomlab.graph_set_key(node="width", param="width")
        scene.frame_set(5)  # Blender evaluates the keys; the handler gives the value to the graph
        between = head["ctl_width_width"]
        self.assertTrue(width < between < width * 2.0)
        ga.push_controls(head)
        value = next(c for c in graph.controls(head) if c["name"] == "width_width")["value"]
        self.assertAlmostEqual(value, between, 5)
        self.assertEqual(ga.key_state(head, node_of(self.tree(), "width"), "width"), "animated")
        # The keys travel in the .groom (controlKeys: every host and the CLI animate them), and a
        # load keys the ctl_ property again from them.
        keyed = ga.control_keys(head)
        self.assertEqual(list(keyed), ["width_width"])
        self.assertEqual([k[0] for k in keyed["width_width"]], [1.0, 10.0])
        self.assertAlmostEqual(keyed["width_width"][1][1], width * 2.0, 5)
        import tempfile
        path = os.path.join(tempfile.mkdtemp(prefix="groomlab_keys_"), "keyed.groom")
        groom.save_groom(head, path)
        with open(path) as f:
            saved = json.load(f)
        entry = saved["grooms"][0] if "grooms" in saved else saved
        self.assertEqual([round(k[0], 3) for k in entry["controlKeys"]["width_width"]], [1.0, 10.0])
        ga.key_controls(head, {"width_width": [[1.0, width], [20.0, width * 3.0]]})
        self.assertEqual([k[0] for k in ga.control_keys(head)["width_width"]], [1.0, 20.0])
        ga.key_controls(head, entry["controlKeys"])  # (back as saved)
        # The param's row shows the key's property; its menu has the key entries.
        log = []
        ge.draw_param(Layout(log), node_of(self.tree(), "width"), node_of(self.tree(), "width").gl_param_specs["width"],
                      scene, {}, set(), {}, None, bpy.context)
        self.assertIn(("prop", '["ctl_width_width"]'), log)
        menu = next(k for k in gu._submenus if k == "param:width:width")
        self.assertEqual(texts_of(gu._submenus[menu][1])[:3], ["Set Key", "Delete Key", "Remove Animation"])
        # A count is never keyed: Set Key disabled, saying why.
        resample = node_of(self.tree(), "resample")
        state, values = ge.param_state(resample, resample.gl_param_specs["segments"], head)
        self.assertNotIn("param.canKey", state)
        self.assertTrue(values["why"])
        # Remove Animation: the value now goes back into the param, the keys go.
        self.assertEqual(bpy.ops.groomlab.graph_remove_animation(node="width", param="width"), {"FINISHED"})
        self.assertNotIn("paramControls", gnode(head, "width"))
        self.assertAlmostEqual(self.param("width", "width"), between, 5)
        self.assertNotIn("ctl_width_width", head.keys())
        settle()
        self.assertEqual(groom.runtime(head).errors, [])

    def test_5_map_sources(self):
        head = self.head
        # A painted map: on the scalp's vertices; Tab search lists it as a Map node.
        tools.map_create(head, "clumpiness", kind="vertices", value=0.5)
        maps = ge.painted_maps(head)
        self.assertIn("clumpiness", [mm["name"] for mm in maps])
        cat = next(c for c in ge._add_categories() if c.name == ge.PAINTED_MAPS)
        items = list(cat.items(None))
        self.assertIn("Map: clumpiness", [i.label for i in items])
        item = next(i for i in items if i.label == "Map: clumpiness")
        tree = self.tree()
        with gt.building():
            made = tree.nodes.new(item.nodetype)
            for k, v in item.settings.items():
                setattr(made, k, eval(v))
        gt.sync_from_tree(tree)
        settle()
        added = gnode(head, "clumpiness")
        self.assertIsNotNone(added)
        self.assertEqual((added["type"], added["params"]["name"]), ("Map", "clumpiness"))
        bpy.ops.groomlab.graph_delete(node="clumpiness")
        # The map source dialog: a color set, an attribute, a painted map; then back to a value.
        for source, name in (("colorSet", "Col"), ("attribute", "clumpId"), ("guide", "clumpiness")):
            self.assertEqual(bpy.ops.groomlab.graph_map_source(node="clump", param="strength", source=source, name=name,
                                                               low=0.0, high=0.8), {"FINISHED"})
            value = self.param("clump", "strength")
            self.assertEqual((value[source], value["max"]), (name, 0.8))
        log = []
        ge.draw_node_panel(Layout(log), bpy.context, node_of(self.tree(), "clump"))
        self.assertIn(("label", "Strength: painted map clumpiness"), log)
        self.assertEqual(bpy.ops.groomlab.graph_map_source(node="clump", param="strength", clear=True), {"FINISHED"})
        self.assertFalse(isinstance(self.param("clump", "strength"), dict))
        # An expression: its text block, errors shown, a good one taken.
        self.assertEqual(bpy.ops.groomlab.graph_map_source(node="noise", param="amplitude", source="expression",
                                                           name="0.5 * u"), {"FINISHED"})
        self.assertEqual(self.param("noise", "amplitude")["expression"], "0.5 * u")
        text = bpy.data.texts[ga.expression_text_name("noise", "amplitude")]
        self.assertEqual(text.as_string(), "0.5 * u")
        text.from_string("0.5 *")
        ga.check_texts()
        self.assertTrue(ga.text_errors(text))
        self.assertEqual(self.param("noise", "amplitude")["expression"], "0.5 * u")  # refused: kept
        log = []
        ge.draw_node_panel(Layout(log), bpy.context, node_of(self.tree(), "noise"))
        self.assertTrue(any(e[0] == "label" and e[1].startswith("Error: ") for e in log))
        text.from_string("0.25 * v\n")
        ga.check_texts()
        self.assertEqual(ga.text_errors(text), [])
        self.assertEqual(self.param("noise", "amplitude")["expression"], "0.25 * v")
        settle()
        self.assertEqual(groom.runtime(head).errors, [])
        bpy.ops.groomlab.graph_map_source(node="noise", param="amplitude", clear=True)
        bpy.data.texts.remove(text)

    def test_6_backdrops(self):
        head, tree = self.head, self.tree()
        for n in tree.nodes:
            n.select = gt.ours(n) and n.gl_name in ("clump", "noise")
        self.assertEqual(bpy.ops.groomlab.graph_backdrop_add(title="Look"), {"FINISHED"})
        backdrops = helper_graph(head)["backdrops"]
        self.assertEqual(len(backdrops), 1)
        b = backdrops[0]
        self.assertEqual((b["title"], b["color"]), ("Look", [70, 90, 120]))
        clump = node_of(self.tree(), "clump")
        self.assertTrue(b["x"] < clump.location_absolute[0] < b["x"] + b["w"])
        frame = gt.frames(self.tree())[0]
        self.assertEqual(clump.parent, frame)
        # The backdrop menu (rename, color, delete) and the sidebar's.
        entries = ge.draw_backdrop_menu(Layout([]), bpy.context, frame)
        self.assertEqual(texts_of(entries), [gu.label("backdrop.rename"), "Color", gu.label("backdrop.fold"),
                                             gu.label("backdrop.delete")])
        color = next(e for e in entries if e[0] == "menu")
        self.assertEqual(texts_of(gu._submenus[color[2]][1]), ["Blue", "Green", "Purple", "Amber", "Gray"])
        self.assertEqual(bpy.ops.groomlab.graph_backdrop_color(frame=frame.name, color="[110, 80, 130]"), {"FINISHED"})
        self.assertEqual(helper_graph(head)["backdrops"][0]["color"], [110, 80, 130])
        self.assertEqual(bpy.ops.groomlab.graph_backdrop_rename(frame=frame.name, title="Frizz"), {"FINISHED"})
        self.assertEqual(helper_graph(head)["backdrops"][0]["title"], "Frizz")
        # Moved (with its nodes): the move goes with the next edit.
        x, y = frame.location_absolute
        frame.location_absolute = (x + 100.0, y)
        self.assertAlmostEqual(node_of(self.tree(), "clump").location_absolute[0], clump.location_absolute[0], 3)
        bpy.ops.groomlab.graph_bypass(node="clump")
        bpy.ops.groomlab.graph_bypass(node="clump")
        data = helper_graph(head)
        self.assertAlmostEqual(data["backdrops"][0]["x"], b["x"] + 100.0, 1)
        self.assertAlmostEqual(gnode(head, "clump")["ui"]["x"], node_of(self.tree(), "clump").location_absolute[0], 1)
        # A backdrop set in the graph (Maya's, a preset's): a frame.
        graph.set_backdrops(head, data["backdrops"] + [{"title": "Two", "x": 0.0, "y": 600.0, "w": 300.0, "h": 200.0,
                                                         "color": [70, 120, 80]}])
        gt.rebuild(head)
        self.assertEqual([f.label for f in gt.frames(self.tree())], ["Frizz", "Two"])
        two = gt.frames(self.tree())[1]
        self.assertEqual((two.width, two.height), (300.0, 200.0))
        self.assertEqual(tuple(two.location_absolute), (0.0, -600.0))
        # Deleted: its nodes stay.
        self.assertEqual(bpy.ops.groomlab.graph_backdrop_delete(frame=two.name), {"FINISHED"})
        self.assertEqual([d["title"] for d in helper_graph(head)["backdrops"]], ["Frizz"])
        self.assertEqual(len(gt.frames(self.tree())), 1)
        # Saved in the .groom where Maya reads it.
        path = os.path.join(bu.TMP, "backdrops.groom")
        groom.save_groom(head, path)
        with open(path, encoding="utf-8") as f:
            self.assertEqual([d["title"] for d in json.load(f)["graph"]["backdrops"]], ["Frizz"])

    def test_7_copy_paste(self):
        head = self.head
        self.assertEqual(bpy.ops.groomlab.graph_copy(), {"FINISHED"})
        text = ga._clipboard[0]
        data = json.loads(text)
        self.assertEqual(data["about"]["category"], "Shared")
        self.assertGreater(data["size"], 1.0)
        self.assertEqual([d["title"] for d in data["backdrops"]], ["Frizz"])
        names = {n["name"] for n in data["nodes"]}
        self.assertEqual(bpy.ops.groomlab.graph_preset(name="curly", mode="APPLY"), {"FINISHED"})
        self.assertNotEqual({n["name"] for n in helper_graph(head)["nodes"]}, names)
        self.assertEqual(bpy.ops.groomlab.graph_paste(text="```json\n%s\n```" % text), {"FINISHED"})
        self.assertEqual({n["name"] for n in helper_graph(head)["nodes"]}, names)
        self.assertEqual([d["title"] for d in helper_graph(head).get("backdrops", [])], ["Frizz"])
        self.assertEqual([f.label for f in gt.frames(self.tree())], ["Frizz"])
        self.assert_tree_is_graph()
        with self.assertRaises(RuntimeError):  # refused, saying why (the operator's error)
            bpy.ops.groomlab.graph_paste(text="not a graph")
        self.assertEqual({n["name"] for n in helper_graph(head)["nodes"]}, names)
        settle()
        self.assertEqual(groom.runtime(head).errors, [])

    def test_8_node_library(self):
        head = self.head
        bpy.ops.groomlab.graph_group(names="noise,color")
        compound = self.tree().nodes.active.gl_name
        self.assertEqual(bpy.ops.groomlab.graph_save_node(node=compound, label="My Look", category="Look",
                                                          about="noise and color"), {"FINISHED"})
        node = node_of(self.tree(), compound)
        info = ga.library_info(node)
        self.assertEqual((info["version"], info["updateAvailable"]), (1, False))
        path = next(os.path.join(os.environ["GROOMLAB_USER_NODES"], f) for f in os.listdir(os.environ["GROOMLAB_USER_NODES"]))
        self.assertIn("My Look", [e.get("label") for e in graph.library_nodes(refresh=True)])
        labels = texts_of(ge.draw_node_menu(Layout([]), bpy.context, node))
        self.assertIn(gu.label("compound.saveToLibrary"), labels)
        # Save to Library: version 2 in the library; this node holds it.
        self.assertEqual(bpy.ops.groomlab.graph_save_to_library(node=compound), {"FINISHED"})
        self.assertEqual(ga.library_info(node_of(self.tree(), compound))["version"], 2)
        # The library newer than the node: the update mark, Update Node (version 3).
        with open(path, encoding="utf-8") as f:
            saved = json.load(f)
        saved["version"] = 3
        with open(path, "w", encoding="utf-8") as f:
            json.dump(saved, f)
        graph.library_nodes(refresh=True)
        gt.rebuild(head)
        node = node_of(self.tree(), compound)
        self.assertTrue(ga.library_info(node)["updateAvailable"])
        state, values = ge.node_state(node)
        self.assertIn("node.updateAvailable", state)
        labels = texts_of(ge.draw_node_menu(Layout([]), bpy.context, node))
        self.assertIn("Update Node (version 3)", labels)
        self.assertEqual(bpy.ops.groomlab.graph_update_node(node=compound), {"FINISHED"})
        self.assertFalse(ga.library_info(node_of(self.tree(), compound))["updateAvailable"])
        # Import Node: a shared .glnode copied into the library and added.
        shared = os.path.join(bu.TMP, "shared")
        os.makedirs(shared, exist_ok=True)
        moved = os.path.join(shared, os.path.basename(path))
        shutil.move(path, moved)
        self.assertEqual(bpy.ops.groomlab.graph_import_node(filepath=moved), {"FINISHED"})
        self.assertTrue(os.path.isfile(path))
        imported = self.tree().nodes.active
        self.assertEqual(imported.gl_type, "Compound")
        self.assertEqual(ga.library_info(imported)["id"], saved["id"])
        bpy.ops.groomlab.graph_delete(node=imported.gl_name)
        bpy.ops.groomlab.graph_ungroup(node=compound)
        self.assertEqual(bpy.ops.groomlab.graph_open_nodes_folder(), {"FINISHED"})  # (no window: only says where)
        # Save as Preset: the user's preset folder.
        self.assertEqual(bpy.ops.groomlab.graph_save_preset(name="my_look", title="My Look"), {"FINISHED"})
        self.assertTrue(os.path.isfile(os.path.join(os.environ["GROOMLAB_USER_PRESETS"], "my_look.json")))
        self.assertIn("my_look", [p["name"] for p in ge.preset_catalog()["presets"]])
        self.assertEqual(bpy.ops.groomlab.graph_save_preset(name="my_look"), {"CANCELLED"})  # exists: Replace
        self.assertEqual(bpy.ops.groomlab.graph_save_preset(name="my_look", overwrite=True), {"FINISHED"})
        settle()
        self.assertEqual(groom.runtime(head).errors, [])

    def test_9_script_nodes(self):
        head = self.head
        tree = self.tree()
        for n in tree.nodes:
            n.select = gt.ours(n) and n.gl_name == "width"
        tree.nodes.active = node_of(tree, "width")
        self.assertEqual(bpy.ops.groomlab.graph_new_script(), {"FINISHED"})
        node = self.tree().nodes.active
        name = node.gl_name
        self.assertEqual(node.gl_type, "Script")
        self.assertIn(("width", name, "strands"), links(head))
        script_name, header_name = ga.script_text_names(name)
        script, header = bpy.data.texts[script_name], bpy.data.texts[header_name]
        self.assertIn("per point", script.as_string())
        self.assertEqual(json.loads(header.as_string())["label"], "My Script")
        log = []
        ge.draw_node_panel(Layout(log), bpy.context, node)
        self.assertIn(("label", "Checks: the node takes each change (live)"), log)
        # An error: shown, the node keeps its script.
        good = script.as_string()
        script.from_string("per point { P += ; }")
        ga.check_texts()
        errors = ga.script_errors_of(name)
        self.assertTrue(errors)
        self.assertEqual(errors[0]["part"], "script")
        self.assertEqual(gnode(head, name)["params"]["definition"]["script"], good)
        log = []
        ge.draw_node_panel(Layout(log), bpy.context, node_of(self.tree(), name))
        self.assertTrue(any(e[0] == "label" and e[1].startswith("script") for e in log))
        # Fixed and relabelled: the node takes it (live).
        script.from_string(good + "\n")
        data = json.loads(header.as_string())
        data["label"] = "Lift"
        header.from_string(json.dumps(data, indent=2))
        ga.check_texts()
        self.assertEqual(ga.script_errors_of(name), [])
        definition = gnode(head, name)["params"]["definition"]
        self.assertEqual((definition["label"], definition["script"]), ("Lift", good + "\n"))
        # Save to Library; Done removes the text blocks.
        self.assertEqual(bpy.ops.groomlab.graph_script_save(node=name), {"FINISHED"})
        self.assertIn("Lift", [e.get("label") for e in graph.library_nodes(refresh=True)])
        self.assertEqual(bpy.ops.groomlab.graph_script_done(node=name), {"FINISHED"})
        self.assertNotIn(script_name, bpy.data.texts)
        settle()
        self.assertEqual(groom.runtime(head).errors, [])


if __name__ == "__main__":
    bu.run(GraphAuthoringTest)
