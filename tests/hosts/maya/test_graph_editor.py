# SPDX-License-Identifier: Apache-2.0
"""Graph API and preset tests (mayapy standalone).

The graph editor widget tests live in editor_cases.py and run in a Maya UI
session (gui_graph_editor.py): standalone Maya has no QApplication.

Usage: mayapy test_graph_editor.py <GroomLab.mll> <maya/scripts dir>
"""

import json
import os
import shutil
import sys
import tempfile
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
USER_PRESETS = tempfile.mkdtemp(prefix="groomlab_presets_")
os.environ["GROOMLAB_USER_PRESETS"] = USER_PRESETS
USER_EXPRESSIONS = tempfile.mkdtemp(prefix="groomlab_expressions_")
os.environ["GROOMLAB_USER_EXPRESSIONS"] = USER_EXPRESSIONS
USER_NODES = tempfile.mkdtemp(prefix="groomlab_nodes_")
os.environ["GROOMLAB_USER_NODES"] = USER_NODES
os.environ.pop("GROOMLAB_NODE_PATH", None)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402


# The default graph before it took a look (Noise, HairColor, uneven Length): the cases that measure
# the shape the guides alone give, or edit that chain by its node names, start from it.
CLASSIC_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
              {"name": "scatter", "type": "Scatter"}, {"name": "interpolate", "type": "Interpolate"},
              {"name": "length", "type": "Length"}, {"name": "width", "type": "Width"},
              {"name": "resample", "type": "Resample"}, {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "interpolate", "input": "strands"},
                    {"from": "guides", "to": "interpolate", "input": "guides"},
                    {"from": "scalp", "to": "interpolate", "input": "scalp"},
                    {"from": "interpolate", "to": "length", "input": "strands"},
                    {"from": "length", "to": "width", "input": "strands"},
                    {"from": "width", "to": "resample", "input": "strands"},
                    {"from": "resample", "to": "output", "input": "strands"}],
}
from groomlab import graph as gm  # noqa: E402
from groomlab import presets  # noqa: E402


class ScriptsTest(unittest.TestCase):
    def test_every_script_compiles_and_the_menu_imports(self):
        # UI modules only run in interactive Maya; a syntax error there would
        # silently lose the GroomLab menu.
        import py_compile
        for folder, _, files in os.walk(os.path.join(SCRIPTS_DIR, "groomlab")):
            for name in files:
                if name.endswith(".py"):
                    py_compile.compile(os.path.join(folder, name), doraise=True)
        import groomlab.ui.menu  # noqa: F401


class GraphApiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)
        cls.catalog = gm.OperatorCatalog.from_maya()

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        self.groom = groomlab.create_groom(self.scalp)
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        cmds.setAttr(self.groom + ".previewDensity", 1.0)

    def test_catalog_lists_every_operator(self):
        for t in ("Scalp", "Guides", "ImportCurves", "Scatter", "Interpolate", "Clump", "Noise", "Scales",
                  "Curl", "HairColor", "Attribute", "Merge", "Switch", "Length", "Width", "Resample", "Output"):
            self.assertIn(t, self.catalog.types())
        # Noise's fraction, HairColor's RGB, Clump's own centers and Bend at the root took their place.
        for gone in ("Frizz", "StrandColor", "AutoClump", "Lift"):
            self.assertNotIn(gone, self.catalog.types())
        strength = self.catalog.param_spec("Clump", "strength")
        self.assertTrue(strength["mappable"])
        self.assertEqual(strength["kind"], "float")
        self.assertEqual(self.catalog.param_spec("Clump", "profile")["kind"], "ramp")
        self.assertEqual(self.catalog.input_spec("Clump", "centers")["type"], "Curves")  # guides or strands

    def test_gpu_drags_send_vectors_as_vectors(self):
        # A dragged value goes to the GPU look preview by its kind: a vec3 param (Curl's
        # weightDirection) as -vector, not as a ramp's keys (which do not parse: the drag stayed on
        # the CPU); the command takes each (standalone Maya draws nothing, so the GPU declines).
        from groomlab.ui import graph_editor as ge
        self.assertEqual(ge.gpu_drag_flags(0.5), {"value": 0.5})
        self.assertEqual(ge.gpu_drag_flags(7), {"value": 7.0})
        self.assertEqual(ge.gpu_drag_flags([1, -1, 0.5]), {"vector": (1.0, -1.0, 0.5)})
        self.assertEqual(ge.gpu_drag_flags((0.0, 0.0, 1.0)), {"vector": (0.0, 0.0, 1.0)})
        keys = [[0.0, 1.0], [0.5, 0.2], [1.0, 0.3]]
        self.assertEqual(ge.gpu_drag_flags(keys), {"ramp": json.dumps(keys)})
        for other in (True, "text", {"map": "density"}):
            self.assertIsNone(ge.gpu_drag_flags(other), other)
        self.assertNotIn("vector", ge.gpu_drag_flags([True, 0.0, 1.0]))  # not three numbers
        model = gm.GraphModel.from_groom(self.groom)
        curl = model.add_node("Curl", params={"weight": 0.8})
        model.insert_after("interpolate", curl)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        for value in ([1.0, -1.0, 0.0], 0.4, keys):
            taken = cmds.groomLabGpuPreview(self.groom, node=curl, param="weightDirection", **ge.gpu_drag_flags(value))
            self.assertFalse(taken)
        cmds.groomLabGpuPreview(self.groom, endDrag=True)

    def test_scatters_length_says_when_guides_set_it(self):
        # Scatter's length beside the Length node confused a tester: the panel says "guides set it"
        # while an Interpolate with guides wired in comes after the Scatter (the default graph).
        from groomlab.ui import graph_editor as ge
        model = gm.GraphModel.from_groom(self.groom)
        self.assertTrue(ge.guides_shape(model, "scatter"))
        model.disconnect("interpolate", "guides")
        self.assertFalse(ge.guides_shape(model, "scatter"))
        self.assertFalse(ge.guides_shape(model, "width"))  # downstream of Interpolate

    def test_new_nodes_and_params_come_from_the_catalog(self):
        # Gravity, Displace and Mirror are Shape nodes, each with its one-line description (the
        # core's, every host's); the new params of Curl, Clump, Noise, Width and Collide are there,
        # choices and all, and a graph using them evaluates on a groom.
        from groomlab.ui import graph_editor as ge
        cats = dict(ge.categories(self.catalog))
        for t, category in (("Gravity", "Deform"), ("Displace", "Deform"), ("Mirror", "Shape"), ("Collide", "Collide"),
                            ("Twist", "Look"), ("Cull", "Grow"), ("ToGuides", "Convert"), ("Cards", "Games")):
            self.assertIn(t, cats[category])  # the core's node catalog: every host lists them so
            self.assertEqual(ge.category_of(t, self.catalog), category)
        self.assertEqual(list(cats)[:5], ["Sources", "Grow", "Shape", "Deform", "Collide"])
        self.assertNotIn("Compound", sum(cats.values(), []))
        self.assertEqual(ge.label_of("Reference", self.catalog), "Linked Groom")  # files keep "Reference"
        self.assertEqual(ge.label_of("BlendNeighbors", self.catalog), "Blend Neighbors")
        for t in self.catalog.types():
            self.assertTrue(ge.describe(t, self.catalog), t)  # every node says what it does
        self.assertEqual(ge.describe("NoSuchNode", self.catalog), "")
        self.assertEqual(self.catalog.param_spec("Curl", "shape")["choices"], ["helix", "wave", "zigzag", "coil"])
        self.assertEqual(self.catalog.param_spec("Curl", "coherence")["choices"], ["strand", "clump"])
        self.assertEqual(self.catalog.param_spec("Noise", "mode")["choices"], ["smooth", "kinky"])
        self.assertEqual(self.catalog.param_spec("Collide", "method")["choices"], ["field", "exact"])
        self.assertEqual(self.catalog.param_spec("Clump", "flatten")["choices"], ["surface", "random", "direction"])
        self.assertEqual(self.catalog.param_spec("Clump", "volumizeProfile")["kind"], "ramp")
        # showWhen by a param, chained: Clump's count with clumpBy count, clumpBy with centersFrom
        # pick; never by an input linked (Merge's blend always).
        count = self.catalog.param_spec("Clump", "count")
        spec_of = lambda name: self.catalog.param_spec("Clump", name)  # noqa: E731
        self.assertTrue(ge.shown_when(count, {"clumpBy": "count", "centersFrom": "pick"}.get, spec_of))
        self.assertFalse(ge.shown_when(count, {"clumpBy": "size", "centersFrom": "pick"}.get, spec_of))
        self.assertFalse(ge.shown_when(count, {"clumpBy": "count", "centersFrom": "inputPoints"}.get, spec_of))
        self.assertTrue(ge.shown_when(count, {"clumpBy": "count", "centersFrom": "inputPoints"}.get))  # unchained
        self.assertEqual(self.catalog.param_spec("Clump", "centersFrom")["choices"], ["pick", "inputPoints", "inputCurves"])
        self.assertIsNone(self.catalog.param_spec("Clump", "shapeFrom"))
        self.assertTrue(ge.shown_when(self.catalog.param_spec("Merge", "blend"), {}.get))
        # A unit by another param (unitWhen): Noise's frequency per scene unit in world space only.
        frequency = self.catalog.param_spec("Noise", "frequency")
        self.assertEqual(ge.unit_of(frequency, {"space": "world"}.get), "perLength")
        self.assertEqual(ge.unit_of(frequency, {"space": "strand"}.get), "")
        self.assertEqual(ge.unit_of(self.catalog.param_spec("Transform", "translate"), {}.get), "length")
        self.assertEqual(self.catalog.param_spec("Width", "randomness")["default"], 0.0)
        self.assertTrue(self.catalog.param_spec("Gravity", "strength")["mappable"])
        self.assertTrue(self.catalog.param_spec("Displace", "distance")["mappable"])
        graph = groomlab.get_graph(self.groom, expand_default=True)
        names = [n["name"] for n in graph["nodes"]]
        into = next(c for c in graph["connections"] if c["to"] == graph["output"])
        graph["nodes"] += [{"name": "gravity", "type": "Gravity", "params": {}},
                           {"name": "kinks", "type": "Noise", "params": {"mode": "kinky", "amplitude": 0.02}},
                           {"name": "mirror", "type": "Mirror"}]
        graph["connections"] = [c for c in graph["connections"] if c is not into] + [
            {"from": into["from"], "to": "gravity", "input": "strands"}, {"from": "gravity", "to": "kinks", "input": "strands"},
            {"from": "kinks", "to": "mirror", "input": "strands"}, {"from": "mirror", "to": graph["output"], "input": "strands"}]
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertTrue(set(names) < {n["name"] for n in groomlab.get_graph(self.groom)["nodes"]})

    def test_motion_nodes_are_listed_and_added_in_one_undo(self):
        from groomlab.ui import graph_editor as ge
        self.assertEqual(dict(ge.categories(self.catalog))["Motion"], ["Wind", "Jiggle", "Simulate"])
        self.assertEqual(ge.category_of("Simulate", self.catalog), "Motion")
        self.assertEqual(ge.category_of("Jiggle", self.catalog), "Motion")
        self.assertTrue(self.catalog.param_spec("Wind", "strength")["mappable"])
        self.assertTrue(self.catalog.param_spec("Jiggle", "strength")["mappable"])
        self.assertEqual(self.catalog.param_spec("Jiggle", "history")["kind"], "int")
        self.assertEqual(self.catalog.param_spec("Wind", "profile")["kind"], "ramp")
        graph = cmds.getAttr(self.groom + ".graph")
        wind = groomlab.add_wind(self.groom, direction=(0, 0, 1), strength=0.2, speed=5.0)
        jiggle = groomlab.add_jiggle(self.groom, strength=0.7, frequency=3.0)
        stored = groomlab.get_graph(self.groom)
        nodes = {n["name"]: n for n in stored["nodes"]}
        self.assertEqual(nodes[wind]["params"]["direction"], [0.0, 0.0, 1.0])
        self.assertEqual(nodes[wind]["params"]["speed"], 5.0)
        self.assertEqual(nodes[jiggle]["params"]["frequency"], 3.0)
        # resample -> wind -> jiggle -> output: the finished look moves, and what comes after sees it.
        self.assertIn({"from": "resample", "to": wind, "input": "strands"}, stored["connections"])
        self.assertIn({"from": wind, "to": jiggle, "input": "strands"}, stored["connections"])
        self.assertIn({"from": jiggle, "to": "output", "input": "strands"}, stored["connections"])
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.undo()
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), graph)

    def test_model_edits_round_trip_through_the_groom(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertEqual(model.validate(), [])
        count = cmds.getAttr(self.groom + ".outStrandCount")
        before = cmds.exactWorldBoundingBox(self.groom)

        model.add_node("Curl", name="curl")
        model.insert_between("length", "width", "curl")
        model.set_param("curl", "radius", 0.08)
        model.save(self.groom)

        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), count)
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)
        stored = json.loads(cmds.getAttr(self.groom + ".graph"))
        self.assertIn({"from": "curl", "to": "width", "input": "strands"}, stored["connections"])
        # The layout lives in the graph (each node's "ui"), as the standalone app keeps it.
        self.assertEqual(next(n for n in stored["nodes"] if n["name"] == "curl")["ui"],
                         {"x": model.position("curl")[0], "y": model.position("curl")[1]})
        self.assertEqual(cmds.getAttr(self.groom + ".graphUi") or "", "")  # the old attribute is left alone

        again = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertEqual(again.param("curl", "radius"), 0.08)
        self.assertEqual(again.position("curl"), model.position("curl"))

    def test_links_read_the_output_they_name(self):
        # A Map node has five outputs; a link picks one ("output", stored only when not the first).
        self.assertEqual(self.catalog.output_names("Map"), ["luminance", "r", "g", "b", "a", "rgb"])
        self.assertEqual(self.catalog.output_names("Clump"), ["out"])
        self.assertEqual(self.catalog.spec("Map")["output"]["name"], "luminance")  # the first, as before
        groomlab.add_map(self.groom, "packed", 1.0, kind="vertices", color=True)
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.add_node("Map", name="mask", params={"name": "packed"})
        self.assertEqual(model.output_names("mask"), ["luminance", "r", "g", "b", "a", "rgb"])
        model.expose_param("length", "keep")
        model.expose_param("scatter", "density")
        model.connect("mask", "length", "keep", output="g")
        model.connect("mask", "scatter", "density", output="luminance")
        self.assertIn({"from": "mask", "to": "length", "input": "keep", "output": "g"}, model.connections)
        self.assertIn({"from": "mask", "to": "scatter", "input": "density"}, model.connections)  # the first
        self.assertEqual(model.output_of("length", "keep"), "g")
        self.assertEqual(model.output_of("scatter", "density"), "luminance")
        self.assertIsNone(model.output_of("width", "width"))
        self.assertEqual(model.output_type("mask", "a"), "Field")
        with self.assertRaises(gm.GraphError):
            model.connect("mask", "width", "strands", output="r")  # a Field into strands
        with self.assertRaises(gm.GraphError):
            model.connect("mask", "length", "keep", output="q")  # no such output
        self.assertEqual(model.output_of("length", "keep"), "g")  # a refused link changes nothing
        self.assertEqual(model.validate(), [])
        # Saved and loaded (and through the plugin's own JSON) the link keeps its output.
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        stored = json.loads(cmds.getAttr(self.groom + ".graph"))
        self.assertIn({"from": "mask", "to": "length", "input": "keep", "output": "g"}, stored["connections"])
        again = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertEqual(again.output_of("length", "keep"), "g")
        self.assertEqual(again.to_dict()["connections"], model.to_dict()["connections"])
        # Copy and paste, rename, insert: the output stays with the link.
        pasted = model.paste_nodes(model.copy_nodes(["mask", "length"]))
        pasted_length = next(n for n in pasted if model.node_type(n) == "Length")
        self.assertEqual(model.source_of(pasted_length, "keep"), next(n for n in pasted if n != pasted_length))
        self.assertEqual(model.output_of(pasted_length, "keep"), "g")
        model.rename_node("mask", "packedMap")
        self.assertEqual(model.output_of("length", "keep"), "g")
        model.add_node("FieldRemap", name="remap")
        model.insert_between("packedMap", "length", "remap")
        self.assertEqual(model.output_of("remap", "in"), "g")
        self.assertEqual(model.source_of("length", "keep"), "remap")
        # Group and ungroup: the compound's input reads the output; ungrouping gives the link back.
        compound = model.collapse(["remap"])
        self.assertEqual(model.output_of(compound, "in"), "g")
        model.expand(compound)
        self.assertEqual(model.output_of("remap", "in"), "g")
        # A group whose node feeds out from another output than its first is refused.
        with self.assertRaises(gm.GraphError):
            model.collapse(["packedMap"])
        # A link naming an output its source lacks is a problem, not a crash.
        model.connections.append({"from": "packedMap", "to": "width", "input": "width", "output": "q"})
        model.expose_param("width", "width")
        self.assertTrue(any("no output 'q'" in p for p in model.validate()))

    def test_use_map_links_the_channel_output(self):
        groomlab.add_map(self.groom, "packed", 1.0, kind="vertices", color=True)
        node = groomlab.use_map(self.groom, "length", "keep", "packed", channel="g")
        graph = groomlab.get_graph(self.groom)
        self.assertIn({"from": node, "to": "length", "input": "keep", "output": "g"}, graph["connections"])
        self.assertNotIn("channel", next(n for n in graph["nodes"] if n["name"] == node)["params"])
        self.assertEqual(groomlab.use_map(self.groom, "width", "width", "packed", channel="r"), node)  # one Map node
        self.assertEqual(groomlab.feed_map_node(self.groom, node, "scatter", "density", output="b"), "scatter.density")
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertEqual([model.output_of(n, p) for n, p in (("length", "keep"), ("width", "width"),
                                                            ("scatter", "density"))], ["g", "r", "b"])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(sorted(groomlab.map_users(self.groom, "packed")),
                         [("length", "keep"), ("scatter", "density"), ("width", "width")])

    def test_layout_changes_touch_only_the_layout(self):
        # A move writes the node's "ui" into the stored graph, nothing else of it (an edit not
        # saved yet stays out). A scene's old graphUi is read while the graph has no layout.
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.save(self.groom)
        model.set_position("scatter", 999, 999)
        model.save_layout(self.groom)
        stored = json.loads(cmds.getAttr(self.groom + ".graph"))
        self.assertEqual(next(n for n in stored["nodes"] if n["name"] == "scatter")["ui"], {"x": 999.0, "y": 999.0})
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), model.to_json())
        self.assertEqual(groomlab.errors(self.groom), [])
        model.set_param("length", "keep", 0.42)  # not saved
        model.set_position("width", 5, 6)
        model.save_layout(self.groom)
        stored = json.loads(cmds.getAttr(self.groom + ".graph"))
        self.assertEqual(next(n for n in stored["nodes"] if n["name"] == "width")["ui"], {"x": 5.0, "y": 6.0})
        self.assertNotEqual(next(n for n in stored["nodes"] if n["name"] == "length")["params"].get("keep"), 0.42)
        # An older scene: no layout in the graph, its graphUi read.
        bare = gm.without_layout(json.loads(cmds.getAttr(self.groom + ".graph")))
        cmds.setAttr(self.groom + ".graph", json.dumps(bare), type="string")
        layout = {n["name"]: [i * 100, 0] for i, n in enumerate(bare["nodes"])}
        layout.update({"scatter": [12, 34], gm.BACKDROPS_KEY: [
            {"title": "old", "x": 0, "y": 0, "w": 10, "h": 10, "color": "blue"}]})
        cmds.setAttr(self.groom + ".graphUi", json.dumps(layout), type="string")
        old = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertEqual(old.position("scatter"), (12.0, 34.0))
        self.assertEqual([b["title"] for b in old.backdrops], ["old"])

    def test_pins_route_links_and_follow_the_graph(self):
        # Pins are layout: written with save_layout, nothing evaluates again; links run through them
        # source first; renames follow; a link gone or from elsewhere takes its route along.
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.save(self.groom)
        a = model.add_pin("width", "strands", 10, 20)
        b = model.add_pin("width", "strands", 30, 40, index=0)
        self.assertEqual(model.route("width", "strands"), [b, a])
        c = model.add_pin("resample", "strands", 50, 60)
        model.save_layout(self.groom)
        stored = json.loads(cmds.getAttr(self.groom + ".graph"))
        self.assertEqual(sorted(p["id"] for p in stored["pins"]["points"]), sorted([a, b, c]))
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), model.to_json())
        again = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertEqual(again.route("width", "strands"), [b, a])
        self.assertEqual(again.pin_position(a), (10.0, 20.0))
        self.assertNotIn("pins", gm.without_layout(stored))
        # Joining: only pins of one signal (width.strands and resample.strands carry other links).
        self.assertFalse(model.merge_pins(a, c))
        self.assertTrue(model.merge_pins(a, b))
        self.assertEqual(model.route("width", "strands"), [a])
        # A rename follows; a removed pin straightens its link; a link gone drops its route and pins.
        model.rename_node("width", "thick")
        self.assertEqual(model.route("thick", "strands"), [a])
        model.remove_pin(a)
        self.assertEqual(model.route("thick", "strands"), [])
        model.disconnect("resample", "strands")
        model.prune_pins()
        self.assertEqual(model.pins, {})
        self.assertNotIn("pins", model.to_dict())
        # A link from another source now: its old route goes.
        d = model.add_pin("thick", "strands", 0, 0)
        model.connect("interpolate", "thick", "strands")
        self.assertEqual(model.route("thick", "strands"), [])
        model.prune_pins()
        self.assertNotIn(d, model.pins)
        # What a link drawn out of a pin runs through, and a route set by hand.
        e = model.add_pin("thick", "strands", 1, 1)
        f = model.add_pin("thick", "strands", 2, 2)
        self.assertEqual(model.route_to(e), [e])
        self.assertEqual(model.route_to(f), [e, f])
        self.assertEqual(model.pin_links(f), [(("thick", "strands"), 1)])
        model.connect("interpolate", "resample", "strands")
        model.set_route("resample", "strands", model.route_to(e))
        self.assertEqual(model.route("resample", "strands"), [e])
        self.assertEqual(model.pin_links(e), [(("resample", "strands"), 0), (("thick", "strands"), 0)])

    def test_model_rejects_bad_edits(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        with self.assertRaises(gm.GraphError):
            model.connect("scalp", "length", "strands")  # Mesh into Strands
        with self.assertRaises(gm.GraphError):
            model.connect("width", "scatter", "scalp")  # type mismatch too
        model.add_node("Noise", name="n1")
        model.add_node("Noise", name="n2")
        model.connect("n1", "n2", "strands")
        with self.assertRaises(gm.GraphError):
            model.connect("n2", "n1", "strands")  # cycle
        with self.assertRaises(gm.GraphError):
            model.set_param("n1", "nope", 1)
        with self.assertRaises(gm.GraphError):
            model.add_node("Bogus")

        model.remove_node("output")
        problems = model.validate()
        self.assertIn("no output node", problems)
        self.assertIn("n1.strands is not connected", model.unused_problems())  # nothing feeds an output now
        with self.assertRaises(gm.GraphError):
            model.save(self.groom)

    def test_a_node_off_to_the_side_never_stops_the_graph(self):
        # A Clump whose output was unplugged (and bypassed) never evaluates: its problems are
        # warnings, and the graph still applies, so the viewport shows what feeds the output.
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.add_node("Clump", name="side")
        model.connect("interpolate", "side", "strands")
        model.set_bypass("side", True)
        self.assertEqual(model.validate(), [])
        model.disconnect("side", "strands")  # and unfed too
        self.assertEqual(model.validate(), [])
        self.assertIn("side.strands is not connected", model.unused_problems())
        self.assertNotIn("side", model.feeding_output())
        model.save(self.groom)  # applies
        # A node the output is made from, unfed: that still stops it.
        model.disconnect("length", "strands")
        self.assertIn("length.strands is not connected", model.validate())

    def test_rename_keeps_connections_and_output(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.rename_node("output", "final")
        self.assertEqual(model.output, "final")
        self.assertEqual(model.source_of("final", "strands"), "resample")
        model.save(self.groom)
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)

    def test_compounds_collapse_expose_and_expand(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.add_node("Noise", name="noise", params={"amplitude": 0.04})
        model.insert_between("length", "width", "noise")
        model.add_node("Curl", name="curl")
        model.insert_between("noise", "width", "curl")
        model.save(self.groom)
        inline = cmds.exactWorldBoundingBox(self.groom)

        name = model.collapse(["noise", "curl"])
        self.assertEqual(model.node_type(name), "Compound")
        self.assertEqual([p["name"] for p in model.node_spec(name)["inputs"]], ["strands"])
        self.assertEqual(model.source_of(name, "strands"), "length")
        self.assertEqual(model.source_of("width", "strands"), name)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), inline)  # same result

        model.expose(name, "amount", "noise.amplitude")
        self.assertEqual(model.param(name, "amount"), 0.04)
        model.set_param(name, "amount", 0.2)
        model.save(self.groom)
        louder = cmds.exactWorldBoundingBox(self.groom)
        self.assertNotEqual(louder, inline)
        with self.assertRaises(gm.GraphError):
            model.expose(name, "x", "noise.nope")

        # Stored as a readable object, and it survives a reload.
        stored = json.loads(cmds.getAttr(self.groom + ".graph"))
        node = next(n for n in stored["nodes"] if n["name"] == name)
        self.assertIsInstance(node["params"]["definition"], dict)
        again = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertEqual(again.param(name, "amount"), 0.2)

        inner = again.expand(name)
        self.assertEqual(sorted(again.node_type(n) for n in inner), ["Curl", "Noise"])
        self.assertEqual(again.validate(), [])
        again.save(self.groom)
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), louder)  # exposed value kept

        with self.assertRaises(gm.GraphError):
            again.collapse(["scatter", "guides"])  # two nodes feed the rest

    def test_group_and_ungroup_keep_every_node_field(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.add_node("Noise", name="noise", params={"amplitude": 0.04})
        model.insert_between("length", "width", "noise")
        model.add_node("Curl", name="curl", params={"radius": 0.05})
        model.insert_between("noise", "width", "curl")
        model.expose_param("noise", "amplitude")
        field = model.add_node("FieldNoise", name="wobble", params={"frequency": 3.0, "min": 0.02, "max": 0.06})
        model.connect(field, "noise", "amplitude")
        model.set_bypass("curl", True)
        model.set_position("curl", 555.0, 666.0)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        loose = cmds.exactWorldBoundingBox(self.groom)

        name = model.collapse(["noise", "curl", field])
        inner = {n["name"]: n for n in model.param(name, "definition")["graph"]["nodes"]}
        self.assertEqual(inner["noise"]["paramInputs"], ["amplitude"])  # the exposed param
        self.assertTrue(inner["curl"]["bypass"])                          # the disabled node
        self.assertEqual(inner["curl"]["ui"], {"x": 555.0, "y": 666.0})  # and where it was
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), loose)  # the same groom, grouped

        model.expand(name)
        self.assertEqual(model.param_inputs("noise"), ["amplitude"])
        self.assertEqual(model.driver_of("noise", "amplitude"), "wobble")
        self.assertTrue(model.bypassed("curl"))
        self.assertEqual(model.position("curl"), (555.0, 666.0))
        self.assertEqual(model.param("curl", "radius"), 0.05)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), loose)

    def test_keyed_params_link_in_the_model_and_travel_into_compounds(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertIsNone(model.keyable("length", "keep"))
        self.assertIn("strands", model.keyable("scatter", "density"))   # the strand count
        self.assertIsNotNone(model.keyable("resample", "segments"))      # points per strand
        self.assertIsNotNone(model.keyable("scatter", "seed"))           # the layout
        self.assertIsNotNone(model.keyable("guides", "group"))           # text
        with self.assertRaises(gm.GraphError):
            model.link_control("scatter", "density", "d")
        with self.assertRaises(gm.GraphError):
            model.link_control("length", "keep", ["a", "b", "c"])     # a number takes one control
        model.link_control("length", "keep", "grow")
        model.save(self.groom)
        self.assertTrue(cmds.attributeQuery("ctl_grow", node=self.groom, exists=True))
        self.assertEqual(groomlab.errors(self.groom), [])
        # Grouped, the link goes inside with its node; the groom still reads the attribute.
        name = model.collapse(["length", "width"])
        inner = {n["name"]: n for n in model.param(name, "definition")["graph"]["nodes"]}
        self.assertEqual(inner["length"]["paramControls"], {"keep": "grow"})
        model.save(self.groom)
        self.assertTrue(cmds.attributeQuery("ctl_grow", node=self.groom, exists=True))
        self.assertEqual(groomlab.errors(self.groom), [])
        before = cmds.exactWorldBoundingBox(self.groom)
        cmds.setAttr(self.groom + ".ctl_grow", 0.3)
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)
        model.expand(name)
        self.assertEqual(model.control_of("length", "keep"), "grow")
        model.unlink_control("length", "keep")
        self.assertNotIn("paramControls", model.to_dict()["nodes"][[n["name"] for n in model.to_dict()["nodes"]]
                                                                  .index("length")])
        model.save(self.groom)
        self.assertFalse(cmds.attributeQuery("ctl_grow", node=self.groom, exists=True))

    def test_ungroup_that_fails_leaves_the_graph_alone(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.add_node("Noise", name="noise")
        model.insert_between("length", "width", "noise")
        name = model.collapse(["noise"])
        # A definition from another build: an operator this one has not got.
        model.nodes[name]["params"]["definition"]["graph"]["nodes"][0]["type"] = "Bogus"
        before = model.to_dict()
        with self.assertRaises(gm.GraphError):
            model.expand(name)
        self.assertEqual(model.to_dict(), before)

    def test_unknown_node_fields_survive_a_load_and_save(self):
        graph = groomlab.get_graph(self.groom, expand_default=True)
        graph["nodes"][0]["extra"] = {"written": "by a newer plugin"}
        groomlab.set_graph(self.groom, graph)
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.set_param("length", "keep", 0.3)  # some edit, then a save
        model.save(self.groom)
        stored = groomlab.get_graph(self.groom)
        self.assertEqual(stored["nodes"][0]["extra"], {"written": "by a newer plugin"})
        self.assertTrue(all("ui" in n for n in stored["nodes"]))  # the layout travels with the graph

    def test_the_view_node_follows_renames_and_removals(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.add_node("Curl", name="curl")
        model.insert_between("length", "width", "curl")
        model.save(self.groom)
        cmds.setAttr(self.groom + ".viewNode", "curl", type="string")
        model.rename_node("curl", "twirl")
        model.save(self.groom)
        self.assertEqual(cmds.getAttr(self.groom + ".viewNode"), "twirl")
        model.remove_node("twirl")
        model.connect("length", "width", "strands")
        model.save(self.groom)
        self.assertEqual(cmds.getAttr(self.groom + ".viewNode"), "")

    def test_a_map_without_a_name_is_flagged(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.set_param("length", "keep", {"texture": "", "component": "r", "min": 0.0, "max": 1.0})
        self.assertIn("length.keep: the texture has no name", model.validate())
        model.set_param("length", "keep", groomlab.guide_map("thick"))
        self.assertEqual(model.validate(), [])

    def test_new_seeded_nodes_get_seeds_of_their_own(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        scatters = [n for n in model.order if model.node_type(n) == "Scatter"]
        self.assertTrue(scatters)
        self.assertEqual(model.warnings(), [])
        # A second Scatter (a new look) must not make the same strands as the first.
        taken = {model.param(n, "seed") for n in scatters}
        second = model.add_node("Scatter")
        self.assertNotIn(model.param(second, "seed"), taken)
        # The first Flyaway keeps the default seed; the next ones get new seeds.
        flyaways = [model.add_node("Flyaway") for _ in range(3)]
        self.assertEqual(model.nodes[flyaways[0]]["params"]["seed"], 0)
        self.assertEqual(len({model.param(n, "seed") for n in flyaways}), 3)
        # A seed given is kept; other types' seeds do not count.
        self.assertEqual(model.param(model.add_node("Instance", params={"seed": 5}), "seed"), 5)
        self.assertEqual(model.param(model.add_node("Instance"), "seed"), 0)  # 5 is taken, the default is free
        self.assertEqual(model.warnings(), [])

        # Shared seeds are a warning, never a validation problem (the groom still saves).
        model.set_param(second, "seed", model.param(scatters[0], "seed"))
        model.set_param(flyaways[2], "seed", model.param(flyaways[0], "seed"))
        warnings = model.warnings()
        self.assertEqual(len(warnings), 2, warnings)
        self.assertTrue(any(scatters[0] in w and second in w and "same roots and IDs" in w for w in warnings), warnings)
        self.assertTrue(any(flyaways[0] in w and flyaways[2] in w for w in warnings), warnings)
        self.assertFalse([p for p in model.validate() if "seed" in p])  # (the new nodes are unconnected)

        # groomlab.add_instances twice: the second picks other strands.
        cube = cmds.polyCube()[0]
        first = groomlab.add_instances(self.groom, cube)
        again = groomlab.add_instances(self.groom, cube)
        saved = gm.GraphModel.from_groom(self.groom, self.catalog)
        self.assertNotEqual(saved.param(first, "seed"), saved.param(again, "seed"))

    def test_helpers_that_write_the_graph_are_one_undo_step(self):
        # add_control: a Control node, its ctl_ attribute and the graph, back in one undo.
        graph = cmds.getAttr(self.groom + ".graph")
        attr = groomlab.add_control(self.groom, "length", "keep")
        self.assertTrue(cmds.objExists(attr))
        self.assertNotEqual(cmds.getAttr(self.groom + ".graph"), graph)
        cmds.undo()
        self.assertFalse(cmds.objExists(attr))
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), graph)
        cmds.redo()
        self.assertTrue(cmds.objExists(attr))
        cmds.undo()

        # set_graph: the graph and the attributes its Control nodes make, back in one undo.
        with_control = groomlab.get_graph(self.groom, expand_default=True)
        with_control["nodes"].append({"name": "amount", "type": "Control",
                                      "params": {"name": "amount", "default": 0.5, "min": 0.0, "max": 1.0}})
        graph = cmds.getAttr(self.groom + ".graph")
        groomlab.set_graph(self.groom, with_control)
        self.assertTrue(cmds.attributeQuery("ctl_amount", node=self.groom, exists=True))
        cmds.undo()
        self.assertFalse(cmds.attributeQuery("ctl_amount", node=self.groom, exists=True))
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), graph)

    def test_undo_restores_the_graph_the_editor_reloads(self):
        # An editor edit is model edits then save() (GraphEditor.apply): one undo step. After
        # cmds.undo() the editor reloads through load_model, which must show the restored graph.
        from groomlab.ui import graph_editor
        before = groomlab.get_graph(self.groom, expand_default=True)
        model = graph_editor.load_model(self.groom, self.catalog)
        restored_json = model.to_json(layout=False)
        model.add_node("Curl", name="curl")
        model.insert_between("length", "width", "curl")
        model.save(self.groom)
        self.assertIn("curl", [n["name"] for n in groomlab.get_graph(self.groom)["nodes"]])
        edited_json = graph_editor.load_model(self.groom, self.catalog).to_json(layout=False)

        cmds.undo()
        self.assertEqual(groomlab.get_graph(self.groom, expand_default=True), before)
        again = graph_editor.load_model(self.groom, self.catalog)
        self.assertNotIn("curl", again.order)
        self.assertEqual(again.to_json(layout=False), restored_json)
        self.assertEqual(again.validate(), [])
        self.assertTrue(all("ui" in again.nodes[n] for n in again.order))  # laid out, ready to draw

        cmds.redo()
        self.assertEqual(graph_editor.load_model(self.groom, self.catalog).to_json(layout=False), edited_json)
        # The editor's key tests: Ctrl+Z is undo, Ctrl+Y and Ctrl+Shift+Z redo, and nothing else is.
        from PySide2 import QtCore, QtGui
        ctrl, shift = QtCore.Qt.ControlModifier, QtCore.Qt.ShiftModifier

        def key(k, mods):
            return QtGui.QKeyEvent(QtCore.QEvent.KeyPress, k, mods)

        self.assertTrue(graph_editor.undo_key(key(QtCore.Qt.Key_Z, ctrl)))
        self.assertTrue(graph_editor.undo_key(key(QtCore.Qt.Key_Z, ctrl | QtCore.Qt.KeypadModifier)))
        self.assertFalse(graph_editor.undo_key(key(QtCore.Qt.Key_Z, ctrl | shift)))
        self.assertFalse(graph_editor.undo_key(key(QtCore.Qt.Key_Z, QtCore.Qt.NoModifier)))
        self.assertTrue(graph_editor.redo_key(key(QtCore.Qt.Key_Y, ctrl)))
        self.assertTrue(graph_editor.redo_key(key(QtCore.Qt.Key_Z, ctrl | shift)))
        self.assertFalse(graph_editor.redo_key(key(QtCore.Qt.Key_Y, shift)))
        self.assertFalse(graph_editor.redo_key(key(QtCore.Qt.Key_Z, ctrl)))

    def test_exposed_params_driven_by_field_nodes(self):
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        before = cmds.exactWorldBoundingBox(self.groom)
        model.expose_param("length", "keep")
        self.assertEqual(model.input_spec("length", "keep")["type"], "Field")
        noise = model.add_node("FieldNoise", params={"frequency": 3.0, "min": 0.1, "max": 0.3})
        model.connect(noise, "length", "keep")
        self.assertEqual(model.driver_of("length", "keep"), noise)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        stored = groomlab.get_graph(self.groom)
        self.assertEqual([n for n in stored["nodes"] if n["name"] == "length"][0]["paramInputs"], ["keep"])
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)  # shorter strands
        with self.assertRaises(gm.GraphError):
            model.connect("scatter", "length", "keep")  # strands do not fit a field port
        # Not mappable: one value for the whole node (a Value port), which a field cannot feed.
        model.expose_param("length", "trimMin")
        self.assertEqual(model.input_spec("length", "trimMin")["type"], "Value")
        self.assertEqual(model.input_spec("length", "trimMin")["valueKind"], "number")
        with self.assertRaises(gm.GraphError):
            model.connect(noise, "length", "trimMin")
        with self.assertRaises(gm.GraphError):
            model.expose_param("length", "nope")
        model.unexpose_param("length", "trimMin")
        model.unexpose_param("length", "keep")
        self.assertIsNone(model.source_of("length", "keep"))
        model.save(self.groom)
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), before)

    def test_value_nodes_drive_any_param(self):
        # Every param of every node can be an input, but the few that set a node's own ports.
        ports = {"Compound.definition", "Script.definition", "Expression.output", "Input.name", "Input.type", "Value.kind"}
        for op_type in self.catalog.types():
            for spec in self.catalog.spec(op_type)["params"]:
                self.assertEqual(gm.linkable(spec), "%s.%s" % (op_type, spec["name"]) not in ports,
                                 "%s.%s" % (op_type, spec["name"]))
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        before = cmds.exactWorldBoundingBox(self.groom)
        # A Value node sets Scatter's seed (an integer, the same on every frame) for the whole node.
        model.expose_param("scatter", "seed")
        self.assertEqual(model.input_spec("scatter", "seed")["type"], "Value")
        value = model.add_node("Value", params={"kind": "integer", "integer": 11})
        self.assertEqual(model.output_spec(value)["type"], "Value")
        self.assertEqual(model.output_spec(value)["valueKind"], "integer")
        model.connect(value, "scatter", "seed")
        self.assertEqual(model.driver_of("scatter", "seed"), value)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)  # other roots
        # Saved, the seed keeps its own value; the link is in the graph.
        stored = groomlab.get_graph(self.groom)
        self.assertEqual([n for n in stored["nodes"] if n["name"] == "scatter"][0]["params"]["seed"], 0)
        self.assertIn({"from": value, "to": "scatter", "input": "seed"}, stored["connections"])
        # A vector cannot set an integer; an Expression set to give a value of the frame cannot set a
        # seed (it must stay the same on every frame).
        vector = model.add_node("Value", params={"kind": "vector"})
        with self.assertRaises(gm.GraphError):
            model.connect(vector, "scatter", "seed")
        frame = model.add_node("Expression", params={"output": "value", "expression": "frame"})
        model.connect(frame, "scatter", "seed")
        model.save(self.groom)
        cmds.exactWorldBoundingBox(self.groom)  # evaluated
        self.assertTrue(any("changes over time" in e for e in groomlab.errors(self.groom)), groomlab.errors(self.groom))
        model.unexpose_param("scatter", "seed")
        for name in (value, vector, frame):
            model.remove_node(name)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), before)

    def test_view_node_stats_bypass_copy_paste(self):
        groomlab.set_param(self.groom, "length", "keep", 0.3)  # the output differs from Scatter's strands
        full = cmds.exactWorldBoundingBox(self.groom)
        cmds.setAttr(self.groom + ".viewNode", "scatter", type="string")  # straight, unshaped strands
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), full)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["view"], "scatter")
        cmds.setAttr(self.groom + ".viewNode", "", type="string")
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), full)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertIn("interpolate", stats["times"])

        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.set_bypass("length", True)
        model.save(self.groom)
        self.assertTrue([n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == "length"][0]["bypass"])
        model.set_bypass("length", False)
        model.save(self.groom)
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), full)

        # When it runs: the viewport (the shape) skips a render-only node, as if bypassed; a
        # viewport-only one runs there. Saved with the graph; always is left out.
        self.assertEqual(model.evaluation("length"), "always")
        model.set_evaluation("length", "renderOnly")
        model.save(self.groom)
        self.assertEqual([n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == "length"][0]["evaluation"],
                         "renderOnly")
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), full)
        model.set_evaluation("length", "viewportOnly")
        model.save(self.groom)
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), full)
        model.set_evaluation("length", "always")
        self.assertNotIn("evaluation", gm.node_fields(model.nodes["length"]))
        with self.assertRaises(gm.GraphError):
            model.set_evaluation("length", "sometimes")
        model.save(self.groom)

        text = model.copy_nodes(["length", "width"])
        pasted = model.paste_nodes(text)
        self.assertEqual(len(pasted), 2)
        self.assertEqual(model.source_of(pasted[1], "strands"), pasted[0])  # the link between them came along
        with self.assertRaises(gm.GraphError):
            model.paste_nodes("{}")

    def test_expression_maps(self):
        groomlab.set_param(self.groom, "length", "keep", groomlab.expression_map("y > 0 ? 1 : 0.3", max=1.0))
        self.assertEqual(groomlab.errors(self.groom), [])
        box = cmds.exactWorldBoundingBox(self.groom)
        self.assertLess(-box[1], box[4] - 0.3)  # shorter underneath
        groomlab.set_param(self.groom, "length", "keep", groomlab.expression_map("y >"))
        self.assertTrue(any("position" in e for e in groomlab.errors(self.groom)), groomlab.errors(self.groom))

    def test_presets(self):
        cmds.setAttr(self.groom + ".previewDensity", 0.05)  # presets are made on a head: dense on this sphere
        self.assertIn("creature_fur", presets.list_presets())
        self.assertIn("curly", presets.list_presets())
        presets.apply_preset(self.groom, "curly")
        self.assertIn("Curl", [n["type"] for n in groomlab.get_graph(self.groom)["nodes"]])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        # Clumps in levels: big locks, then finer clumps inside them, each sized in scene units.
        presets.apply_preset(self.groom, "cinematic_hair")
        types = [n["type"] for n in groomlab.get_graph(self.groom)["nodes"]]
        self.assertEqual(types.count("Clump"), 2)
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        self.assertEqual(groomlab.errors(self.groom), [])
        # Every shipped preset evaluating is PresetsTest's.

        path = presets.save_preset(self.groom, "mine")
        self.assertTrue(path.startswith(USER_PRESETS))
        self.assertIn("mine", presets.list_presets())
        saved = presets.read_preset("mine")
        self.assertGreater(saved.pop("size"), 0.0)  # the scalp size it was made on, for fitting
        # The layout comes along (the preset's, which apply_preset stored in the graph).
        for node in saved["nodes"]:
            self.assertIn("ui", node, node["name"])
        self.assertEqual(saved, groomlab.get_graph(self.groom))
        self.assertNotIn("about", saved)  # the shipped preset's description is not the saved one's
        path = presets.save_preset(self.groom, "described", about={"title": "Described", "category": "Hair",
                                                                   "description": "Mine, with a line."})
        info = presets.preset_info("described")
        self.assertEqual((info["title"], info["category"], info["description"]),
                         ("Described", "Hair", "Mine, with a line."))
        self.assertEqual(presets.preset_info("mine")["category"], "")  # none: the menu's top level
        self.assertEqual(presets.preset_info("mine")["title"], "Mine")
        with self.assertRaises(ValueError):
            presets.save_preset(self.groom, "mine")  # exists
        with self.assertRaises(ValueError):
            presets.save_preset(self.groom, "bad/name")
        with self.assertRaises(ValueError):
            presets.read_preset("missing")

    def test_a_fitted_preset_scales_its_maps(self):
        catalog = gm.OperatorCatalog.from_maya()
        op, param = next((t, p["name"]) for t in sorted(catalog.types()) for p in catalog.spec(t)["params"]
                         if p.get("unit") == "length")
        graph = {"size": 1.0, "nodes": [{"name": "n", "type": op, "params": {param: {"texture": "a.png"}}}]}
        fitted = presets.fitted(graph, 2.0, catalog)
        self.assertNotIn("size", fitted)
        # A map without a range: its default max (1) scales too.
        self.assertEqual(fitted["nodes"][0]["params"][param], {"texture": "a.png", "max": 2.0})
        graph["nodes"][0]["params"][param] = {"texture": "a.png", "min": 0.25, "max": 1.5}
        fitted = presets.fitted(graph, 2.0, catalog)
        self.assertEqual(fitted["nodes"][0]["params"][param], {"texture": "a.png", "min": 0.5, "max": 3.0})

    def test_a_fitted_preset_scales_inside_compounds(self):
        # A feather design is a Compound: its inner nodes' lengths and its exposed params scale too.
        catalog = gm.OperatorCatalog.from_maya()
        design = presets.read_preset("feathers_contour")
        self.assertEqual(design["size"], 2.0)
        fitted = presets.fitted(design, 20.0, catalog)
        node = next(n for n in fitted["nodes"] if n["type"] == "Compound")
        original = next(n for n in design["nodes"] if n["type"] == "Compound")
        self.assertAlmostEqual(node["params"]["vaneWidth"], original["params"]["vaneWidth"] * 10.0)
        self.assertAlmostEqual(node["params"]["barbWidth"], original["params"]["barbWidth"] * 10.0)
        self.assertEqual(node["params"]["count"], original["params"]["count"])  # no unit
        inner = {n["name"]: n for n in node["params"]["definition"]["graph"]["nodes"]}
        was = {n["name"]: n for n in original["params"]["definition"]["graph"]["nodes"]}
        self.assertAlmostEqual(inner["propagate"]["params"]["length"], was["propagate"]["params"]["length"] * 10.0)
        self.assertAlmostEqual(inner["ruffle"]["params"]["amplitude"], was["ruffle"]["params"]["amplitude"] * 10.0)
        self.assertEqual(inner["propagate"]["params"]["angle"], was["propagate"]["params"]["angle"])
        shafts = next(n for n in fitted["nodes"] if n["type"] == "Scatter")
        self.assertAlmostEqual(shafts["params"]["density"], 14.0 / 100.0)

    def test_libraries_are_found_without_a_user_folder(self):
        # Where Maya cannot name the user's folder (a mayapy not initialized), the rest still counts.
        def no_user_folder():
            raise AttributeError("maya.cmds has no internalVar yet")
        extra = tempfile.mkdtemp(prefix="groomlab_library_")
        os.environ["GROOMLAB_TEST_LIBRARY_PATH"] = extra
        try:
            self.assertEqual(groomlab.library_dirs("GROOMLAB_TEST_LIBRARY_PATH", no_user_folder, presets.SHIPPED_DIR),
                             [extra, presets.SHIPPED_DIR])
        finally:
            del os.environ["GROOMLAB_TEST_LIBRARY_PATH"]
        self.assertEqual(presets.search_dirs()[-2:], [USER_PRESETS, presets.SHIPPED_DIR])


