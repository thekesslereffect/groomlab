# SPDX-License-Identifier: Apache-2.0
"""Groom files back into Maya: import_groom, reload_groom, save_groom, and the round trips
Maya -> .groom -> Maya and app <-> Maya (the standalone app, headless).

Usage: mayapy test_groom_io.py <GroomLab.mll> <maya/scripts dir> <groomlab-cli> <work dir> <tests/data dir> [<GroomLab.exe>]
"""

import json
import os
import shutil
import subprocess
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
CLI = os.path.abspath(sys.argv[3])
WORK_DIR = os.path.abspath(sys.argv[4])
DATA_DIR = os.path.abspath(sys.argv[5])
APP = os.path.abspath(sys.argv[6]) if len(sys.argv) > 6 and sys.argv[6] else None
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

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
import groomlab_core  # noqa: E402
from groomlab import presets  # noqa: E402
from groomlab import shots  # noqa: E402

DENSITY = 0.3  # of the strand comparisons (exports at it, viewports at it)


def run(*args):
    result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True, env=dict(os.environ, QT_QPA_PLATFORM="offscreen"))
    if result.returncode != 0:
        raise AssertionError("%s failed:\n%s" % (" ".join(map(str, args)), result.stdout))
    return result.stdout


def hashes(path):
    """Each frame's strands as the CLI hashes them (IDs, every point's and width's float bits, root UVs)."""
    return [line.split("hash=")[1] for line in run(CLI, "hash", path).splitlines()[:-1]]


def read_json(path):
    with open(path) as f:
        return json.load(f)


def file_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def new_scene(name):
    """A new scene saved-as <work>/<name>/<name>.ma (its groomlab folder beside it), film rate, frames 1-10."""
    cmds.file(new=True, force=True)
    folder = os.path.join(WORK_DIR, name)
    if not os.path.isdir(folder):
        os.makedirs(folder)
    cmds.file(rename=os.path.join(folder, name + ".ma"))
    cmds.currentUnit(time="film")
    cmds.playbackOptions(minTime=1, maxTime=10)
    cmds.currentTime(1)
    return folder


def strand_points(groom, frames):
    """The viewport's strand points (at DENSITY) on each frame. (The density is set only when it is
    another: a setAttr is an undo step, and the tests undo what they did before.)"""
    if abs(cmds.getAttr(groom + ".previewDensity") - DENSITY) > 1e-6:
        cmds.setAttr(groom + ".previewDensity", DENSITY)
    out = {}
    for frame in frames:
        if cmds.currentTime(query=True) != frame:  # (a time change is an undo step in mayapy too)
            cmds.currentTime(frame)
        out[frame] = groomlab.viewport_points(groom)
    if cmds.currentTime(query=True) != 1:
        cmds.currentTime(1)
    return out


class GroomFileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)
        shutil.rmtree(WORK_DIR, ignore_errors=True)
        os.makedirs(WORK_DIR)

    def assertPointsClose(self, a, b, tolerance, what=""):
        self.assertEqual(len(a), len(b), what)
        self.assertGreater(len(a), 100, what)
        worst = max(max(abs(p - q) for p, q in zip(pa, pb)) for pa, pb in zip(a, b))
        self.assertLess(worst, tolerance, what)

    def rich_groom(self):
        """A groom with every kind of thing a groom file holds: a deforming scalp with a rest mesh
        and a color set of its own, guide groups and a guide layer, a graph with ramps, a bypassed
        node, an expression, a param input fed by a Control node, vertex (gray and color) and
        texture maps feeding params, parts, flow, a keyed corrective, a Cards node with a bake,
        followScalp."""
        scalp = cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=12, name="head")[0]
        cmds.polyColorPerVertex(scalp, rgb=(0.2, 0.4, 0.6), colorDisplayOption=True)  # a color set of the mesh's own
        squash = cmds.nonLinear(scalp, type="squash")[0]
        cmds.setKeyframe(squash, attribute="factor", time=1, value=0.0)
        cmds.setKeyframe(squash, attribute="factor", time=10, value=0.6)
        groom = groomlab.create_groom(scalp, name="hair")  # the deformer's Orig as rest mesh
        curves = [cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)]),
                  cmds.curve(degree=1, point=[(0.7, 0.7, 0), (1.2, 1.0, 0.1), (1.6, 1.1, 0.3)]),
                  cmds.curve(degree=1, point=[(-0.7, 0.7, 0), (-1.2, 1.0, 0.1), (-1.6, 1.1, 0.3)])]
        groomlab.guides_from_curves(groom, curves)
        groomlab.generate_children(groom, "primary", count=4)
        groomlab.set_guide_attribute(groom, "frizz", 0.7, group="primary")
        presets.apply_preset(groom, "cinematic_hair", fit=False)
        groomlab.set_param(groom, "scatter", "density", 60.0)
        # Painted maps on params: a vertex map on the length, a texture map on the density, and a
        # color vertex map.
        groomlab.add_map(groom, "fringe", 0.5, kind="vertices")
        groomlab.brush_stroke(groom, "paint", [(0.0, 1.0, 0.0)], radius=0.8, strength=1.0, attribute="fringe", value=1.0)
        groomlab.use_map(groom, "length", "keep", "fringe")
        groomlab.add_map(groom, "dense", 1.0, kind="texture", resolution=64)
        groomlab.fill_map(groom, "dense", 0.8)
        groomlab.use_map(groom, "scatter", "density", "dense")
        groomlab.add_map(groom, "tint", kind="vertices", color=(0.8, 0.3, 0.1, 1.0))
        # An expression, a Control node feeding a param input, a bypassed node.
        groomlab.set_param(groom, "width", "width", groomlab.expression_map("0.5 + 0.5 * rand", 0.002, 0.004))
        cmds.setAttr(groomlab.add_control(groom, "noise", "amplitude", name="frizzy", default=0.08, max=0.3), 0.12)
        graph = groomlab.get_graph(groom)
        next(n for n in graph["nodes"] if n["name"] == "resample")["bypass"] = True
        groomlab.set_graph(groom, graph)
        # Parts and flow.
        part = cmds.curve(degree=1, point=[(0.05, 1.0, -0.6), (0.05, 1.0, 0.0), (0.05, 1.0, 0.6)])
        groomlab.add_part(groom, [part], delete_curves=True)
        groomlab.flow_stroke(groom, [(0.0, 0.95, 0.3), (0.3, 0.9, 0.3)], radius=0.3)
        # A shot corrective keyed around frame 5, sculpted.
        layer = shots.add_layer(groom, frame=5, fade=3)
        cmds.currentTime(5)
        groomlab.brush_stroke(groom, "comb", [(0.0, 1.7, 0.1), (0.4, 1.8, 0.4)], radius=0.6)
        shots.set_target(groom, None)
        cmds.currentTime(1)
        # Cards with a bake.
        groomlab.create_cards(groom, max_cards=12)
        groomlab.bake_cards(groom, resolution=64)
        groomlab.set_follow_scalp(groom, True)
        self.assertEqual(groomlab.errors(groom), [])
        return groom, layer

    def test_maya_round_trip(self):
        """Maya -> .groom -> Maya: the same strands on every frame (the scalp cache drives the made
        scalp), the same stored data, and an equivalent file when exported again."""
        new_scene("source")
        groom, layer = self.rich_groom()
        original = strand_points(groom, (1, 4, 7, 10))
        stats = groomlab.card_stats(groom)
        self.assertTrue(stats["baked"])
        source_abc = os.path.join(WORK_DIR, "source.abc")
        groomlab.export_alembic(groom, source_abc, density=DENSITY)
        weights = [cmds.getAttr(shots.weight_plug(groom, layer), time=f) for f in range(1, 11)]
        stored = {attr: cmds.getAttr(groom + "." + attr) for attr in ("graph", "guideData", "partData", "flowData")}
        stamp = groomlab._card_maps(groom)["Strands"]
        # The last transfer's marker pairs travel with the groom file.
        groomlab.set_transfer_markers(groom, [((0, 1, 0), (5, 1, 0)), ((1, 0, 0), (6, 0, 0)), ((0, 0, 1), (5, 0, 1))])
        # Save Groom File's starting file: a document of the scene's grooms beside the scene, named
        # after it (two versions of a scene in one folder never share one), outside the groomlab folder.
        from groomlab import paths
        from groomlab.ui import menu
        start = menu.groom_source_file(groom)
        self.assertEqual(start, paths.scene_folder().replace("\\", "/") + "/" + paths.scene_stem() + ".groom")
        self.assertNotIn("/exports/", start)
        path = os.path.join(WORK_DIR, "exports", "hair.groom")
        os.makedirs(os.path.dirname(path))
        written = groomlab.export_groom(groom, path, scalp_cache=True, start=1, end=10)
        self.assertEqual([os.path.basename(p) for p in written], ["hair.groom", "hair.scalp.abc"])
        exported = read_json(path)
        self.assertEqual(exported["upAxis"], "Y")
        self.assertEqual([m["new"]["point"] for m in exported["transferMarkers"]], [[5, 1, 0], [6, 0, 0], [5, 0, 1]])
        # The files the groom owns in the file's own folder (groomlab/<file>/<kind>/<groom>: the core's
        # groom_package.h), named relative to it: the file stands alone.
        self.assertEqual(exported["name"], "hair")
        self.assertEqual(exported["cardBake"], {"prefix": "groomlab/hair/cards/hair/hair_", "strands": stamp})
        for name in ("Color.tga", "Atlas.json", "Preview.png"):
            self.assertTrue(os.path.isfile(os.path.join(WORK_DIR, "exports", "groomlab", "hair", "cards", "hair", "hair_" + name)), name)
        self.assertEqual(exported["maps"]["dense"]["texture"], "groomlab/hair/masks/hair/dense.png")

        new_scene("imported")
        imported = groomlab.import_groom(path)
        self.assertEqual(imported.split("|")[-1], "hairShape")
        self.assertEqual(len(groomlab.get_transfer_markers(imported)), 3)
        # One undo step takes the import away (the copied files too; the scalp cache AbcImport read
        # stays: AbcImport does not undo); redo makes it again.
        copied = groomlab.resolve_path(json.loads(cmds.getAttr(imported + ".scalpMaps"))["dense"]["texture"], imported)
        self.assertTrue(os.path.exists(copied))
        cmds.undo()
        self.assertFalse(cmds.objExists("hair"))
        self.assertFalse(cmds.ls(type="groomLabShape"))
        self.assertFalse(os.path.exists(copied))
        self.assertTrue(cmds.objExists("hairScalpCache"))
        cmds.redo()
        self.assertTrue(cmds.objExists(imported))
        self.assertTrue(os.path.exists(copied))
        self.assertEqual(groomlab.errors(imported), [])
        # The scalp: the cache's own mesh (export_groom's caches carry the UVs and color sets), its
        # rest pose from the file.
        scalp = groomlab.scalp_mesh(imported)
        self.assertTrue(cmds.ls(cmds.listHistory(scalp), type="AlembicNode"))
        self.assertFalse(cmds.objExists("hairScalp"))
        self.assertIn("colorSet1", cmds.polyColorSet(scalp, query=True, allColorSets=True))
        self.assertTrue(groomlab.rest_mesh(imported))
        self.assertTrue(cmds.getAttr(imported + ".followScalp"))
        # The stored data: the same texts (the graph, guides, parts, flow), the maps' values and images.
        for attr, text in stored.items():
            if attr == "graph" and text:  # a groom file writes every param (graph_io.h)
                text = groomlab_core.complete_graph_json(text)
            self.assertEqual(json.loads(cmds.getAttr(imported + "." + attr) or "null"), json.loads(text or "null"), attr)
        maps = json.loads(cmds.getAttr(imported + ".scalpMaps"))
        self.assertEqual(sorted(maps), ["dense", "fringe", "tint"])
        self.assertEqual(maps["tint"]["channels"], 4)
        self.assertEqual(maps["dense"]["texture"], "groomlab/imported/masks/hair/dense.png")  # copied into this scene
        self.assertEqual(file_bytes(groomlab.resolve_path(maps["dense"]["texture"], imported)),
                         file_bytes(os.path.join(WORK_DIR, "exports", "groomlab", "hair", "masks", "hair", "dense.png")))
        self.assertEqual(list(groomlab.list_controls(imported)), ["frizzy"])
        self.assertAlmostEqual(groomlab.list_controls(imported)["frizzy"], 0.12, places=6)
        # The bake in this scene's cards folder, its stamp kept: the cards are not stale.
        bake = groomlab._card_maps(imported)
        self.assertEqual(bake["Strands"], stamp)
        self.assertTrue(bake["Atlas"].startswith("groomlab/imported/cards/hair/"))
        self.assertFalse(groomlab.card_stats(imported)["stale"])
        # The corrective's weight keyed on the transform as the file's curve.
        self.assertEqual([round(cmds.getAttr(shots.weight_plug(imported, layer), time=f), 3) for f in range(1, 11)],
                         [round(w, 3) for w in weights])
        # The strands: bit for bit on the first frame, the cache's frames within float rounding.
        imported_abc = os.path.join(WORK_DIR, "imported.abc")
        groomlab.export_alembic(imported, imported_abc, density=DENSITY)
        self.assertEqual(hashes(imported_abc), hashes(source_abc))
        again = strand_points(imported, (1, 4, 7, 10))
        for frame in (1, 4, 7, 10):
            self.assertPointsClose(again[frame], original[frame], 1e-3, "frame %d" % frame)
        self.assertNotEqual(again[1], again[10])  # the scalp does move them

        # Exported again: an equivalent file (the same content; paths are this scene's).
        path2 = os.path.join(WORK_DIR, "exports", "hair2.groom")
        groomlab.export_groom(imported, path2)
        a, b = read_json(path), read_json(path2)
        for key in ("graph", "hierarchy", "parts", "flow", "controls", "followScalp", "upAxis", "units", "guides"):
            self.assertEqual(a.get(key), b.get(key), key)
        self.assertEqual(sorted(a["scalp"]), sorted(b["scalp"]))
        for key in ("triangles", "uvs"):
            self.assertEqual(a["scalp"][key], b["scalp"][key], key)
        for key in ("points", "restPoints"):  # the scalp now is the cache's frame through the blendShape
            self.assertEqual(len(a["scalp"][key]), len(b["scalp"][key]), key)
            self.assertLess(max(abs(p - q) for p, q in zip(a["scalp"][key], b["scalp"][key])), 1e-5, key)
        self.assertEqual(a["scalp"]["colorSets"], b["scalp"]["colorSets"])
        self.assertEqual({k: (m["kind"], m.get("channels")) for k, m in a["maps"].items()},
                         {k: (m["kind"], m.get("channels")) for k, m in b["maps"].items()})
        self.assertEqual(file_bytes(os.path.join(os.path.dirname(path2), b["maps"]["dense"]["texture"])),
                         file_bytes(os.path.join(os.path.dirname(path), a["maps"]["dense"]["texture"])))
        self.assertEqual(b["cardBake"]["strands"], stamp)
        layers_a, layers_b = a["correctives"]["layers"], b["correctives"]["layers"]
        self.assertEqual([dict(l, weightCurve=None) for l in layers_a], [dict(l, weightCurve=None) for l in layers_b])
        for (ta, wa), (tb, wb) in zip(layers_a[0]["weightCurve"], layers_b[0]["weightCurve"]):
            self.assertAlmostEqual(ta, tb, places=5)
            self.assertAlmostEqual(wa, wb, places=3)

    def test_reload_in_place(self):
        """Reload from Groom File: the file's edits replace the groom's, the node, its scalp and its
        connections stay; one undo step; Save to Groom File writes back to the same file."""
        new_scene("reload")
        scalp = cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10)[0]
        groom = groomlab.create_groom(scalp, name="fur")
        cmds.setAttr(groom + ".previewDensity", DENSITY)
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)])
        groomlab.guides_from_curves(groom, [curve])
        groomlab.generate_children(groom, "primary", count=5)
        groomlab.add_map(groom, "dense", 1.0, kind="texture", resolution=32)
        groomlab.use_map(groom, "scatter", "density", "dense")
        layer = shots.add_layer(groom, frame=5, fade=3)  # keyed 0 -> 1 -> 0: three keys
        shots.set_target(groom, None)
        xform = cmds.listRelatives(groom, parent=True, fullPath=True)[0]
        with self.assertRaises(ValueError):
            groomlab.save_groom(groom)  # linked to no file yet
        path = os.path.join(WORK_DIR, "reload", "fur.groom")
        groomlab.save_groom(groom, path)
        self.assertEqual(groomlab.groom_file(groom), path.replace("\\", "/"))
        self.assertEqual(cmds.getAttr(groom + ".groomFile"), "fur.groom")  # relative to the scene

        # The file edited elsewhere (as the app saves it): a param, a group renamed, a vertex map.
        data = read_json(path)
        node = next(n for n in data["graph"]["nodes"] if n["name"] == "length")
        node.setdefault("params", {})["keep"] = 0.25
        data["hierarchy"]["groups"][0]["name"] = "main"
        with open(path, "w") as f:
            json.dump(data, f)
        before = {attr: cmds.getAttr(groom + "." + attr) for attr in ("graph", "guideData", "scalpMaps")}
        points = strand_points(groom, (1,))[1]
        texture = json.loads(before["scalpMaps"])["dense"]["texture"]

        groomlab.reload_groom(groom)
        self.assertEqual(groomlab.scalp_mesh(groom).split("|")[-1], cmds.listRelatives(scalp, shapes=True)[0])
        self.assertEqual(groomlab.get_graph(groom)["nodes"][[n["name"] for n in groomlab.get_graph(groom)["nodes"]]
                                                           .index("length")]["params"]["keep"], 0.25)
        self.assertEqual([g["name"] for g in groomlab.list_guide_groups(groom)][0], "main")
        self.assertEqual(json.loads(cmds.getAttr(groom + ".scalpMaps"))["dense"]["texture"], texture)  # read in place
        # The corrective's keys stay as they were (the file's weight curve is what they key).
        self.assertEqual(cmds.keyframe(xform, attribute=layer, query=True, keyframeCount=True), 3)
        self.assertNotEqual(strand_points(groom, (1,))[1], points)
        # One undo step back to before, and redo.
        cmds.undo()
        for attr, text in before.items():
            self.assertEqual(cmds.getAttr(groom + "." + attr), text, attr)
        self.assertEqual(strand_points(groom, (1,))[1], points)
        cmds.redo()
        self.assertEqual([g["name"] for g in groomlab.list_guide_groups(groom)][0], "main")

        # Saved back: the same file, read the same.
        groomlab.save_groom(groom)
        self.assertEqual(read_json(path)["hierarchy"]["groups"][0]["name"], "main")

        # A file of another scalp is refused, the groom untouched.
        other = os.path.join(WORK_DIR, "reload", "other.groom")
        new_groom = groomlab.create_groom(cmds.polySphere(subdivisionsX=8, subdivisionsY=6)[0], name="other")
        groomlab.export_groom(new_groom, other)
        with self.assertRaises(ValueError) as caught:
            groomlab.reload_groom(groom, other)
        self.assertIn("vertices", str(caught.exception))
        self.assertEqual([g["name"] for g in groomlab.list_guide_groups(groom)][0], "main")

    def test_scalp_must_match(self):
        """A mesh given (or selected) must be the file's scalp; the error points at Transfer Groom to
        Scalp. The matching mesh is attached as it is (no scalp made)."""
        new_scene("match")
        scalp = cmds.polySphere(radius=1, subdivisionsX=16, subdivisionsY=8, name="skull")[0]
        groom = groomlab.create_groom(scalp, name="fuzz")
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0.1, 1.5, 0.1)])])
        path = os.path.join(WORK_DIR, "match", "fuzz.groom")
        groomlab.export_groom(groom, path)
        points = strand_points(groom, (1,))[1]

        new_scene("match2")
        wrong = cmds.polySphere(radius=1, subdivisionsX=18, subdivisionsY=8)[0]
        cmds.select(wrong)
        with self.assertRaises(ValueError) as caught:
            groomlab.import_groom(path)
        self.assertIn("Transfer Groom to Scalp", str(caught.exception))
        self.assertFalse(cmds.ls(type="groomLabShape"))
        right = cmds.polySphere(radius=1, subdivisionsX=16, subdivisionsY=8, name="skull")[0]
        cmds.select(right)
        imported = groomlab.import_groom(path)
        self.assertEqual(groomlab.scalp_mesh(imported).split("|")[-1], cmds.listRelatives(right, shapes=True)[0])
        self.assertFalse(cmds.objExists("fuzzScalp"))
        self.assertEqual(strand_points(imported, (1,))[1], points)

    def test_undone_import_flushes(self):
        """An import undone (the scalp made from the file taken out with the groom), redone, undone
        again, then the undo queue flushed by a new scene while it stands undone."""
        new_scene("flush")
        groom = groomlab.create_groom(cmds.polySphere(subdivisionsX=10, subdivisionsY=6)[0], name="tuft")
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0.1, 1.5, 0.1)])])
        path = os.path.join(WORK_DIR, "flush", "tuft.groom")
        groomlab.export_groom(groom, path)
        new_scene("flush2")
        imported = groomlab.import_groom(path)
        cmds.undo()
        self.assertFalse(cmds.objExists("tuftScalp"))
        cmds.redo()
        self.assertEqual(groomlab.scalp_mesh(imported).split("|")[-1], "tuftScalpShape")
        count = cmds.getAttr(imported + ".outStrandCount")
        self.assertGreater(count, 0)
        # The made scalp is saved with the scene (a mesh of its own, no history).
        scene = cmds.file(save=True, type="mayaAscii")
        cmds.file(scene, open=True, force=True)
        self.assertEqual(cmds.polyEvaluate("tuftScalp", vertex=True), 52)
        self.assertEqual(cmds.getAttr("tuftShape.outStrandCount"), count)
        groomlab.import_groom(path, name="tuft2", use_selection=False)
        cmds.undo()
        cmds.file(new=True, force=True)
        self.assertFalse(cmds.ls(type="groomLabShape"))

    def test_z_up_file(self):
        """A file written Z-up (a Z-up scene, Blender) comes into a Y-up scene turned a quarter about X."""
        new_scene("zup")
        cmds.upAxis(axis="z")
        try:
            scalp = cmds.polySphere(radius=1, subdivisionsX=16, subdivisionsY=8)[0]
            groom = groomlab.create_groom(scalp, name="zfur")
            # A Gravity pulling down the Z-up world's way: a world direction, turned with the file.
            graph = json.loads(json.dumps(CLASSIC_GRAPH))
            graph["nodes"].append({"name": "gravity", "type": "Gravity", "params": {"direction": [0, 0, -1], "strength": 0.7}})
            graph["connections"] = [c for c in graph["connections"] if c["to"] != "width"]
            graph["connections"] += [{"from": "length", "to": "gravity", "input": "strands"},
                                     {"from": "gravity", "to": "width", "input": "strands"}]
            groomlab.set_graph(groom, graph)
            groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 0, 1), (0.1, 0.1, 1.5), (0.3, 0.1, 1.9)])])
            groomlab.generate_children(groom, "primary", count=4)
            z_points = strand_points(groom, (1,))[1]
            path = os.path.join(WORK_DIR, "zup", "zfur.groom")
            groomlab.export_groom(groom, path)
            self.assertEqual(read_json(path)["upAxis"], "Z")
        finally:
            cmds.upAxis(axis="y")
        new_scene("yup")
        info = groomlab.groom_file_info(path)
        self.assertEqual((info["fileUpAxis"], info["upAxis"]), ("Z", "Y"))
        imported = groomlab.import_groom(path)
        self.assertEqual(groomlab.errors(imported), [])
        gravity = next(n for n in groomlab.get_graph(imported)["nodes"] if n["name"] == "gravity")
        self.assertEqual(gravity["params"]["direction"], [0.0, -1.0, 0.0])  # Y-up's down
        y_points = strand_points(imported, (1,))[1]
        # The same strands turned (a turned world sums in another order: within float rounding,
        # which Scatter's relaxation grows a little, and within the accuracy of the scalp's distance
        # field, whose grid does not turn, where Interpolate keeps the strands clear of the scalp).
        self.assertPointsClose(y_points, [(x, z, -y) for x, y, z in z_points], 6e-3)
        # Exported from the Y-up scene, the file says so.
        path2 = os.path.join(WORK_DIR, "yup", "zfur.groom")
        groomlab.export_groom(imported, path2)
        self.assertEqual(read_json(path2)["upAxis"], "Y")

    def test_map_names_follow_one_rule(self):
        """A map named as an older standalone app allowed (-, a leading digit) imports under the
        name Maya's rule gives it (the core sanitizes on read), its readers with it; the app's
        rule is Maya's now, so any name it accepts Maya does too."""
        new_scene("names")
        groom = groomlab.create_groom(cmds.polySphere(subdivisionsX=12, subdivisionsY=6)[0], name="fuzz")
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0.1, 1.5, 0.1)])])
        groomlab.add_map(groom, "fringe", 0.5, kind="vertices")
        groomlab.use_map(groom, "length", "keep", "fringe")
        path = os.path.join(WORK_DIR, "names", "fuzz.groom")
        groomlab.export_groom(groom, path)
        with open(path) as f:
            text = f.read()
        with open(path, "w") as f:
            f.write(text.replace('"fringe"', '"2-fringe"'))  # catalog, color set and the length's map
        for bad in ("2-fringe", "fringe.r", "hair-mask"):
            with self.assertRaises(ValueError):
                groomlab.add_map(groom, bad, kind="vertices")
            self.assertTrue(groomlab_core.map_name_error(bad))

        new_scene("names2")
        imported = groomlab.import_groom(path)
        self.assertIn("_2_fringe", groomlab.list_maps(imported))
        self.assertNotIn("2-fringe", groomlab.list_maps(imported))
        self.assertEqual(groomlab.map_users(imported, "_2_fringe"), [("length", "keep")])
        self.assertEqual(groomlab.errors(imported), [])

    def test_a_quad_scalp_imports_as_quads(self):
        """A groom file keeps its scalp's polygons ("polygons": triangles per polygon, as the mesh it
        was made on had them): imported with no mesh, the scalp is made of those quads. Split as
        Maya splits them (the app splits OBJ quads so), the groom is as it was; split along the
        other diagonals (an older app's file), the roots move onto Maya's triangles."""
        folder = new_scene("quads")
        plane = cmds.polyPlane(width=2, height=2, subdivisionsX=8, subdivisionsY=8)[0]
        cmds.move(0, 0.06, 0, plane + ".vtx[20:40]", relative=True)  # bent a little: the two splits differ a little
        groom = groomlab.create_groom(plane, name="fur")
        cmds.setAttr(groom + ".previewDensity", 1.0)
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(x, 0.4, z), (x + 0.1, 0.8, z), (x + 0.2, 1.1, z + 0.1)])
                                            for x, z in ((-0.6, -0.5), (0.1, 0.05), (0.55, 0.4))])

        def guide_cvs(g):
            """The guides' points (Guides to Curves), sorted."""
            from groomlab import utils
            made = utils.guides_to_curves(g)
            out = sorted(tuple(round(c, 6) for c in cmds.pointPosition(cv)) for crv in made["curves"]
                         for cv in cmds.ls(crv + ".cv[*]", flatten=True))
            cmds.delete(made["group"])
            return out

        guides = guide_cvs(groom)
        path = os.path.join(folder, "fur.groom")
        groomlab.export_groom(groom, path)
        count = cmds.getAttr(groom + ".outStrandCount")
        stored = read_json(path)
        self.assertEqual(stored["scalp"]["polygons"], [2] * 64)

        new_scene("quads2")
        imported = groomlab.import_groom(path)
        scalp = groomlab.scalp_mesh(imported)
        self.assertEqual(cmds.polyEvaluate(scalp, face=True), 64)  # quads, not 128 triangles
        self.assertEqual(groomlab.errors(imported), [])
        cmds.setAttr(imported + ".previewDensity", 1.0)
        self.assertEqual(cmds.getAttr(imported + ".outStrandCount"), count)
        for p, q in zip(guide_cvs(imported), guides):
            self.assertLess(max(abs(a - b) for a, b in zip(p, q)), 1e-4)

        # The other diagonal in every quad (its corners' UVs with them; the fingerprint dropped).
        scalp_json = stored["scalp"]
        tris, uvs = scalp_json["triangles"], scalp_json.get("uvs")
        flipped, flipped_uvs = [], []
        for q in range(64):
            corners = [(tris[6 * q + k], uvs[12 * q + 2 * k:12 * q + 2 * k + 2] if uvs else None) for k in range(6)]
            edges = {}
            for t in range(2):
                for c in range(3):
                    a, b = corners[3 * t + c], corners[3 * t + (c + 1) % 3]
                    edges[(a[0], b[0])] = a
            boundary = {a: edges[(a, b)] for (a, b) in edges if (b, a) not in edges}
            nxt = {a: b for (a, b) in edges if (b, a) not in edges}
            loop = [next(iter(nxt))]
            while len(loop) < 4:
                loop.append(nxt[loop[-1]])
            # The split the file does not have: the diagonal its two triangles do not share.
            shared = [(a, b) for (a, b) in edges if (b, a) in edges][0]
            i0 = 0 if {loop[0], loop[2]} != set(shared) else 1
            v = loop[i0:] + loop[:i0]
            for t in ((v[0], v[1], v[2]), (v[0], v[2], v[3])):
                for vert in t:
                    flipped.append(vert)
                    if uvs:
                        flipped_uvs.extend(boundary[vert][1])
        scalp_json["triangles"] = flipped
        if uvs:
            scalp_json["uvs"] = flipped_uvs
        scalp_json.pop("topology", None)
        other = os.path.join(folder, "fur_other_split.groom")
        with open(other, "w") as f:
            json.dump(stored, f)
        new_scene("quads3")
        moved = groomlab.import_groom(other)
        self.assertEqual(cmds.polyEvaluate(groomlab.scalp_mesh(moved), face=True), 64)
        self.assertEqual(groomlab.errors(moved), [])
        cmds.setAttr(moved + ".previewDensity", 1.0)
        self.assertAlmostEqual(cmds.getAttr(moved + ".outStrandCount"), count, delta=count * 0.1)
        # The guides where they were (moved onto Maya's triangles at the same points; the plane's
        # bend differs between the splits by a little).
        moved_cvs = guide_cvs(moved)
        self.assertEqual(len(moved_cvs), len(guides))
        for p, q in zip(moved_cvs, guides):
            self.assertLess(max(abs(a - b) for a, b in zip(p, q)), 0.05)

    def test_an_imported_groom_keeps_its_name(self):
        """A groom saved into a file of another name (groomLab1 into filetest02.groom) comes back as
        groomLab1: the file keeps each groom's name."""
        folder = new_scene("names_kept")
        groom = groomlab.create_groom(cmds.polySphere(subdivisionsX=12, subdivisionsY=6)[0])
        name = cmds.listRelatives(groom, parent=True)[0]
        path = os.path.join(folder, "filetest02.groom")
        groomlab.export_groom(groom, path)
        self.assertEqual(read_json(path)["name"], groomlab.asset_name(groom))
        new_scene("names_kept2")
        imported = groomlab.import_groom(path)
        self.assertEqual(cmds.listRelatives(imported, parent=True)[0], name)

    def test_other_scalps_come_back_as_meshes(self):
        """A groom on another scalp too (Add > Scalp: the file's "scalps") imports with that scalp
        made as a mesh of its own and kept as the groom's other scalp: the same hair."""
        from groomlab import extra_scalps
        folder = new_scene("ears")
        head = cmds.polySphere(radius=1, subdivisionsX=16, subdivisionsY=8)[0]
        ear = cmds.polyCube(width=0.3, height=0.3, depth=0.3)[0]
        cmds.move(1.2, 0, 0, ear)
        groom = groomlab.create_groom(head, name="fur")
        extra_scalps.add_scalp(groom, ear, name="ear")
        cmds.setAttr(groom + ".previewDensity", 1.0)
        count = cmds.getAttr(groom + ".outStrandCount")
        path = os.path.join(folder, "fur.groom")
        groomlab.export_groom(groom, path)
        self.assertEqual([s["name"] for s in read_json(path)["scalps"]], ["ear"])
        self.assertEqual([s["name"] for s in groomlab.groom_file_info(path)["scalps"]], ["ear"])

        new_scene("ears2")
        imported = groomlab.import_groom(path)
        scalps = extra_scalps.scalps(imported)
        self.assertEqual([s["name"] for s in scalps], ["ear"])
        self.assertEqual(cmds.polyEvaluate(scalps[0]["mesh"], vertex=True), 8)
        self.assertEqual(groomlab.errors(imported), [])
        cmds.setAttr(imported + ".previewDensity", 1.0)
        self.assertEqual(cmds.getAttr(imported + ".outStrandCount"), count)

    def test_a_file_of_several_grooms_imports_each(self):
        """The app saves a document of several grooms in one file (the first at the top, the others
        in "grooms"): Import Groom makes each on a mesh of its own scalp, named after it, not
        linked to the file (saving one groom there would drop the others)."""
        folder = new_scene("several")
        hair = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10)[0], name="hair")
        beard = groomlab.create_groom(cmds.polyCube(width=1, height=1, depth=1, subdivisionsX=4, subdivisionsY=4)[0],
                                      name="beard")
        groomlab.set_param(beard, "scatter", "density", 40.0)
        counts = [cmds.polyEvaluate(groomlab.scalp_mesh(g), vertex=True) for g in (hair, beard)]
        groomlab.export_groom(hair, os.path.join(folder, "hair.groom"))
        groomlab.export_groom(beard, os.path.join(folder, "beard.groom"))
        files = [groomlab_core.read_groom_file(os.path.join(folder, n + ".groom")) for n in ("hair", "beard")]
        files[0].name, files[1].name = "hair", "beard"
        path = os.path.join(folder, "head.groom")
        groomlab_core.write_groom_files(path, files)
        self.assertEqual(groomlab.groom_file_info(path)["grooms"], ["hair", "beard"])
        self.assertEqual(groomlab.groom_file_info(path, 1)["name"], "beard")
        with self.assertRaises(RuntimeError):
            groomlab.groom_file_info(path, 2)

        new_scene("several2")
        cmds.undoInfo(state=True, infinity=True)  # (mayapy's queue: on, whatever ran before)
        made = groomlab.import_grooms(path)
        self.assertEqual([g.split("|")[-1] for g in made], ["hairShape", "beardShape"])
        scalps = [groomlab.scalp_mesh(g) for g in made]
        self.assertEqual(len(set(scalps)), 2)
        self.assertEqual([cmds.polyEvaluate(m, vertex=True) for m in scalps], counts)
        self.assertAlmostEqual(next(n for n in groomlab.get_graph(made[1])["nodes"] if n["name"] == "scatter")
                               ["params"]["density"], 40.0)
        for g in made:
            self.assertTrue(cmds.getAttr(g + ".groomFile"))  # linked: its own entry, by name
            self.assertEqual(groomlab.errors(g), [])
        self.assertEqual([groomlab.groom_entry(g, path) for g in made], [0, 1])
        # Saving one groom writes its own entry; the other stays. Reload finds each by name.
        groomlab.set_param(made[1], "scatter", "density", 12.0)
        groomlab.save_groom(made[1])
        self.assertEqual(groomlab.groom_file_info(path)["grooms"], ["hair", "beard"])
        hair_graph = read_json(path)["graph"]
        groomlab.set_param(made[0], "scatter", "density", 99.0)
        groomlab.reload_groom(made[0])
        scatter = next(n for n in groomlab.get_graph(made[0], expand_default=True)["nodes"] if n["name"] == "scatter")
        self.assertNotEqual(scatter.get("params", {}).get("density"), 99.0)  # the file's again
        self.assertEqual(read_json(path)["graph"], hair_graph)
        self.assertAlmostEqual(next(n for n in read_json(path)["grooms"][0]["graph"]["nodes"] if n["name"] == "scatter")
                               ["params"]["density"], 12.0)
        # One undo step takes them all away.
        new_scene("several3")
        cmds.undoInfo(state=True, infinity=True)
        groomlab.import_grooms(path)
        cmds.undo()
        self.assertFalse(cmds.ls(type="groomLabShape"))
        # One groom: as before, linked to its file.
        one = groomlab.import_grooms(os.path.join(folder, "hair.groom"))
        self.assertEqual(len(one), 1)
        self.assertTrue(cmds.getAttr(one[0] + ".groomFile"))

    def test_a_texture_param_reads_the_map_maya_paints(self):
        """The app and Blender paint a setting into a texture map the param reads by its file
        ({"texture": <the map's image>}). Imported into a scene (here the file's own folder and
        name, as the app's groom and a Maya scene beside it share groomlab/<name>), the map's image
        is copied for the groom: the param reads the copy, so painting it in Maya moves the groom,
        and Paint on the param continues that map. The file's own image stays as it was."""
        folder = new_scene("app_paint")
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0], name="hair")
        cmds.setAttr(groom + ".previewDensity", 1.0)
        image = os.path.join(folder, "groomlab", "app_paint", "masks", "scatter_density.png").replace("\\", "/")
        groomlab.add_map(groom, "scatter_density", 1.0, kind="texture", resolution=32, texture=image)
        groomlab.set_param(groom, "scatter", "density", groomlab.texture_map(image, "r", 0.0, 60.0))
        path = os.path.join(folder, "app_paint.groom")
        groomlab.export_groom(groom, path)
        stored = read_json(path)
        # (Written into the groom's own folder of the file, the param with it: groom_package.h.)
        self.assertEqual(stored["maps"]["scatter_density"]["texture"], "groomlab/app_paint/masks/hair/scatter_density.png")
        density = next(n for n in stored["graph"]["nodes"] if n["name"] == "scatter")["params"]["density"]
        self.assertEqual(density["texture"], "groomlab/app_paint/masks/hair/scatter_density.png")
        before = file_bytes(image)

        new_scene("app_paint")
        imported = groomlab.import_groom(path)
        cmds.setAttr(imported + ".previewDensity", 1.0)
        copy = groomlab.map_texture(imported, "scatter_density")
        self.assertNotEqual(os.path.normcase(copy), os.path.normcase(image))
        param = next(n for n in groomlab.get_graph(imported)["nodes"] if n["name"] == "scatter")["params"]["density"]
        self.assertEqual(os.path.normcase(groomlab.resolve_path(param["texture"], imported)), os.path.normcase(copy))
        self.assertEqual(groomlab.param_map(imported, "scatter", "density"), ("scatter_density", "luminance"))
        count = cmds.getAttr(imported + ".outStrandCount")
        self.assertGreater(count, 100)
        groomlab.brush_stroke(imported, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="scatter_density",
                              value=0.0, paint_kind="texture")
        self.assertLess(cmds.getAttr(imported + ".outStrandCount"), count * 0.9)  # the stroke's top of the sphere bare
        self.assertEqual(file_bytes(image), before)
        self.assertEqual(groomlab.errors(imported), [])

        def reads():
            value = next(n for n in groomlab.get_graph(imported)["nodes"] if n["name"] == "scatter")["params"]["density"]
            return os.path.normcase(os.path.normpath(groomlab.resolve_path(value["texture"], imported)))

        # Renamed (its file follows the name) and relinked in Maya: the param follows the map.
        groomlab.rename_map(imported, "scatter_density", "thick")
        self.assertEqual(reads(), os.path.normcase(os.path.normpath(groomlab.map_texture(imported, "thick"))))
        self.assertIn("thick", os.path.basename(groomlab.map_texture(imported, "thick")))
        cmds.undo()
        self.assertEqual(reads(), os.path.normcase(os.path.normpath(copy)))
        cmds.redo()
        relinked = os.path.join(folder, "library", "thick.png").replace("\\", "/")
        os.makedirs(os.path.dirname(relinked))
        shutil.copyfile(groomlab.map_texture(imported, "thick"), relinked)
        groomlab.set_map_texture(imported, "thick", relinked)
        self.assertEqual(reads(), os.path.normcase(os.path.normpath(relinked)))
        self.assertEqual(groomlab.errors(imported), [])

    @unittest.skipUnless(APP, "the standalone app is not built (GROOMLAB_BUILD_APP)")
    def test_app_to_maya(self):
        """A groom the standalone app saved (a stroke and a param edit, headless) imports into Maya
        and evaluates exactly as the app evaluates it."""
        folder = os.path.join(WORK_DIR, "app")
        os.makedirs(folder)
        saved, app_abc = os.path.join(folder, "strokes.groom"), os.path.join(folder, "app.abc")
        run(APP, os.path.join(DATA_DIR, "strokes", "strokes.groom"), "--set", "scatter.density=150", "--brush", "mode=comb",
            "--brush", "radius=120", "--stroke", "500,300 700,320 900,340", "--save", saved, "--export-alembic", app_abc,
            "--export-density", str(DENSITY))
        self.assertNotEqual(read_json(saved)["hierarchy"], read_json(os.path.join(DATA_DIR, "strokes", "strokes.groom"))["hierarchy"])
        new_scene("from_app")
        imported = groomlab.import_groom(saved)
        self.assertEqual(groomlab.errors(imported), [])
        maya_abc = os.path.join(folder, "maya.abc")
        groomlab.export_alembic(imported, maya_abc, density=DENSITY)
        self.assertEqual(hashes(maya_abc), hashes(app_abc))

    @unittest.skipUnless(APP, "the standalone app is not built (GROOMLAB_BUILD_APP)")
    def test_maya_to_app_and_back(self):
        """A Maya groom (maps, a card bake) opens in the app, which evaluates it exactly as Maya and
        keeps its bake; the app's save reloads into the Maya groom unchanged."""
        new_scene("to_app")
        scalp = cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10)[0]
        groom = groomlab.create_groom(scalp, name="mane")
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0.2, 1.6, 0.1), (0.5, 2.0, 0.2)]),
                                            cmds.curve(degree=1, point=[(0.7, 0.7, 0), (1.2, 1.0, 0.1), (1.6, 1.1, 0.3)])])
        groomlab.generate_children(groom, "primary", count=5)
        groomlab.add_map(groom, "fringe", 0.3, kind="vertices")
        groomlab.use_map(groom, "length", "keep", "fringe")
        groomlab.add_map(groom, "dense", 0.9, kind="texture", resolution=32)
        groomlab.use_map(groom, "scatter", "density", "dense")
        groomlab.create_cards(groom, max_cards=8)
        groomlab.bake_cards(groom, resolution=32)
        # The Graph Editor's layout: nodes moved and a backdrop (kept in the graph, graph_io.h).
        from groomlab import graph as gm
        model = gm.GraphModel.from_groom(groom)
        model.set_position("scatter", 123.5, -40.0)
        model.set_position("length", -7.0, 260.25)
        model.backdrops.append({"title": "Roots", "x": 1.0, "y": 2.0, "w": 300.0, "h": 200.0, "color": [70, 90, 120]})
        model.save_layout(groom)
        layout = {n: gm.GraphModel.from_groom(groom).position(n) for n in model.order}
        path = os.path.join(WORK_DIR, "to_app", "mane.groom")
        groomlab.save_groom(groom, path)
        maya_abc = os.path.join(WORK_DIR, "to_app", "maya.abc")
        groomlab.export_alembic(groom, maya_abc, density=DENSITY)
        points = strand_points(groom, (1,))[1]

        saved, app_abc = os.path.join(WORK_DIR, "to_app", "mane_app.groom"), os.path.join(WORK_DIR, "to_app", "app.abc")
        run(APP, path, "--save", saved, "--export-alembic", app_abc, "--export-density", str(DENSITY))
        self.assertEqual(hashes(app_abc), hashes(maya_abc))
        app_file = read_json(saved)
        self.assertEqual(app_file["upAxis"], "Y")
        self.assertEqual(app_file["cardBake"]["strands"], groomlab._card_maps(groom)["Strands"])  # the app took the bake
        # The app's file stands alone: the map copied into its own folder (groom_package.h).
        self.assertEqual(app_file["maps"]["dense"]["texture"], "groomlab/mane_app/masks/mane/dense.png")
        # The layout went through the app and its save as the app's node graph reads it.
        app_nodes = {n["name"]: n for n in app_file["graph"]["nodes"]}
        self.assertEqual(app_nodes["scatter"]["ui"], {"x": 123.5, "y": -40.0})
        self.assertEqual(app_file["graph"]["backdrops"][0]["title"], "Roots")

        groomlab.reload_groom(groom, saved)
        self.assertEqual(groomlab.errors(groom), [])
        self.assertEqual(strand_points(groom, (1,))[1], points)
        self.assertFalse(groomlab.card_stats(groom)["stale"])
        # Maya -> .groom -> app -> .groom -> Maya: every node where it was, the backdrop too.
        back = gm.GraphModel.from_groom(groom)
        self.assertEqual({n: back.position(n) for n in back.order}, layout)
        self.assertEqual([b["title"] for b in back.backdrops], ["Roots"])
        # And an import into a new scene reads the same.
        new_scene("from_app_layout")
        imported = groomlab.import_groom(saved)
        self.assertEqual(gm.GraphModel.from_groom(imported).position("length"), (-7.0, 260.25))


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