class MeshPickTest(unittest.TestCase):
    """The eyedroppers of a groom's mesh inputs (groomlab.ui.pick), as the Attribute Editor and the
    Graph Editor use them. Standalone Maya runs no scriptJobs: each test calls selection_changed
    where the SelectionChanged job would (editor_cases.py's MeshPickGuiTest runs the real one)."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        from groomlab.ui import pick
        self.pick = pick
        cmds.file(new=True, force=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10, name="head")[0]
        self.groom = groomlab.create_groom(self.scalp)
        self.ball = cmds.polySphere(radius=0.3, name="ball")[0]
        self.ended = []
        cmds.select(self.groom)

    def tearDown(self):
        self.pick.cancel()

    def colliders(self):
        return [(c["name"], c["mesh"].split("|")[-1]) for c in groomlab.colliders(self.groom)]

    def arm(self, kind, name=None):
        result = self.pick.pick_input(self.groom, kind, name, on_end=self.ended.append, ask=False)
        self.assertEqual(result, "armed")
        self.assertIsNotNone(self.pick.armed(self.groom, self.pick.input_key(kind, name)))

    def test_the_next_mesh_selected_is_applied_and_the_groom_selected_again(self):
        self.arm("collider")
        cmds.select(self.ball)
        self.assertEqual(self.pick.selection_changed(), "applied")
        self.assertEqual(self.colliders(), [("ball", "ballShape")])
        self.assertIsNone(self.pick.armed())
        self.assertTrue(self.pick.groom_selected(self.groom))
        self.assertEqual(self.ended, [True])
        # The scalp, and a mesh that is no collider yet but another groom's input, work the same way.
        other = cmds.polySphere(name="otherHead")[0]
        self.arm("scalp")
        cmds.select(other)
        self.assertEqual(self.pick.selection_changed(), "applied")
        self.assertEqual(groomlab.scalp_mesh(self.groom).split("|")[-1], "otherHeadShape")

    def test_a_mesh_selected_with_the_groom_is_used_at_once(self):
        cmds.select(self.groom, self.ball)
        self.assertEqual(self.pick.pick_input(self.groom, "collider", on_end=self.ended.append), "applied")
        self.assertEqual(self.colliders(), [("ball", "ballShape")])
        self.assertTrue(self.pick.groom_selected(self.groom))
        self.assertIsNone(self.pick.armed())
        # A mesh selected without the groom (the Graph Editor shows whatever is selected) arms instead.
        hat = cmds.polyCube(name="hat")[0]
        cmds.select(hat)
        self.assertEqual(self.pick.pick_input(self.groom, "collider"), "armed")

    def test_invalid_picks_cancel_and_select_the_groom_again(self):
        groomlab.add_collider(self.groom, self.ball, wire=False)
        locator = cmds.spaceLocator()[0]
        for pick_what in (self.scalp, self.ball, locator, None):  # the scalp, a collider already, no mesh, nothing
            self.arm("collider")
            if pick_what:
                cmds.select(pick_what)
            else:
                cmds.select(clear=True)
            self.assertEqual(self.pick.selection_changed(), "cancelled", pick_what)
            self.assertIsNone(self.pick.armed())
            self.assertTrue(self.pick.groom_selected(self.groom), pick_what)
        self.assertEqual(self.colliders(), [("ball", "ballShape")])
        self.assertEqual(self.ended, [False] * 4)
        # Rest meshes have the scalp's vertices; the scalp's undeformed copy does.
        self.arm("rest")
        cmds.select(self.ball)
        self.assertEqual(self.pick.selection_changed(), "cancelled")
        self.assertIsNone(groomlab.rest_mesh(self.groom))
        copy = cmds.duplicate(self.scalp, name="headRest")[0]
        self.arm("rest")
        cmds.select(copy)
        self.assertEqual(self.pick.selection_changed(), "applied")
        self.assertEqual(groomlab.rest_mesh(self.groom).split("|")[-1], "headRestShape")

    def test_pressing_again_cancels_and_leaves_the_selection(self):
        self.arm("collider")
        self.assertEqual(self.pick.pick_input(self.groom, "collider", on_end=self.ended.append), "cancelled")
        self.assertIsNone(self.pick.armed())
        self.assertEqual(self.ended, [False])
        self.arm("collider")
        self.arm("scalp")  # another input's eyedropper: the first pick ends
        self.assertEqual(self.ended, [False, False])
        self.assertIsNone(self.pick.armed(self.groom, "collider"))
        cmds.select(self.ball)
        self.assertTrue(self.pick.cancel())  # Esc
        self.assertEqual(self.pick.selection_changed(), "cancelled")
        self.assertEqual(cmds.ls(selection=True), [self.ball])  # left as it was
        self.assertEqual(groomlab.scalp_mesh(self.groom).split("|")[-1], "headShape")

    def test_a_collider_node_gets_its_mesh_and_the_graph_stays(self):
        # A ColliderMesh node added in the Graph Editor: picking its mesh wires colliderMeshes, nothing else.
        model = gm.GraphModel.from_groom(self.groom)
        model.add_node("ColliderMesh", name="hat", params={"name": "hat"})
        model.save(self.groom)
        before = cmds.getAttr(self.groom + ".graph")
        self.arm("collider", "hat")
        cmds.select(self.ball)
        self.assertEqual(self.pick.selection_changed(), "applied")
        self.assertEqual(self.colliders(), [("hat", "ballShape")])
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), before)
        # Another mesh in its place (the node's mesh menu): swapped, still one collider.
        cube = cmds.polyCube(name="box")[0]
        self.assertIn("box", self.pick.set_input(self.groom, "collider", cube, name="hat"))
        self.assertEqual(self.colliders(), [("hat", "boxShape")])
        # The menu lists the meshes it takes: not the scalp, nor another collider's mesh.
        groomlab.add_collider(self.groom, self.ball, wire=False)
        takes = [m.split("|")[-1] for m in self.pick.scene_meshes() if not self.pick.problem(self.groom, "collider", m, "hat")]
        self.assertEqual(takes, ["boxShape"])
        with self.assertRaises(ValueError):
            self.pick.set_input(self.groom, "collider", self.scalp, name="hat")


class PositionPickTest(unittest.TestCase):
    """Pick beside a world position param (a Transform's pivot: groomlab.ui.pick.selected_position and
    pick_position, what the Graph Editor's Pick button calls): the selection's position, in world
    space and centimeters as groom data is."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        from groomlab.ui import pick
        self.pick = pick
        cmds.file(new=True, force=True)
        self.scalp = cmds.polySphere(radius=10, subdivisionsX=20, subdivisionsY=10, name="head")[0]
        self.groom = groomlab.create_groom(self.scalp)
        model = gm.GraphModel.from_groom(self.groom)
        model.add_node("Transform", name="tie")
        model.set_param("tie", "space", "world")
        model.save(self.groom)

    def tearDown(self):
        cmds.currentUnit(linear="cm")

    def center(self):
        return gm.GraphModel.from_groom(self.groom).param("tie", "pivot")

    def assertClose(self, a, b):
        self.assertEqual(len(a), 3)
        for x, y in zip(a, b):
            self.assertAlmostEqual(x, y, places=4)

    def test_pick_sets_pivot_from_a_locator(self):
        # What gives a param its Pick in the Graph Editor: a world position (a world vec3 of length).
        model = gm.GraphModel.from_groom(self.groom)
        self.assertTrue(model.param_spec("tie", "pivot").get("worldDirection"))
        self.assertEqual(model.param_spec("tie", "pivot").get("unit"), "length")
        self.assertFalse(model.param_spec("tie", "translate").get("worldDirection"))
        locator = cmds.spaceLocator(name="tieHere")[0]
        cmds.xform(locator, worldSpace=True, translation=(1.5, 2.0, -12.0))
        cmds.select(locator)
        message = self.pick.pick_position(self.groom, "tie", "pivot")
        self.assertIn("tieHere", message)
        self.assertClose(self.center(), (1.5, 2.0, -12.0))
        # The groom selected too (the Graph Editor shows the selected groom) is passed over.
        cmds.select(self.groom, locator)
        cmds.xform(locator, worldSpace=True, translation=(-3.0, 1.0, -11.0))
        self.pick.pick_position(self.groom, "tie", "pivot")
        self.assertClose(self.center(), (-3.0, 1.0, -11.0))
        # A locator under a moved parent: its world position.
        parent = cmds.group(empty=True, name="rig")
        cmds.parent(locator, parent)
        cmds.xform(parent, worldSpace=True, translation=(0.0, 5.0, 0.0))
        cmds.select(locator)
        self.pick.pick_position(self.groom, "tie", "pivot")
        self.assertClose(self.center(), (-3.0, 6.0, -11.0))
        # The UI in meters: still centimeters, as groom data.
        cmds.currentUnit(linear="m")
        cmds.xform(parent, worldSpace=True, translation=(0.0, 0.0, 0.0))
        cmds.xform(locator, worldSpace=True, translation=(0.02, 0.03, -0.12))
        cmds.select(locator)
        self.pick.pick_position(self.groom, "tie", "pivot")
        cmds.currentUnit(linear="cm")
        self.assertClose(self.center(), (2.0, 3.0, -12.0))

    def test_pick_takes_the_mean_of_selected_vertices(self):
        vertices = ["%s.vtx[%d]" % (self.scalp, i) for i in (0, 1, 2)]
        cmds.select(vertices)
        expected = [sum(cmds.pointPosition(v, world=True)[k] for v in vertices) / 3.0 for k in range(3)]
        position, what = self.pick.selected_position(self.groom)
        self.assertClose(position, expected)
        self.assertEqual(what, "3 points")
        # Nothing with a position selected: a clear error, the param left alone.
        before = self.center()
        cmds.select(clear=True)
        with self.assertRaises(ValueError):
            self.pick.pick_position(self.groom, "tie", "pivot")
        cmds.select(self.groom)
        with self.assertRaises(ValueError):
            self.pick.selected_position(self.groom)
        self.assertEqual(self.center(), before)


class GraphTextTest(unittest.TestCase):
    """Copy Graph and Paste Graph: a whole look as text (a preset with control values)."""

    @classmethod
    def setUpClass(cls):
        if not cmds.pluginInfo(PLUGIN_PATH, query=True, loaded=True):
            cmds.loadPlugin(PLUGIN_PATH)
        cls.catalog = gm.OperatorCatalog.from_maya()

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.a = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10)[0], name="lookA")
        self.b = groomlab.create_groom(cmds.polySphere(radius=3, subdivisionsX=20, subdivisionsY=10)[0], name="lookB")
        for groom in (self.a, self.b):
            groomlab.set_graph(groom, CLASSIC_GRAPH)
            cmds.setAttr(groom + ".previewDensity", 0.02)

    def make_look(self):
        """Groom A: a preset with a slider (ctl_curls), a compound, a keyed length and a keyed color."""
        presets.apply_preset(self.a, "blend_two_looks")
        model = gm.GraphModel.from_groom(self.a, self.catalog)
        if "color" not in [n["name"] for n in groomlab.get_graph(self.a)["nodes"]]:
            model.add_node("HairColor", name="color")
            model.insert_before("output", "color")
        self.compound = model.collapse(["width"])
        model.save(self.a)
        cmds.setAttr(self.a + ".ctl_curls", 0.7)
        groomlab.set_param_key(self.a, "curls", "radius", value=0.05, time=1)
        groomlab.set_param_key(self.a, "color", "dye", value=[0.5, 0.4, 0.3], time=1)
        self.assertEqual(groomlab.errors(self.a), [])
        return groomlab.copy_graph_text(self.a)

    def node(self, groom, name):
        return next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == name)

    def test_the_text_is_a_preset_with_control_values(self):
        text = self.make_look()
        data = json.loads(text)
        self.assertEqual(list(data)[:3], ["version", "size", "about"])
        self.assertAlmostEqual(data["size"], 2.0, places=4)
        self.assertEqual(data["about"]["title"], "lookA")
        self.assertEqual(data["about"]["category"], "Shared")
        self.assertEqual(presets.check(data, self.catalog, warnings=False), [])
        self.assertEqual(data["controls"]["ctl_curls"], 0.7)
        self.assertAlmostEqual(data["controls"]["ctl_curls_radius"], 0.05, places=5)
        self.assertEqual([round(v, 4) for v in data["controls"]["ctl_color_dye"]], [0.5, 0.4, 0.3])
        stored = {n["name"]: n for n in groomlab.get_graph(self.a)["nodes"]}  # the layout is the graph's
        for node in data["nodes"]:
            self.assertEqual(node["ui"], stored[node["name"]]["ui"])
        # The same text Save as Preset writes, plus the controls.
        path = presets.save_preset(self.a, "as_saved", overwrite=True)
        try:
            saved = presets.read_preset("as_saved")
        finally:
            os.remove(path)
        del data["controls"], data["about"]
        self.assertEqual(saved, data)

    def test_paste_fits_the_look_to_another_scalp_in_one_undo_step(self):
        text = self.make_look()
        before = (cmds.getAttr(self.b + ".graph"), cmds.getAttr(self.b + ".graphUi"))
        result = groomlab.paste_graph_text(self.b, text)
        self.assertEqual(result["title"], "lookA")
        self.assertAlmostEqual(result["fit"][1] / result["fit"][0], 3.0, places=4)
        self.assertIn("x3", result["message"])
        # Lengths scale with the scalp: on the graph, inside the compound and on a keyed length.
        k = 3.0
        self.assertAlmostEqual(self.node(self.b, "scatter")["params"]["length"],
                               self.node(self.a, "scatter")["params"]["length"] * k, places=3)
        compound_b, compound_a = self.node(self.b, self.compound), self.node(self.a, self.compound)
        self.assertEqual(compound_b["type"], "Compound")
        inner = lambda c: next(n for n in c["params"]["definition"]["graph"]["nodes"] if n["type"] == "Width")
        self.assertAlmostEqual(inner(compound_b)["params"]["width"], inner(compound_a)["params"]["width"] * k, places=5)
        self.assertAlmostEqual(cmds.getAttr(self.b + ".ctl_curls_radius"), 0.05 * k, places=4)
        # Control values came along; keys did not.
        self.assertAlmostEqual(cmds.getAttr(self.b + ".ctl_curls"), 0.7, places=5)
        self.assertEqual([round(v, 4) for v in cmds.getAttr(self.b + ".ctl_color_dye")[0]], [0.5, 0.4, 0.3])
        self.assertTrue(cmds.keyframe(self.a + ".ctl_curls_radius", query=True, keyframeCount=True))
        self.assertFalse(cmds.keyframe(self.b + ".ctl_curls_radius", query=True, keyframeCount=True))
        self.assertEqual(sorted(result["controls"]), ["ctl_color_dye", "ctl_curls", "ctl_curls_radius"])
        self.assertEqual(groomlab.errors(self.b), [])
        self.assertGreater(cmds.getAttr(self.b + ".outStrandCount"), 0)
        # The layout came along.
        self.assertEqual(self.node(self.b, "scatter")["ui"], self.node(self.a, "scatter")["ui"])
        # One undo step takes it all back.
        cmds.undo()
        self.assertEqual((cmds.getAttr(self.b + ".graph"), cmds.getAttr(self.b + ".graphUi")), before)
        self.assertFalse(cmds.attributeQuery("ctl_curls", node=self.b, exists=True))
        # A code block pasted from a chat works too, and fit=False keeps the sizes.
        result = groomlab.paste_graph_text(self.b, "```json\n%s\n```" % text, fit=False)
        self.assertIsNone(result["fit"])
        self.assertAlmostEqual(self.node(self.b, "scatter")["params"]["length"],
                               self.node(self.a, "scatter")["params"]["length"], places=5)

    def test_what_did_not_travel_is_reported(self):
        groomlab.add_map(self.a, "mask", value=1.0, kind="vertices")
        model = gm.GraphModel.from_groom(self.a, self.catalog)
        model.add_node("Map", name="maskMap", params={"name": "mask"})
        model.set_param("length", "keep", groomlab.texture_map("textures/not_here.png"))
        model.set_param("scatter", "density", groomlab.color_set_map("dirt"))
        model.add_node("HairColor", name="color", params={"map": "\\\\someone\\share\\hair.png"})
        model.insert_before("output", "color")
        model.add_node("Instance", name="leaves", params={"prototype": "leafMesh"})
        model.insert_before("output", "leaves")
        model.add_node("ColliderMesh", name="body", params={"name": "bodyCollider"})
        model.set_param("guides", "group", "fringe")
        model.save(self.a)
        text = groomlab.copy_graph_text(self.a)
        result = groomlab.paste_graph_text(self.b, text)
        missing = result["missing"]
        self.assertEqual(missing["maps"], {"mask": "map"})
        self.assertEqual(missing["colorSets"], ["dirt"])
        self.assertEqual(missing["files"], {"length.keep": "textures/not_here.png"})
        self.assertEqual(missing["meshes"], {"leaves.prototype": "leafMesh"})
        self.assertEqual(missing["colliders"], ["bodyCollider"])
        self.assertEqual(missing["groups"], ["fringe"])
        # A file on someone else's machine is never looked up: the param is left out.
        self.assertEqual(result["removed"], {"color.map": "\\\\someone\\share\\hair.png"})
        self.assertNotIn("texture", self.node(self.b, "color")["params"])
        for word in ("missing here", "mask", "dirt", "not_here.png", "leafMesh", "bodyCollider", "fringe",
                     "network paths left out"):
            self.assertIn(word, result["message"])
        # The maps fall back to the nodes' defaults; groom A's map stays A's.
        self.assertNotIn("mask", groomlab.list_maps(self.b))
        # Painted here, nothing is missing.
        groomlab.add_map(self.b, "mask", value=1.0, kind="vertices")
        self.assertEqual(presets.missing_inputs(self.b, groomlab.get_graph(self.b))["maps"], {})

    def test_text_that_is_no_graph_changes_nothing(self):
        presets.apply_preset(self.b, "curly")
        good = json.loads(self.make_look())
        unknown = json.loads(json.dumps(good))
        next(n for n in unknown["nodes"] if n["name"] == "curls")["type"] = "Teleport"
        newer = dict(good, version=99)
        many = {"nodes": [{"name": "n%d" % i, "type": "Scalp"} for i in range(presets.MAX_NODES + 1)],
                "connections": []}
        bad_param = json.loads(json.dumps(good))
        next(n for n in bad_param["nodes"] if n["name"] == "scatter")["params"]["density"] = "lots"
        model = gm.GraphModel.from_groom(self.a, self.catalog)
        cases = {
            "": "empty",
            "hello there": "not JSON",
            "[1, 2]": "no GroomLab graph",
            '{"nodes": "scalp"}': "no GroomLab graph",
            model.copy_nodes(["scatter"]): "copied nodes",
            json.dumps(unknown): "unknown node type 'Teleport'",
            json.dumps(newer): "newer GroomLab",
            json.dumps(many): "more than %d nodes" % presets.MAX_NODES,
            json.dumps(bad_param): "wants a number",
            '{"nodes": [], "size": NaN}': "not JSON",
            '{"nodes": [{"name": "a", "type": 7}]}': "without a name and a type",
            '{"nodes": [], "connections": [{"from": 1}]}': "connections",
            "[" * 100000 + "]" * 100000: "nested too deeply",
            " " * (presets.MAX_TEXT + 1) + "{}": "MB",
        }
        cmds.undoInfo(state=True)
        before = (cmds.getAttr(self.b + ".graph"), cmds.getAttr(self.b + ".graphUi"),
                  cmds.undoInfo(query=True, undoName=True))
        for text, why in cases.items():
            with self.assertRaises(ValueError, msg=text[:80]) as caught:
                groomlab.paste_graph_text(self.b, text)
            self.assertIn("cannot paste", str(caught.exception))
            self.assertIn(why, str(caught.exception), text[:80])
            self.assertLess(len(str(caught.exception)), 1000)
            self.assertEqual((cmds.getAttr(self.b + ".graph"), cmds.getAttr(self.b + ".graphUi"),
                              cmds.undoInfo(query=True, undoName=True)), before, text[:80])

    def test_pasted_text_saved_as_a_file_is_a_preset(self):
        text = self.make_look()
        path = os.path.join(USER_PRESETS, "shared_look.json")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        try:
            self.assertIn("shared_look", presets.list_presets())
            info = presets.preset_info("shared_look")
            self.assertEqual((info["title"], info["category"]), ("lookA", "Shared"))
            self.assertEqual(presets.check(presets.read_preset("shared_look"), self.catalog, warnings=False), [])
            presets.apply_preset(self.b, "shared_look")
            self.assertNotIn("controls", groomlab.get_graph(self.b))
            self.assertAlmostEqual(cmds.getAttr(self.b + ".ctl_curls"), 0.7, places=5)  # its control values too
            self.assertAlmostEqual(cmds.getAttr(self.b + ".ctl_curls_radius"), 0.15, places=4)  # fitted
            self.assertEqual(groomlab.errors(self.b), [])
        finally:
            os.remove(path)


def shipped_presets():
    return sorted(f[:-5] for f in os.listdir(presets.SHIPPED_DIR) if f.endswith(".json"))


def card_height(model, name):
    """A node card's height in the Graph Editor (graph_editor.NodeItem: header, a row per port, the
    summary line)."""
    spec = model.node_spec(name)
    return 40 + 22 * max(1, len(spec["inputs"]), len(spec["outputs"])) + 10 + 18


class PresetsTest(unittest.TestCase):
    """Every shipped preset: valid against the operators (a renamed param fails here, where the
    plugin would ignore it), described for the Presets menu, laid out cleanly, and growing hair
    without errors on a head with guides."""

    @classmethod
    def setUpClass(cls):
        if not cmds.pluginInfo(PLUGIN_PATH, query=True, loaded=True):
            cmds.loadPlugin(PLUGIN_PATH)
        cls.catalog = gm.OperatorCatalog.from_maya()

    def test_every_preset_fits_the_operators(self):
        names = shipped_presets()
        self.assertGreaterEqual(len(names), 20)
        for name in names:
            preset = presets.read_preset(name)
            self.assertEqual(presets.check(preset, self.catalog), [], name)
            self.assertGreater(preset.get("size", 0), 0, name + " records the scalp size it was made on")
            # The Frizz node is Noise's fraction now: frizz is a Noise, named so.
            for node in preset["nodes"]:
                self.assertNotEqual(node["type"], "Frizz", name)
                self.assertNotEqual(node["name"], "frizz", name + ": name a frizz Noise frizzNoise")

    def test_the_checker_catches_what_the_plugin_would_ignore(self):
        preset = presets.read_preset("hair_basics")
        nodes = {n["name"]: n for n in preset["nodes"]}
        nodes["noise"]["params"]["amplitud"] = 0.2           # a misspelt (or renamed) param
        nodes["noise"]["params"]["space"] = "galaxy"         # not one of its choices
        nodes["resample"]["params"]["segments"] = 12.5       # not a whole number
        nodes["width"]["params"]["profile"] = "tapered"      # not a ramp
        preset["nodes"].append({"name": "frizz", "type": "Frizz"})
        problems = "\n".join(presets.check(preset, self.catalog))
        for expected in ("Noise has no param 'amplitud'", "'galaxy' is not one of strand, world",
                         "wants a whole number", "wants a ramp", "unknown node type 'Frizz'"):
            self.assertIn(expected, problems)
        # Inside a compound too, and exposed params naming nothing.
        preset = presets.read_preset("merge_layers")
        compound = next(n for n in preset["nodes"] if n["type"] == "Compound")
        definition = compound["params"]["definition"]
        definition["graph"]["nodes"][1]["params"]["octave"] = 2
        definition["expose"]["gone"] = "waves.nothing"
        problems = "\n".join(presets.check(preset, self.catalog))
        self.assertIn("wavyFrizz/waves: Noise has no param 'octave'", problems)
        self.assertIn("exposed 'gone' names 'waves.nothing'", problems)

    def test_every_preset_is_described_for_the_menu(self):
        infos = {i["name"]: i for i in presets.list_presets(details=True)}
        seen = set()
        for name in shipped_presets():
            info = infos[name]
            self.assertIn(info["category"], presets.CATEGORIES, name)
            self.assertTrue(info["title"] and info["title"] != name, name)
            self.assertGreater(len(info["description"]), 40, name)
            self.assertLess(len(info["description"]), 400, name + ": one or two sentences")
            seen.add(info["category"])
        self.assertEqual(seen, set(presets.CATEGORIES))  # every category has presets
        # The reference's presets table lists them all.
        table = os.path.join(os.path.dirname(SCRIPTS_DIR), "..", "..", "docs", "reference", "graph-editor.md")
        with open(table, encoding="utf-8") as f:
            reference = f.read()
        for name in shipped_presets():
            self.assertIn("`%s`" % name, reference, name + " is missing from docs/reference/graph-editor.md's presets")

    def test_every_preset_is_laid_out_left_to_right(self):
        for name in shipped_presets():
            preset = presets.read_preset(name)
            model = gm.GraphModel(self.catalog, preset)
            boxes = []
            for node in preset["nodes"]:
                self.assertIn("ui", node, "%s: %s has no position" % (name, node["name"]))
                x, y = node["ui"]["x"], node["ui"]["y"]
                self.assertEqual((x % 240, y % 180), (0, 0), "%s: %s is off the grid" % (name, node["name"]))
                boxes.append((node["name"], x, y, x + 208, y + card_height(model, node["name"])))
            for i, a in enumerate(boxes):
                for b in boxes[i + 1:]:
                    overlap = a[1] < b[3] and b[1] < a[3] and a[2] < b[4] and b[2] < a[4]
                    self.assertFalse(overlap, "%s: %s and %s overlap" % (name, a[0], b[0]))
            where = {b[0]: b for b in boxes}
            for c in preset["connections"]:
                self.assertLess(where[c["from"]][1], where[c["to"]][1],
                                "%s: %s -> %s runs right to left" % (name, c["from"], c["to"]))
            # A backdrop holds the nodes it frames and no others.
            for drop in preset.get("backdrops", []):
                inside = [b[0] for b in boxes if drop["x"] <= b[1] and b[3] <= drop["x"] + drop["w"]
                          and drop["y"] <= b[2] and b[4] <= drop["y"] + drop["h"]]
                self.assertTrue(inside, "%s: backdrop %r frames nothing" % (name, drop["title"]))
                for b in boxes:
                    if b[0] in inside:
                        continue
                    centre = ((b[1] + b[3]) / 2.0, (b[2] + b[4]) / 2.0)
                    self.assertFalse(drop["x"] <= centre[0] <= drop["x"] + drop["w"] and
                                     drop["y"] <= centre[1] <= drop["y"] + drop["h"],
                                     "%s: backdrop %r takes %s along" % (name, drop["title"], b[0]))

    def head(self):
        """A head: a sphere with a few guides combed down its sides, and a part on top."""
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        groom = groomlab.create_groom(scalp)
        cmds.setAttr(groom + ".previewDensity", 0.05)  # made on a 20 cm head: a hundredfold denser here
        import math
        curves = []
        for i in range(8):
            a = 2 * math.pi * i / 8
            s, c = math.sin(a), math.cos(a)
            curves.append(cmds.curve(degree=2, point=[(0.5 * s, 0.87, 0.5 * c), (0.9 * s, 0.9, 0.9 * c),
                                                      (1.2 * s, 0.4, 1.2 * c), (1.25 * s, -0.2, 1.25 * c)]))
        groomlab.guides_from_curves(groom, curves)
        return groom

    def test_every_preset_grows_on_a_head(self):
        groom = self.head()
        self.assertAlmostEqual(presets.scalp_size(groom), 2.0, places=3)
        for name in shipped_presets():
            presets.apply_preset(groom, name)
            for group in presets.required_groups(name):
                # A hand-placed look: its group made empty; two guides drawn into it grow it.
                self.assertIn(group["name"], [g["name"] for g in groomlab.list_guide_groups(groom)], name)
                if not groomlab.guide_ids(groom, group["name"]):
                    groomlab.guides_from_curves(groom, [
                        cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.3, 0.6), (0.0, 1.2, 1.4)]),
                        cmds.curve(degree=1, point=[(0.3, 0.95, 0.0), (0.5, 1.2, 0.5), (0.6, 1.1, 1.2)])],
                        group=group["name"])
            count = cmds.getAttr(groom + ".outStrandCount")
            self.assertEqual(groomlab.errors(groom), [], name)
            self.assertGreater(count, 100, name)
            self.assertLess(count, 200000, name)
            graph = groomlab.get_graph(groom)
            for key in presets.PRESET_FIELDS:
                if key != "backdrops":  # the preset's own fields stay out of the groom; the layout goes in
                    self.assertNotIn(key, graph, name)
            preset = presets.read_preset(name)
            # The layout lives in the graph: each node's "ui", the backdrops (graph_io.h).
            self.assertEqual({n["name"] for n in graph["nodes"] if "ui" in n}, {n["name"] for n in preset["nodes"]}, name)
            self.assertEqual(len(graph.get("backdrops", [])), len(preset.get("backdrops", [])), name)

    def test_hand_placed_flight_feathers_start_from_an_empty_group(self):
        groom = self.head()
        hair = len(groomlab.guide_ids(groom, "primary"))
        self.assertEqual(presets.required_groups("hand_placed_feathers"), [{"name": "feathers", "draw": True}])
        self.assertEqual(presets.draw_group("hand_placed_feathers"), "feathers")
        self.assertIsNone(presets.draw_group("hair_basics"))
        self.assertEqual(presets.read_preset("hand_placed_feathers")["about"]["title"], "Flight feathers (hand-placed)")
        # Applied: the feathers group made, empty, and nothing grows yet (the head's guides are not
        # feathers); the Draw tool would draw into it (none in batch mode).
        presets.apply_preset(groom, "hand_placed_feathers")
        self.assertEqual(groomlab.errors(groom), [])
        self.assertEqual(groomlab.guide_ids(groom, "feathers"), [])
        self.assertEqual(len(groomlab.guide_ids(groom, "primary")), hair)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), 0)
        self.assertEqual(groomlab.feather_guide_groups(groom), ["feathers"])
        # Every guide drawn into it is one flight feather: 70 barbs a side and the shaft.
        curves = [cmds.curve(degree=1, point=[(0.1 * i, 1.0, 0.0), (0.1 * i, 1.3, 0.6), (0.1 * i, 1.2, 1.4)])
                  for i in range(3)]
        groomlab.guides_from_curves(groom, curves, group="feathers")
        self.assertEqual(groomlab.errors(groom), [])
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), 3 * (1 + 140))
        # Applied again: the group and its guides stay.
        presets.apply_preset(groom, "hand_placed_feathers")
        self.assertEqual(len(groomlab.guide_ids(groom, "feathers")), 3)

    def test_what_the_teaching_presets_show(self):
        groom = self.head()

        def reach(underneath=False):
            """How far the strands' points are from the head's center, on average (the bounding box
            holds the guides too)."""
            points = [p for p in groomlab.viewport_points(groom) if not underneath or p[1] < -0.3]
            return sum((x * x + y * y + z * z) ** 0.5 for x, y, z in points) / len(points)

        # Fitted: lengths shrink with the scalp, rates per length (curl turns) grow.
        presets.apply_preset(groom, "curly")
        nodes = {n["name"]: n for n in groomlab.get_graph(groom)["nodes"]}
        self.assertAlmostEqual(nodes["curl"]["params"]["radius"], 0.045, places=5)
        self.assertAlmostEqual(nodes["curl"]["params"]["frequency"], 4.0, places=4)

        # Blend: the keyable ctl_curls takes the strands from sleek to curly.
        presets.apply_preset(groom, "blend_two_looks")
        self.assertTrue(cmds.attributeQuery("ctl_curls", node=groom, keyable=True))
        sleek = cmds.groomLabGraphStats(groom, points=True)
        cmds.setAttr(groom + ".ctl_curls", 1.0)
        self.assertNotEqual(cmds.groomLabGraphStats(groom, points=True), sleek)
        self.assertEqual(groomlab.errors(groom), [])

        # Merge: the layer adds its strands to the base's.
        presets.apply_preset(groom, "merge_layers")
        both = cmds.getAttr(groom + ".outStrandCount")
        self.assertEqual(groomlab.errors(groom), [])
        # Switch: the light variation has about a third of the base's strands.
        groomlab.set_param(groom, "pickLook", "index", 1)
        self.assertLess(cmds.getAttr(groom + ".outStrandCount"), both * 0.5)
        groomlab.set_param(groom, "pickLook", "index", 0)
        graph = groomlab.get_graph(groom)
        graph["nodes"] = [n for n in graph["nodes"] if n["name"] != "mergeLayers"]
        graph["connections"] = [c for c in graph["connections"] if "mergeLayers" not in (c["from"], c["to"])]
        graph["connections"].append({"from": "clump", "to": "pickLook", "input": "a"})
        groomlab.set_graph(groom, graph)
        self.assertLess(cmds.getAttr(groom + ".outStrandCount"), both)

        # Painted maps: the map is made (clumps and length full, a little frizz); B lower is shorter.
        presets.apply_preset(groom, "painted_maps")
        self.assertIn("groomMasks", groomlab.list_maps(groom))
        self.assertEqual(groomlab.map_channels(groom, "groomMasks"), 4)
        full = reach()
        groomlab.fill_map(groom, "groomMasks", color=(0, 0, 0.3, 1), channels="b")
        self.assertLess(reach() - 1.0, (full - 1.0) * 0.85)  # the hair is shorter
        presets.apply_preset(groom, "painted_maps")  # applied again: the painted map is kept
        self.assertLess(reach() - 1.0, (full - 1.0) * 0.85)

        # Expressions: shorter underneath than on top, and the keyable $nape.
        presets.apply_preset(groom, "expression_driven")
        self.assertTrue(cmds.attributeQuery("ctl_crownLength_nape", node=groom, keyable=True))
        before = reach(underneath=True)
        cmds.setAttr(groom + ".ctl_crownLength_nape", 0.1)
        self.assertLess(reach(underneath=True) - 1.0, (before - 1.0) * 0.85)  # the underside's hair is shorter
        self.assertEqual(groomlab.errors(groom), [])

        # Parts: Interpolate and Clump respect a part drawn on top, and the Part node opens it.
        presets.apply_preset(groom, "parted_style")
        points = cmds.groomLabGraphStats(groom, points=True)
        part = cmds.curve(degree=1, point=[(-0.05, 1.0, -0.9), (0.0, 1.0, 0.0), (0.05, 1.0, 0.9)])
        groomlab.add_part(groom, [part], name="middle")
        self.assertNotEqual(cmds.groomLabGraphStats(groom, points=True), points)
        self.assertEqual(groomlab.errors(groom), [])

        # Motion: the wind moves the strands from frame to frame.
        presets.apply_preset(groom, "wind_and_jiggle")
        cmds.currentTime(1)
        first = cmds.groomLabGraphStats(groom, points=True)
        cmds.currentTime(12)
        self.assertNotEqual(cmds.groomLabGraphStats(groom, points=True), first)
        cmds.currentTime(1)
        self.assertEqual(groomlab.errors(groom), [])

        # Game cards: the Cards node makes cards, within the mid level of detail's budget.
        cards = next(n for n in presets.read_preset("game_cards")["nodes"] if n["type"] == "Cards")
        self.assertEqual(cards["params"], groomlab._preset_params("mid", "hair", None))  # what the LOD menu sets
        presets.apply_preset(groom, "game_cards")
        cmds.setAttr(groom + ".displayCards", True)
        stats = groomlab.card_stats(groom)
        self.assertGreater(stats["cards"], 0)
        self.assertLessEqual(stats["triangles"], groomlab.CARD_PRESETS["mid"]["triangles"])
        self.assertEqual(groomlab.errors(groom), [])


class NodesTest(unittest.TestCase):
    """Compound nodes (groomlab.nodes): Save as Node, the library, Import, Update."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)
        cls.catalog = gm.OperatorCatalog.from_maya()

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        for name in os.listdir(USER_NODES):
            os.remove(os.path.join(USER_NODES, name))
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        self.groom = groomlab.create_groom(self.scalp)
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        model = gm.GraphModel.from_groom(self.groom, self.catalog)
        model.add_node("Noise", name="noise", params={"amplitude": 0.04})
        model.insert_between("length", "width", "noise")
        self.compound = model.collapse(["noise"])
        model.expose(self.compound, "amount", "noise.amplitude")
        model.save(self.groom)

    def test_shipped_nodes_are_listed_and_added(self):
        from groomlab import nodes
        listed = dict((n["id"], n) for n in groomlab.library_nodes())
        for node_id in ("groomlab.soft_frizz", "groomlab.grey_streaks", "groomlab.windswept_ends"):
            self.assertIn(node_id, listed)
            self.assertEqual(listed[node_id]["type"], "node:" + node_id)
        self.assertEqual(listed["groomlab.soft_frizz"]["categoryTitle"], "Look")
        self.assertEqual(groomlab.node_dirs()[-2:], [USER_NODES, nodes.SHIPPED_DIR])
        name = nodes.add_node(self.groom, "groomlab.soft_frizz", after="width")
        self.assertEqual(name, "softFrizz")
        self.assertEqual(groomlab.errors(self.groom), [])
        info = nodes.node_info(self.groom, name)
        self.assertEqual((info["id"], info["version"], info["updateAvailable"]), ("groomlab.soft_frizz", 1, False))

    def test_script_nodes(self):
        # A script node: added from the template, edited (a broken script refused, saying where),
        # saved to the library (version up), the shipped ones listed and added; each evaluates.
        from groomlab import nodes
        before = cmds.exactWorldBoundingBox(self.groom)
        name = groomlab.add_script_node(self.groom, nodes.script_template("Lift", "Tester"), after="width")
        self.assertEqual(name, "lift")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)  # it pushes the tips out
        definition = nodes.script_definition(self.groom, name)
        self.assertEqual(definition["id"], "tester.lift")
        broken = dict(definition, script="per point {\n    P += N *;\n}")
        errors = groomlab.script_errors(broken)
        self.assertEqual([(e["part"], e["line"], e["column"]) for e in errors], [("script", 2, 13)])
        with self.assertRaises(ValueError):
            groomlab.set_script(self.groom, name, broken)
        # Widths only.
        thin = dict(definition, writes=["width"], script="per point { width = 0.001 + 0.004 * (1 - t); }")
        groomlab.set_script(self.groom, name, thin)
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.undo()  # one step
        self.assertIn("P +=", nodes.script_definition(self.groom, name)["script"])
        path = groomlab.save_script_node(self.groom, name)
        self.assertTrue(path.replace("\\", "/").endswith("tester.lift.glnode"))
        listed = dict((n["id"], n) for n in groomlab.library_nodes())
        self.assertEqual((listed["tester.lift"]["kind"], listed["tester.lift"]["version"]), ("script", 1))
        groomlab.save_script_node(self.groom, name)
        self.assertEqual(dict((n["id"], n["version"]) for n in groomlab.library_nodes())["tester.lift"], 2)
        with self.assertRaises(ValueError):
            groomlab.save_script_node(self.groom, name, as_new=True)  # the library has its id
        # The shipped script nodes: listed, added, evaluated.
        for node_id in ("groomlab.wave", "groomlab.kink", "groomlab.taper_by_channel", "groomlab.random_length"):
            self.assertEqual(listed[node_id]["kind"], "script")
            added = nodes.add_node(self.groom, node_id, after="width")
            self.assertEqual(groomlab.errors(self.groom), [], node_id)
            self.assertEqual(nodes.node_info(self.groom, added)["kind"], "script")

    def test_save_import_and_update(self):
        before = cmds.exactWorldBoundingBox(self.groom)
        path = groomlab.save_node(self.groom, self.compound, label="My Frizz", category="Look", author="Tester",
                                  description="Noise, saved.")
        self.assertEqual(os.path.dirname(path).replace("\\", "/"), USER_NODES.replace("\\", "/"))
        self.assertTrue(path.endswith("tester.my_frizz.glnode"))
        listed = dict((n["id"], n) for n in groomlab.library_nodes())
        self.assertEqual(listed["tester.my_frizz"]["version"], 1)
        # The compound became that library node (same look), and a new one adds from the library.
        from groomlab import nodes
        self.assertEqual(nodes.node_info(self.groom, self.compound)["id"], "tester.my_frizz")
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), before)
        added = nodes.add_node(self.groom, "tester.my_frizz")
        self.assertEqual(added, "myFrizz")

        # Saved again louder: version 2 in the library; the other node shows an update, takes it.
        groomlab.set_param(self.groom, self.compound, "amount", 0.3)
        groomlab.save_node(self.groom, self.compound)
        self.assertEqual(dict((n["id"], n["version"]) for n in groomlab.library_nodes())["tester.my_frizz"], 2)
        info = nodes.node_info(self.groom, added)
        self.assertEqual((info["version"], info["latest"], info["updateAvailable"]), (1, 2, True))
        self.assertTrue(groomlab.update_node(self.groom, added))
        self.assertFalse(nodes.node_info(self.groom, added)["updateAvailable"])
        cmds.undo()  # one step
        self.assertTrue(nodes.node_info(self.groom, added)["updateAvailable"])

        # Import: a file from elsewhere, checked.
        elsewhere = tempfile.mkdtemp(prefix="groomlab_share_")
        try:
            shared = os.path.join(elsewhere, "shared.glnode")
            with open(path) as f:
                data = json.load(f)
            data["id"], data["label"] = "friend.wave", "Wave"
            with open(shared, "w") as f:
                json.dump(data, f)
            self.assertTrue(groomlab.import_node(shared).endswith("friend.wave.glnode"))
            self.assertIn("friend.wave", [n["id"] for n in groomlab.library_nodes()])
            data["glnode"] = 99
            with open(shared, "w") as f:
                json.dump(data, f)
            with self.assertRaises(ValueError):
                groomlab.import_node(shared)
        finally:
            shutil.rmtree(elsewhere, ignore_errors=True)


class ExpressionTest(unittest.TestCase):
    """Expression nodes: the reference the editor completes from, their own variables, the library."""

    @classmethod
    def setUpClass(cls):
        if not cmds.pluginInfo(PLUGIN_PATH, query=True, loaded=True):
            cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        self.groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=12)[0])
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        graph = groomlab.get_graph(self.groom, expand_default=True)
        graph["nodes"].append({"name": "mask", "type": "Expression", "params": {"expression": "$length / $longest"}})
        length = next(n for n in graph["nodes"] if n["type"] == "Length")
        length.setdefault("paramInputs", []).append("keep")
        # A clumpId channel before it, as a Clump writes (the per-clump example reads it).
        graph["nodes"].append({"name": "clumps", "type": "Attribute",
                               "params": {"name": "clumpId", "value": {"expression": "rand", "min": 0.0, "max": 20.0}}})
        feed = next(c for c in graph["connections"] if c["to"] == length["name"] and c["input"] == "strands")
        graph["connections"].append({"from": feed["from"], "to": "clumps", "input": "strands"})
        feed["from"] = "clumps"
        graph["connections"].append({"from": "mask", "to": length["name"], "input": "keep"})
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])

    def evaluates(self, text, variables=None):
        from groomlab import expressions
        groomlab.set_param(self.groom, "mask", "variables", json.dumps(expressions.check_variables(variables or {})))
        groomlab.set_param(self.groom, "mask", "expression", text)
        cmds.getAttr(self.groom + ".outStrandCount")
        return groomlab.errors(self.groom)

    def test_every_variable_and_function_the_editor_offers_compiles(self):
        from groomlab import expressions
        for name, text in expressions.VARIABLES:
            # A vector (P, N...) as a number: its length.
            sample = "length($%s) * 0 + 1" if text.startswith("Vector") else "$%s * 0 + 1"
            self.assertEqual(self.evaluates(sample % name), [], name)
        args = {1: "0.5", 2: "0.5, 0.25", 3: "0.1, 0.2, 0.3", 5: "0.5, 0, 1, 2, 3"}
        # Those taking vectors, curve keys or a map's name, as written.
        samples = {"dot": "dot(P, N)", "cross": "length(cross(P, N))", "length": "length(P)",
                   "normalize": "length(normalize(P))", "distance": "distance(P, N)", "angle": "angle(P, N)",
                   "ortho": "length(ortho(P, N))", "up": "length(up(P, N))",
                   "rotate": "length(rotate(P, N, 0.5))", "vnoise": "length(vnoise(P))", "cnoise": "length(cnoise(P))",
                   "vnoise4": "length(vnoise4(P, 0.5))", "cnoise4": "length(cnoise4(P, 0.5))", "fbm": "fbm(P)",
                   "vfbm": "length(vfbm(P, 3))", "cfbm": "length(cfbm(P, 3))", "turbulence": "turbulence(P * 2, 3, 2, 0.5)",
                   "vturbulence": "length(vturbulence(P))", "cturbulence": "length(cturbulence(P))",
                   "vfbm4": "length(vfbm4(P, 0.5))", "cfbm4": "length(cfbm4(P, 0.5))",
                   "cellnoise": "cellnoise(P * 4)", "ccellnoise": "length(ccellnoise(P * 4))", "voronoi": "voronoi(P * 4, 4)",
                   "cvoronoi": "length(cvoronoi(P * 4, 1))", "pvoronoi": "length(pvoronoi(P * 4))",
                   "rgbtohsl": "length(rgbtohsl(P))", "hsltorgb": "length(hsltorgb(P))", "hsi": "length(hsi(P, 30, 1, 1))",
                   "midhsi": "length(midhsi(P, 30, 1, 1, 0.5))", "saturate": "length(saturate(P, 0.5))",
                   "curve": "curve(0.5, 0, 0, 4, 1, 1, 4)", "ccurve": "length(ccurve(0.5, 0, [0, 0, 0], 1, 1, [1, 1, 1], 1))",
                   "linearcurve": "linearcurve(0.5, 0, 0, 1, 1)", "spline": "spline(0.5, 0, 1, 0, 1)",
                   "pick": "pick(3, 1, 4)", "choose": "choose(0.5, 1, 2)", "wchoose": "wchoose(0.5, 1, 1, 2, 3)",
                   "map": None, "cmap": None}  # a map needs one painted: below
        for name, signature, _ in expressions.FUNCTIONS:
            sample = samples.get(name, "%s(%s)" % (name, args.get(signature.count(",") + 1)))
            if sample is None:
                continue
            self.assertEqual(self.evaluates("%s * 0 + 1" % sample), [], name)
        errors = self.evaluates('map("no_such_map") * 0 + 1')
        self.assertTrue(any("no_such_map" in e for e in errors), errors)  # the groom's maps, by name
        self.assertTrue(self.evaluates("$nope("))  # a mistake is reported on the node
        self.assertIn("$length", [t for t, _ in expressions.completions()])
        offered = [t for t, _ in expressions.completions({"amount": 0.5, "center": [0, 1, 0]}, ["myChannel"])]
        self.assertEqual(offered[:4], ["$amount", "$center.x", "$center.y", "$center.z"])
        self.assertIn("$myChannel", offered)
        self.assertIn("smoothstep(", offered)

    def test_variables_and_blocks(self):
        self.assertEqual(self.evaluates("# the long ones\n$l = $length / $longest;\nclamp($l * $gain, 0, 1)",
                                        {"gain": 2.0}), [])
        self.assertEqual(self.evaluates("$d = $y - $center.y;\nabs($d)", {"center": [0, 0.5, 0]}), [])
        errors = self.evaluates("$center + 1", {"center": [0, 0.5, 0]})
        self.assertTrue(any("vector" in e for e in errors), errors)
        with self.assertRaises(ValueError):
            from groomlab import expressions
            expressions.check_variables({"2bad": 1.0})
        # Checked as typed, without evaluating: what is wrong, and where.
        self.assertEqual(expressions.check("$a * 2", {"a": 1.0}), "")
        self.assertIn("unexpected end", expressions.check("1 +"))
        message = expressions.check("$a = 1;\n$a + $b.x", {"b": 3.0})
        self.assertEqual(expressions.error_place(message)[0], 2, message)

    def test_every_shipped_example_evaluates(self):
        from groomlab import expressions
        examples = [e for e in expressions.list_expressions() if e["shipped"]]
        self.assertGreaterEqual(len(examples), 8)
        for e in examples:
            expressions.apply_expression(self.groom, "mask", e["name"])
            cmds.getAttr(self.groom + ".outStrandCount")
            self.assertEqual(groomlab.errors(self.groom), [], e["name"])
            self.assertEqual(expressions.node_values(self.groom, "mask")["expression"], e["expression"])

    def test_the_library_keeps_saved_expressions_for_every_scene(self):
        from groomlab import expressions
        path = expressions.save_expression("Crown mask", "smoothstep($lo, 1, $ny)", {"lo": 0.3}, "the top")
        self.assertTrue(path.startswith(USER_EXPRESSIONS.replace("\\", "/")), path)
        with self.assertRaises(ValueError):
            expressions.save_expression("Crown mask", "1")  # exists
        expressions.save_expression("Crown mask", "smoothstep($lo, 1, $ny)", {"lo": 0.4}, "the top", overwrite=True)
        cmds.file(new=True, force=True)  # another scene: still there
        entry = expressions.find_expression("Crown mask")
        self.assertFalse(entry["shipped"])
        self.assertEqual(entry["variables"], {"lo": 0.4})
        self.assertEqual(expressions.list_expressions()[0]["name"], "Crown mask")  # yours first, then examples
        expressions.delete_expression("Crown mask")
        with self.assertRaises(ValueError):
            expressions.find_expression("Crown mask")
        with self.assertRaises(ValueError):
            expressions.delete_expression("Stripes around the scalp")  # shipped: stays


class KeyableVariableTest(unittest.TestCase):
    """An Expression node's keyable variable: an attribute on the groom that sets it, and keys."""

    @classmethod
    def setUpClass(cls):
        if not cmds.pluginInfo(PLUGIN_PATH, query=True, loaded=True):
            cmds.loadPlugin(PLUGIN_PATH)

    def test_the_attribute_drives_the_expression(self):
        cmds.file(new=True, force=True)
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=12)[0])
        cmds.setAttr(groom + ".previewDensity", 1.0)
        graph = groomlab.get_graph(groom, expand_default=True)
        graph["nodes"].append({"name": "cut", "type": "Expression", "params": {
            "expression": "$keep", "variables": json.dumps({"keep": {"value": 1.0, "control": "cut_keep"}})}})
        scatter = next(n for n in graph["nodes"] if n["type"] == "Scatter")
        scatter.setdefault("paramInputs", []).append("density")
        graph["connections"].append({"from": "cut", "to": scatter["name"], "input": "density"})
        groomlab.set_graph(groom, graph)
        self.assertTrue(cmds.attributeQuery("ctl_cut_keep", node=groom, exists=True))  # made for it
        self.assertTrue(cmds.attributeQuery("ctl_cut_keep", node=groom, keyable=True))
        full = cmds.getAttr(groom + ".outStrandCount")
        self.assertGreater(full, 50)
        cmds.setAttr(groom + ".ctl_cut_keep", 0.25)
        self.assertLess(cmds.getAttr(groom + ".outStrandCount"), full * 0.5)  # the attribute sets $keep
        cmds.setKeyframe(groom + ".ctl_cut_keep", time=1, value=1.0)
        cmds.setKeyframe(groom + ".ctl_cut_keep", time=10, value=0.0)
        cmds.currentTime(10)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), 0)  # animated
        cmds.currentTime(1)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), full)
        self.assertEqual(groomlab.errors(groom), [])


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    shutil.rmtree(USER_PRESETS, ignore_errors=True)
    shutil.rmtree(USER_EXPRESSIONS, ignore_errors=True)
    shutil.rmtree(USER_NODES, ignore_errors=True)
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
