# SPDX-License-Identifier: Apache-2.0
"""M5 grooming in Maya: guide selection, masks, symmetry, clump paint, hierarchy tools, reference photos.

Usage: mayapy test_grooming.py <GroomLab.mll> <maya/scripts dir>
"""

import hashlib
import json
import math
import os
import re
import shutil
import sys
import tempfile
import time
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)
os.environ["GROOMLAB_USER_CARD_PRESETS"] = tempfile.mkdtemp(prefix="groomlab_card_presets_")
os.environ.pop("GROOMLAB_CARD_PRESET_PATH", None)

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
from groomlab import sim  # noqa: E402

def _dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


# Sparse hand guides -> an even Scatter + Interpolate -> ToGuides -> Sculpt -> the strands'
# Interpolate (sculpt.h's workflow).
SCULPT_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "sparse", "type": "Guides"},
              {"name": "even", "type": "Scatter", "params": {"density": 6.0, "randomness": 0.0, "length": 0.5,
                                                             "previewAll": True}},
              {"name": "dense", "type": "Interpolate"}, {"name": "toGuides", "type": "ToGuides"},
              {"name": "sculpt1", "type": "Sculpt"}, {"name": "roots", "type": "Scatter", "params": {"density": 60.0}},
              {"name": "hair", "type": "Interpolate"}, {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "even", "input": "scalp"},
                    {"from": "even", "to": "dense", "input": "strands"},
                    {"from": "sparse", "to": "dense", "input": "guides"},
                    {"from": "scalp", "to": "dense", "input": "scalp"},
                    {"from": "dense", "to": "toGuides", "input": "strands"},
                    {"from": "toGuides", "to": "sculpt1", "input": "guides"},
                    {"from": "scalp", "to": "sculpt1", "input": "scalp"},
                    {"from": "scalp", "to": "roots", "input": "scalp"},
                    {"from": "roots", "to": "hair", "input": "strands"},
                    {"from": "sculpt1", "to": "hair", "input": "guides"},
                    {"from": "scalp", "to": "hair", "input": "scalp"},
                    {"from": "hair", "to": "output", "input": "strands"}],
}


def guide_points(groom):
    """{guide id: evaluated points} of every guide."""
    out = {}
    for g in groomlab.get_hierarchy(groom).get("guides", []):
        xyz = cmds.groomLabGuides(groom, guidePoints=str(g["id"]))
        out[str(g["id"])] = [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]
    return out


def moved(a, b):
    return max(math.sqrt(sum((x - y) ** 2 for x, y in zip(p, q))) for p, q in zip(a, b))


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def png_info(data):
    """(width, height, color type) of a PNG's bytes; its image data must inflate to the right size."""
    import struct
    import zlib
    assert data[:8] == b"\x89PNG\r\n\x1a\n", data[:8]
    width, height, depth, color = struct.unpack(">IIBB", data[16:26])
    at, idat = 8, b""
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        if kind == b"IDAT":
            idat += data[at + 8:at + 8 + length]
        at += 12 + length
    channels = {0: 1, 4: 2, 2: 3, 6: 4}[color]
    assert len(zlib.decompress(idat)) == height * (1 + width * channels)
    return width, height, color


def parse_glb(path):
    """A .glb's JSON and binary chunk, checked the way glTF 2.0 lays them out."""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    magic, version, length = struct.unpack("<III", data[:12])
    assert (magic, version, length) == (0x46546C67, 2, len(data)), (magic, version, length)
    json_length, json_kind = struct.unpack("<II", data[12:20])
    assert json_kind == 0x4E4F534A and json_length % 4 == 0
    doc = json.loads(data[20:20 + json_length].decode("utf-8"))
    bin_length, bin_kind = struct.unpack("<II", data[20 + json_length:28 + json_length])
    assert bin_kind == 0x004E4942 and bin_length % 4 == 0
    return doc, data[28 + json_length:28 + json_length + bin_length]


def gltf_accessor(doc, binary, index):
    """An accessor's values as a flat list."""
    import struct
    a = doc["accessors"][index]
    view = doc["bufferViews"][a["bufferView"]]
    width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    code = {5126: "f", 5125: "I", 5123: "H"}[a["componentType"]]
    count = a["count"] * width
    offset = view["byteOffset"] + a.get("byteOffset", 0)
    return list(struct.unpack_from("<%d%s" % (count, code), binary, offset))


def fbx_unit_scale(path):
    """The UnitScaleFactor a binary FBX declares (centimeters per unit), or None."""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    at = data.find(b"UnitScaleFactor")
    if at < 0:
        return None
    at += len(b"UnitScaleFactor")
    for _ in range(6):  # the property's type strings, then its value
        kind = data[at:at + 1]
        if kind == b"S":
            at += 5 + struct.unpack("<I", data[at + 1:at + 5])[0]
        elif kind == b"D":
            return struct.unpack("<d", data[at + 1:at + 9])[0]
        else:
            return None
    return None


class GroomingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        self.groom = groomlab.create_groom(self.scalp)
        cmds.setAttr(self.groom + ".previewDensity", 0.5)
        curves = []
        for angle in (0.35, 0.8, 1.2):
            n = (math.sin(angle), math.cos(angle), 0.2)
            curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.1 * c for c in n)]))
        groomlab.guides_from_curves(self.groom, curves)
        groomlab.mirror_group(self.groom, "primary", "x")
        groomlab.generate_children(self.groom, "primary", name="children", count=3)
        self.primaries = groomlab.guide_ids(self.groom, "primary")
        self.children = groomlab.guide_ids(self.groom, "children")

    def test_symmetry_sync_copies_children_and_undoes(self):
        # Bend a +X child by hand, then sync: its mirror image comes across, counts stay.
        plus = [c for c in self.children if cmds.groomLabGuides(self.groom, guidePoints=c)[0] > 0.05]
        before = cmds.getAttr(self.groom + ".guideData")
        created = groomlab.symmetry_sync(self.groom, "x")
        self.assertEqual(len(created), 3 + len(plus))
        self.assertEqual(len(groomlab.guide_ids(self.groom, "primary")), 6)
        self.assertEqual(len(groomlab.guide_ids(self.groom, "children")), 18)
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before)
        self.assertEqual(len(groomlab.symmetry_sync(self.groom, "x", from_negative=True)), 3 + 9)

    def test_one_current_groom(self):
        # The Groom Tools, the Graph Editor and the brushes share one current groom: a pick holds
        # while other things are selected, a groom selected since takes over (and stays current with
        # nothing selected), listeners hear each change, a new groom is current.
        other = groomlab.create_groom(cmds.polySphere(name="otherHead")[0], name="other")
        heard = []
        groomlab.on_current_groom(heard.append)
        self.addCleanup(groomlab.remove_current_groom_listener, heard.append)
        self.addCleanup(groomlab.set_current_groom, None)
        cmds.select(self.groom)
        groomlab.set_current_groom(other)  # picked with another groom selected: the pick leads
        self.assertEqual((groomlab.active_groom(), groomlab.pinned_groom(), heard[-1]), (other, other, other))
        cmds.select(self.scalp)  # a scalp, curves, anything else: the pick holds
        self.assertEqual(groomlab.active_groom(), other)
        cmds.select(self.groom)  # a groom selected since: current now
        self.assertEqual((groomlab.active_groom(), groomlab.pinned_groom(), heard[-1]), (self.groom, self.groom, self.groom))
        cmds.select(clear=True)
        self.assertEqual(groomlab.active_groom(), self.groom)
        cmds.select(other)
        cmds.select(self.groom, add=True)  # two grooms: the first selected
        self.assertEqual(groomlab.active_groom(), other)
        groomlab.set_current_groom(None)  # none current: the selected one leads
        cmds.select(self.groom)
        self.assertEqual(groomlab.active_groom(), self.groom)
        cmds.select(clear=True)
        groomlab.pin_new_groom(other)  # a groom made in the UI
        self.assertEqual(groomlab.active_groom(), other)
        n = len(heard)
        groomlab.remove_current_groom_listener(heard.append)
        groomlab.set_current_groom(self.groom)
        self.assertEqual(len(heard), n)

    def test_show_and_hide_the_hair_and_the_guides(self):
        # The Groom Tools' eyes and GroomLab > Display: one groom or every groom, its hair (displayHair:
        # the strands alone, the transform and the guides left) or its guides (displayGuides, every
        # guide group: the hair left), each one undo step.
        from groomlab.ui import menu
        other = groomlab.create_groom(cmds.polySphere(name="otherHead")[0], name="other")
        both = (self.groom, other)
        transform = cmds.listRelatives(self.groom, parent=True, fullPath=True)[0]
        self.assertTrue(groomlab.hair_visible(self.groom))
        groomlab.set_hair_visible(self.groom, False)
        self.assertEqual([groomlab.hair_visible(g) for g in both], [False, True])
        self.assertTrue(cmds.getAttr(transform + ".visibility"))  # the groom node stays: its guides draw
        self.assertTrue(groomlab.guides_visible(self.groom))
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)  # a viewport toggle: still evaluated
        cmds.undo()
        self.assertTrue(groomlab.hair_visible(self.groom))
        groomlab.set_guides_visible(self.groom, False)
        self.assertEqual([groomlab.guides_visible(g) for g in both], [False, True])
        cmds.undo()
        self.assertTrue(groomlab.guides_visible(self.groom))
        menu.ACTIONS["display.hideAllGuides"]()
        self.assertEqual([cmds.getAttr(g + ".displayGuides") for g in both], [False, False])
        cmds.undo()  # one step
        self.assertEqual([groomlab.guides_visible(g) for g in both], [True, True])
        self.assertFalse(groomlab.toggle_all_guides())  # any shown: all hidden
        self.assertTrue(groomlab.toggle_all_guides())
        self.assertEqual([groomlab.hair_visible(g) for g in both], [True, True])  # the guides' eye leaves the hair
        menu.ACTIONS["display.hideAllHair"]()
        self.assertEqual([groomlab.hair_visible(g) for g in both], [False, False])
        self.assertEqual([groomlab.guides_visible(g) for g in both], [True, True])
        cmds.undo()  # one step
        self.assertEqual([groomlab.hair_visible(g) for g in both], [True, True])
        menu.ACTIONS["display.hideAllHair"]()
        menu.ACTIONS["display.showAllHair"]()
        self.assertEqual([groomlab.hair_visible(g) for g in both], [True, True])
        menu.ACTIONS["display.hideAllGuides"]()
        menu.ACTIONS["display.showAllGuides"]()
        self.assertEqual([groomlab.guides_visible(g) for g in both], [True, True])
        cmds.setAttr(other + ".displayHair", lock=True)
        self.assertEqual(sorted(groomlab.set_all_hair_visible(False)), sorted(both))  # a locked one left as it is
        self.assertEqual([groomlab.hair_visible(g) for g in both], [False, True])
        cmds.setAttr(other + ".displayHair", lock=False)
        # Hardware 2.0 renders draw the hair whatever the viewport's eye says.
        from groomlab import hardware2_render
        self.assertIn(("displayHair", True), hardware2_render.GROOM_DISPLAY)

    def test_tube_sections_are_set_read_undone_and_filled(self):
        # Tube guides: a primary's tube made a flat, twisting ribbon; its children follow; one undo
        # step; Interpolate's fillTubes then fills it (tubeId on the strands).
        parent = self.primaries[0]
        kids = [c for c in self.children if json.loads(cmds.groomLabGuides(self.groom, tube=c))["shape"] == "round"]
        self.assertEqual(groomlab.get_tube(self.groom, parent)["shape"], "round")
        before = cmds.getAttr(self.groom + ".guideData")
        tips = {c: cmds.groomLabGuides(self.groom, guidePoints=c)[-3:] for c in kids}
        changed = groomlab.set_tube(self.groom, guides=[parent], width=[0.3, 0.15], aspect=0.25, twist=[0.0, 1.5],
                                    shape="ribbon")
        self.assertEqual(changed, 1)
        tube = groomlab.get_tube(self.groom, parent)
        self.assertEqual(tube["shape"], "ribbon")
        self.assertEqual(len(tube["width"]), 2)
        self.assertAlmostEqual(tube["width"][0], 0.3, places=5)
        self.assertAlmostEqual(tube["aspect"][0], 0.25, places=5)
        self.assertAlmostEqual(tube["twist"][1], 1.5, places=5)
        moved = [c for c in kids if cmds.groomLabGuides(self.groom, guidePoints=c)[-3:] != tips[c]]
        self.assertEqual(len(moved), 3, "the parent's three children follow its tube")
        groomlab.set_tube(self.groom, guides=[parent], samples=5)
        self.assertEqual(len(groomlab.get_tube(self.groom, parent)["aspect"]), 5)
        cmds.undo()
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before)
        cmds.redo()
        self.assertEqual(groomlab.get_tube(self.groom, parent)["shape"], "ribbon")
        with self.assertRaises(ValueError):
            groomlab.set_tube(self.groom, guides=[parent], shape="square")
        with self.assertRaises(RuntimeError):
            cmds.groomLabGuides(self.groom, setTube='{"width": -1}', guide=[str(parent)])
        # Every primary's tube at once (by group), then filled with strands.
        self.assertEqual(groomlab.set_tube(self.groom, group="primary", width=0.25), 6)
        groomlab.set_param(self.groom, "guides", "level", 0)
        groomlab.set_param(self.groom, "interpolate", "fillTubes", True)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertIn("tubeId", json.dumps(stats["channels"]))
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_shape_brush_paints_tubes_and_fill_tubes_is_set(self):
        # The Shape brush (it replaced Tube): a stroke paints the tube sections near it, one per
        # guide point, for each channel, one undo step each; scale_tube scales a tube as a whole;
        # set_fill_tubes turns Fill Tubes on the graph's Interpolate (every guide level's tubes fill:
        # the Guides node stays as it is).
        parent = self.primaries[0]
        pts = cmds.groomLabGuides(self.groom, guidePoints=parent)
        count = len(pts) // 3
        tip = tuple(pts[-3:])
        before = cmds.getAttr(self.groom + ".guideData")
        groomlab.brush_stroke(self.groom, "shape", [tip], radius=0.3, strength=1.0, tube_channel="width")
        tube = groomlab.get_tube(self.groom, parent)
        self.assertEqual(len(tube["width"]), count)
        self.assertGreater(tube["width"][-1], tube["width"][0])
        self.assertEqual(len(tube["aspect"]), 1, "only the channel painted")
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before)
        groomlab.brush_stroke(self.groom, "shape", [tip], radius=0.3, strength=1.0, tube_channel="flatness")
        tube = groomlab.get_tube(self.groom, parent)
        self.assertLess(tube["aspect"][-1], 1.0)
        self.assertAlmostEqual(tube["aspect"][0], 1.0, places=5)
        groomlab.brush_stroke(self.groom, "shape", [tip], radius=0.3, strength=-1.0, tube_channel="twist")
        self.assertLess(groomlab.get_tube(self.groom, parent)["twist"][-1], 0.0)  # negative: the other way
        cmds.undo()
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before)
        with self.assertRaises(ValueError):
            groomlab.brush_stroke(self.groom, "shape", [tip], tube_channel="radius")
        with self.assertRaises(ValueError):
            groomlab.brush_stroke(self.groom, "tube", [tip])  # the old Tube brush is gone
        self.assertNotIn("tube", groomlab.BRUSH_MODES)
        groomlab.brush_tool("shape", tube_channel="twist", activate=False)
        # The Groom Tools' tube panel: the whole tube scaled (its taper kept), Fill Tubes and Tube Blend.
        widths = groomlab.get_tube(self.groom, parent)["width"]
        self.assertEqual(groomlab.scale_tube(self.groom, 2.0, guides=[parent]), 1)
        for a, b in zip(groomlab.get_tube(self.groom, parent)["width"], widths):
            self.assertAlmostEqual(a, 2.0 * b, places=5)
        cmds.undo()
        # smooth_tube (the card's Smooth): the sections evened out along the guide, one undo step.
        groomlab.set_tube(self.groom, guides=[parent], aspect=[1.0, 0.2, 1.0, 0.2, 1.0])
        self.assertEqual(groomlab.smooth_tube(self.groom, guides=[parent], channel="flatness", passes=4), 1)
        smoothed = groomlab.get_tube(self.groom, parent)["aspect"]
        self.assertEqual(len(smoothed), max(count, 5))  # a section per guide point
        self.assertGreater(min(smoothed), 0.3)
        self.assertAlmostEqual(smoothed[0], 1.0, places=5)
        with self.assertRaises(ValueError):
            groomlab.smooth_tube(self.groom, guides=[parent], channel="radius")
        cmds.undo()
        cmds.undo()
        # A guide placed among tube guides takes the placement tube's shape (the Tubes panel's
        # choice; Place's mirror too) and its neighbours' width, flatness and twist.
        primaries = cmds.groomLabGuides(self.groom, listGuides="primary")
        groomlab.set_tube(self.groom, group="primary", shape="blade", aspect=0.4)
        root = cmds.groomLabGuides(self.groom, guidePoints=parent)[:3]
        aside = 0.05 if root[0] >= 0.0 else -0.05  # away from the mirror plane: two guides
        groomlab.brush_stroke(self.groom, "place", [(root[0] + aside, root[1], root[2])], symmetry="x",
                              tube={"shape": "teardrop"})
        placed = [g for g in cmds.groomLabGuides(self.groom, listGuides="primary") if g not in primaries]
        self.assertEqual(len(placed), 2)
        for g in placed:
            self.assertEqual(groomlab.get_tube(self.groom, g)["shape"], "teardrop")
            self.assertAlmostEqual(groomlab.get_tube(self.groom, g)["aspect"][0], 0.4, places=4)
        cmds.undo()
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before)
        self.assertEqual(groomlab.fill_tubes(self.groom), {"fillTubes": False, "tubeBlend": 0.25})
        self.assertEqual(groomlab.set_fill_tubes(self.groom, on=True, blend=0.5), 1)
        self.assertEqual(groomlab.fill_tubes(self.groom), {"fillTubes": True, "tubeBlend": 0.5})
        self.assertEqual(groomlab.set_fill_tubes(self.groom, on=True), 0)  # as it is: nothing to do
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertIn("tubeId", json.dumps(stats["channels"]))
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.undo()
        self.assertFalse(groomlab.fill_tubes(self.groom)["fillTubes"])

    def test_tube_shape_library_is_saved_edited_and_applied(self):
        # Tube guides' custom shapes: a library saved with the groom (scene and .groom file), its
        # shapes given by name like the built-ins, edits reaching every guide taking one (one undo
        # step each), a bad outline refused, a missing shape loading round, Place's tube shape.
        star = [(math.cos(math.pi / 2 + math.pi * i / 5) * (1.0 if i % 2 == 0 else 0.5),
                 math.sin(math.pi / 2 + math.pi * i / 5) * (1.0 if i % 2 == 0 else 0.5)) for i in range(10)]
        before = cmds.getAttr(self.groom + ".guideData")
        groomlab.add_tube_shape(self.groom, "star", star, smooth=False)
        names = [s["name"] for s in groomlab.tube_shapes(self.groom)]
        self.assertEqual(names, ["round", "ribbon", "teardrop", "blade", "star"])
        library = groomlab.tube_shapes(self.groom)[-1]
        self.assertFalse(library["builtin"])
        self.assertEqual(library["users"], 0)
        self.assertEqual(len(library["points"]), 10)
        self.assertAlmostEqual(max(v for _, v in library["points"]), 1.0, places=2)  # fitted into the square
        with self.assertRaises(RuntimeError):
            groomlab.add_tube_shape(self.groom, "star", star)  # taken
        with self.assertRaises(RuntimeError) as bad:
            groomlab.add_tube_shape(self.groom, "bowtie", [(-1, -1), (1, 1), (1, -1), (-1, 1)])
        self.assertIn("crosses itself", str(bad.exception))
        with self.assertRaises(RuntimeError):
            groomlab.add_tube_shape(self.groom, "cup", [(-1, 1), (-1, -1), (1, -1), (1, 1), (0.8, 1), (0.8, -0.8),
                                                         (-0.8, -0.8), (-0.8, 1)])  # its center left out
        self.assertEqual(groomlab.set_tube(self.groom, group="primary", shape="star"), 6)
        self.assertEqual(groomlab.get_tube(self.groom, self.primaries[0])["shape"], "star")
        self.assertEqual(groomlab.tube_shapes(self.groom)[-1]["users"], 6)
        with self.assertRaises(ValueError):
            groomlab.set_tube(self.groom, group="primary", shape="moon")
        # An edit reaches every guide taking it: one undo step.
        tip = cmds.groomLabGuides(self.groom, guidePoints=self.children[0])[-3:]
        data = cmds.getAttr(self.groom + ".guideData")
        self.assertEqual(groomlab.set_tube_shape_points(self.groom, "star", [(x * 0.8, y) for x, y in star], smooth=True), 6)
        self.assertTrue(groomlab.tube_shapes(self.groom)[-1]["smooth"])
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), data)
        cmds.redo()
        # A live preview changes nothing stored and makes no undo step.
        groomlab.preview_tube_shape(self.groom, "star", star, smooth=False)
        groomlab.end_guide_preview(self.groom)
        self.assertTrue(groomlab.tube_shapes(self.groom)[-1]["smooth"])
        self.assertEqual(cmds.groomLabGuides(self.groom, guidePoints=self.children[0])[-3:], tip)  # (round children)
        # Fill Tubes fills the custom outline.
        groomlab.set_param(self.groom, "guides", "level", 0)
        groomlab.set_param(self.groom, "interpolate", "fillTubes", True)
        self.assertIn("tubeId", json.dumps(json.loads(cmds.groomLabGraphStats(self.groom))["channels"]))
        self.assertEqual(groomlab.errors(self.groom), [])
        # Saved with the scene...
        scene = os.path.join(tempfile.mkdtemp(), "tube_shapes.ma")
        cmds.file(rename=scene)
        cmds.file(save=True, type="mayaAscii", force=True)
        cmds.file(scene, open=True, force=True)
        self.assertEqual([s["name"] for s in groomlab.tube_shapes(self.groom)][-1], "star")
        self.assertEqual(groomlab.get_tube(self.groom, self.primaries[0])["shape"], "star")
        # ...and in .groom files.
        path = os.path.join(tempfile.mkdtemp(), "tubes.groom")
        groomlab.export_groom(self.groom, path)
        with open(path) as f:
            self.assertIn("tubeShapes", f.read())
        copy = groomlab.import_groom(path, scalp=self.scalp, name="tubesCopy")
        self.assertEqual(groomlab.get_tube(copy, self.primaries[0])["shape"], "star")
        self.assertTrue([s for s in groomlab.tube_shapes(copy) if s["name"] == "star"][0]["smooth"])
        # Renamed (its guides keep it), then removed (they go round): one undo step each.
        groomlab.rename_tube_shape(self.groom, "star", "spiky")
        self.assertEqual(groomlab.get_tube(self.groom, self.primaries[0])["shape"], "spiky")
        self.assertEqual(groomlab.remove_tube_shape(self.groom, "spiky"), 6)
        self.assertEqual(groomlab.get_tube(self.groom, self.primaries[0])["shape"], "round")
        cmds.undo()
        self.assertEqual(groomlab.get_tube(self.groom, self.primaries[0])["shape"], "spiky")
        # A guide whose library shape is missing loads round (a warning says so).
        text = cmds.getAttr(self.groom + ".guideData")
        doc = json.loads(text)
        del doc["tubeShapes"]
        cmds.setAttr(self.groom + ".guideData", json.dumps(doc), type="string")
        self.assertEqual(groomlab.get_tube(self.groom, self.primaries[0])["shape"], "round")
        cmds.setAttr(self.groom + ".guideData", text, type="string")
        # Place takes the placement tube's shape (here the library's), its sections the neighbours'.
        primaries = cmds.groomLabGuides(self.groom, listGuides="primary")
        groomlab.set_tube(self.groom, group="primary", shape="blade", width=0.3)
        root = cmds.groomLabGuides(self.groom, guidePoints=self.primaries[0])[:3]
        groomlab.brush_stroke(self.groom, "place", [(root[0] + 0.05, root[1], root[2])], tube={"shape": "spiky"})
        placed = [g for g in cmds.groomLabGuides(self.groom, listGuides="primary") if g not in primaries]
        self.assertEqual(len(placed), 1)
        tube = groomlab.get_tube(self.groom, placed[0])
        self.assertEqual(tube["shape"], "spiky")
        self.assertAlmostEqual(max(tube["width"]), 0.3, places=3)  # blended from the blades around it
        # The Tubes panel's targets: the selection, else the soloed guides (left unfrozen).
        groomlab.clear_guide_selection(self.groom)
        self.assertEqual(groomlab.tube_targets(self.groom)["source"], "none")
        groomlab.set_guide_attribute(self.groom, "freeze", 1.0, group="primary")
        groomlab.set_guide_attribute(self.groom, "freeze", 0.0, guides=[self.primaries[1]])
        targets = groomlab.tube_targets(self.groom)
        self.assertEqual(targets["source"], "solo")
        self.assertEqual(targets["guides"], [str(self.primaries[1])])
        self.assertEqual(targets["tube"]["shape"], "blade")
        groomlab.select_guides(self.groom, guides=[self.primaries[0]])
        self.assertEqual(groomlab.tube_targets(self.groom)["source"], "selection")
        # A groom of one guide: that guide, selected or not (soloing it freezes nothing).
        one = groomlab.create_groom(self.scalp, name="oneGuide")
        groomlab.guides_from_curves(one, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 1.6, 0), (0, 2.1, 0)])])
        targets = groomlab.tube_targets(one)
        self.assertEqual(targets["guides"], [str(g) for g in groomlab.guide_ids(one, "primary")])
        self.assertEqual(len(targets["guides"]), 1)
        del before

    def test_flow_arrows_lay_strands(self):
        # A stroke across the top of the head, toward +x: arrows along it.
        path = [(-0.6 + 0.1 * i, 0.9, 0.3) for i in range(13)]
        groomlab.flow_stroke(self.groom, path, radius=0.2)
        arrows = groomlab.flow_arrows(self.groom)
        self.assertGreater(len(arrows), 4)
        cmds.undo()
        self.assertEqual(groomlab.flow_arrows(self.groom), [])
        cmds.redo()
        self.assertEqual(len(groomlab.flow_arrows(self.groom)), len(arrows))
        # A second stroke over the same spot turns arrows instead of piling them up.
        groomlab.flow_stroke(self.groom, [(0.0, 0.95, 0.3 - 0.1 * i) for i in range(4)], radius=0.2)
        self.assertLess(len(groomlab.flow_arrows(self.groom)), len(arrows) + 4)

        node = groomlab.add_flow_node(self.groom, after="interpolate", lift=5.0, bend=0.5)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertIn(node, [n["name"] for n in groomlab.get_graph(self.groom)["nodes"]])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)

        # Erasing along the first stroke removes the arrows there.
        before = len(groomlab.flow_arrows(self.groom))
        groomlab.flow_stroke(self.groom, path, radius=0.3, erase=True)
        self.assertLess(len(groomlab.flow_arrows(self.groom)), before)

        path_file = os.path.join(tempfile.mkdtemp(), "flow.groom")
        groomlab.export_groom(self.groom, path_file)
        with open(path_file) as f:
            self.assertIn("flow", json.load(f))
        # GroomLab > Utilities > Delete All Flow Arrows: every arrow off, one undo step, said with the count.
        from groomlab.ui import menu
        arrows = len(groomlab.flow_arrows(self.groom))
        self.assertGreater(arrows, 0)
        self.assertEqual(groomlab.flow_counts(self.groom), (arrows, 0))
        cmds.select(self.groom)
        self.assertEqual(menu.delete_all_flow_arrows(), (arrows, 0))
        self.assertEqual(groomlab.flow_arrows(self.groom), [])
        self.assertEqual(groomlab.flow_counts(self.groom), (0, 0))
        cmds.undo()
        self.assertEqual(len(groomlab.flow_arrows(self.groom)), arrows)
        groomlab.clear_flow(self.groom)
        self.assertEqual(groomlab.flow_arrows(self.groom), [])
        self.assertEqual(menu.delete_all_flow_arrows(self.groom), (0, 0))  # nothing to delete: no step

    def test_flow_sinks(self):
        # A whorl on the crown (mirrored across x gives a clockwise twin), then removed.
        groomlab.flow_sink(self.groom, (0.3, 1.0, 0.0), kind="whorl", radius=0.3, symmetry="x")
        sinks = groomlab.flow_sinks(self.groom)
        self.assertEqual(len(sinks), 2)
        self.assertAlmostEqual(sinks[0][3], 70.0, places=3)
        self.assertAlmostEqual(sinks[1][3], -70.0, places=3)  # the mirror turns the other way
        self.assertAlmostEqual(sinks[0][4], 0.3, places=5)
        cmds.undo()
        self.assertEqual(groomlab.flow_sinks(self.groom), [])
        cmds.redo()
        # Sinks alone drive the Flow node.
        groomlab.add_flow_node(self.groom, after="interpolate")
        self.assertEqual(groomlab.errors(self.groom), [])
        groomlab.flow_sink(self.groom, (0.3, 1.0, 0.0), erase=True, radius=0.2)
        self.assertEqual(len(groomlab.flow_sinks(self.groom)), 1)
        with self.assertRaises(ValueError):
            groomlab.flow_sink(self.groom, (0, 1, 0), kind="spiral")

    def test_shot_corrective_layers(self):
        from groomlab import shots
        # A fix keyed around frame 12: its weight is a keyable attribute named after it.
        name = shots.add_layer(self.groom, frame=12, fade_in=3, fade_out=9)
        self.assertEqual(name, "shot12")
        self.assertEqual(shots.target(self.groom), "shot12")  # brushes edit it
        plug = shots.weight_plug(self.groom, name)
        xform = cmds.listRelatives(self.groom, parent=True, fullPath=True)[0]
        self.assertEqual(plug, xform + ".shot12")  # on the transform: its keys show in the timeline
        self.assertEqual(cmds.getAttr(plug, time=12), 1.0)
        self.assertEqual(cmds.keyframe(xform, query=True, attribute="shot12", keyframeCount=True), 3)
        self.assertEqual(cmds.getAttr(self.groom + ".correctiveWeight[0]", time=12), 1.0)  # driving the shape
        self.assertEqual([k[0] for k in shots.layers(self.groom)[0]["keys"]], [9.0, 12.0, 21.0])
        self.assertAlmostEqual(shots.weight_at(self.groom, name, 9), 0.0, places=4)
        self.assertAlmostEqual(shots.weight_at(self.groom, name, 12), 1.0, places=4)
        self.assertEqual(shots.add_layer(self.groom, frame=12, fade=2), "shot12_2")  # names stay unique
        # An every-frame corrective: unkeyed.
        sculpt = shots.add_layer(self.groom, all_frames=True)
        self.assertTrue(next(l for l in shots.layers(self.groom) if l["name"] == sculpt)["all_frames"])
        shots.set_layer(self.groom, sculpt, weight=0.5)
        self.assertEqual(shots.weight_at(self.groom, sculpt, 100), 0.5)
        # A keyed weight is keyed where it changes (auto key).
        cmds.currentTime(12)
        shots.set_layer(self.groom, "shot12", weight=0.5, rename="hero")
        self.assertEqual(shots.active_layer(self.groom), sculpt)  # the newest stays picked
        self.assertAlmostEqual(shots.weight_at(self.groom, "hero", 12), 0.5, places=4)
        self.assertEqual(shots.weight_plug(self.groom, "hero"), xform + ".hero")  # renamed, keys kept
        self.assertFalse(cmds.attributeQuery("shot12", node=xform, exists=True))
        self.assertEqual(cmds.keyframe(xform, query=True, attribute="hero", keyframeCount=True), 3)
        with self.assertRaises(ValueError):
            shots.set_layer(self.groom, "hero", rename="bad name!")
        shots.clear_keys(self.groom, "hero")
        self.assertTrue(next(l for l in shots.layers(self.groom) if l["name"] == "hero")["all_frames"])
        shots.set_layer(self.groom, "hero", frame=30, fade=5)  # a shot fix again, at 30
        self.assertEqual(next(l for l in shots.layers(self.groom) if l["name"] == "hero")["frame"], 30.0)
        # The stack: reorder, solo, base groom.
        shots.move_layer(self.groom, "hero", 5)
        self.assertEqual([l["name"] for l in shots.layers(self.groom)][-1], "hero")
        shots.solo_layer(self.groom, "hero")
        self.assertEqual([l["enabled"] for l in shots.layers(self.groom)], [False, False, True])
        shots.solo_layer(self.groom, "hero")
        shots.set_target(self.groom, None)
        self.assertIsNone(shots.target(self.groom))
        self.assertFalse(cmds.getAttr(self.groom + ".shotSculpt"))
        shots.remove_layer(self.groom, "shot12_2")
        self.assertFalse(cmds.attributeQuery("shot12_2", node=xform, exists=True))
        with self.assertRaises(ValueError):
            shots.set_layer(self.groom, "nope", weight=1)
        # shot_sculpt makes a fix on a frame where the current one has no weight.
        cmds.currentTime(60)
        shots.set_target(self.groom, "hero")
        self.assertEqual(shots.shot_sculpt(self.groom), "shot60")
        self.assertTrue(cmds.getAttr(self.groom + ".shotSculpt"))
        shots.shot_sculpt(self.groom, False)
        self.assertFalse(cmds.getAttr(self.groom + ".shotSculpt"))
        self.assertEqual(groomlab.errors(self.groom), [])

        # Layers travel with groom files, keyed weights as weight curves (for the procedural and groomlab-cli).
        path = os.path.join(tempfile.mkdtemp(), "shot.groom")
        groomlab.export_groom(self.groom, path)
        with open(path) as f:
            saved = {l["name"]: l for l in json.load(f)["correctives"]["layers"]}
        self.assertEqual(sorted(saved), ["hero", "sculpt3", "shot60"])
        curve = saved["hero"]["weightCurve"]
        self.assertAlmostEqual(max(w for _, w in curve), 0.5, places=4)  # peaks at its keyed weight
        self.assertAlmostEqual(curve[0][1], 0.0, places=4)
        self.assertNotIn("weightCurve", saved["sculpt3"])  # unkeyed: its value
        self.assertAlmostEqual(saved["sculpt3"]["weight"], 0.5, places=4)

    def test_sculpt_node_strokes_follow_the_guides_upstream(self):
        from groomlab import sculpts, shots
        from groomlab.graph import GraphModel
        groomlab.set_graph(self.groom, SCULPT_GRAPH)
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(sculpts.sculpt_nodes(self.groom), ["sculpt1"])
        shots.add_layer(self.groom, all_frames=True)  # a corrective picked: picking the node takes over
        sculpts.set_target(self.groom, "sculpt1")
        self.assertEqual(sculpts.target(self.groom), "sculpt1")
        self.assertFalse(cmds.getAttr(self.groom + ".shotSculpt"))
        with self.assertRaises(ValueError):
            sculpts.set_target(self.groom, "nope")

        # Scripted strokes shape the node's guides: one undo step each, the stored guides untouched.
        guide_data = cmds.getAttr(self.groom + ".guideData")
        groomlab.brush_stroke(self.groom, "comb", [(-0.4, 1.3, 0.0), (0.0, 1.35, 0.2), (0.4, 1.3, 0.4)], radius=0.6)
        first = cmds.getAttr(self.groom + ".sculptData")
        self.assertTrue(first)
        self.assertGreater(sculpts.edited_guides(self.groom, "sculpt1"), 0)
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), guide_data)
        groomlab.brush_stroke(self.groom, "curl", [(0.3, 1.25, 0.3)], radius=0.6, strength=1.0)
        second = cmds.getAttr(self.groom + ".sculptData")
        self.assertNotEqual(second, first)
        with self.assertRaises(RuntimeError):  # Place, Draw, Erase, Paint are not for a Sculpt node
            groomlab.brush_stroke(self.groom, "erase", [(0.0, 1.3, 0.0)], radius=0.6)
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".sculptData"), first)
        cmds.undo()
        self.assertFalse(cmds.getAttr(self.groom + ".sculptData"))
        cmds.redo()
        cmds.redo()
        self.assertEqual(cmds.getAttr(self.groom + ".sculptData"), second)

        def evaluated(path):
            groomlab.export_groom(self.groom, path)
            with open(path) as f:
                self.assertIn("sculpts", json.load(f))
            session = groomlab_core.GroomSession(groomlab_core.read_groom_file(path).data)
            session.evaluate()
            fed = {g.id: [tuple(p[k] for k in range(3)) for p in g.points]
                   for g in session.sculpt_input("sculpt1")["guides"].guides}
            out = {g.id: [tuple(p[k] for k in range(3)) for p in g.points]
                   for g in session.evaluate_node("sculpt1").guides}
            return fed, out

        def form(a, b):  # per segment: the turn from a's segment to b's, and the length ratio
            result = []
            for k in range(len(a) - 1):
                u = [a[k + 1][i] - a[k][i] for i in range(3)]
                v = [b[k + 1][i] - b[k][i] for i in range(3)]
                lu, lv = math.sqrt(sum(x * x for x in u)), math.sqrt(sum(x * x for x in v))
                result.append((math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(u, v)) / (lu * lv)))), lv / lu))
            return result

        folder = tempfile.mkdtemp()
        fed, out = evaluated(os.path.join(folder, "before.groom"))
        edited = max(out, key=lambda i: max(_dist(p, q) for p, q in zip(out[i], fed[i])))
        self.assertGreater(max(_dist(p, q) for p, q in zip(out[edited], fed[edited])), 0.05)
        # The viewport shows the node's output as the guides.
        cmds.getAttr(self.groom + ".outStrandCount")
        shown = cmds.groomLabGuides(self.groom, guidePoints=str(edited), evaluated=True)
        shown = [tuple(shown[k:k + 3]) for k in range(0, len(shown), 3)]
        self.assertEqual(len(shown), len(out[edited]))
        self.assertLess(max(_dist(p, q) for p, q in zip(shown, out[edited])), 1e-3)
        before = form(fed[edited], out[edited])

        # The hand guides combed upstream: the node's guides are made again (the same IDs), and the
        # sculpted one keeps its relative form instead of snapping back.
        shots.set_target(self.groom, None)
        self.assertIsNone(sculpts.target(self.groom))
        groomlab.brush_stroke(self.groom, "comb", [(-1.0, 1.4, 0.2), (1.0, 1.4, 0.2)], radius=3.0, level=-1)
        fed2, out2 = evaluated(os.path.join(folder, "after.groom"))
        self.assertGreater(max(_dist(p, q) for p, q in zip(fed2[edited], fed[edited])), 0.05)
        for (turn, ratio), (turn2, ratio2) in zip(before, form(fed2[edited], out2[edited])):
            self.assertAlmostEqual(turn2, turn, delta=3e-3)
            self.assertAlmostEqual(ratio2, ratio, delta=1e-3)
        self.assertGreater(max(_dist(p, q) for p, q in zip(out2[edited], out[edited])), 0.05)

        # Saved with the scene and back.
        sculpts.set_target(self.groom, "sculpt1")
        scene = os.path.join(folder, "sculpt.ma")
        cmds.file(rename=scene)
        cmds.file(save=True, type="mayaAscii")
        cmds.file(new=True, force=True)
        cmds.file(scene, open=True, force=True)
        groom = cmds.ls(type="groomLabShape")[0]
        self.assertEqual(cmds.getAttr(groom + ".sculptData"), second)
        self.assertEqual(sculpts.target(groom), "sculpt1")
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 0)

        # Renamed in the graph model: the edits and the target go along, in its undo step.
        model = GraphModel.from_groom(groom)
        model.rename_node("sculpt1", "shape")
        model.save(groom)
        self.assertEqual(sculpts.sculpted_nodes(groom), ["shape"])
        self.assertEqual(sculpts.target(groom), "shape")
        cmds.undo()
        self.assertEqual(sculpts.sculpted_nodes(groom), ["sculpt1"])
        self.assertEqual(sculpts.target(groom), "sculpt1")
        # Duplicated: the copy gets the edits; deleted: they go.
        model = GraphModel.from_groom(groom)
        copies = model.paste_nodes(model.copy_nodes(["sculpt1"]))
        model.save(groom)
        self.assertEqual(sorted(sculpts.sculpted_nodes(groom)), sorted(["sculpt1"] + copies))
        model.remove_node(copies[0])
        model.save(groom)
        self.assertEqual(sculpts.sculpted_nodes(groom), ["sculpt1"])
        # Clear Edits: one undo step.
        sculpts.clear_edits(groom, "sculpt1")
        self.assertEqual(sculpts.edited_guides(groom, "sculpt1"), 0)
        cmds.undo()
        self.assertGreater(sculpts.edited_guides(groom, "sculpt1"), 0)
        self.assertEqual(groomlab.errors(groom), [])

        # Grouped into a compound (the Graph Editor's Ctrl+G): the edits move inside with the node
        # ("compound/sculpt1"), so its guides stay sculpted, and the target is cleared (brushes
        # shape top-level nodes); undo brings both back. Renamed, the compound takes the edits
        # along; Ungroup brings them back out.
        def guides_of(node):
            path = os.path.join(folder, "grouped.groom")
            groomlab.export_groom(groom, path)
            session = groomlab_core.GroomSession(groomlab_core.read_groom_file(path).data)
            session.evaluate()
            return {g.id: [tuple(p[k] for k in range(3)) for p in g.points] for g in session.evaluate_node(node).guides}

        edits = cmds.getAttr(groom + ".sculptData")
        sculpted = guides_of("sculpt1")
        model = GraphModel.from_groom(groom)
        compound = model.collapse(["sculpt1"])
        model.save(groom)
        self.assertEqual(sculpts.sculpted_nodes(groom), [compound + "/sculpt1"])
        self.assertIsNone(sculpts.target(groom))
        self.assertEqual(guides_of(compound), sculpted)
        self.assertEqual(groomlab.errors(groom), [])
        cmds.undo()
        self.assertEqual(cmds.getAttr(groom + ".sculptData"), edits)
        self.assertEqual(sculpts.target(groom), "sculpt1")
        model = GraphModel.from_groom(groom)
        compound = model.collapse(["sculpt1"])
        model.save(groom)
        model = GraphModel.from_groom(groom)
        model.rename_node(compound, "shaping")
        model.save(groom)
        self.assertEqual(sculpts.sculpted_nodes(groom), ["shaping/sculpt1"])
        self.assertEqual(guides_of("shaping"), sculpted)
        model = GraphModel.from_groom(groom)
        self.assertEqual(model.expand("shaping"), ["sculpt1"])
        model.save(groom)
        self.assertEqual(sculpts.sculpted_nodes(groom), ["sculpt1"])
        self.assertEqual(guides_of("sculpt1"), sculpted)
        cmds.undo()
        self.assertEqual(sculpts.sculpted_nodes(groom), ["shaping/sculpt1"])

    def test_review_tools(self):
        groomlab.density_heatmap(self.groom)
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorMode"), 5)
        groomlab.density_heatmap(self.groom, False)
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorMode"), 0)

        stats = groomlab.groom_stats(self.groom)
        self.assertEqual(stats["strands"], cmds.getAttr(self.groom + ".outStrandCount"))
        self.assertEqual(stats["guides"], 6 + 18)
        self.assertAlmostEqual(stats["render_strands"], stats["strands"] / 0.5, delta=1)
        self.assertGreater(stats["points"], stats["strands"])
        cmds.select(self.groom)
        from groomlab import review
        self.assertIn("strands", review.hud_text())

        rig = groomlab.turntable(self.groom, frames=48, start=1)
        self.assertEqual(len(rig["lights"]), 3)
        shape = cmds.listRelatives(rig["camera"], shapes=True)[0]
        self.assertTrue(cmds.getAttr(shape + ".renderable"))
        self.assertEqual(cmds.playbackOptions(query=True, maxTime=True), 48)

        def camera_at(frame):
            cmds.currentTime(frame)
            return cmds.xform(rig["camera"], query=True, worldSpace=True, translation=True)

        a, quarter, full = camera_at(1), camera_at(13), camera_at(49)
        for i in range(3):
            self.assertAlmostEqual(a[i], full[i], places=3)  # once round
        self.assertGreater(abs(quarter[0] - a[0]), 1.0)     # and it moves
        # The whole groom in view: its bounds fit the frame at every angle (distance from center).
        center = [sum(v) / 2 for v in zip(cmds.exactWorldBoundingBox(self.groom)[:3],
                                          cmds.exactWorldBoundingBox(self.groom)[3:])]
        self.assertGreater(_dist(a, center) if hasattr(math, "dist") else
                           math.sqrt(sum((p - q) ** 2 for p, q in zip(a, center))), 2.0)

    def test_selection_edits_are_undoable(self):
        self.assertEqual(len(self.primaries), 6)
        self.assertEqual(sorted(groomlab.select_guides(self.groom, level=0)), sorted(self.primaries))
        groomlab.select_guides(self.groom, guides=self.primaries[:2], mode="deselect")
        self.assertEqual(sorted(groomlab.selected_guides(self.groom)), sorted(self.primaries[2:]))
        groomlab.select_guides(self.groom, guides=self.primaries[:3], mode="toggle")
        expected = set(self.primaries[3:]) | set(self.primaries[:2])
        self.assertEqual(set(groomlab.selected_guides(self.groom)), expected)
        cmds.undo()
        self.assertEqual(sorted(groomlab.selected_guides(self.groom)), sorted(self.primaries[2:]))
        groomlab.select_guides(self.groom, group="children", mode="add")
        self.assertEqual(len(groomlab.selected_guides(self.groom)), 4 + 18)
        groomlab.clear_guide_selection(self.groom)
        self.assertEqual(groomlab.selected_guides(self.groom), [])
        with self.assertRaises(RuntimeError):
            groomlab.select_guides(self.groom, guides=["12345678"])

        # The mirror of a +x primary is a -x primary.
        points = guide_points(self.groom)
        right = [g for g in self.primaries if points[g][0][0] > 0][0]
        groomlab.select_guides(self.groom, guides=[right])
        (mirrored,) = groomlab.mirror_guide_selection(self.groom, "x")
        self.assertLess(points[mirrored][0][0], 0)
        self.assertAlmostEqual(points[mirrored][0][0], -points[right][0][0], places=3)

    def test_selected_only_brush_with_soft_selection(self):
        before = guide_points(self.groom)
        target = self.primaries[0]
        groomlab.select_guides(self.groom, guides=[target])
        stroke = [(0, 1.8, -1.0), (0, 1.8, 1.0)]
        groomlab.brush_stroke(self.groom, "comb", stroke, radius=5.0, selected_only=True)
        after = guide_points(self.groom)
        self.assertGreater(moved(after[target], before[target]), 0.01)
        for g in self.primaries[1:]:
            self.assertEqual(after[g], before[g])

        cmds.undo()
        groomlab.set_soft_selection(self.groom, 2.0)  # reaches the neighbours, weaker
        groomlab.brush_stroke(self.groom, "comb", stroke, radius=5.0, selected_only=True)
        after = guide_points(self.groom)
        others = [moved(after[g], before[g]) for g in self.primaries[1:]]
        self.assertTrue(any(m > 0.0 for m in others))
        self.assertLess(max(others), moved(after[target], before[target]))

    def test_symmetric_brush_keeps_the_groom_symmetric(self):
        groomlab.brush_stroke(self.groom, "comb", [(0.5, 1.5, 0.0), (0.5, 1.5, 0.6)], radius=0.6, symmetry="x")
        points = guide_points(self.groom)
        for g in self.primaries:
            groomlab.select_guides(self.groom, guides=[g])
            (m,) = groomlab.mirror_guide_selection(self.groom, "x")
            for p, q in zip(points[g], points[m]):
                self.assertAlmostEqual(p[0], -q[0], places=3)
                self.assertAlmostEqual(p[1], q[1], places=3)
                self.assertAlmostEqual(p[2], q[2], places=3)

    def test_follow_surface_keeps_guides_from_reaching_through_a_thin_shell(self):
        # A thin slab (a flap, an ear): guides stand straight up on top and lie combed along +x
        # underneath. Along the skin the two faces are far apart (round the rim), so with
        # followSurface the top strands take only the top guides and stand straight; measured in a
        # straight line the guides 0.1 below reach them and comb them along x too.
        cmds.file(new=True, force=True)
        slab = cmds.polyCube(width=2, height=0.1, depth=2, subdivisionsX=16, subdivisionsY=1, subdivisionsZ=16,
                             constructionHistory=False, name="slab")[0]
        groom = groomlab.create_groom(slab)
        curves = []
        for x in (-0.75, -0.25, 0.25, 0.75):
            for z in (-0.75, -0.25, 0.25, 0.75):
                curves.append(cmds.curve(degree=1, point=[(x, 0.05, z), (x, 0.45, z), (x, 0.85, z)]))
                curves.append(cmds.curve(degree=1, point=[(x, -0.05, z), (x + 0.4, -0.1, z), (x + 0.8, -0.15, z)]))
        groomlab.guides_from_curves(groom, curves)
        self.groom = groom

        def top_drift(follow):
            """How far, on average, the top strands' tips lie along +x of their roots."""
            groomlab.set_graph(groom, {"nodes": [
                {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
                {"name": "scatter", "type": "Scatter", "params": {"density": 60.0}},
                {"name": "interp", "type": "Interpolate", "params": {"followSurface": follow}},
                {"name": "output", "type": "Output"}], "connections": [
                {"from": "scalp", "to": "scatter", "input": "scalp"},
                {"from": "scatter", "to": "interp", "input": "strands"},
                {"from": "guides", "to": "interp", "input": "guides"},
                {"from": "scalp", "to": "interp", "input": "scalp"},
                {"from": "interp", "to": "output", "input": "strands"}], "output": "output"})
            self.assertEqual(groomlab.errors(groom), [])
            path = os.path.join(tempfile.gettempdir(), "groomlab_follow_surface.usda")
            groomlab.export_usd(groom, path, density=0.5)
            with open(path) as f:
                text = f.read()
            counts = [int(c) for c in re.search(r"curveVertexCounts\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", text).group(1).split(",")]
            block = re.search(r"points\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", text).group(1)
            points = [tuple(float(v) for v in p.split(",")) for p in re.findall(r"\(([^)]*)\)", block)]
            drifts, start = [], 0
            for n in counts:
                root, tip = points[start], points[start + n - 1]
                start += n
                if root[1] > 0.04 and abs(root[0]) < 0.8 and abs(root[2]) < 0.8:  # on top, clear of the rim
                    drifts.append(tip[0] - root[0])
            self.assertGreater(len(drifts), 50)
            return sum(drifts) / len(drifts)

        along, straight = top_drift(True), top_drift(False)
        self.assertLess(abs(along), 0.05, "along %.4f, straight %.4f" % (along, straight))  # as the top guides stand
        # The premise: in a straight line the faces blend (the comb from below comes turned to the
        # top's normal, so along -x).
        self.assertGreater(abs(straight), 0.1)

    def test_clump_paint_drives_clump_strength(self):
        graph = groomlab.get_graph(self.groom, expand_default=True)
        graph["nodes"].insert(-1, {"name": "clump", "type": "Clump", "params": {"strength": 1.0, "centersFrom": "inputPoints"}})
        graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
        graph["connections"] += [{"from": "width", "to": "clump", "input": "strands"},
                                 {"from": "guides", "to": "clump", "input": "centers"},
                                 {"from": "clump", "to": "output", "input": "strands"}]
        groomlab.set_graph(self.groom, graph)
        groomlab.clump_paint(self.groom, activate=False)
        self.assertEqual(groomlab.get_graph(self.groom)["nodes"][-2]["params"]["strength"], {"attribute": "clump", "min": 0.0, "max": 1.0})
        unpainted = cmds.exactWorldBoundingBox(self.groom)  # clump 0 everywhere: no clumping
        self.assertEqual(groomlab.errors(self.groom), [])

        groomlab.brush_stroke(self.groom, "paint", [(0, 1.3, 0)], radius=5.0, strength=1.0, level=0, attribute="clump")
        stored = groomlab.get_hierarchy(self.groom)
        painted = [g for g in stored["guides"] if g.get("attributes", {}).get("clump", 0) > 0.5]
        self.assertEqual(len(painted), 6)  # primaries; children inherit
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), unpainted)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_guide_attributes_reach_strand_maps(self):
        groomlab.set_guide_attribute(self.groom, "frizz", 1.0, group="primary")
        groomlab.set_param(self.groom, "length", "keep", groomlab.attribute_map("frizz", min=0.5, max=1.0))
        self.assertEqual(groomlab.errors(self.groom), [])
        full = cmds.exactWorldBoundingBox(self.groom)
        groomlab.set_guide_attribute(self.groom, "frizz", 0.0, group="primary")
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), full)

    def test_hierarchy_tools_are_undoable(self):
        start = cmds.getAttr(self.groom + ".guideData")
        child = self.children[0]
        stored = {str(g["id"]): g for g in groomlab.get_hierarchy(self.groom)["guides"]}
        new_parent = [p for p in self.primaries if p != str(stored[child]["parent"])][0]
        groomlab.reparent_guides(self.groom, new_parent, guides=[child])
        stored = {str(g["id"]): g for g in groomlab.get_hierarchy(self.groom)["guides"]}
        self.assertEqual(str(stored[child]["parent"]), new_parent)
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), start)

        groomlab.promote_group(self.groom, "children")
        depths = {g["name"]: g for g in groomlab.list_guide_groups(self.groom)}
        self.assertEqual(depths["children"]["depth"], 0)
        groomlab.demote_group(self.groom, "children", "primary")
        depths = {g["name"]: g for g in groomlab.list_guide_groups(self.groom)}
        self.assertEqual(depths["children"]["depth"], 1)
        with self.assertRaises(RuntimeError):
            groomlab.demote_group(self.groom, "children", "nope")
        cmds.undo()
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), start)

    def test_export_vrscene(self):
        # A V-Ray scene: the strands in object space (the USD export's count), the shader, and the
        # groom's world matrix on the Node (here its transform moved: unlocked for the test).
        transform = cmds.listRelatives(self.groom, parent=True, fullPath=True)[0]
        cmds.setAttr(transform + ".translate", lock=False)
        cmds.setAttr(transform + ".translate", 5.0, 0.0, 0.0)
        try:
            folder = tempfile.mkdtemp(prefix="groomlab_vrscene_")
            path = os.path.join(folder, "hair.vrscene")
            self.assertEqual(groomlab.export_vrscene(self.groom, path, start=1, end=2, density=0.5), 2)
            with open(path) as f:
                text = f.read()
            usd = os.path.join(folder, "hair.usda")
            groomlab.export_usd(self.groom, usd, density=0.5)
            with open(usd) as f:
                counts = re.search(r"curveVertexCounts\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", f.read()).group(1)
            strands = len([c for c in counts.split(",") if int(c) >= 2])
            self.assertIn("// %d strands" % strands, text)
            for plugin in ("GeomMayaHair", "TexHairSampler", "BRDFHair4", "MtlSingleBRDF", "Node", "SettingsUnitsInfo"):
                self.assertRegex(text, r"\n%s \S+@\w+ \{" % plugin)
            world = cmds.getAttr(self.groom + ".worldMatrix[0]")
            self.assertAlmostEqual(world[12], 5.0)
            self.assertRegex(text, r"transform=Transform\(Matrix\([^;]*\), Vector\(5, 0, 0\)\);")
        finally:
            cmds.setAttr(transform + ".translate", 0.0, 0.0, 0.0)
            cmds.setAttr(transform + ".translate", lock=True)

    def strand_extent(self):
        """Bounds of the strands alone (the groom's bounding box also holds its guides)."""
        path = os.path.join(tempfile.gettempdir(), "groomlab_extent.usda")
        groomlab.export_usd(self.groom, path, density=0.5)
        with open(path) as f:
            text = f.read()
        numbers = re.search(r"extent\.timeSamples = \{\s*[\d.-]+: \[\(([^)]*)\), \(([^)]*)\)\]", text)
        lo = [float(v) for v in numbers.group(1).split(",")]
        hi = [float(v) for v in numbers.group(2).split(",")]
        return lo + hi

    def strand_points(self):
        """Every exported strand point, in order (density 0.5, as strand_extent)."""
        path = os.path.join(tempfile.gettempdir(), "groomlab_points.usda")
        groomlab.export_usd(self.groom, path, density=0.5)
        with open(path) as f:
            text = f.read()
        block = re.search(r"points\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", text).group(1)
        return [tuple(float(v) for v in p.split(",")) for p in re.findall(r"\(([^)]*)\)", block)]

    def paint_top(self, name="mask", value=1.0, kind="vertices"):
        """A Paint stroke on the top of the scalp: the map `name` (a vertex or texture map) there."""
        groomlab.brush_stroke(self.groom, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute=name,
                              value=value, paint_kind=kind, paint_texture=groomlab.map_texture(self.groom, name))

    def test_painted_mask_is_a_reusable_map_node(self):
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        node = groomlab.paint_scalp_map(self.groom, "mask", paint=False)
        model_types = {n["name"]: n["type"] for n in groomlab.get_graph(self.groom)["nodes"]}
        self.assertEqual(model_types[node], "Attribute")
        self.assertEqual(next(n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == node)["params"]["value"],
                         groomlab.color_set_map("mask"))
        self.assertEqual(groomlab.map_kind(self.groom, "mask"), "vertices")
        # One mask, two params.
        groomlab.set_param(self.groom, "length", "keep", groomlab.attribute_map("mask", min=0.3, max=1.0))
        groomlab.set_param(self.groom, "width", "width", groomlab.attribute_map("mask", min=0.002, max=0.02))
        self.assertEqual(groomlab.errors(self.groom), [])
        unpainted = self.strand_extent()
        self.paint_top()  # the groom follows the paint, no extra step
        painted = self.strand_extent()
        self.assertGreater(painted[4], unpainted[4] + 0.2)  # longer on top
        self.assertAlmostEqual(painted[1], unpainted[1], places=3)  # bottom unpainted: unchanged
        self.assertEqual(groomlab.paint_scalp_map(self.groom, "mask", paint=False), node)  # reused, not duplicated

    def test_the_sample_meshes_come_in_sized_and_selected(self):
        # Nika, Manny and the wolf: their real size in any scene unit, turned for a z-up scene, the
        # part to groom selected (the wolf's body, its other parts beside it in a group), a groom
        # grows on it.
        for sample, extent_cm, part in (("nika", 34.61, "nika_head"), ("manny", 34.21, "manny_head"),
                                         ("wolf", 109.7, "wolf_body")):
            for unit, axis in (("cm", "y"), ("m", "y"), ("cm", "z")):
                cmds.file(new=True, force=True)
                cmds.currentUnit(linear=unit)
                cmds.upAxis(axis=axis)
                target = groomlab.add_sample(sample)
                self.assertEqual(target, part)
                self.assertEqual(cmds.ls(selection=True), [part])
                top = cmds.listRelatives(target, parent=True) or [target]
                bbox = cmds.exactWorldBoundingBox(top[0])
                extent = max(bbox[3] - bbox[0], bbox[4] - bbox[1], bbox[5] - bbox[2])
                want = extent_cm if unit == "cm" else extent_cm / 100.0
                self.assertAlmostEqual(extent / want, 1.0, delta=0.01, msg="%s %s %s-up" % (sample, unit, axis))
            cmds.upAxis(axis="y")
            cmds.currentUnit(linear="cm")
            groom = groomlab.create_groom(target, fit=True)
            self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 0)
        self.assertEqual(sorted(cmds.listRelatives("wolf", children=True)),
                         ["wolf_body", "wolf_eye_l", "wolf_eye_r", "wolf_mouthbag", "wolf_nails"])
        with self.assertRaises(ValueError):
            groomlab.add_sample("nope")

    def test_the_buddy_rig_comes_in_posable_and_a_groom_follows_it(self):
        # Buddy, a rig (body, eyes, teeth and tongue skinned to 63 joints, posed by *_CTL controls):
        # its real size in any scene unit, turned for a z-up scene at its top group (nothing frozen),
        # its body selected, a pose moving the skin. A second one takes free names.
        from maya.api import OpenMaya as om

        def meshes():
            return cmds.ls(cmds.listRelatives("buddy_rig", allDescendents=True, type="mesh", fullPath=True),
                           noIntermediate=True)

        def body_points():
            return [om.MPoint(p) for p in om.MFnMesh(om.MSelectionList().add("buddy_body").getDagPath(0))
                    .getPoints(om.MSpace.kWorld)]

        def head_matrix():
            return om.MMatrix(cmds.xform("head", query=True, worldSpace=True, matrix=True))

        def skin_area():
            fn = om.MFnMesh(om.MSelectionList().add("buddy_bodyShape").getDagPath(0))
            p, (_, v) = fn.getPoints(om.MSpace.kWorld), fn.getTriangles()
            return sum(0.5 * ((p[v[i + 1]] - p[v[i]]) ^ (p[v[i + 2]] - p[v[i]])).length() for i in range(0, len(v), 3))

        counts = []
        for unit, axis in (("cm", "y"), ("m", "y"), ("cm", "z")):
            cmds.file(new=True, force=True)
            cmds.currentUnit(linear=unit)
            cmds.upAxis(axis=axis)
            self.assertEqual(groomlab.add_sample("buddy"), "buddy_body")
            self.assertEqual(cmds.ls(selection=True), ["buddy_body"])
            self.assertEqual(len(meshes()), 5)
            self.assertEqual(len(cmds.ls(type="joint")), 63)
            self.assertEqual(len(cmds.ls("*_CTL", type="transform")), 50)
            bbox = cmds.exactWorldBoundingBox(meshes())
            size = 1.0 if unit == "cm" else 0.01
            up = 1 if axis == "y" else 2
            msg = "%s %s-up" % (unit, axis)
            self.assertAlmostEqual((bbox[3] - bbox[0]) / (96.65 * size), 1.0, delta=0.01, msg=msg)
            self.assertAlmostEqual(bbox[up], 0.0, delta=0.01 * size, msg=msg)  # feet on the ground
            self.assertGreater(bbox[up + 3], 50.0 * size, msg)
            # A head turn moves the skin with the head joint.
            before, m0 = body_points(), head_matrix()
            cmds.setAttr("head_CTL.rotateZ", 30)
            after, carry = body_points(), m0.inverse() * head_matrix()
            top = max(range(len(before)), key=lambda i: before[i][up])  # the crown: on the head joint alone
            self.assertGreater(after[top].distanceTo(before[top]), 2.0 * size, msg)
            self.assertLess(after[top].distanceTo(before[top] * carry), 0.01 * size, msg)
            cmds.setAttr("head_CTL.rotateZ", 0)
            # A groom on it: its rest mesh (the skin's original shape) is the skin at the bind pose in
            # every unit and up axis (a z-up rig is bound again where it was turned), so its 1x, and
            # its strands, are the same in each.
            groom = groomlab.create_groom("buddy_body", fit=True)
            skin = om.MFnMesh(om.MSelectionList().add("buddy_bodyShape").getDagPath(0)).getPoints(om.MSpace.kWorld)
            plug = om.MFnDependencyNode(om.MSelectionList().add(groom).getDependNode(0)).findPlug("restMesh", False)
            rest = om.MFnMesh(plug.asMObject()).getPoints()
            self.assertLess(max(a.distanceTo(b) for a, b in zip(skin, rest)), 1e-4 * size, msg)
            self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4, msg=msg)
            counts.append(cmds.getAttr(groom + ".outStrandCount"))
        self.assertEqual(len(set(counts)), 1, counts)
        cmds.delete(cmds.listRelatives(groom, parent=True))
        # Scaled at its top group, the meshes scale once (geometry_GRP doesn't inherit: the joints
        # carry them), and the controls still pose them.
        cmds.setAttr("buddy_rig.scale", 2, 2, 2)
        doubled = cmds.exactWorldBoundingBox(meshes())
        self.assertAlmostEqual((doubled[3] - doubled[0]) / (bbox[3] - bbox[0]), 2.0, delta=0.01)
        before, m0 = body_points(), head_matrix()
        cmds.setAttr("head_CTL.rotateZ", 30)
        self.assertLess(body_points()[top].distanceTo(before[top] * (m0.inverse() * head_matrix())), 0.02)
        # A second one: a free name, no namespace left behind (its parts keep theirs, under it).
        self.assertEqual(groomlab.add_sample("buddy"), "buddy_rig1|geometry_GRP|buddy_body")
        self.assertIn("buddy_rig1", cmds.ls(assemblies=True))
        self.assertNotIn("groomlab_sample", cmds.namespaceInfo(":", listOnlyNamespaces=True) or [])

        # A groom on the body (cm, y up): its rest mesh, the skin's original shape.
        cmds.file(new=True, force=True)
        cmds.upAxis(axis="y")
        cmds.currentUnit(linear="cm")
        groom = groomlab.create_groom(groomlab.add_sample("buddy"), fit=True)
        count = cmds.getAttr(groom + ".outStrandCount")
        self.assertEqual(count, counts[0])
        self.assertTrue(cmds.listConnections(groom + ".restMesh", source=True))
        path = os.path.join(tempfile.gettempdir(), "groomlab_buddy.usda")

        def strands():
            """The exported strands' points (density 0.05) and their strand IDs."""
            groomlab.export_usd(groom, path, density=0.05)
            with open(path) as f:
                text = f.read()

            def sampled(name):
                return re.search(name + r"\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", text).group(1)

            sizes = [int(c) for c in sampled("curveVertexCounts").split(",")]
            flat = [om.MPoint(*[float(v) for v in p.split(",")]) for p in re.findall(r"\(([^)]*)\)", sampled("points"))]
            ids = [int(v) for v in sampled("primvars:strandId").split(",")]
            out, i = [], 0
            for c in sizes:
                out.append(flat[i:i + c])
                i += c
            return out, ids

        # A pose (an arm raised, the spine bent, the head turned) keeps the strands, with Follow
        # Scalp and without: the same count and IDs (scattered on the rest pose), and the groom's
        # scale (measured without the pose's stretch: the median triangle's), though the skin's
        # area changes.
        rest_area, (_, rest_ids) = skin_area(), strands()
        pose = (("upShoulder_L_CTL", "rotateZ", 40), ("spine_CTL", "rotateZ", 20), ("head_CTL", "rotateZ", 30))
        for follow in (False, True):
            groomlab.set_follow_scalp(groom, follow)
            for control, attr, value in pose:
                cmds.setAttr(control + "." + attr, value)
            self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), count)
            self.assertEqual(strands()[1], rest_ids)
            self.assertGreater(abs(skin_area() / rest_area - 1.0), 0.005)  # the pose stretched the skin (about 1%)
            self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=3)
            for control, attr, value in pose:
                cmds.setAttr(control + "." + attr, 0)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4)
        # The rig scaled at its root control (main_CTL's globalScale): the groom with it, the
        # strand count kept (guides-and-brushes: Rigs).
        cmds.setAttr("main_CTL.globalScale", 2)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), count)
        cmds.setAttr("main_CTL.globalScale", 1)
        # Follow Scalp: the head bent carries the strands rooted on the crown with it, each keeping
        # its shape (times the groom's scale, 1 through the bend).
        (rest, _), m0 = strands(), head_matrix()
        cmds.setAttr("head_CTL.rotateZ", 30)
        (posed, _), carry = strands(), m0.inverse() * head_matrix()
        k = cmds.getAttr(groom + ".groomScale")
        self.assertEqual(len(posed), len(rest))
        crown = max(p[1] for p in body_points())
        on_crown = [i for i, s in enumerate(rest) if s[0][1] > crown - 6.0]
        self.assertGreater(len(on_crown), 10)
        for i in on_crown:  # (the same frame exported again: the export reads the pose, not the one before)
            root = rest[i][0] * carry
            self.assertLess(posed[i][0].distanceTo(root), 0.01)  # the root, on the bent skin
            self.assertLess(posed[i][-1].distanceTo(root + (rest[i][-1] * carry - root) * k), 0.02)  # the tip with it

    def test_leaving_the_paint_brush_puts_the_view_back(self):
        # Painting shows the map on the strands and the scalp; another brush, or another tool
        # (Q), puts back what showed before, without an undo step of its own.
        from groomlab import painting
        from groomlab.brushes import brush_tool
        def view():
            return [cmds.getAttr(self.groom + ".strandColorMode"), cmds.getAttr(self.groom + ".scalpMapDisplay") or ""]
        before = view()
        name = groomlab.paint_param(self.groom, "scatter", "density", kind="vertices")
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorMode"), 2)
        self.assertEqual(cmds.getAttr(self.groom + ".scalpMapDisplay"), name)
        brush_tool("comb")
        self.assertEqual(view(), before)
        groomlab.paint_map(self.groom, name)
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorMode"), 2)
        cmds.setToolTo("selectSuperContext")
        painting._tool_changed()  # Maya's ToolChanged event (not sent without a UI)
        self.assertEqual(view(), before)
        self.assertEqual(groomlab.map_kind(self.groom, name), "vertices", "the map itself stays")

    def test_paint_param_before_strands_exist(self):
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        groomlab.set_param(self.groom, "scatter", "density", 200.0)
        everywhere = cmds.getAttr(self.groom + ".outStrandCount")
        name = groomlab.paint_param(self.groom, "scatter", "density", kind="vertices", paint=False)
        self.assertEqual(name, "scatter_density")
        self.assertEqual(groomlab.map_kind(self.groom, name), "vertices")
        graph = groomlab.get_graph(self.groom)
        nodes = {n["name"]: n for n in graph["nodes"]}
        self.assertEqual(nodes["scatter_density"]["type"], "Map")
        self.assertEqual(nodes["scatter_density"]["params"], {"name": "scatter_density", "min": 0.0, "max": 1.0})
        self.assertIn("density", nodes["scatter"].get("paramInputs", []))
        self.assertIn({"from": "scatter_density", "to": "scatter", "input": "density"}, graph["connections"])
        self.assertEqual(groomlab.errors(self.groom), [])
        # Made white: the look stays (the param's own 200 multiplies it); painted black on top, nothing grows there.
        self.assertAlmostEqual(cmds.getAttr(self.groom + ".outStrandCount") / float(everywhere), 1.0, delta=0.05)
        self.paint_top(name, 0.0)
        self.paint_top(name, 0.0)  # strokes build up toward the value by the brush's falloff
        fewer = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertLess(fewer, everywhere * 0.8)
        self.assertGreater(fewer, 50)
        cmds.undo()  # each stroke: one step
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), everywhere)

    def test_map_for_a_setting_replaces_the_width_and_density_brushes(self):
        # The Width and Density brushes are gone: brush_tool and brush_stroke name the new way.
        for mode in ("width", "density"):
            with self.assertRaises(ValueError) as caught:
                groomlab.brush_tool(mode)
            self.assertIn("+ New Map > for a setting", str(caught.exception))
            self.assertIn("paint_param", str(caught.exception))
            with self.assertRaises(ValueError):
                groomlab.brush_stroke(self.groom, mode, [(0, 1, 0)])
            self.assertNotIn(mode, groomlab.BRUSH_MODES)
        # What + New Map > for a setting offers: the common settings (a node of each type, or none),
        # then every mappable param of the graph.
        settings, others = groomlab.setting_targets(self.groom)
        by_id = dict((s["id"], s) for s in settings)
        self.assertEqual([s["id"] for s in settings][:2], ["density", "width"])
        self.assertEqual((by_id["density"]["node"], by_id["density"]["param"]), ("scatter", "density"))
        self.assertEqual((by_id["length"]["node"], by_id["length"]["param"]), ("length", "keep"))
        self.assertEqual(by_id["curl"]["node"], "")  # the default graph has no Curl node
        self.assertIn(("noise", "fraction"), [(o["node"], o["param"]) for o in others])
        # Made and linked in one undo step (the brush on it, toward 1 with Ctrl toward 0, is checked
        # in the GUI suite: gui_brush; a context answers queries only in a Maya window); the
        # strands follow the map: painted 0, no hair there.
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        everywhere = cmds.getAttr(self.groom + ".outStrandCount")
        graph0 = cmds.getAttr(self.groom + ".graph")
        self.assertIsNone(groomlab.param_map(self.groom, "scatter", "density"))
        name = groomlab.paint_param(self.groom, "scatter", "density", kind="vertices", paint=False)
        self.assertEqual(name, "scatter_density")
        self.assertEqual(groomlab.param_map(self.groom, "scatter", "density"), (name, "luminance"))
        self.assertAlmostEqual(cmds.getAttr(self.groom + ".outStrandCount") / float(everywhere), 1.0, delta=0.05)
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), graph0)
        self.assertNotIn(name, groomlab.list_maps(self.groom))
        cmds.redo()
        self.assertIn(name, groomlab.list_maps(self.groom))
        self.paint_top(name, 0.0)
        self.paint_top(name, 0.0)
        self.assertLess(cmds.getAttr(self.groom + ".outStrandCount"), everywhere * 0.8)
        cmds.undo()
        cmds.undo()
        # Asked again (any kind): the map the param reads is picked, nothing made.
        maps = groomlab.list_maps(self.groom)
        self.assertEqual(groomlab.paint_param(self.groom, "scatter", "density", kind="texture", paint=False), name)
        self.assertEqual(groomlab.list_maps(self.groom), maps)
        # A map the old Density brush made (a vertex map read through a color set) keeps working and
        # is the one picked.
        groomlab.remove_map(self.groom, name)
        cmds.setAttr(self.groom + ".graph", graph0, type="string")
        groomlab.add_map(self.groom, "old_density", 1.0, kind="vertices")
        groomlab.set_param(self.groom, "scatter", "density", groomlab.color_set_map("old_density", max=200.0))
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.paint_param(self.groom, "scatter", "density", kind="vertices", paint=False),
                         "old_density")
        self.assertNotIn("scatter_density", groomlab.list_maps(self.groom))

    def test_guide_layers_drive_density_and_travel_with_renames(self):
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        self.assertEqual(groomlab.add_map(self.groom, "fur"), "fur")
        self.assertEqual(groomlab.list_maps(self.groom), ["fur"])
        with self.assertRaises(ValueError):
            groomlab.add_map(self.groom, "fur")
        with self.assertRaises(ValueError):
            groomlab.add_map(self.groom, "bad name")

        groomlab.set_param(self.groom, "scatter", "density", 200.0)
        map_node = groomlab.use_map(self.groom, "scatter", "density", "fur")
        graph = groomlab.get_graph(self.groom)
        self.assertIn({"from": map_node, "to": "scatter", "input": "density"}, graph["connections"])
        self.assertEqual(groomlab.map_users(self.groom, "fur"), [("scatter", "density")])
        self.assertEqual(groomlab.use_map(self.groom, "length", "keep", "fur"), map_node)  # one node per map
        self.assertEqual(sorted(groomlab.map_users(self.groom, "fur")), [("length", "keep"), ("scatter", "density")])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), 0)  # layer 0 everywhere

        # Paint the top guides only: strands grow there, the groom follows each stroke.
        groomlab.brush_stroke(self.groom, "paint", [(0.0, 1.1, 0.2)], radius=0.6, strength=1.0,
                              attribute="fur", value=1.0)
        top = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(top, 50)
        groomlab.fill_map(self.groom, "fur", 1.0)
        everywhere = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(everywhere, 2 * top)

        groomlab.view_map(self.groom, "fur")
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorMode"), 2)
        groomlab.rename_map(self.groom, "fur", "coat")
        self.assertEqual(groomlab.list_maps(self.groom), ["coat"])
        self.assertEqual(sorted(groomlab.map_users(self.groom, "coat")), [("length", "keep"), ("scatter", "density")])
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorSource"), "coat")
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), everywhere)
        cmds.undo()  # one step: guides, graph and view
        self.assertEqual(groomlab.list_maps(self.groom), ["fur"])
        self.assertEqual(sorted(groomlab.map_users(self.groom, "fur")), [("length", "keep"), ("scatter", "density")])
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorSource"), "fur")

        # An attribute map on Scatter reads the painted layer too; set_param wires the guides in.
        groomlab.set_graph(self.groom, {})
        groomlab.set_param(self.groom, "scatter", "density", groomlab.attribute_map("fur", max=200.0))
        self.assertIn({"from": "guides", "to": "scatter", "input": "guides"},
                      groomlab.get_graph(self.groom)["connections"])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), everywhere)

        groomlab.remove_map(self.groom, "fur")
        self.assertEqual(groomlab.list_maps(self.groom), [])
        self.assertEqual(cmds.getAttr(self.groom + ".strandColorMode"), 0)
        # An attribute map reads a strand channel (Attribute nodes write them too): kept, so it
        # reports the missing channel, a clear error, not a crash.
        self.assertTrue(groomlab.errors(self.groom))
        with self.assertRaises(RuntimeError):
            groomlab.remove_map(self.groom, "fur")

    def test_removing_a_map_lets_what_reads_it_go(self):
        groomlab.set_param(self.groom, "scatter", "density", 150.0)
        map_node = groomlab.use_map(self.groom, "scatter", "density", "fur")
        groomlab.use_map(self.groom, "length", "keep", "fur")
        groomlab.add_map(self.groom, "cs", kind="vertices")
        groomlab.set_param(self.groom, "noise", "amplitude", {"colorSet": "cs", "min": 0.0, "max": 1.0})
        before = groomlab.get_graph(self.groom)
        self.assertEqual(sorted(groomlab.remove_map(self.groom, "fur")), [("length", "keep"), ("scatter", "density")])
        graph = groomlab.get_graph(self.groom)
        self.assertNotIn(map_node, [n["name"] for n in graph["nodes"]])
        scatter = next(n for n in graph["nodes"] if n["name"] == "scatter")
        self.assertEqual(scatter["params"]["density"], 150.0)  # its slider value again
        self.assertNotIn("density", scatter.get("paramInputs", []))
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        cmds.undo()  # one step: the map, its node and links
        self.assertEqual(groomlab.get_graph(self.groom), before)
        self.assertIn("fur", groomlab.list_maps(self.groom))
        # A map param naming the map goes back to the node's default.
        self.assertEqual(groomlab.remove_map(self.groom, "cs"), [("noise", "amplitude")])
        noise = next(n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == "noise")
        self.assertNotIsInstance(noise["params"].get("amplitude"), dict)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_sculpt_tools_scripted(self):
        points = guide_points(self.groom)
        primary = self.primaries[0]
        tip = points[primary][-1]

        # Comb with stretch: the tip follows the stroke and the guide gets longer.
        def length(pts):
            return sum(math.sqrt(sum((u - v) ** 2 for u, v in zip(a, b))) for a, b in zip(pts, pts[1:]))
        groomlab.brush_stroke(self.groom, "comb", [tip, (tip[0] + 0.4, tip[1], tip[2])], radius=0.3, level=0,
                              stretch=True)
        combed = guide_points(self.groom)[primary]
        self.assertGreater(combed[-1][0], tip[0] + 0.1)
        self.assertGreater(length(combed), length(points[primary]) + 0.01)
        self.assertLess(moved([combed[0]], [points[primary][0]]), 1e-4)  # root pinned
        cmds.undo()
        self.assertLess(moved(guide_points(self.groom)[primary], points[primary]), 1e-4)  # one undo step

        # Draw: one new primary shaped like the stroke, rooted on the scalp.
        before = set(groomlab.guide_ids(self.groom, "primary"))
        stroke = [(0.0, 0.0, 1.0)] + [(0.0, 0.1 * k, 1.0 + 0.08 * k) for k in range(1, 8)]
        groomlab.brush_stroke(self.groom, "draw", stroke, segments=6)
        drawn = (set(groomlab.guide_ids(self.groom, "primary")) - before).pop()
        pts = guide_points(self.groom)[drawn]
        self.assertEqual(len(pts), 7)
        self.assertAlmostEqual(math.sqrt(sum(c * c for c in pts[0])), 1.0, delta=0.02)
        self.assertGreater(pts[-1][1], 0.5)

        # Place copies its neighbors' shape (here: about their length).
        before = set(groomlab.guide_ids(self.groom, "primary"))
        groomlab.brush_stroke(self.groom, "place", [(0.45, 0.85, 0.2)])
        placed = (set(groomlab.guide_ids(self.groom, "primary")) - before).pop()
        length = lambda p: sum(math.sqrt(sum((a - b) ** 2 for a, b in zip(p[i], p[i + 1]))) for i in range(len(p) - 1))
        self.assertAlmostEqual(length(guide_points(self.groom)[placed]), length(points[primary]), delta=0.3)

        # Erase removes what the brush touches, children too.
        count = len(groomlab.get_hierarchy(self.groom)["guides"])
        groomlab.brush_stroke(self.groom, "erase", [points[primary][0]], radius=0.15, level=0)
        self.assertNotIn(primary, groomlab.guide_ids(self.groom, "primary"))
        self.assertEqual(len(groomlab.get_hierarchy(self.groom)["guides"]), count - 4)  # the guide and 3 children

        # Resample keeps the shape with a new point count.
        other = self.primaries[1]
        groomlab.resample_guides(self.groom, 20, guides=[other])
        self.assertEqual(len(guide_points(self.groom)[other]), 21)

        # A frozen guide keeps its points; a whole group resampled keeps the count for its new
        # guides (from curves here), until set_group_segments clears it.
        groomlab.set_guide_attribute(self.groom, "freeze", 1.0, guides=[other])
        groomlab.resample_guides(self.groom, 6, group="primary")
        self.assertEqual(len(guide_points(self.groom)[other]), 21)
        # A guide given its own segments (picked) keeps them when its group's change.
        groomlab.set_guide_attribute(self.groom, "freeze", 0.0, guides=[other])
        groomlab.resample_guides(self.groom, 6, group="primary")
        self.assertEqual(len(guide_points(self.groom)[other]), 21)
        groomlab.resample_guides(self.groom, 6, guides=[other])  # its group's count: it follows the group again
        groomlab.resample_guides(self.groom, 5, group="primary")
        self.assertEqual(len(guide_points(self.groom)[other]), 6)
        # Reset Segments: every guide of the group at its count, its own given up.
        groomlab.resample_guides(self.groom, 11, guides=[other])
        self.assertEqual(groomlab.reset_group_segments(self.groom, "primary"), 5)
        self.assertEqual(set(groomlab.guide_segments(self.groom, group="primary")), {5})
        groomlab.resample_guides(self.groom, 6, group="primary")
        group = lambda: next(g for g in groomlab.list_guide_groups(self.groom) if g["name"] == "primary")
        self.assertEqual(group()["segments"], 6)
        curve = cmds.curve(degree=1, point=[(0.3, 0.95, 0), (0.3, 1.6, 0.3), (0.6, 1.9, 0.6)])
        made = groomlab.guides_from_curves(self.groom, [curve])
        self.assertEqual(len(guide_points(self.groom)[made[0]]), 7)
        cmds.undo()
        groomlab.set_group_segments(self.groom, "primary", 0)
        self.assertEqual(group()["segments"], 0)
        cmds.undo()
        self.assertEqual(group()["segments"], 6)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_scripted_strokes_reach_the_other_scalps(self):
        # A scripted stroke sees every scalp of the groom (its own and the ones add_scalp joins), as
        # the interactive brush does: a guide placed on the ear is rooted there, flow arrows laid
        # across it sit on its triangles.
        ear = cmds.polyPlane(width=0.8, height=0.8, subdivisionsX=6, subdivisionsY=6, name="ear")[0]
        cmds.xform(ear, translation=(1.6, 0.2, 0.0), rotation=(0, 0, 90))
        self.assertEqual(groomlab.add_scalp(self.groom, ear), "ear")
        head_triangles = cmds.polyEvaluate(self.scalp, triangle=True)
        before = set(groomlab.guide_ids(self.groom, "primary"))
        groomlab.brush_stroke(self.groom, "place", [(1.6, 0.25, 0.1)], match_neighbors=False, length=0.3)
        placed = (set(groomlab.guide_ids(self.groom, "primary")) - before).pop()
        root = guide_points(self.groom)[placed][0]
        self.assertLess(math.sqrt(sum((a - b) ** 2 for a, b in zip(root, (1.6, 0.25, 0.1)))), 1e-3, root)
        guide = next(g for g in groomlab.get_hierarchy(self.groom)["guides"] if str(g["id"]) == str(placed))
        self.assertGreaterEqual(guide["root"][0], head_triangles)  # on the ear's triangles of the joined scalp
        groomlab.flow_stroke(self.groom, [(1.6, -0.1 + 0.05 * i, 0.0) for i in range(9)], radius=0.1)
        arrows = groomlab.flow_arrows(self.groom)
        self.assertGreater(len(arrows), 2)
        self.assertTrue(all(face >= head_triangles for face, _, _ in arrows), arrows)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_tonic_children_by_choice_and_by_hand(self):
        groomlab.clear_guides(self.groom)
        curves = [cmds.curve(degree=1, point=[(math.sin(a), math.cos(a), 0.0), (1.5 * math.sin(a), 1.5 * math.cos(a), 0.0)])
                  for a in (-0.5, 0.0, 0.5)]
        groomlab.guides_from_curves(self.groom, curves)
        primaries = groomlab.guide_ids(self.groom, "primary")
        groomlab.select_guides(self.groom, guides=[primaries[0]])
        groomlab.generate_children(self.groom, count=2, selected=True, name="kids")
        parents = {g["parent"] for g in groomlab.get_hierarchy(self.groom)["guides"] if g.get("parent")}
        self.assertEqual({str(p) for p in parents}, {primaries[0]})  # only the chosen parent
        groomlab.generate_children(self.groom, count=3, guides=[primaries[2]], name="kids")  # added to the group
        self.assertEqual(len(groomlab.guide_ids(self.groom, "kids")), 5)

        # Place at level 1 adds a child by hand to the nearest parent; at level 2 it starts a new group.
        before = set(groomlab.guide_ids(self.groom, "kids"))
        groomlab.brush_stroke(self.groom, "place", [(0.0, 1.0, 0.1)], level=1)
        new = set(groomlab.guide_ids(self.groom, "kids")) - before
        self.assertEqual(len(new), 1)
        placed = [g for g in groomlab.get_hierarchy(self.groom)["guides"] if str(g["id"]) in new][0]
        self.assertEqual(str(placed["parent"]), primaries[1])
        groomlab.brush_stroke(self.groom, "place", [(0.05, 1.0, 0.1)], level=2)
        self.assertIn("kids_children", [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_a_cards_node_shows_without_a_mesh(self):
        from groomlab import graph as gm
        model = gm.GraphModel.from_groom(self.groom)
        node = model.add_node("Cards", params={"clumpChannel": "none", "maxCards": 12})
        model.insert_between(model.source_of("output", "strands"), "output", node)
        model.save(self.groom)
        self.assertFalse(cmds.getAttr(self.groom + ".displayCards"))  # the strands show until the cards view
        self.assertEqual(json.loads(cmds.groomLabGraphStats(self.groom))["cards"], 0)  # nothing built
        groomlab.compare_cards(self.groom, True)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertGreater(stats["cards"], 6)  # built for the viewport, no mesh needed
        groomlab.compare_cards(self.groom, False)
        self.assertEqual(json.loads(cmds.groomLabGraphStats(self.groom))["cards"], 0)
        # A card mesh shows them from then on (and follows the view); the groom draws none of its own.
        mesh = groomlab.create_card_mesh(self.groom)
        self.assertGreater(cmds.polyEvaluate(mesh, face=True), 6)
        self.assertFalse(cmds.getAttr(mesh + ".visibility"))
        # It carries what the groom's drawing shades the cards with, for the exports: each card's
        # tint (half of it: 0.5 none) and the strands' normals, locked.
        self.assertIn("groomLabTint", cmds.polyColorSet(mesh, query=True, allColorSets=True) or [])
        cmds.polyColorSet(mesh, currentColorSet=True, colorSet="groomLabTint")
        tint = cmds.polyColorPerVertex(mesh + ".vtx[0]", query=True, rgb=True)
        self.assertTrue(tint and all(0.0 <= c <= 1.0 for c in tint))
        self.assertTrue(all(cmds.polyNormalPerVertex(mesh + ".vtx[0]", query=True, freezeNormal=True)))
        groomlab.compare_cards(self.groom, True)
        self.assertTrue(cmds.getAttr(mesh + ".visibility"))
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_cards_are_built_at_full_density(self):
        # The viewport thins itself (previewDensity, previewPointBudget); the cards never do:
        # they are built from the whole groom (cardDensity 1), whatever the preview shows.
        self.assertEqual(cmds.getAttr(self.groom + ".cardDensity"), 1.0)
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        cmds.setAttr(self.groom + ".previewPointBudget", 0.0)
        full = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(full, 500)
        cmds.setAttr(self.groom + ".previewDensity", 0.1)
        cmds.setAttr(self.groom + ".previewPointBudget", 0.0005)  # 500 points: thins even the 10%
        preview = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertLess(preview, 0.08 * full)
        groomlab.create_cards(self.groom, max_cards=12)  # shown: built for the viewport
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["strands"], preview)  # the viewport, as thinned
        self.assertEqual(stats["cardStrands"], full)  # the cards, from every strand
        self.assertGreater(stats["cards"], 0)
        # The card mesh (outCards) is made from the same evaluation.
        mesh = groomlab.create_card_mesh(self.groom)
        self.assertGreater(cmds.polyEvaluate(mesh, face=True), 0)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["cardStrands"], full)
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), preview)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_instances_take_their_prototype_mesh(self):
        # An Instance node with a polyCube prototype: the viewport draws the cube at each instance
        # (headless, the draw itself can't be seen; the groom must evaluate and redraw without error
        # as the prototype is edited, swapped for another mesh and deleted).
        cube, maker = cmds.polyCube(name="bead")
        cmds.setAttr(cube + ".visibility", False)
        node = groomlab.add_instances(self.groom, cube, probability=0.5, scale=0.05, orient="strand")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        cmds.refresh(force=True)
        cmds.setAttr(maker + ".subdivisionsWidth", 3)  # the mesh changes shape
        cmds.refresh(force=True)
        drop = cmds.polySphere(name="drop", radius=1, subdivisionsX=8, subdivisionsY=6)[0]
        self.assertEqual(groomlab.set_instance_prototype(self.groom, node, drop), "drop")
        params = {n["name"]: n["params"] for n in groomlab.get_graph(self.groom)["nodes"]}
        self.assertEqual(params[node]["prototype"], "drop")
        cmds.refresh(force=True)
        cmds.delete(drop)  # a missing prototype: markers, no error
        cmds.refresh(force=True)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)

    def test_dragon_scales_preset_lays_a_scale_on_each_root(self):
        # The preset on the groom's scalp: a scale per strand (the built-in plates, no scene mesh),
        # the viewport's -instances matrices on the skin; a mesh of the user's own takes over as a
        # variant, and its edits redraw without error.
        from groomlab import presets
        presets.apply_preset(self.groom, "dragon_scales")
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        self.assertEqual(groomlab.errors(self.groom), [])
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertGreater(stats["instances"], 50)
        self.assertEqual(stats["instances"], cmds.getAttr(self.groom + ".outStrandCount"))
        matrices = cmds.groomLabGraphStats(self.groom, instances=True)
        self.assertEqual(len(matrices), 16 * stats["instances"])
        cmds.refresh(force=True)
        plate, maker = cmds.polyPlane(name="myPlate", width=1, height=1, subdivisionsX=2, subdivisionsY=2)
        cmds.setAttr(plate + ".visibility", False)
        groomlab.set_param(self.groom, "scales", "prototype", "groomlab:scale myPlate")
        cmds.refresh(force=True)
        cmds.setAttr(maker + ".subdivisionsWidth", 3)
        cmds.refresh(force=True)
        self.assertEqual(groomlab.errors(self.groom), [])

    def _scale_headings(self):
        """Each scale's direction on the skin (its +Y laid onto the unit sphere's tangent plane),
        for the scales on the upper half: the viewport's -instances matrices."""
        m = cmds.groomLabGraphStats(self.groom, instances=True)
        out = []
        for i in range(0, len(m), 16):
            y, origin = m[i + 4:i + 7], m[i + 12:i + 15]
            r = math.sqrt(sum(c * c for c in origin))
            n = [c / r for c in origin]
            d = sum(a * b for a, b in zip(y, n))
            h = [a - d * b for a, b in zip(y, n)]
            length = math.sqrt(sum(c * c for c in h))
            if origin[1] > 0.3 and length > 1e-9:
                out.append([c / length for c in h])
        return out

    def test_scales_turn_with_the_guides_and_the_flow(self):
        # Scales from guides: combing the guides across the head turns the scales with them; Scales
        # from flow: flow arrows drawn across turn them from the Comb's fallback down the body; Swept
        # spikes (follow strand) evaluates on the guides.
        from groomlab import presets
        presets.apply_preset(self.groom, "scales_from_guides")
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        self.assertEqual(groomlab.errors(self.groom), [])
        before = self._scale_headings()
        self.assertGreater(len(before), 50)
        groomlab.brush_stroke(self.groom, "comb", [(-1.0, 1.4, 0.2), (1.0, 1.4, 0.2)], radius=3.0, level=-1)
        after = self._scale_headings()
        self.assertEqual(len(after), len(before))
        mean_x = lambda hs: sum(h[0] for h in hs) / len(hs)
        self.assertGreater(mean_x(after), mean_x(before) + 0.3, "the scales turn with the combed guides")
        turned = sum(1 for a, b in zip(after, before) if sum(p * q for p, q in zip(a, b)) < 0.95)
        self.assertGreater(turned, len(before) // 2)
        cmds.undo()
        undone = self._scale_headings()
        self.assertTrue(all(abs(p - q) < 1e-5 for a, b in zip(undone, before) for p, q in zip(a, b)))

        presets.apply_preset(self.groom, "scales_from_flow")
        self.assertEqual(groomlab.errors(self.groom), [])
        down = self._scale_headings()
        self.assertLess(abs(mean_x(down)), 0.2)  # the Comb's fallback: down the body
        groomlab.flow_stroke(self.groom, [(-0.8, 0.7, 0.3), (0.0, 0.8, 0.4), (0.8, 0.7, 0.3)], radius=1.5)
        self.assertTrue(groomlab.flow_arrows(self.groom))
        across = self._scale_headings()
        self.assertGreater(mean_x(across), 0.5, "the scales follow the flow arrows")

        presets.apply_preset(self.groom, "swept_spikes")
        self.assertEqual(groomlab.errors(self.groom), [])
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["instances"], cmds.getAttr(self.groom + ".outStrandCount"))
        params = {n["name"]: n.get("params", {}) for n in groomlab.get_graph(self.groom)["nodes"]}
        self.assertEqual(params["spikes"]["follow"], "strand")
        cmds.refresh(force=True)

    def _points_inside(self, center, radius):
        """How many points of the groom's exported strands (density 1) lie inside the sphere."""
        path = os.path.join(tempfile.mkdtemp(prefix="groomlab_collide_"), "strands.abc")
        groomlab.export_alembic(self.groom, path)
        before = set(cmds.ls(long=True))
        cmds.loadPlugin("AbcImport", quiet=True)
        cmds.AbcImport(path, mode="import")
        inside = 0
        curves = [c for c in cmds.ls(type="nurbsCurve", long=True) if c not in before]
        self.assertGreater(len(curves), 100)
        for curve in curves:
            xyz = cmds.xform(curve + ".cv[*]", query=True, worldSpace=True, translation=True)
            inside += sum(1 for i in range(0, len(xyz), 3) if moved([xyz[i:i + 3]], [center]) < radius)
        cmds.delete([n for n in cmds.ls(long=True, assemblies=True) if n not in before])
        return inside

    def test_collider_keeps_strands_out(self):
        if not hasattr(cmds, "groomLabExportAlembic"):
            self.skipTest("built without Alembic")
        groomlab.set_param(self.groom, "scatter", "density", 30)  # a few hundred strands: quick exports
        groomlab.set_param(self.groom, "resample", "mode", "even")  # every strand its 8 segments: points to count
        # A small sphere in the hair over the first guide: strands run through it until it collides.
        n = (math.sin(0.35), math.cos(0.35), 0.2)
        center = tuple(1.5 * c for c in n)
        ball = cmds.polySphere(radius=0.4, subdivisionsX=24, subdivisionsY=16, name="collarBall")[0]
        cmds.xform(ball, translation=center)
        self.assertGreater(self._points_inside(center, 0.4), 20)
        node = groomlab.add_collider(self.groom, ball)
        self.assertEqual(node, "collarBall")
        graph = groomlab.get_graph(self.groom)
        nodes = {n["name"]: n for n in graph["nodes"]}
        self.assertEqual(nodes["collarBall"]["type"], "ColliderMesh")
        self.assertEqual(nodes["collarBall"]["params"]["name"], "collarBall")
        self.assertEqual(nodes["collide_collarBall"]["type"], "Collide")
        self.assertIn({"from": "collarBall", "to": "collide_collarBall", "input": "collider"}, graph["connections"])
        self.assertIn({"from": "scalp", "to": "collide_collarBall", "input": "scalp"}, graph["connections"])
        self.assertIn({"from": "collide_collarBall", "to": "output", "input": "strands"}, graph["connections"])
        self.assertEqual([(c["name"], c["mesh"].split("|")[-1]) for c in groomlab.colliders(self.groom)],
                         [("collarBall", "collarBallShape")])
        self.assertEqual(groomlab.inputs_summary(self.groom)["colliders"], groomlab.colliders(self.groom))
        self.assertEqual(groomlab.errors(self.groom), [])
        # Its friction for a Simulate node it feeds: the Simulate node's (-1) until set.
        self.assertEqual(groomlab.colliders(self.groom)[0]["friction"], -1.0)
        self.assertTrue(groomlab.set_collider_friction(self.groom, "collarBall", 0.7))
        self.assertAlmostEqual(groomlab.colliders(self.groom)[0]["friction"], 0.7)
        nodes = {n["name"]: n for n in groomlab.get_graph(self.groom)["nodes"]}
        self.assertAlmostEqual(nodes["collarBall"]["params"]["friction"], 0.7)
        self.assertFalse(groomlab.set_collider_friction(self.groom, "noSuchCollider", 0.2))
        # The export (NodeEvaluator) collides too: no strand point inside the ball (its faces
        # lie a little inside the true sphere; Collide keeps points `offset` above them).
        self.assertEqual(self._points_inside(center, 0.38), 0)
        # The collider moves: the strands keep out of where it is now.
        cmds.xform(ball, translation=tuple(c + 0.3 for c in center))
        self.assertEqual(self._points_inside(tuple(c + 0.3 for c in center), 0.38), 0)
        self.assertEqual(groomlab.errors(self.groom), [])
        # Removed: the nodes go, strands pass straight through again and run through the ball.
        self.assertTrue(groomlab.remove_collider(self.groom, "collarBall"))
        self.assertEqual(groomlab.colliders(self.groom), [])
        graph = groomlab.get_graph(self.groom)
        names = [n["name"] for n in graph["nodes"]]
        self.assertNotIn("collarBall", names)
        self.assertNotIn("collide_collarBall", names)
        self.assertIn({"from": "resample", "to": "output", "input": "strands"}, graph["connections"])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(self._points_inside(tuple(c + 0.3 for c in center), 0.4), 20)
        self.assertFalse(groomlab.remove_collider(self.groom, "collarBall"))
        # wire=False: the mesh input alone, for wiring by hand in the Graph Editor.
        self.assertEqual(groomlab.add_collider(self.groom, ball, name="ball", wire=False, friction=0.5), "ball")
        graph = groomlab.get_graph(self.groom)
        self.assertEqual([n["type"] for n in graph["nodes"] if n["name"] == "ball"], ["ColliderMesh"])
        self.assertEqual([c["friction"] for c in groomlab.colliders(self.groom)], [0.5])
        self.assertFalse([n for n in graph["nodes"] if n["type"] == "Collide"])
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_deforming_collider_asks_its_rest_field(self):
        # A ball in the hair over the first guide, moved and squashed by an animated blendShape:
        # add_collider connects its original shape as its rest pose, so Collide asks the rest pose's
        # distance field, made once, on every frame, where without it each frame's ball gets a field
        # of its own. Strands keep out of the deformed ball either way.
        if not hasattr(cmds, "groomLabExportAlembic"):
            self.skipTest("built without Alembic")
        groomlab.set_param(self.groom, "scatter", "density", 30)  # a few hundred strands: quick exports
        groomlab.set_param(self.groom, "resample", "mode", "even")
        n = (math.sin(0.35), math.cos(0.35), 0.2)
        center = tuple(1.5 * c for c in n)
        ball = cmds.polySphere(radius=0.4, subdivisionsX=24, subdivisionsY=16, name="bouncyBall")[0]
        cmds.xform(ball, translation=center)
        target = cmds.polySphere(radius=0.4, subdivisionsX=24, subdivisionsY=16, name="bouncyBallTarget")[0]
        cmds.scale(1.0, 0.9, 1.0, target + ".vtx[*]", pivot=(0, 0, 0), relative=True)
        cmds.move(0.2, 0.0, 0.0, target + ".vtx[*]", relative=True)
        bounce = cmds.blendShape(target, ball, name="bounce")[0]
        cmds.delete(target)
        cmds.setKeyframe(bounce + ".w[0]", time=1, value=0.0)
        cmds.setKeyframe(bounce + ".w[0]", time=5, value=1.0)
        deformed = (center[0] + 0.2, center[1], center[2])
        inner = 0.32  # inside the squashed ball (0.36 high), its faces a little inside it
        cmds.currentTime(5)
        self.assertGreater(self._points_inside(deformed, inner), 10)

        groomlab.add_collider(self.groom, ball)
        [collider] = groomlab.colliders(self.groom)
        self.assertTrue(collider["rest"] and cmds.getAttr(collider["rest"] + ".intermediateObject"), collider)
        self.assertEqual(groomlab.errors(self.groom), [])

        def fields_made_per_frame():
            made = []
            for frame in range(1, 6):
                cmds.currentTime(frame)
                cmds.getAttr(self.groom + ".outStrandCount")  # evaluated at this frame
                made.append(json.loads(cmds.groomLabGraphStats(self.groom))["distanceFieldsMade"])
            return [b - a for a, b in zip(made, made[1:])]

        # Through the rest pose: no field made after the first frames (the scalp's, the rest pose's).
        self.assertEqual(fields_made_per_frame()[1:], [0, 0, 0])
        cmds.currentTime(5)
        self.assertEqual(self._points_inside(deformed, inner), 0)
        # Without a rest pose (rest=False swaps the collider's connection): a field for each frame,
        # the strands kept out all the same.
        groomlab.add_collider(self.groom, ball, rest=False)
        self.assertIsNone(groomlab.colliders(self.groom)[0]["rest"])
        self.assertTrue(all(made >= 1 for made in fields_made_per_frame()))
        cmds.currentTime(5)
        self.assertEqual(self._points_inside(deformed, inner), 0)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_live_cards_and_bake(self):
        node = groomlab.create_cards(self.groom, max_cards=24, segments=6)
        self.assertEqual([n["type"] for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == node], ["Cards"])
        self.assertTrue(cmds.getAttr(self.groom + ".displayCards"))  # shown, drawn by the groom
        self.assertIsNone(groomlab.card_mesh(self.groom))  # no mesh needed
        # A live mesh on request (for exporting by hand), following the groom.
        cards = groomlab.create_card_mesh(self.groom)
        mesh = cmds.listRelatives(cards, shapes=True)[0]
        faces = cmds.polyEvaluate(mesh, face=True)
        self.assertGreater(faces, 24 * 6 / 2)  # most clusters make a card
        self.assertEqual(cmds.polyEvaluate(mesh, uvcoord=True), cmds.polyEvaluate(mesh, vertex=True))
        tall = cmds.exactWorldBoundingBox(mesh)
        groomlab.set_param(self.groom, "length", "keep", 0.4)  # the cards follow the groom
        short = cmds.exactWorldBoundingBox(mesh)
        self.assertLess(short[4] - short[1], tall[4] - tall[1])
        self.assertEqual(groomlab.create_cards(self.groom), node)  # one Cards node per groom
        self.assertEqual(groomlab.create_card_mesh(self.groom), cards)  # one mesh
        # A Cards node in the graph holds the settings; changing them reshapes the cards.
        nodes = [n for n in groomlab.get_graph(self.groom)["nodes"] if n["type"] == "Cards"]
        self.assertEqual(len(nodes), 1)
        vertices = cmds.polyEvaluate(mesh, vertex=True)
        groomlab.create_cards(self.groom, max_cards=24, segments=6, width_segments=3, curvature=0.5, twist=45, taper=0.3)
        self.assertEqual(cmds.polyEvaluate(mesh, vertex=True), vertices * 2)  # 4 across instead of 2
        self.assertEqual(groomlab.errors(self.groom), [])

        folder = tempfile.mkdtemp(prefix="groomlab_cards_")
        paths = groomlab.bake_cards(self.groom, folder, resolution=256, name="fur")
        self.assertEqual(sorted(os.path.basename(p) for p in paths),
                         ["fur_AO.tga", "fur_Atlas.json", "fur_Attribute.tga", "fur_Color.tga", "fur_Coverage.tga",
                          "fur_Depth.tga", "fur_Normal.tga", "fur_Tangent.tga"])
        self.assertEqual(os.path.getsize(os.path.join(folder, "fur_Coverage.tga")), 18 + 256 * 256)
        self.assertEqual(os.path.getsize(os.path.join(folder, "fur_Attribute.tga")), 18 + 4 * 256 * 256)
        # The cards view shows them (cardMaps); a live mesh, when there is one, gets them as its material.
        maps = cmds.getAttr(self.groom + ".cardMaps")
        self.assertIn("Color=" + folder.replace("\\", "/") + "/fur_Color.tga", maps)
        self.assertIn("Coverage=", maps)
        shape = cmds.listRelatives(cards, shapes=True)[0]
        group = cmds.listConnections(shape, type="shadingEngine")[0]
        material = cmds.listConnections(group + ".surfaceShader")[0]
        self.assertEqual(cmds.nodeType(material), "lambert")
        self.assertTrue(cmds.listConnections(material + ".transparency"))
        self.assertTrue(cmds.listConnections(material + ".color"))
        # A live mesh made after the bake gets the maps too (Maya's own transparency, its ambient
        # occlusion with it: the groom's own textured cards stay out of it).
        cmds.delete(cards)
        cards = groomlab.create_card_mesh(self.groom)
        shape = cmds.listRelatives(cards, shapes=True)[0]
        material = cmds.listConnections(cmds.listConnections(shape, type="shadingEngine")[0] + ".surfaceShader")[0]
        coverage = cmds.listConnections(material + ".transparency") or []
        self.assertTrue(coverage)
        self.assertTrue(cmds.listConnections(material + ".color"))
        files = cmds.listHistory(coverage[0]) or []
        self.assertIn(folder.replace("\\", "/") + "/fur_Coverage.tga",
                      [cmds.getAttr(f + ".fileTextureName").replace("\\", "/") for f in files if cmds.nodeType(f) == "file"])

    def test_baked_cards_keep_their_atlas_cells_when_guides_move(self):
        # Once baked, the cards rebuilt after a brush stroke keep the cells they were baked
        # with (the atlas saved next to the maps, matched by the strands each card holds), so
        # the maps keep fitting; unbaked, a rebuild lays the atlas out afresh.
        groomlab.create_cards(self.groom, max_cards=16)
        cards = groomlab.create_card_mesh(self.groom)
        shape = cmds.listRelatives(cards, shapes=True)[0]

        def cells():
            # Every face's v range (a slice of its card's cell, v0 at the root, v1 at the tip).
            n = cmds.polyEvaluate(shape, uvcoord=True)
            vs = cmds.polyEditUV(shape + ".map[0:%d]" % (n - 1), query=True)[1::2]
            found = set()
            for f in range(cmds.polyEvaluate(shape, face=True)):
                ids = cmds.ls(cmds.polyListComponentConversion(shape + ".f[%d]" % f, fromFace=True, toUV=True), flatten=True)
                ids = [int(re.search(r"map\[(\d+)\]", m).group(1)) for m in ids]
                found.add((min(vs[i] for i in ids), max(vs[i] for i in ids)))
            return found

        def atlas_cells():
            with open(os.path.join(folder, "lock_Atlas.json")) as f:
                return {(e["cell"][1], e["cell"][3]) for e in json.load(f)["cards"]}

        def within(faces, atlas):  # every face's range lies in some cell of the atlas
            return all(any(v0 - 1e-4 <= a <= b <= v1 + 1e-4 for v0, v1 in atlas) for a, b in faces)

        folder = tempfile.mkdtemp(prefix="groomlab_atlas_")
        groomlab.bake_cards(self.groom, folder, resolution=64, name="lock")
        self.assertTrue(os.path.exists(os.path.join(folder, "lock_Atlas.json")))
        self.assertIn("Atlas=" + folder.replace("\\", "/") + "/lock_Atlas.json", cmds.getAttr(self.groom + ".cardMaps"))
        baked = atlas_cells()
        self.assertGreater(len(baked), 4)
        self.assertTrue(within(cells(), baked))
        groomlab.brush_stroke(self.groom, "comb", [(0.2, 1.2, 0.3), (0.6, 1.4, 0.3), (1.0, 1.5, 0.3)], radius=0.6)
        self.assertEqual(groomlab.errors(self.groom), [])
        after = cells()
        self.assertGreater(len(after), 4)
        self.assertTrue(within(after, baked), "faces outside the baked cells")
        # Rebaked after a change upstream: the cards up (laid out for the old atlas) take the
        # new one at once, without another change to rebuild them.
        groomlab.set_param(self.groom, "length", "keep", 0.7)  # the cards rebuild, for the old atlas
        self.assertEqual(groomlab.errors(self.groom), [])
        cells()
        groomlab.bake_cards(self.groom, folder, resolution=64, name="lock")
        self.assertEqual(groomlab.errors(self.groom), [])
        fresh = atlas_cells()
        self.assertNotEqual(fresh, baked)  # the strands changed: another layout
        self.assertTrue(within(cells(), fresh), "faces outside the new bake's cells")
        # The maps' age: fresh now, out of date once the cards change (strands or card params),
        # fresh again after a rebake.
        self.assertEqual((groomlab.card_stats(self.groom)["baked"], groomlab.card_stats(self.groom)["stale"]), (True, False))
        groomlab.set_param(self.groom, "length", "keep", 0.6)
        self.assertTrue(groomlab.card_stats(self.groom)["stale"])
        self.assertTrue(groomlab.card_display(self.groom)["stale"])
        groomlab.set_param(self.groom, "length", "keep", 0.7)  # back to what was baked: fits again
        self.assertFalse(groomlab.card_stats(self.groom)["stale"])
        node = [n["name"] for n in groomlab.get_graph(self.groom)["nodes"] if n["type"] == "Cards"][0]
        groomlab.set_param(self.groom, node, "widthScale", 1.5)
        self.assertTrue(groomlab.card_stats(self.groom)["stale"])
        groomlab.bake_cards(self.groom, folder, resolution=64, name="lock")
        self.assertFalse(groomlab.card_stats(self.groom)["stale"])

    def test_bake_without_a_card_mesh(self):
        groomlab.create_cards(self.groom, max_cards=20)
        folder = tempfile.mkdtemp(prefix="groomlab_nomesh_")
        paths = groomlab.bake_cards(self.groom, folder, resolution=128, name="bare")
        self.assertEqual(len(paths), 8)  # seven maps and the atlas
        self.assertIsNone(groomlab.card_mesh(self.groom))  # none made
        self.assertIn("bare_Color.tga", cmds.getAttr(self.groom + ".cardMaps"))
        first = cmds.getAttr(self.groom + ".cardMaps")
        groomlab.bake_cards(self.groom, folder, resolution=128, name="bare")
        self.assertNotEqual(cmds.getAttr(self.groom + ".cardMaps"), first)  # a rebake reloads in the viewport

    def test_card_map_exports_pack_maps_as_presets_say(self):
        groomlab.create_cards(self.groom, max_cards=20)
        folder = tempfile.mkdtemp(prefix="groomlab_card_maps_")
        names = [p["name"] for p in groomlab.card_map_presets()]
        self.assertEqual(names[:4], ["Unreal Hair Cards", "Unreal (packed)", "Unity HDRP Hair", "Separate maps (all)"])
        sources = groomlab.card_map_sources()
        self.assertIn("normal.z", sources["channels"])
        self.assertIn("translucency", [s["name"] for s in sources["sources"]])
        stored = cmds.getAttr(self.groom + ".cardMaps")
        # The default preset: Bake Maps' six files, byte for byte, and the groom's own bake untouched.
        baked = groomlab.bake_cards(self.groom, folder, resolution=64, name="old", store=False)
        paths = groomlab.bake_card_maps(self.groom, resolution=64, directory=folder, name="new")
        self.assertEqual([os.path.basename(p) for p in paths], ["new_%s.tga" % m for m in groomlab.CARD_MAPS])
        for old, new in zip([p for p in baked if p.endswith(".tga")], paths):
            with open(old, "rb") as a, open(new, "rb") as b:
                self.assertEqual(a.read(), b.read(), new)
        self.assertEqual(cmds.getAttr(self.groom + ".cardMaps"), stored)
        # Every map its own file, as 16-bit PNG, supersampled, named by a template.
        separate = groomlab.bake_card_maps(self.groom, preset="Separate maps (all)", resolution=64, format="png",
                                           bit_depth=16, supersample=2, padding=4, directory=folder, name="fur",
                                           name_template="{name}_{res}_{map}")
        self.assertIn("fur_64_Normal.png", [os.path.basename(p) for p in separate])
        with open(separate[0], "rb") as f:
            header = f.read(26)
        self.assertEqual(header[1:4], b"PNG")
        self.assertEqual(header[24], 16)  # bit depth
        # A packing of our own, then saved as a preset and used by name; EXR half floats.
        mine = {"name": "Test Packed", "normal": "directx",
                "files": [{"map": "Mask", "channels": ["depth", "root", "id", "1-coverage"]},
                          {"map": "Normal", "channels": ["normal.x", "normal.y", "normal.z"]}]}
        packed = groomlab.bake_card_maps(self.groom, preset=mine, resolution=32, format="exr", directory=folder,
                                         name="mine")
        self.assertEqual([os.path.basename(p) for p in packed], ["mine_Mask.exr", "mine_Normal.exr"])
        with open(packed[0], "rb") as f:
            self.assertEqual(f.read(4), b"\x76\x2f\x31\x01")  # OpenEXR's magic number
        path = groomlab.save_card_map_preset("Test Packed", mine)
        self.assertTrue(os.path.isfile(path))
        self.assertIn("Test Packed", [p["name"] for p in groomlab.card_map_presets()])
        again = groomlab.bake_card_maps(self.groom, preset="Test Packed", resolution=32, format="tga", directory=folder,
                                        name="again")
        self.assertEqual(len(again), 2)
        with self.assertRaises(ValueError):
            groomlab.save_card_map_preset("Unreal Hair Cards", mine)  # a built-in's name
        self.assertTrue(groomlab.delete_card_map_preset("Test Packed"))
        self.assertNotIn("Test Packed", [p["name"] for p in groomlab.card_map_presets()])
        with self.assertRaises(RuntimeError):  # no such preset; no 32-bit TGA
            groomlab.bake_card_maps(self.groom, preset="Nothing", resolution=32, directory=folder)
        with self.assertRaises(RuntimeError):
            groomlab.bake_card_maps(self.groom, resolution=32, bit_depth=32, directory=folder)

    def _layered_cards(self):
        """... -> width -> outer (Cards) -> frizz (Noise) -> inner (Cards) -> look (Noise) -> output."""
        from groomlab import graph as gm
        outer = groomlab.create_cards(self.groom, max_cards=12)
        model = gm.GraphModel.from_groom(self.groom)
        model.set_param(outer, "clumpChannel", "none")
        frizz = model.add_node("Noise", name="frizz", params={"amplitude": 0.02})
        model.insert_before("output", frizz)
        inner = model.add_node("Cards", name="inner", params={"clumpChannel": "none", "maxCards": 20})
        model.insert_before("output", inner)
        look = model.add_node("Noise", name="look", params={"amplitude": 0.05})
        model.insert_before("output", look)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        return outer, inner

    def test_each_cards_node_has_its_own_stats_and_bake_stamp(self):
        outer, inner = self._layered_cards()
        stats = groomlab.card_stats(self.groom)
        self.assertEqual(set(stats["nodes"]), {outer, inner})
        self.assertEqual(stats["cards"], stats["nodes"][outer]["cards"] + stats["nodes"][inner]["cards"])
        self.assertEqual(stats["triangles"], stats["nodes"][outer]["triangles"] + stats["nodes"][inner]["triangles"])
        self.assertGreater(stats["nodes"][inner]["cards"], stats["nodes"][outer]["cards"])
        self.assertEqual(groomlab.card_stats(self.groom, node=inner)["cards"], stats["nodes"][inner]["cards"])
        folder = tempfile.mkdtemp(prefix="groomlab_layers_")
        groomlab.bake_cards(self.groom, folder, resolution=64, name="layers")
        # The bake's stamp and atlas are per Cards node.
        maps = cmds.getAttr(self.groom + ".cardMaps")
        stamp = re.search(r"Strands=([^;]*)", maps).group(1)
        self.assertEqual({entry.split(":")[0] for entry in stamp.split(",")}, {outer, inner})
        with open(os.path.join(folder, "layers_Atlas.json")) as f:
            self.assertEqual({e["node"] for e in json.load(f)["cards"]}, {outer, inner})
        stats = groomlab.card_stats(self.groom)
        self.assertEqual((stats["baked"], stats["stale"]), (True, False))
        # A look after the cards: neither node's maps go out of date.
        groomlab.set_param(self.groom, "look", "amplitude", 0.2)
        self.assertFalse(groomlab.card_stats(self.groom)["stale"])
        # The strands between the two: only the inner node's.
        groomlab.set_param(self.groom, "frizz", "amplitude", 0.08)
        stats = groomlab.card_stats(self.groom)
        self.assertTrue(stats["stale"])
        self.assertEqual((stats["nodes"][outer]["stale"], stats["nodes"][inner]["stale"]), (False, True))
        self.assertFalse(groomlab.card_stats(self.groom, node=outer)["stale"])
        self.assertTrue(groomlab.card_stats(self.groom, node=inner)["stale"])
        groomlab.set_param(self.groom, "frizz", "amplitude", 0.02)  # back as baked
        self.assertFalse(groomlab.card_stats(self.groom)["stale"])
        # A card param of the outer node: the outer node's only.
        groomlab.set_param(self.groom, outer, "widthScale", 1.5)
        stats = groomlab.card_stats(self.groom)
        self.assertEqual((stats["nodes"][outer]["stale"], stats["nodes"][inner]["stale"]), (True, False))
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_layered_cards_keep_their_own_baked_cells(self):
        # Two Cards nodes over the same strands: after a bake, the inner node's cards (subsets
        # of the outer ones' strands) keep their own cells instead of taking the outer cards'.
        outer, inner = self._layered_cards()
        groomlab.set_param(self.groom, outer, "variations", 0)  # every card its own texture
        groomlab.set_param(self.groom, inner, "variations", 0)
        mesh = groomlab.create_card_mesh(self.groom)
        shape = cmds.listRelatives(mesh, shapes=True)[0]

        def uvs():
            n = cmds.polyEvaluate(shape, uvcoord=True)
            return cmds.polyEditUV(shape + ".map[0:%d]" % (n - 1), query=True)

        before = uvs()
        folder = tempfile.mkdtemp(prefix="groomlab_layers_")
        groomlab.bake_cards(self.groom, folder, resolution=64, name="layers")
        with open(os.path.join(folder, "layers_Atlas.json")) as f:
            cells = [tuple(e["cell"]) for e in json.load(f)["cards"]]
        self.assertEqual(len(set(cells)), len(cells))  # every card its own cell at the bake
        # The rebuild (the same cards, laid out for the bake) shows every card where it was baked.
        groomlab.set_param(self.groom, "look", "amplitude", 0.1)  # re-evaluates, the cards stay
        after = uvs()
        self.assertEqual(len(after), len(before))
        for a, b in zip(after, before):
            self.assertAlmostEqual(a, b, places=4)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_only_cards_nodes_making_the_output_build_cards(self):
        from groomlab import graph as gm
        node = groomlab.create_cards(self.groom, max_cards=12)  # shown
        self.assertGreater(groomlab.card_stats(self.groom)["cards"], 0)
        model = gm.GraphModel.from_groom(self.groom)
        model.set_bypass(node, True)
        model.save(self.groom)
        stats = groomlab.card_stats(self.groom)
        self.assertEqual((stats["cards"], stats["nodes"]), (0, {}))
        # Not connected to the output: nothing either.
        model = gm.GraphModel.from_groom(self.groom)
        source = model.source_of(node, "strands")
        model.remove_node(node)
        model.connect(source, "output", "strands")
        loose = model.add_node("Cards", name="loose", params={"clumpChannel": "none", "maxCards": 12})
        model.connect(source, loose, "strands")
        model.save(self.groom)
        self.assertEqual(groomlab.card_stats(self.groom)["cards"], 0)
        # Inside a compound on the way to the output: built, named by where it sits.
        model = gm.GraphModel.from_groom(self.groom)
        model.remove_node(loose)
        cards = model.add_node("Cards", name="cards", params={"clumpChannel": "none", "maxCards": 12})
        model.insert_before("output", cards)
        wrap = model.collapse([cards], compound="wrap")
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        stats = groomlab.card_stats(self.groom)
        self.assertGreater(stats["cards"], 0)
        self.assertEqual(set(stats["nodes"]), {"wrap/cards"})
        self.assertEqual(groomlab.card_stats(self.groom, node=wrap)["cards"], stats["cards"])

    def test_the_look_comes_from_the_nodes_making_the_output(self):
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)  # no HairColor yet
        from groomlab import graph as gm
        model = gm.GraphModel.from_groom(self.groom)
        dull = model.add_node("HairColor", name="dull", params={"specular": 0.1, "roughness": 0.2, "tilt": 4.5})
        model.insert_before("output", dull)
        shiny = model.add_node("HairColor", name="shiny", params={"specular": 0.9, "roughness": 0.8})
        model.connect("width", shiny, "strands")  # connected to nothing downstream
        model.save(self.groom)
        self.assertEqual(groomlab.hair_color_node(self.groom), "dull")  # not the last by name
        self.assertEqual(groomlab.hair_look(self.groom), {"specular": 0.1, "roughness": 0.2, "tilt": 4.5})
        # A Switch picks one of the two.
        model = gm.GraphModel.from_groom(self.groom)
        pick = model.add_node("Switch", name="pick", params={"index": 1})
        model.connect(dull, pick, "a")
        model.connect(shiny, pick, "b")
        model.connect(pick, "output", "strands")
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.hair_color_node(self.groom), "shiny")
        self.assertAlmostEqual(groomlab.hair_look(self.groom)["specular"], 0.9, places=5)
        groomlab.set_param(self.groom, pick, "index", 0)
        self.assertEqual(groomlab.hair_color_node(self.groom), "dull")
        # Bypassed: it makes nothing, the defaults show.
        model = gm.GraphModel.from_groom(self.groom)
        model.set_bypass(dull, True)
        model.save(self.groom)
        self.assertEqual(groomlab.hair_color_node(self.groom), "")
        self.assertEqual(groomlab.hair_look(self.groom), {"specular": 0.3, "roughness": 0.5, "tilt": 3.0})
        self.assertEqual(groomlab.render_data_params(self.groom), None)  # no RenderData at all
        self.assertEqual(groomlab.hair_color_node(self.groom), "")
        self.assertEqual(groomlab.hair_color_params(self.groom), {})
        # Inside a compound, with an exposed param set on the compound.
        model = gm.GraphModel.from_groom(self.groom)
        model.set_bypass(dull, False)
        wrap = model.collapse([dull], compound="wrap")
        model.expose(wrap, "shine", "dull.specular")
        model.set_param(wrap, "shine", 0.6)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.hair_color_node(self.groom), "wrap/dull")
        self.assertEqual(groomlab.hair_look(self.groom), {"specular": 0.6, "roughness": 0.2, "tilt": 4.5})

    def test_create_cards_and_presets_set_every_cards_node_or_the_named_one(self):
        from groomlab import graph as gm
        first = groomlab.create_cards(self.groom, max_cards=12)
        model = gm.GraphModel.from_groom(self.groom)
        second = model.add_node("Cards", name="second", params={"maxCards": 30})
        model.insert_before("output", second)
        model.save(self.groom)

        def param(node, name):
            return [n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == node][0]["params"].get(name)

        self.assertEqual(groomlab.create_cards(self.groom, segments=5), first)  # all of them
        self.assertEqual((param(first, "segments"), param(second, "segments")), (5, 5))
        self.assertEqual(groomlab.create_cards(self.groom, segments=7, node=second), second)  # that one
        self.assertEqual((param(first, "segments"), param(second, "segments")), (5, 7))
        low = groomlab.cards_preset(self.groom, "low")
        self.assertEqual((param(first, "maxCards"), param(second, "maxCards")), (low["maxCards"], low["maxCards"]))
        hero = groomlab.cards_preset(self.groom, "hero", node=first)
        self.assertEqual((param(first, "maxCards"), param(second, "maxCards")), (hero["maxCards"], low["maxCards"]))
        self.assertEqual((param(first, "atlasSize"), param(second, "atlasSize")), (4096, 1024))
        with self.assertRaises(ValueError):
            groomlab.cards_preset(self.groom, "low", node="nothing")
        with self.assertRaises(ValueError):
            groomlab.create_cards(self.groom, node="nothing")
        self.assertEqual(len([n for n in groomlab.get_graph(self.groom)["nodes"] if n["type"] == "Cards"]), 2)

    def test_card_lods_and_export_for_unreal(self):
        groomlab.create_cards(self.groom, max_cards=40)
        groomlab.create_card_mesh(self.groom)  # a live one: A/B shows it (the export makes its own otherwise)
        stats = {}
        for lod in ("hero", "mid", "low"):
            params = groomlab.cards_preset(self.groom, lod)
            stats[lod] = groomlab.card_stats(self.groom)
            self.assertLessEqual(stats[lod]["cards"], params["maxCards"])
            self.assertLessEqual(stats[lod]["textures"], params["variations"])
            self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(stats["hero"]["triangles"], stats["mid"]["triangles"])
        self.assertGreater(stats["mid"]["triangles"], stats["low"]["triangles"])
        self.assertLessEqual(stats["low"]["triangles"], 4000)
        with self.assertRaises(ValueError):
            groomlab.cards_preset(self.groom, "ultra")
        # A/B against the strands.
        mesh = groomlab.card_mesh(self.groom)
        # A/B is the cards view: the card mesh shows instead of the strands; the groom (guides) stays.
        self.assertEqual(groomlab.compare_cards(self.groom, True), "cards")
        self.assertTrue(cmds.getAttr(self.groom + ".displayCards"))
        self.assertTrue(cmds.getAttr(mesh + ".visibility"))
        self.assertTrue(cmds.getAttr(self.groom + ".visibility"))
        self.assertEqual(groomlab.compare_cards(self.groom), "strands")
        self.assertFalse(cmds.getAttr(self.groom + ".displayCards"))
        self.assertFalse(cmds.getAttr(mesh + ".visibility"))  # it follows the view
        self.assertTrue(cmds.getAttr(self.groom + ".visibility"))
        # Unreal: FBX, maps and a manifest.
        folder = tempfile.mkdtemp(prefix="groomlab_unreal_cards_")
        result = groomlab.export_cards_unreal(self.groom, folder, name="hero", resolution=128)
        self.assertTrue(os.path.getsize(result["fbx"]) > 1000)
        self.assertEqual(len(result["maps"]), 9)  # the cards' seven and the scalp's two
        self.assertTrue(os.path.getsize(result["scalpCap"]) > 1000)
        with open(result["manifest"]) as f:
            manifest = json.load(f)
        self.assertEqual(manifest["maps"]["Coverage"], "hero_Coverage.tga")
        self.assertEqual(manifest["triangles"], stats["low"]["triangles"])
        self.assertIn("coordU", manifest["channels"]["Attribute"])
        self.assertEqual(manifest["maps"]["ScalpColor"], "hero_ScalpColor.tga")
        # Without a live mesh, the export makes one for the FBX and removes it.
        cmds.delete(mesh)
        before = set(cmds.ls())
        again = groomlab.export_cards_unreal(self.groom, folder, name="again", resolution=64, scalp=False)
        self.assertTrue(os.path.getsize(again["fbx"]) > 1000)
        self.assertIsNone(groomlab.card_mesh(self.groom))
        self.assertEqual(set(cmds.ls()) - before, set())

    def _read_back_strands(self, path):
        """A groom whose strands are the file's curves, read by GroomLab's own Alembic reader
        (ImportCurves, points as written): (strands, points, bounding box)."""
        reader = groomlab.create_groom(name="readBack")
        groomlab.set_graph(reader, {
            "nodes": [{"name": "guides", "type": "ImportCurves", "params": {"path": path, "samples": 0}},
                      {"name": "strands", "type": "ToStrands"}, {"name": "output", "type": "Output"}],
            "connections": [{"from": "guides", "to": "strands", "input": "guides"},
                            {"from": "strands", "to": "output", "input": "strands"}],
            "output": "output"})
        cmds.setAttr(reader + ".previewDensity", 1.0)
        self.assertEqual(groomlab.errors(reader), [])
        stats = json.loads(cmds.groomLabGraphStats(reader))
        box = cmds.exactWorldBoundingBox(reader)
        cmds.delete(cmds.listRelatives(reader, parent=True))
        return stats["strands"], stats["points"], box

    def test_export_unity_strands(self):
        folder = tempfile.mkdtemp(prefix="groomlab_unity_strands_")
        plain = os.path.join(folder, "plain.abc").replace("\\", "/")
        unity = os.path.join(folder, "unity.abc").replace("\\", "/")
        self.assertEqual(groomlab.export_unity_strands(self.groom, plain), 1)  # centimeters, as Unity's defaults want
        self.assertEqual(groomlab.export_unity_strands(self.groom, unity, scale=0.01, resample_to=8), 1)
        strands, points, box = self._read_back_strands(plain)
        u_strands, u_points, u_box = self._read_back_strands(unity)
        self.assertGreater(strands, 100)
        self.assertEqual(u_strands, strands)
        self.assertEqual(u_points, 8 * u_strands)  # every strand 8 points
        self.assertGreater(points, 2 * strands)
        for a, b in zip(u_box, box):  # meters: a hundredth (resampled points lie on the strands)
            self.assertAlmostEqual(a, 0.01 * b, delta=0.002 * max(abs(v) for v in box))
        self.assertLess(max(abs(v) for v in u_box), 0.03)
        # Hair alone is one groom group: grouped (the default) or not, the same file.
        one = os.path.join(folder, "one.abc").replace("\\", "/")
        self.assertEqual(groomlab.export_unity_strands(self.groom, one, groups=None), 1)
        self.assertEqual(md5(one), md5(plain))
        with self.assertRaises(ValueError):
            groomlab.export_unity_strands(self.groom, unity, resample_to=2)  # Unity keeps 3 to 128
        with self.assertRaises(RuntimeError):  # Unreal's layout has its own units
            cmds.groomLabExportAlembic(self.groom, file=unity, unreal=True, scale=0.01)
        with self.assertRaises(RuntimeError):
            cmds.groomLabExportAlembic(self.groom, file=unity, pointsPerStrand=1)

    def test_export_cards_for_unity(self):
        groomlab.create_cards(self.groom, max_cards=12)
        folder = tempfile.mkdtemp(prefix="groomlab_unity_cards_")
        groomlab.bake_cards(self.groom, os.path.join(folder, "own"), resolution=32)  # the groom's own bake
        maps = cmds.getAttr(self.groom + ".cardMaps")
        before = set(cmds.ls())
        result = groomlab.export_cards_unity(self.groom, folder, resolution=64, lod="mid")
        name = groomlab.asset_name(self.groom) + "_mid"
        self.assertEqual(result["fbx"], "%s/%s_cards.fbx" % (folder.replace("\\", "/"), name))
        self.assertGreater(os.path.getsize(result["fbx"]), 1000)
        self.assertGreater(os.path.getsize(result["scalpCap"]), 1000)
        self.assertEqual(fbx_unit_scale(result["fbx"]), 100.0)  # meters: 100 cm per unit
        written = sorted(os.path.basename(p) for p in result["maps"])
        self.assertEqual(written, sorted("%s_%s.png" % (name, m) for m in groomlab.UNITY_CARD_MAPS + ("ScalpBaseColor",)))
        kinds = {}
        for p in result["maps"]:
            with open(p, "rb") as f:
                width, height, color = png_info(f.read())
            self.assertEqual(width, height)
            kinds[os.path.basename(p)[len(name) + 1:-4]] = color
        self.assertEqual(kinds["BaseColor"], 6)  # RGBA: color and coverage
        self.assertEqual(kinds["Mask"], 6)
        self.assertEqual(kinds["AO"], 0)         # gray
        self.assertEqual(kinds["Tangent"], 2)    # RGB
        self.assertEqual(kinds["Normal"], 2)     # RGB
        with open(result["manifest"]) as f:
            manifest = json.load(f)
        self.assertEqual(manifest["engine"], "Unity")
        self.assertEqual(manifest["maps"]["BaseColor"], name + "_BaseColor.png")
        self.assertIn("coverage", manifest["channels"]["BaseColor"])
        self.assertIn("Alpha Clipping", manifest["materials"]["URP"])
        self.assertEqual(manifest["scalpCap"], name + "_scalpCap.fbx")
        # The groom's own bake and scene are as they were: nothing re-pointed, nothing left over.
        self.assertEqual(cmds.getAttr(self.groom + ".cardMaps"), maps)
        self.assertIsNone(groomlab.card_mesh(self.groom))
        self.assertEqual(set(cmds.ls()) - before, set())
        # A live card mesh keeps its own material.
        mesh = groomlab.create_card_mesh(self.groom)
        group = cmds.listConnections(cmds.listRelatives(mesh, shapes=True)[0], type="shadingEngine")
        groomlab.export_cards_unity(self.groom, folder, name="live", resolution=32, scalp=False)
        self.assertEqual(cmds.listConnections(cmds.listRelatives(mesh, shapes=True)[0], type="shadingEngine"), group)
        self.assertEqual(groomlab.card_mesh(self.groom), mesh)

    def test_export_cards_as_gltf(self):
        groomlab.create_cards(self.groom, max_cards=12)
        graph = cmds.getAttr(self.groom + ".graph")
        maps = cmds.getAttr(self.groom + ".cardMaps")
        stats = groomlab.card_stats(self.groom)
        folder = tempfile.mkdtemp(prefix="groomlab_gltf_").replace("\\", "/")
        path = folder + "/hair.glb"
        self.assertEqual(groomlab.export_cards_gltf(self.groom, path, resolution=64), [path])
        doc, binary = parse_glb(path)
        self.assertEqual(doc["asset"]["version"], "2.0")
        self.assertEqual(doc["buffers"][0]["byteLength"], len(binary))
        for view in doc["bufferViews"]:
            self.assertEqual(view["byteOffset"] % 4, 0)
            self.assertLessEqual(view["byteOffset"] + view["byteLength"], len(binary))
        name = groomlab.asset_name(self.groom)
        self.assertEqual([m["name"] for m in doc["meshes"]], [name + "_cards", name + "_scalpCap"])
        root = doc["nodes"][doc["scenes"][0]["nodes"][0]]
        self.assertEqual(root["name"], name)
        self.assertEqual(len(root["children"]), 2)
        cards = doc["meshes"][0]["primitives"][0]
        attributes = cards["attributes"]
        self.assertEqual(set(attributes), {"POSITION", "NORMAL", "TANGENT", "TEXCOORD_0", "TEXCOORD_1", "_TINT"})
        count = doc["accessors"][attributes["POSITION"]]["count"]
        for index in attributes.values():
            self.assertEqual(doc["accessors"][index]["count"], count)
        indices = gltf_accessor(doc, binary, cards["indices"])
        self.assertEqual(len(indices), 3 * stats["triangles"])
        self.assertEqual(doc["accessors"][cards["indices"]]["componentType"], 5125 if count > 65535 else 5123)
        self.assertLess(max(indices), count)
        points = gltf_accessor(doc, binary, attributes["POSITION"])
        self.assertTrue(all(math.isfinite(v) for v in points))
        for axis in range(3):  # exact bounds, in meters (the head is 2 units across: 0.02 m)
            values = points[axis::3]
            self.assertEqual(doc["accessors"][attributes["POSITION"]]["min"][axis], min(values))
            self.assertEqual(doc["accessors"][attributes["POSITION"]]["max"][axis], max(values))
            self.assertLess(max(abs(v) for v in values), 0.03)
        material = doc["materials"][cards["material"]]
        self.assertEqual(material["alphaMode"], "MASK")
        self.assertEqual(material["alphaCutoff"], 0.5)
        self.assertTrue(material["doubleSided"])
        self.assertAlmostEqual(material["pbrMetallicRoughness"]["roughnessFactor"], groomlab.hair_look(self.groom)["roughness"])
        for image in doc["images"]:
            view = doc["bufferViews"][image["bufferView"]]
            self.assertEqual(image["mimeType"], "image/png")
            width, height, _ = png_info(binary[view["byteOffset"]:view["byteOffset"] + view["byteLength"]])
            self.assertEqual((width, height), (64, 64))
        base = doc["textures"][material["pbrMetallicRoughness"]["baseColorTexture"]["index"]]
        self.assertEqual(doc["images"][base["source"]]["name"], name + "_cards_BaseColor")
        self.assertEqual(doc["materials"][doc["meshes"][1]["primitives"][0]["material"]]["alphaMode"], "BLEND")
        # Levels of detail from copies of the graph, as a .gltf with its files beside it.
        written = groomlab.export_cards_gltf(self.groom, folder + "/lods.gltf", lods=["mid", "low"], resolution=32,
                                             binary=False, cap=False)
        self.assertEqual(written[0], folder + "/lods.gltf")
        self.assertTrue(all(os.path.exists(p) for p in written))
        with open(written[0]) as f:
            text = json.load(f)
        self.assertEqual([m["name"] for m in text["meshes"]], [name + "_mid", name + "_low"])
        self.assertEqual(os.path.getsize(folder + "/lods.bin"), text["buffers"][0]["byteLength"])
        triangles = [text["accessors"][m["primitives"][0]["indices"]]["count"] // 3 for m in text["meshes"]]
        self.assertGreater(triangles[0], triangles[1])  # mid holds more than low
        # The groom is untouched: same graph, same bake, same cards.
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), graph)
        self.assertEqual(cmds.getAttr(self.groom + ".cardMaps"), maps)
        self.assertEqual(groomlab.card_stats(self.groom)["triangles"], stats["triangles"])

    def test_bake_the_groom_onto_its_scalp(self):
        folder = tempfile.mkdtemp(prefix="groomlab_scalp_")
        paths = groomlab.bake_scalp(self.groom, folder, resolution=128, name="head")
        self.assertEqual(sorted(os.path.basename(p) for p in paths), ["head_ScalpColor.tga", "head_ScalpCoverage.tga"])
        self.assertEqual(os.path.getsize(paths[0]), 18 + 3 * 128 * 128)
        # A hair cap: the scalp just above itself, textured with the maps, made once.
        cap = groomlab.scalp_cap(self.groom)
        self.assertTrue(cap and cmds.objExists(cap))
        self.assertEqual(groomlab.bake_scalp(self.groom, folder, resolution=64, name="head") and groomlab.scalp_cap(self.groom), cap)
        shape = cmds.listRelatives(cap, shapes=True)[0]
        self.assertEqual(cmds.polyEvaluate(shape, face=True), cmds.polyEvaluate(self.scalp, face=True))
        group = cmds.listConnections(shape, type="shadingEngine")[0]
        material = cmds.listConnections(group + ".surfaceShader")[0]
        self.assertTrue(cmds.listConnections(material + ".color"))
        self.assertTrue(cmds.listConnections(material + ".transparency"))
        # It shows with the cards in A/B.
        groomlab.create_cards(self.groom, max_cards=20)
        groomlab.compare_cards(self.groom, False)
        self.assertFalse(cmds.getAttr(cap + ".visibility"))
        groomlab.compare_cards(self.groom, True)
        self.assertTrue(cmds.getAttr(cap + ".visibility"))

    def test_feathers_from_a_scatter_and_from_guides(self):
        # Shafts scattered over the scalp and shaped by the groom's guides, each growing a contour
        # feather, merged with the hair before the output.
        hair = cmds.getAttr(self.groom + ".outStrandCount")
        made = groomlab.add_feathers(self.groom, "contour")
        self.assertEqual(groomlab.errors(self.groom), [])
        graph = groomlab.get_graph(self.groom)
        types = {n["name"]: n["type"] for n in graph["nodes"]}
        self.assertEqual(types[made["shafts"]], "Interpolate")
        self.assertEqual([types[d] for d in made["designs"]], ["Compound"])
        self.assertIsNone(made["split"])
        self.assertEqual(types[made["merge"]], "Merge")
        self.assertIn({"from": made["merge"], "to": "output", "input": "strands"}, graph["connections"])
        self.assertIn({"from": made["designs"][0], "to": made["merge"], "input": "b"}, graph["connections"])
        feathered = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(feathered, hair + 1000)
        # Exported like any hair.
        path = os.path.join(tempfile.mkdtemp(prefix="groomlab_feathers_"), "feathers.abc")
        groomlab.export_alembic(self.groom, path)
        self.assertGreater(os.path.getsize(path), 100000)
        cmds.undo()  # one undo step
        self.assertNotIn(made["designs"][0], [n["name"] for n in groomlab.get_graph(self.groom, True)["nodes"]])
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), hair)
        cmds.redo()

        # Hand-placed feathers: curves drawn from the scalp become the "feathers" guide group, left
        # out of the hair's guides, each a flight feather's shaft: 70 barbs a side and the shaft.
        curves = [cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.3, 0.6), (0.0, 1.2, 1.4)]),
                  cmds.curve(degree=1, point=[(0.3, 0.95, 0.0), (0.5, 1.2, 0.5), (0.6, 1.1, 1.2)])]
        made = groomlab.add_feathers(self.groom, "flight", source="guides", curves=curves)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertIn("feathers", [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertEqual(len(groomlab.guide_ids(self.groom, "feathers")), 2)
        nodes = {n["name"]: n for n in groomlab.get_graph(self.groom)["nodes"]}
        self.assertEqual(nodes[made["shafts"]]["type"], "Guides")
        self.assertEqual(nodes[made["shafts"]]["params"]["group"], "feathers")
        self.assertIn("feathers", nodes["guides"]["params"]["exclude"])  # not hair guides
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), feathered + 2 * (1 + 140))
        self.assertEqual(groomlab.feather_designs(),
                         ["afterfeather", "barred", "contour", "down", "eye_spot", "flight", "iridescent", "spotted"])
        with self.assertRaises(ValueError):
            groomlab.add_feathers(self.groom, "contour", source="nothing")

    def test_one_feather_per_guide_in_guide_groups_of_their_own(self):
        from groomlab.ui import menu
        # Add > Feathers' form: scattered feathers by default, the guide group "feathers" offered
        # first with the groom's groups after it (a new name typed in makes another).
        fields, note = menu.feathers_form(self.groom)
        form = {f[0]: f for f in fields}
        self.assertEqual(form["source"][2:5], ("choice", "scatter", [
            ("scatter", "Scattered feathers (shaped by the guides)"), ("guides", "One feather per guide (hand-placed)")]))
        groups = [g["name"] for g in groomlab.list_guide_groups(self.groom)]
        self.assertEqual(form["group"][2:5], ("edit", "feathers", ["feathers"] + groups))
        for text in [note] + [f[1] + " " + f[-1] for f in fields]:
            self.assertNotIn("shaft", text.lower())  # "scattered feathers", never shafts
        # Curves selected: one feather per guide.
        wing = [cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.3, 0.6), (0.0, 1.2, 1.4)]),
                cmds.curve(degree=1, point=[(0.3, 0.95, 0.0), (0.5, 1.2, 0.5), (0.6, 1.1, 1.2)])]
        fields, note = menu.feathers_form(self.groom, wing)
        self.assertEqual({f[0]: f for f in fields}["source"][3], "guides")
        self.assertIn("2 curve(s) selected", note)

        from groomlab import utils
        interpolate = next(n["name"] for n in groomlab.get_graph(self.groom, True)["nodes"] if n["type"] == "Interpolate")

        def hair_points():
            made = utils.strands_to_curves(self.groom, node=interpolate, max_curves=40, attributes=False)
            points = [cmds.xform(c + ".cv[*]", query=True, worldSpace=True, translation=True) for c in made["curves"]]
            cmds.delete(made["group"])
            return points

        hair = cmds.getAttr(self.groom + ".outStrandCount")
        hair_before = hair_points()
        # The left wing's flight feathers: the selected curves become guide group flight_left, a
        # flight feather each (70 barbs a side and the shaft).
        cmds.select(wing)
        menu.add_feathers_dialog(design="flight", source="guides", group="flight_left", groom=self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(len(groomlab.guide_ids(self.groom, "flight_left")), 2)
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), hair + 2 * (1 + 140))
        self.assertEqual(groomlab.feather_guide_groups(self.groom), ["flight_left"])
        # The form now starts on one feather per guide, in that group.
        form = {f[0]: f for f in menu.feathers_form(self.groom)[0]}
        self.assertEqual((form["source"][3], form["group"][3]), ("guides", "flight_left"))
        # A tail of three in a group of its own: one feather per guide again.
        tail = [cmds.curve(degree=1, point=[(x, -0.2, -0.98), (x, -0.4, -1.5), (x, -0.5, -2.1)]) for x in (-0.2, 0.0, 0.2)]
        groomlab.add_feathers(self.groom, "flight", source="guides", curves=tail, group="tail")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), hair + 5 * (1 + 140))
        self.assertEqual(groomlab.feather_guide_groups(self.groom), ["flight_left", "tail"])
        # Neither group's guides are hair guides: the hair grows as it did.
        excluded = {n["name"]: n for n in groomlab.get_graph(self.groom)["nodes"]}["guides"]["params"]["exclude"].split()
        self.assertTrue({"flight_left", "tail"} <= set(excluded), excluded)
        hair_after = hair_points()
        self.assertEqual(len(hair_after), len(hair_before))
        for after, before in zip(hair_after, hair_before):
            for a, b in zip(after, before):
                self.assertAlmostEqual(a, b, places=4)
        # A new group with no guides yet: its feathers wait for guides drawn into it (the Draw tool
        # starts on it where there is a viewport); nothing else changes.
        cmds.select(clear=True)
        menu.add_feathers_dialog(design="flight", source="guides", group="flight_right", groom=self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertIn("flight_right", [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertEqual(groomlab.guide_ids(self.groom, "flight_right"), [])
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), hair + 5 * (1 + 140))
        self.assertFalse(menu.draw_guides_into(self.groom, "flight_right"))  # batch: no Draw tool
        mirrored = [cmds.curve(degree=1, point=[(-x, y, z) for x, y, z in p])
                    for p in ([(0.0, 1.0, 0.0), (0.0, 1.3, 0.6), (0.0, 1.2, 1.4)], [(0.3, 0.95, 0.0), (0.5, 1.2, 0.5), (0.6, 1.1, 1.2)])]
        groomlab.guides_from_curves(self.groom, mirrored, group="flight_right")
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), hair + 7 * (1 + 140))

    def test_two_feather_designs_share_the_shafts(self):
        # A Split sends three quarters of the shafts to contour feathers and the rest to down; the
        # two designs are merged and replace the hair.
        made = groomlab.add_feathers(self.groom, ["contour", "down"], weights=[3, 1], keep_hair=False)
        self.assertEqual(groomlab.errors(self.groom), [])
        graph = groomlab.get_graph(self.groom)
        nodes = {n["name"]: n for n in graph["nodes"]}
        split = made["split"]
        self.assertEqual(nodes[split]["type"], "Split")
        self.assertEqual((nodes[split]["params"]["weightA"], nodes[split]["params"]["weightB"]), (3.0, 1.0))
        self.assertEqual(nodes[split]["params"]["weightC"], 0.0)
        contour, down = made["designs"]
        self.assertIn({"from": split, "to": contour, "input": "shafts"}, graph["connections"])
        self.assertIn({"from": split, "to": down, "input": "shafts", "output": "b"}, graph["connections"])
        self.assertIsNone(made["merge"])
        merged = next(c["from"] for c in graph["connections"] if c["to"] == "output")
        self.assertEqual(nodes[merged]["type"], "Merge")
        both = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(both, 1000)
        # A third design on the Split's free output: nothing grows there until it has a share.
        third = groomlab.add_feathers(self.groom, "flight", source=split + ".c")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertIn({"from": split, "to": third["designs"][0], "input": "shafts", "output": "c"},
                      groomlab.get_graph(self.groom)["connections"])
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), both)
        groomlab.set_param(self.groom, split, "weightC", 1.0)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertNotEqual(cmds.getAttr(self.groom + ".outStrandCount"), both)
        with self.assertRaises(ValueError):
            groomlab.add_feathers(self.groom, ["contour"] * 5)

    def abc_curves(self, path, namespace):
        """{object: curve count} of an Alembic file's curves objects (AbcImport makes a curve each)."""
        if not cmds.pluginInfo("AbcImport", query=True, loaded=True):
            cmds.loadPlugin("AbcImport", quiet=True)
        cmds.file(path, i=True, type="Alembic", namespace=namespace)
        out = {}
        for t in cmds.ls(namespace + ":*", type="transform", long=True):
            shapes = cmds.listRelatives(t, shapes=True, fullPath=True, type="nurbsCurve") or []
            if shapes:
                out[t.rsplit(":", 1)[-1]] = len(shapes)
        cmds.delete(cmds.ls(namespace + ":*", long=True, type="transform"))
        return out

    def test_feathers_for_unreal_the_meerkat_way(self):
        # Hand-placed flight feathers: a groom group of their own, and a rachis (the preset's width).
        curves = [cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.3, 0.6), (0.0, 1.2, 1.4)]),
                  cmds.curve(degree=1, point=[(0.3, 0.95, 0.0), (0.5, 1.2, 0.5), (0.6, 1.1, 1.2)])]
        groomlab.add_feathers(self.groom, "flight", source="guides", curves=curves)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.feather_groups(self.groom), {1: "flight"})
        width = groomlab.rachis_width(self.groom)
        self.assertGreater(width, 0.0)
        # The rachis: a live mesh of a tube round each shaft (6 sides, a ring per shaft point and
        # the tip closed), its feathers told apart by the featherId UV set.
        rachis = groomlab.create_rachis_mesh(self.groom)
        self.assertEqual(groomlab.create_rachis_mesh(self.groom), rachis)  # reused
        shape = cmds.listRelatives(rachis, shapes=True)[0]
        verts = cmds.polyEvaluate(shape, vertex=True)
        self.assertEqual(cmds.polyEvaluate(shape, face=True), verts - 2)  # two tubes: 6n + 1 points, 6n faces
        self.assertEqual((verts - 2) % 6, 0)
        self.assertIn("featherId", cmds.polyUVSet(shape, query=True, allUVSets=True))
        cmds.polyUVSet(shape, currentUVSet=True, uvSet="featherId")
        feathers = cmds.polyEditUV(shape + ".map[*]", query=True)
        cmds.polyUVSet(shape, currentUVSet=True, uvSet="map1")
        self.assertEqual(len(set(round(u) for u in feathers[0::2])), 2)  # two feathers, each its number
        self.assertEqual(sorted(set(round(v) for v in feathers[1::2])), [1])  # its groom group
        # It follows the shafts (the feathers group's guides): every shaft point has the rachis
        # round it, no farther than its radius.
        points = cmds.xform(shape + ".vtx[*]", query=True, worldSpace=True, translation=True)
        points = [points[i:i + 3] for i in range(0, len(points), 3)]
        for guide in groomlab.guide_ids(self.groom, "feathers"):
            for p in guide_points(self.groom)[str(guide)]:
                near = min(math.sqrt(sum((a - b) ** 2 for a, b in zip(p, q))) for q in points)
                self.assertLess(near, 0.5 * width + 1e-4)
        # A wider rachis follows live.
        groomlab.set_rachis_width(self.groom, 2 * width)
        self.assertAlmostEqual(groomlab.rachis_width(self.groom), 2 * width)
        wider = cmds.exactWorldBoundingBox(shape)
        self.assertEqual(cmds.polyEvaluate(shape, vertex=True), verts)

        # For Unreal: the hair and the flight feathers each their own curves object (group), the
        # shafts left out (the rachis carries them), guides for the hair alone.
        folder = tempfile.mkdtemp(prefix="groomlab_meerkat_")
        path = os.path.join(folder, "bird.abc").replace("\\", "/")
        self.assertEqual(groomlab.export_unreal_groom(self.groom, path, density=0.05), 1)
        name = cmds.listRelatives(self.groom, parent=True)[0]
        objects = self.abc_curves(path, "full")
        self.assertEqual(objects["strands_flight"], 2 * 140)  # barbs only
        self.assertIn("strands_" + name, objects)
        self.assertIn("guides_" + name, objects)
        self.assertNotIn("guides_flight", objects)
        hair = objects["strands_" + name]
        # The shafts as strands again, and as the feathers' guides.
        groomlab.export_unreal_groom(self.groom, path, density=0.05, shafts=True, feather_guides=True)
        objects = self.abc_curves(path, "shafts")
        self.assertEqual(objects["strands_flight"], 2 * 141)
        self.assertEqual(objects["guides_flight"], 2)
        # The lighter groom for heavy shots: a quarter of the barbs, the hair as it was.
        groomlab.export_unreal_groom(self.groom, path, density=0.05, lod="low")
        objects = self.abc_curves(path, "low")
        self.assertLess(objects["strands_flight"], 2 * 140 // 2)
        self.assertGreater(objects["strands_flight"], 2 * 140 // 8)
        self.assertEqual(objects["strands_" + name], hair)
        # One object, as before groups: every strand together.
        groomlab.export_unreal_groom(self.groom, path, density=0.05, groups=None)
        objects = self.abc_curves(path, "one")
        self.assertEqual(sorted(objects), ["guides", "strands"])
        self.assertEqual(objects["strands"], hair + 2 * 140)
        with self.assertRaises(ValueError):
            groomlab.export_unreal_groom(self.groom, path, lod="tiny")
        with self.assertRaises(RuntimeError):
            cmds.groomLabExportAlembic(self.groom, file=path, groups="auto")  # Unreal only

        # The rachis on disk, for skinning with the body: FBX and Alembic.
        fbx = groomlab.export_rachis(self.groom, os.path.join(folder, "rachis.fbx"))
        self.assertGreater(os.path.getsize(fbx), 1000)
        abc = groomlab.export_rachis(self.groom, os.path.join(folder, "rachis.abc"))
        self.assertGreater(os.path.getsize(abc), 1000)
        self.assertTrue(cmds.objExists(rachis))  # the live one stays
        cmds.delete(rachis)
        before = set(cmds.ls())
        groomlab.export_rachis(self.groom, os.path.join(folder, "again.fbx"))  # a mesh of its own, removed after
        self.assertEqual(set(cmds.ls()), before)
        self.assertIsNone(groomlab.rachis_mesh(self.groom))
        self.assertGreater(wider[3] - wider[0], 0.0)

        # No rachis width: no rachis to make.
        groomlab.set_rachis_width(self.groom, 0.0)
        with self.assertRaises(RuntimeError):
            groomlab.create_rachis_mesh(self.groom)

    def unity_curves(self, path):
        """{object: {"counts", "uvs", "uvScope", "rootWidths"}} of the curves objects under an
        Alembic file's root, in file order, read with Maya's own Alembic module (as Unity's
        importer sees them: one AlembicCurves, so one strand group, per object)."""
        from alembic import Abc, AbcGeom
        archive = Abc.IArchive(path)
        root = archive.getTop().getChild(0)
        out = {}
        for i in range(root.getNumChildren()):
            child = root.getChild(i)
            node = child if AbcGeom.ICurves.matches(child.getHeader()) else child.getChild(0)
            schema = AbcGeom.ICurves(node, Abc.WrapExistingFlag.kWrapExisting).getSchema()
            sample = schema.getValue()
            counts = list(sample.getCurvesNumVertices())
            widths = list(schema.getWidthsParam().getExpandedValue().getVals())
            uvs = schema.getUVsParam()
            starts = [sum(counts[:k]) for k in range(len(counts))]
            out[child.getName()] = {
                "counts": counts,
                "uvs": len(uvs.getExpandedValue().getVals()) if uvs.valid() else 0,
                "uvScope": str(uvs.getScope()) if uvs.valid() else None,
                "rootWidths": [widths[s] for s in starts],
            }
        return out

    def test_feathers_for_unity(self):
        # Flight feathers with a rachis, beside the hair: for Unity, a strand group (curves object)
        # per groom group, the barbs without their shafts, the rachis as an FBX in Unity's units.
        curves = [cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.3, 0.6), (0.0, 1.2, 1.4)]),
                  cmds.curve(degree=1, point=[(0.3, 0.95, 0.0), (0.5, 1.2, 0.5), (0.6, 1.1, 1.2)])]
        groomlab.add_feathers(self.groom, "flight", source="guides", curves=curves)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(groomlab.rachis_width(self.groom), 0.0)
        name = cmds.listRelatives(self.groom, parent=True)[0]
        folder = tempfile.mkdtemp(prefix="groomlab_unity_feathers_").replace("\\", "/")
        before = set(cmds.ls())
        result = groomlab.export_unity_feathers(self.groom, folder, density=0.05, name="bird")
        self.assertEqual(set(cmds.ls()), before)  # the rachis mesh made for the export is gone
        self.assertEqual(result["abc"], folder + "/bird_strands.abc")
        self.assertEqual(result["rachis"], folder + "/bird_rachis.fbx")
        self.assertGreater(os.path.getsize(result["rachis"]), 1000)
        self.assertEqual(fbx_unit_scale(result["rachis"]), 100.0)  # meters, as the Unity cards
        objects = self.unity_curves(result["abc"])
        self.assertEqual(list(objects), [name, "flight"])  # Unity's strand groups, in this order
        flight, hair = objects["flight"], objects[name]
        self.assertEqual(len(flight["counts"]), 2 * 140)  # barbs only: the rachis carries the shafts
        self.assertEqual(set(flight["counts"]), {6})       # barb_points
        self.assertEqual(set(hair["counts"]), {16})        # points
        for o in (flight, hair):
            self.assertEqual(o["uvs"], sum(o["counts"]))   # a root UV per point
            self.assertIn("Vertex", o["uvScope"])
            self.assertGreater(min(o["rootWidths"]), 0.0)  # Unity keeps the widths
        with open(result["manifest"]) as f:
            manifest = json.load(f)
        self.assertEqual(manifest["strands"], "bird_strands.abc")
        self.assertEqual(manifest["rachis"], "bird_rachis.fbx")
        self.assertEqual(manifest["hairAsset"]["Alembic Asset Groups"], "Preserve")
        self.assertFalse(manifest["hairAsset"]["Resample Curves"])
        self.assertEqual(manifest["hairAsset"]["Root UV"], "Resolve From Curves")
        self.assertIn("com.unity.demoteam.digital-human", manifest["packages"])
        self.assertEqual([(g["index"], g["group"], g["feathers"], g["strands"], g["pointsPerStrand"])
                          for g in manifest["groups"]],
                         [(0, name, False, len(hair["counts"]), 16), (1, "flight", True, 2 * 140, 6)])
        self.assertEqual(manifest["groups"][1]["object"], "/%s/flight/flightShape" % name)
        self.assertIn("bird_rachis.fbx", manifest["groups"][1]["hairInstance"]["Skinning"]["Roots Attach Target"])
        self.assertTrue(manifest["groups"][1]["hairInstance"]["Physics"]["Global Position"])
        self.assertEqual(result["groups"], manifest["groups"])

        # The lighter groom to swap in: a quarter of the barbs, the hair as it was.
        low = groomlab.export_unity_feathers(self.groom, folder, density=0.05, lod="low", name="low", rachis=False)
        self.assertIsNone(low["rachis"])
        objects = self.unity_curves(low["abc"])
        self.assertLess(len(objects["flight"]["counts"]), 2 * 140 // 2)
        self.assertGreater(len(objects["flight"]["counts"]), 2 * 140 // 8)
        self.assertEqual(objects[name]["counts"], hair["counts"])
        # Past Unity's strand limit (lowered here), a group goes in even parts; the shafts back as strands.
        path = folder + "/parts.abc"
        groomlab.export_unity_strands(self.groom, path, density=0.05, max_strands=150, barb_points=5, shafts=True)
        objects = self.unity_curves(path)
        parts = [o for o in objects if o.startswith("flight")]
        self.assertEqual(parts, ["flight_0", "flight_1"])
        self.assertEqual([len(objects[p]["counts"]) for p in parts], [141, 141])  # 282: barbs and shafts
        self.assertEqual(set(objects["flight_0"]["counts"]), {5})
        # One object, the old way.
        groomlab.export_unity_strands(self.groom, path, density=0.05, groups=None)
        self.assertEqual(list(self.unity_curves(path)), [name + "Shape"])
        with self.assertRaises(ValueError):
            groomlab.export_unity_strands(self.groom, path, groups=None, barb_points=5)
        with self.assertRaises(ValueError):
            groomlab.export_unity_feathers(self.groom, folder, barb_points=2)  # Unity keeps 3 to 128
        with self.assertRaises(ValueError):
            groomlab.export_rachis(self.groom, folder + "/rachis.abc", unity=True)
        with self.assertRaises(RuntimeError):
            cmds.groomLabExportAlembic(self.groom, file=path, barbPoints=4)  # Unity only
        with self.assertRaises(RuntimeError):
            cmds.groomLabExportAlembic(self.groom, file=path, unity=True, unreal=True)

    def test_feather_cards_beside_the_hair_cards(self):
        # Feathers merged with the hair, then a Cards node before the output: it cards both, a card
        # per feather beside the hair's. Feather cards of their own: a Cards node on the feather
        # branch, the hair's node set to card the hair only; one atlas and bake, stats per node.
        made = groomlab.add_feathers(self.groom, "contour")
        self.assertIsNone(made["cards"])
        with self.assertRaises(ValueError):
            groomlab.create_feather_cards(self.groom, feathers="output")  # nothing there to card
        hair = groomlab.create_cards(self.groom)
        self.assertEqual(groomlab.card_node_kind(self.groom, hair), "both")
        stats = groomlab.card_stats(self.groom)
        self.assertGreater(stats["feathers"], 5)
        both = stats["nodes"][hair]["feathers"]

        feathers = groomlab.create_feather_cards(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        graph = groomlab.get_graph(self.groom)
        nodes = {n["name"]: n for n in graph["nodes"]}
        self.assertIn({"from": made["designs"][0], "to": feathers, "input": "strands"}, graph["connections"])
        self.assertIn({"from": feathers, "to": made["merge"], "input": "b"}, graph["connections"])
        self.assertEqual(nodes[feathers]["params"]["featherMode"], "perFeather")
        self.assertEqual(nodes[hair]["params"]["featherMode"], "off")  # it cards the hair only now
        self.assertEqual(groomlab.card_node_kind(self.groom, feathers), "feathers")
        self.assertEqual(groomlab.card_node_kind(self.groom, hair), "hair")
        self.assertEqual(groomlab.create_feather_cards(self.groom), feathers)  # already there: reused
        stats = groomlab.card_stats(self.groom)
        mine, theirs = stats["nodes"][feathers], stats["nodes"][hair]
        self.assertEqual(mine["feathers"], both)
        self.assertEqual(mine["cards"], mine["feathers"])  # contour feathers: one card each
        self.assertLess(mine["textures"], mine["cards"])   # alike feathers share textures
        self.assertEqual(theirs["feathers"], 0)
        self.assertGreater(theirs["cards"], 0)
        self.assertEqual(stats["cards"], mine["cards"] + theirs["cards"])
        self.assertEqual(stats["triangles"], mine["triangles"] + theirs["triangles"])
        self.assertEqual(groomlab.card_stats(self.groom, node=feathers)["feathers"], mine["feathers"])

        # One bake for both: the atlas records the feather cards, stale per node.
        folder = tempfile.mkdtemp(prefix="groomlab_feather_cards_")
        groomlab.bake_cards(self.groom, folder, resolution=128, name="feathered")
        with open(os.path.join(folder, "feathered_Atlas.json")) as f:
            entries = json.load(f)["cards"]
        self.assertEqual(sum(1 for e in entries if e["feather"]), mine["cards"])
        self.assertEqual({e["node"] for e in entries if e["feather"]}, {feathers})
        self.assertEqual(groomlab.card_stats(self.groom)["stale"], False)
        groomlab.set_param(self.groom, feathers, "featherSegments", 3)
        stats = groomlab.card_stats(self.groom)
        self.assertEqual((stats["nodes"][feathers]["stale"], stats["nodes"][hair]["stale"]), (True, False))
        # The spine (on by default): two quads a segment; off, the flat strip's one.
        self.assertEqual(stats["nodes"][feathers]["triangles"], 4 * 3 * stats["nodes"][feathers]["cards"])
        self.assertEqual(stats["nodes"][feathers]["triangles"], groomlab.feather_card_triangles(3) * mine["cards"])
        groomlab.set_param(self.groom, feathers, "featherSpine", False)
        flat = groomlab.card_stats(self.groom)["nodes"][feathers]
        self.assertEqual(flat["triangles"], 2 * 3 * flat["cards"])
        self.assertEqual(flat["triangles"], groomlab.feather_card_triangles(3, spine=False) * mine["cards"])
        groomlab.set_param(self.groom, feathers, "featherSpine", True)

        # LODs: the feather node takes the feather presets, the hair node the hair's.
        levels = {}
        for lod in ("hero", "mid", "low"):
            groomlab.cards_preset(self.groom, lod)
            nodes = {n["name"]: n for n in groomlab.get_graph(self.groom)["nodes"]}
            preset = groomlab.FEATHER_CARD_PRESETS[lod]
            self.assertEqual(nodes[feathers]["params"]["featherSegments"], preset["featherSegments"])
            self.assertIs(nodes[feathers]["params"]["featherSpine"], True)
            self.assertEqual(nodes[hair]["params"]["cardsPerClump"], groomlab.CARD_PRESETS[lod]["cardsPerClump"])
            # The core's table, the standalone app's Cards panel's too: every param it sets.
            for key, value in groomlab_core.card_preset_params(lod, "hair").items():
                self.assertEqual(nodes[hair]["params"][key], value, (lod, key))
            self.assertEqual(nodes[hair]["params"]["featherSegments"], 6)  # the spec's: no feather preset
            self.assertIs(nodes[hair]["params"]["featherSpine"], True)
            # maxCards from the budget at 4 triangles a segment (the spine).
            self.assertEqual(nodes[feathers]["params"]["maxCards"],
                             preset["triangles"] // (4 * preset["featherSegments"]))
            levels[lod] = groomlab.card_stats(self.groom, node=feathers)
            self.assertLessEqual(levels[lod]["triangles"], preset["triangles"])
            self.assertEqual(levels[lod]["triangles"], 4 * preset["featherSegments"] * levels[lod]["cards"])
            self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(levels["hero"]["triangles"], levels["mid"]["triangles"])
        self.assertGreater(levels["mid"]["triangles"], levels["low"]["triangles"])
        self.assertEqual(levels["low"]["triangles"], 4 * levels["low"]["cards"])  # a V (two quads) per feather
        self.assertLessEqual(levels["low"]["feathers"], levels["hero"]["feathers"])

        # Exports: the feather cards go with the hair's, alpha-tested and double-sided.
        stats = groomlab.card_stats(self.groom)
        unreal = groomlab.export_cards_unreal(self.groom, folder, name="unreal", resolution=64, scalp=False)
        self.assertGreater(os.path.getsize(unreal["fbx"]), 1000)
        with open(unreal["manifest"]) as f:
            manifest = json.load(f)
        self.assertEqual((manifest["triangles"], manifest["feathers"]), (stats["triangles"], stats["feathers"]))
        self.assertIn("barb", manifest["channels"]["Attribute"])
        self.assertIn("spine", manifest["featherCards"])
        # The FBX's mesh (the card mesh on outCards) holds the spine's quads too.
        mesh = groomlab.create_card_mesh(self.groom)
        self.assertEqual(cmds.polyEvaluate(mesh, triangle=True), stats["triangles"])
        self.assertEqual(cmds.polyEvaluate(mesh, face=True), stats["triangles"] // 2)
        cmds.delete(mesh)
        unity = groomlab.export_cards_unity(self.groom, folder, name="unity", resolution=64, scalp=False)
        with open(unity["manifest"]) as f:
            manifest = json.load(f)
        self.assertEqual((manifest["triangles"], manifest["feathers"]), (stats["triangles"], stats["feathers"]))
        self.assertTrue(manifest["materials"]["HDRP"]["Double-Sided"])
        # Alpha-tested and seen from both sides in either pipeline; the barbs in the maps' notes.
        self.assertEqual((manifest["materials"]["HDRP"]["Alpha Clipping"], manifest["materials"]["HDRP"]["Alpha Clip Threshold"]),
                         (True, 0.5))
        self.assertEqual((manifest["materials"]["URP"]["Alpha Clipping"], manifest["materials"]["URP"]["Render Face"]),
                         (True, "Both"))
        self.assertIn("barb", manifest["channels"]["Attribute"])
        self.assertIn("spine", manifest["featherCards"])
        self.assertEqual(fbx_unit_scale(unity["fbx"]), 100.0)
        path = folder + "/feathered.glb"
        groomlab.export_cards_gltf(self.groom, path, resolution=64, cap=False)
        doc, binary = parse_glb(path)
        cards = doc["meshes"][0]["primitives"][0]
        self.assertEqual(len(gltf_accessor(doc, binary, cards["indices"])), 3 * stats["triangles"])
        material = doc["materials"][cards["material"]]
        self.assertEqual(material["alphaMode"], "MASK")
        self.assertTrue(material["doubleSided"])
        # The spine goes out with the cards: the feather cards' triangles are 4 a segment (the low
        # preset: a V each), 2 without it; the spine's TEXCOORD_1 x is 0.5.
        spined = stats["nodes"][feathers]
        self.assertEqual(spined["triangles"], 4 * spined["cards"])
        across = gltf_accessor(doc, binary, cards["attributes"]["TEXCOORD_1"])[0::2]
        self.assertGreaterEqual(sum(1 for x in across if abs(x - 0.5) < 1e-6), 2 * spined["cards"])  # 2 stations a V
        groomlab.set_param(self.groom, feathers, "featherSpine", False)
        groomlab.export_cards_gltf(self.groom, folder + "/flat.glb", resolution=64, cap=False)
        doc, binary = parse_glb(folder + "/flat.glb")
        flat = len(gltf_accessor(doc, binary, doc["meshes"][0]["primitives"][0]["indices"])) // 3
        self.assertEqual(flat, stats["triangles"] - 2 * spined["cards"])
        groomlab.set_param(self.groom, feathers, "featherSpine", True)
        written = groomlab.export_cards_gltf(self.groom, folder + "/lods.glb", lods=["hero", "low"], resolution=32,
                                             cap=False)
        doc, binary = parse_glb(written[0])
        triangles = [doc["accessors"][m["primitives"][0]["indices"]]["count"] // 3 for m in doc["meshes"]]
        self.assertGreater(triangles[0], triangles[1])
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_down_feathers_get_crossed_cards(self):
        # Contour and down feathers replacing the hair, carded as they are made: the down ones'
        # cards cross, two a feather; the low preset leaves the smallest out.
        made = groomlab.add_feathers(self.groom, ["contour", "down"], weights=[1, 1], keep_hair=False, cards=True,
                                     cards_lod="hero")
        self.assertEqual(groomlab.errors(self.groom), [])
        node = made["cards"]
        self.assertEqual(groomlab.card_node_kind(self.groom, node), "feathers")
        self.assertIsNone(made["merge"])
        self.assertIn({"from": node, "to": "output", "input": "strands"}, groomlab.get_graph(self.groom)["connections"])
        hero = groomlab.card_stats(self.groom, node=node)
        self.assertGreater(hero["feathers"], 5)
        self.assertGreater(hero["cards"], hero["feathers"])  # down feathers: a cross each
        groomlab.cards_preset(self.groom, "low")
        low = groomlab.card_stats(self.groom, node=node)
        self.assertLess(low["triangles"], hero["triangles"])
        self.assertLessEqual(low["triangles"], groomlab.FEATHER_CARD_PRESETS["low"]["triangles"])
        folder = tempfile.mkdtemp(prefix="groomlab_down_cards_")
        paths = groomlab.bake_cards(self.groom, folder, resolution=64, name="down")
        self.assertTrue(any(p.endswith("_Coverage.tga") for p in paths))
        self.assertFalse(groomlab.card_stats(self.groom)["stale"])

    def test_braid_from_curves(self):
        before = cmds.exactWorldBoundingBox(self.groom)
        path = cmds.curve(degree=2, point=[(0.0, 1.0, 0.0), (0.0, 1.5, 0.3), (0.0, 1.6, 1.2)])
        node = groomlab.add_braid(self.groom, [path], style="fishtail", region=0.6, width=0.2)
        graph = groomlab.get_graph(self.groom)
        types = {n["name"]: n["type"] for n in graph["nodes"]}
        self.assertEqual(types[node], "Braid")
        paths = [n for n in graph["nodes"] if n["type"] == "Guides" and n.get("params", {}).get("group") == "braids"]
        self.assertEqual(len(paths), 1)
        self.assertIn("braids", [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)

    def test_maps_of_three_kinds_convert_into_each_other(self):
        # A vertex map: kept on the groom (scalpMaps), read by a Map node feeding a param through
        # its exposed input; the param's own value multiplies it.
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        self.assertEqual(groomlab.add_map(self.groom, "fringe", 0.0, kind="vertices"), "fringe")
        self.assertEqual(groomlab.map_kind(self.groom, "fringe"), "vertices")
        self.assertEqual([m["kind"] for m in groomlab.maps(self.groom)], ["vertices"])
        count = cmds.polyEvaluate(self.scalp, vertex=True)
        self.assertEqual(len(cmds.groomLabScalpMaps(self.groom, values="fringe")), count)
        mask = groomlab.use_map(self.groom, "length", "keep", "fringe")
        graph = groomlab.get_graph(self.groom)
        nodes = {n["name"]: n for n in graph["nodes"]}
        self.assertEqual(nodes[mask]["type"], "Map")
        self.assertEqual(nodes[mask]["params"], {"name": "fringe", "min": 0.0, "max": 1.0})  # the name alone: the groom says where it lives
        self.assertIn("keep", nodes["length"].get("paramInputs", []))
        self.assertIn({"from": mask, "to": "length", "input": "keep"}, graph["connections"])
        self.assertEqual(groomlab.errors(self.groom), [])
        black = self.strand_extent()  # the map is black: length 0 everywhere
        groomlab.fill_map(self.groom, "fringe", 1.0)
        self.assertEqual(groomlab.errors(self.groom), [])
        white = self.strand_extent()
        self.assertGreater(white[4] - white[1], black[4] - black[1] + 0.5)  # full length again
        groomlab.set_param(self.groom, mask, "min", 1.0)  # inverted: min above max
        groomlab.set_param(self.groom, mask, "max", 0.0)
        self.assertEqual(groomlab.errors(self.groom), [])
        groomlab.set_param(self.groom, mask, "min", 0.0)
        groomlab.set_param(self.groom, mask, "max", 1.0)
        groomlab.set_param(self.groom, mask, "name", "nowhere")
        self.assertTrue(any("nowhere" in e for e in groomlab.errors(self.groom)))
        groomlab.set_param(self.groom, mask, "name", "fringe")
        # A stroke on the scalp paints the vertices under it (one undo step); the groom follows.
        groomlab.fill_map(self.groom, "fringe", 0.0)
        self.paint_top("fringe", 1.0)
        values = cmds.groomLabScalpMaps(self.groom, values="fringe")
        heights = [cmds.pointPosition("%s.vtx[%d]" % (self.scalp, i), world=True)[1] for i in range(count)]
        apex = [values[i] for i in range(count) if heights[i] > 0.99]
        top = [values[i] for i in range(count) if heights[i] > 0.9]
        bottom = [values[i] for i in range(count) if heights[i] < -0.5]
        self.assertGreater(min(apex), 0.9)  # under the brush center: the value
        self.assertGreater(min(top), 0.4)   # by the brush's falloff around it
        self.assertEqual(max(bottom), 0.0)
        painted = self.strand_extent()
        self.assertGreater(painted[4], black[4] + 0.2)
        cmds.undo()
        self.assertEqual(max(cmds.groomLabScalpMaps(self.groom, values="fringe")), 0.0)
        cmds.redo()
        # Smoothing (Shift+drag in the tool; the smooth brush by script) blends the map with its
        # neighbors: the painted top gets softer, nothing beyond the brush moves.
        before = cmds.groomLabScalpMaps(self.groom, values="fringe")
        groomlab.brush_stroke(self.groom, "smooth", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="fringe",
                              paint_kind="vertices")
        after = cmds.groomLabScalpMaps(self.groom, values="fringe")
        self.assertLess(max(after), max(before))
        self.assertGreater(sum(after), sum(before) * 0.9)
        self.assertEqual([after[i] for i in range(count) if heights[i] < -0.7], [before[i] for i in range(count) if heights[i] < -0.7])
        cmds.undo()
        self.assertEqual(cmds.groomLabScalpMaps(self.groom, values="fringe"), before)
        # A stronger smooth (several passes) blurs more; a hard brush paints full value to its edge.
        groomlab.brush_stroke(self.groom, "smooth", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="fringe",
                              paint_kind="vertices", smooth_strength=3.0)
        stronger = cmds.groomLabScalpMaps(self.groom, values="fringe")
        self.assertLess(max(stronger), max(after))
        cmds.undo()
        groomlab.fill_map(self.groom, "fringe", 0.0)
        groomlab.brush_stroke(self.groom, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="fringe",
                              value=1.0, paint_kind="vertices", hardness=1.0)
        hard = cmds.groomLabScalpMaps(self.groom, values="fringe")
        self.assertGreater(min(hard[i] for i in range(count) if heights[i] > 0.9), 0.99)  # full to the edge
        self.assertEqual(max(hard[i] for i in range(count) if heights[i] < -0.7), 0.0)
        cmds.undo()
        cmds.undo()
        # Feeding Scatter's density: the map stays 0-1 and Scatter's own density multiplies it.
        groomlab.add_map(self.groom, "where", 0.0, kind="vertices")
        groomlab.use_map(self.groom, "scatter", "density", "where")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), 0)  # black: nothing grows
        groomlab.fill_map(self.groom, "where", 1.0)
        white_count = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(white_count, 100)
        groomlab.set_param(self.groom, "scatter", "density", 50.0)
        self.assertLess(cmds.getAttr(self.groom + ".outStrandCount"), white_count)
        groomlab.set_param(self.groom, "scatter", "density", 100.0)
        # Converted to a texture: the same values, now a PNG at the scalp's UVs in the scene's
        # masks folder (untitled: the project's groomlab/untitled), in the groom's own folder,
        # the Map node reading it; strokes rewrite the file.
        folder = tempfile.mkdtemp(prefix="groomlab_maps_")
        cmds.workspace(folder, openWorkspace=True)
        self.assertEqual(groomlab.convert_map(self.groom, "fringe", "texture", resolution=64), "texture")
        self.assertEqual(groomlab.map_kind(self.groom, "fringe"), "texture")
        path = groomlab.map_texture(self.groom, "fringe")
        self.assertTrue(path.endswith("groomlab/untitled/masks/groomLab1/fringe.png"), path)
        self.assertTrue(os.path.exists(path))
        self.assertEqual(next(m["texture"] for m in groomlab.maps(self.groom) if m["name"] == "fringe"),
                         "groomlab/untitled/masks/groomLab1/fringe.png")
        self.assertEqual(groomlab.errors(self.groom), [])
        as_texture = self.strand_extent()
        self.assertAlmostEqual(as_texture[4], painted[4], delta=0.15)  # long on top, as painted
        before = os.path.getmtime(path)
        self.paint_top("fringe", 0.0, kind="texture")  # the top black again, in the file
        self.assertGreaterEqual(os.path.getmtime(path), before)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertLess(self.strand_extent()[4], as_texture[4] - 0.2)
        cmds.undo()  # the file as it was
        self.assertAlmostEqual(self.strand_extent()[4], as_texture[4], delta=0.05)
        # Renamed, the file follows (and comes back on undo); the Map node reads it under its new name.
        groomlab.rename_map(self.groom, "fringe", "bangs")
        moved = groomlab.map_texture(self.groom, "bangs")
        self.assertTrue(moved.endswith("groomlab/untitled/masks/groomLab1/bangs.png"), moved)
        self.assertTrue(os.path.exists(moved))
        self.assertFalse(os.path.exists(path))
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertAlmostEqual(self.strand_extent()[4], as_texture[4], delta=0.05)
        cmds.undo()
        self.assertTrue(os.path.exists(path))
        self.assertFalse(os.path.exists(moved))
        self.assertEqual(groomlab.map_texture(self.groom, "fringe"), path)
        cmds.redo()
        self.assertTrue(os.path.exists(moved))
        cmds.undo()
        # And onto the guides: each guide takes the value at its root; the Map node reads it there.
        self.assertEqual(groomlab.convert_map(self.groom, "fringe", "guides"), "guides")
        self.assertIn("fringe", cmds.groomLabGuides(self.groom, listAttributes=True))
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.map_kind(self.groom, "fringe"), "guides")
        # Back to vertices, and renamed: the Map node follows.
        groomlab.convert_map(self.groom, "fringe", "vertices")
        groomlab.rename_map(self.groom, "fringe", "crown")
        nodes = {n["name"]: n for n in groomlab.get_graph(self.groom)["nodes"]}
        self.assertEqual(nodes[mask]["params"]["name"], "crown")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(sorted(groomlab.list_maps(self.groom)), ["crown", "where"])
        groomlab.remove_map(self.groom, "where")  # what read it lets go of it: no error
        self.assertFalse(any("where" in e for e in groomlab.errors(self.groom)))
        # The map shown on the scalp: a display attribute; a fresh vertex map shows too.
        groomlab.show_map_on_scalp(self.groom, "crown")
        self.assertEqual(cmds.getAttr(self.groom + ".scalpMapDisplay"), "crown")
        groomlab.show_map_on_scalp(self.groom, None)

    def test_color_maps_paint_fill_convert_and_pack_channels(self):
        # A color vertex map: r g b a per vertex, filled with a color; the stored entry says so.
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        count = cmds.polyEvaluate(self.scalp, vertex=True)
        heights = [cmds.pointPosition("%s.vtx[%d]" % (self.scalp, i), world=True)[1] for i in range(count)]
        groomlab.add_map(self.groom, "packed", kind="vertices", color=(0.2, 0.4, 0.6))
        self.assertEqual(groomlab.map_channels(self.groom, "packed"), 4)
        self.assertEqual(groomlab.map_kind(self.groom, "packed"), "vertices")
        self.assertEqual([m["channels"] for m in groomlab.maps(self.groom)], [4])
        stored = json.loads(cmds.getAttr(self.groom + ".scalpMaps"))["packed"]
        self.assertEqual(stored["channels"], 4)
        values = cmds.groomLabScalpMaps(self.groom, values="packed")
        self.assertEqual(len(values), 4 * count)
        self.assertEqual([round(v, 3) for v in values[:4]], [0.2, 0.4, 0.6, 1.0])
        self.assertEqual(groomlab.map_channels(self.groom, "nowhere"), 0)
        # Guide layers are gray: a color one is refused.
        with self.assertRaises(ValueError):
            groomlab.add_map(self.groom, "tint", kind="guides", color=True)
        with self.assertRaises(RuntimeError):
            cmds.groomLabScalpMaps(self.groom, make="tint", kind="guides", color=True)
        # Fill: a color in some channels (one undo step), or a value in r g b (alpha kept).
        groomlab.fill_map(self.groom, "packed", color=(0.0, 1.0, 0.0, 0.5), channels="ga")
        self.assertEqual([round(v, 3) for v in cmds.groomLabScalpMaps(self.groom, values="packed")[:4]], [0.2, 1.0, 0.6, 0.5])
        cmds.undo()
        self.assertEqual([round(v, 3) for v in cmds.groomLabScalpMaps(self.groom, values="packed")[:4]], [0.2, 0.4, 0.6, 1.0])
        with self.assertRaises(RuntimeError):  # -fillColor needs a color map
            groomlab.add_map(self.groom, "plain", kind="vertices")
            groomlab.fill_map(self.groom, "plain", color=(1, 0, 0))
        groomlab.remove_map(self.groom, "plain")
        # Masks packed in channels: a stroke paints red into r only; g, b and a keep their values.
        groomlab.fill_map(self.groom, "packed", color=(0.0, 0.4, 0.6, 1.0))
        groomlab.brush_stroke(self.groom, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="packed",
                              paint_kind="vertices", paint_color=(1.0, 0.0, 0.0, 0.0), paint_channels="r")
        painted = cmds.groomLabScalpMaps(self.groom, values="packed")
        apex = [i for i in range(count) if heights[i] > 0.99]
        bottom = [i for i in range(count) if heights[i] < -0.5]
        self.assertGreater(min(painted[i * 4] for i in apex), 0.9)
        self.assertEqual(max(painted[i * 4] for i in bottom), 0.0)
        self.assertEqual({round(painted[i * 4 + 1], 3) for i in range(count)}, {0.4})
        self.assertEqual({round(painted[i * 4 + 3], 3) for i in range(count)}, {1.0})
        cmds.undo()
        self.assertEqual(max(cmds.groomLabScalpMaps(self.groom, values="packed")[0::4]), 0.0)
        cmds.redo()
        # Smoothing the r channel only.
        groomlab.brush_stroke(self.groom, "smooth", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="packed",
                              paint_kind="vertices", paint_channels="r")
        smoothed = cmds.groomLabScalpMaps(self.groom, values="packed")
        self.assertLess(max(smoothed[0::4]), max(painted[0::4]))
        self.assertEqual(smoothed[2::4], painted[2::4])
        cmds.undo()
        # A Map node's outputs are the channels: r (painted on top) makes long hair on top only; g
        # (0.4 everywhere) a uniform length; luminance mixes them.
        def link_into(node, param):
            return next(c for c in groomlab.get_graph(self.groom)["connections"]
                        if c["to"] == node and c["input"] == param)
        node = groomlab.use_map(self.groom, "length", "keep", "packed", channel="r")
        params = next(n["params"] for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == node)
        self.assertEqual(params, {"name": "packed", "min": 0.0, "max": 1.0})  # every param written
        self.assertEqual(link_into("length", "keep"), {"from": node, "to": "length", "input": "keep", "output": "r"})
        self.assertEqual(groomlab.errors(self.groom), [])
        on_top = self.strand_extent()
        self.assertEqual(groomlab.use_map(self.groom, "length", "keep", "packed", channel="g"), node)  # relinked
        self.assertEqual(link_into("length", "keep")["output"], "g")
        self.assertEqual(groomlab.errors(self.groom), [])
        flat = self.strand_extent()
        self.assertGreater(on_top[4], flat[4] + 0.2)
        cmds.undo()  # back to r, one undo step
        self.assertEqual(link_into("length", "keep")["output"], "r")
        # The same Map node feeds another param from another channel: no second node.
        other = groomlab.use_map(self.groom, "scatter", "density", "packed", channel="a")
        self.assertEqual(other, node)
        self.assertEqual(link_into("scatter", "density")["output"], "a")
        self.assertEqual([n["name"] for n in groomlab.get_graph(self.groom)["nodes"] if n["type"] == "Map"], [node])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 100)  # a = 1: every strand
        # Luminance, the first output, is stored as no output at all.
        groomlab.use_map(self.groom, "scatter", "density", "packed", channel="luminance")
        self.assertNotIn("output", link_into("scatter", "density"))
        with self.assertRaises(ValueError):
            groomlab.use_map(self.groom, "scatter", "density", "packed", channel="q")
        # Gray <-> color: the luminance, and back as r = g = b (undoable).
        before = cmds.groomLabScalpMaps(self.groom, values="packed")
        self.assertEqual(groomlab.set_map_channels(self.groom, "packed", 1), 1)
        gray = cmds.groomLabScalpMaps(self.groom, values="packed")
        self.assertEqual(len(gray), count)
        self.assertAlmostEqual(gray[bottom[0]], 0.7152 * 0.4 + 0.0722 * 0.6, places=2)
        self.assertEqual(groomlab.map_channels(self.groom, "packed"), 1)
        cmds.undo()
        self.assertEqual(cmds.groomLabScalpMaps(self.groom, values="packed"), before)
        folder = tempfile.mkdtemp(prefix="groomlab_colormaps_")
        # Exported as a PNG for an engine (packed masks): RGBA, the map itself left as it was.
        exported = groomlab.export_map(self.groom, "packed", os.path.join(folder, "engine", "packed.png"), resolution=32)
        with open(exported, "rb") as f:
            self.assertEqual(f.read(26)[25], 6)
        self.assertEqual(groomlab.map_kind(self.groom, "packed"), "vertices")
        self.assertEqual(cmds.groomLabScalpMaps(self.groom, values="packed"), before)
        # To a texture: an RGBA PNG with the colors kept, the Map node reading its r.
        cmds.workspace(folder, openWorkspace=True)
        self.assertEqual(groomlab.convert_map(self.groom, "packed", "texture", resolution=64), "texture")
        self.assertEqual(groomlab.map_channels(self.groom, "packed"), 4)
        path = groomlab.map_texture(self.groom, "packed")
        with open(path, "rb") as f:
            self.assertEqual(f.read(26)[25], 6)  # PNG color type 6: RGBA
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertAlmostEqual(self.strand_extent()[4], on_top[4], delta=0.15)
        # A texture stroke in the b channel only; undo gives the file back byte for byte.
        original = md5(path)
        self.assertEqual(md5(groomlab.export_map(self.groom, "packed", os.path.join(folder, "engine", "copy.png"))), original)
        groomlab.brush_stroke(self.groom, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="packed",
                              paint_kind="texture", paint_texture=path, paint_color=(0.0, 0.0, 1.0, 1.0),
                              paint_channels="b")
        self.assertNotEqual(md5(path), original)
        cmds.undo()
        self.assertEqual(md5(path), original)
        # Fill with a color, undone with the file's bytes.
        groomlab.fill_map(self.groom, "packed", color=(1.0, 1.0, 1.0, 1.0), channels="r")
        self.assertNotEqual(md5(path), original)
        self.assertGreater(self.strand_extent()[4] - self.strand_extent()[1], 0.5)
        cmds.undo()
        self.assertEqual(md5(path), original)
        # Texture gray <-> color (the file rewritten, the bytes back on undo).
        groomlab.set_map_channels(self.groom, "packed", 1)
        with open(path, "rb") as f:
            self.assertEqual(f.read(26)[25], 0)  # gray
        cmds.undo()
        self.assertEqual(md5(path), original)
        self.assertEqual(groomlab.map_channels(self.groom, "packed"), 4)
        # Back to vertices (colors kept), then onto the guides: the luminance, gray.
        groomlab.convert_map(self.groom, "packed", "vertices")
        back = cmds.groomLabScalpMaps(self.groom, values="packed")
        self.assertEqual(len(back), 4 * count)
        self.assertGreater(max(back[i * 4] for i in apex), 0.8)
        self.assertAlmostEqual(back[bottom[0] * 4 + 1], 0.4, places=1)
        groomlab.convert_map(self.groom, "packed", "guides")
        self.assertEqual(groomlab.map_kind(self.groom, "packed"), "guides")
        self.assertEqual(groomlab.map_channels(self.groom, "packed"), 1)
        with self.assertRaises(RuntimeError):
            groomlab.set_map_channels(self.groom, "packed", 4)
        # The brush takes the paint color and channels (the tool's queries are checked in the GUI
        # suites: a context answers queries only in a Maya window); bad channels are refused.
        groomlab.brush_tool("paint", attribute="packed", paint_color=(0.25, 0.5, 0.75), paint_channels="gb",
                            activate=False)
        with self.assertRaises(RuntimeError):
            cmds.groomLabBrushStroke(self.groom, mode="paint", attribute="packed", paintChannels="xyz",
                                     point=(0.0, 1.0, 0.0))
        with self.assertRaises(ValueError):
            groomlab.brush_stroke(self.groom, "paint", [(0.0, 1.0, 0.0)], attribute="packed",
                                  paint_color=(1.0, 0.0))
        # The scalp shows a color map in color, or one channel.
        groomlab.show_map_on_scalp(self.groom, "packed", channel="g")
        self.assertEqual(cmds.getAttr(self.groom + ".scalpMapChannel"), "g")
        with self.assertRaises(ValueError):
            groomlab.show_map_on_scalp(self.groom, "packed", channel="luminance")

    def test_a_picked_color_is_what_the_groom_shows_and_the_ae_edits_it(self):
        # HairColor's mode color: the RGB picked is the hair's color (no pigment browns it), the
        # look colors by RGB (what the viewport, create_hair_shader and Arnold's default shader
        # read), and the Attribute Editor's Hair Color section shows and edits the node.
        from groomlab.ui import ae
        shown = ae.color_rows(self.groom)
        self.assertEqual(shown["node"], "color")
        params = [r["param"] for r in shown["rows"]]
        self.assertIn("melanin", params)  # natural hair by default
        self.assertNotIn("color", params)
        # Every everyday param, the randomness in either mode, and the pigment's resulting color.
        for param in ("redness", "dye", "result", "greys", "melaninRandom", "hueRandom", "saturationRandom", "clumpShare",
                      "tipColor", "tipStrength", "tipOffsetRandom", "specular", "roughness"):
            self.assertIn(param, params)
        result = next(r for r in shown["rows"] if r["param"] == "result")["value"]
        for got, want in zip(result, groomlab_core.melanin_color(0.35, 0.2)):
            self.assertAlmostEqual(got, want, places=5)
        # Each row edits its param.
        ae.set_color_param(self.groom, "color", "hueRandom", 0.25)
        ae.set_color_param(self.groom, "color", "saturationRandom", 0.4)
        ae.set_color_param(self.groom, "color", "clumpShare", 1.0)
        set_ = groomlab.hair_color_params(self.groom)
        self.assertEqual((set_["hueRandom"], set_["saturationRandom"], set_["clumpShare"]), (0.25, 0.4, 1.0))
        self.assertIn("clumpId", ae.color_rows(self.groom)["note"])  # no Clump before it: the note says so
        ae.set_color_param(self.groom, "color", "mode", "color")
        ae.set_color_param(self.groom, "color", "color", [1.0, 1.0, 1.0])
        params = [r["param"] for r in ae.color_rows(self.groom)["rows"]]
        self.assertIn("color", params)
        self.assertIn("valueRandom", params)
        for param in ("melanin", "melaninRandom", "result", "greys"):
            self.assertNotIn(param, params)
        look = groomlab.shading.hair_look_json(self.groom)
        self.assertTrue(look["colorsByRgb"])
        self.assertEqual([round(c, 4) for c in look["dye"]], [1.0, 1.0, 1.0])
        usd = os.path.join(tempfile.mkdtemp(prefix="groomlab_picked_"), "white.usda")
        groomlab.export_usd(self.groom, usd, density=0.5)
        with open(usd) as f:
            block = re.search(r"primvars:displayColor\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", f.read()).group(1)
        colors = [tuple(float(v) for v in c.split(",")) for c in re.findall(r"\(([^)]*)\)", block)]
        self.assertTrue(colors)
        for c in colors:
            self.assertAlmostEqual(min(c), 1.0, places=3)  # white, not a dyed brown
        # A tip color picked while the tips are at 0 raises them, so it shows.
        ae.set_color_param(self.groom, "color", "tipColor", [0.9, 0.6, 0.5])
        self.assertAlmostEqual(groomlab.hair_color_params(self.groom)["tipStrength"], 1.0)

    def test_a_color_maps_rgb_output_drives_a_color_param(self):
        # A Map node's rgb output linked to HairColor's color (exposed as an input): each strand the
        # map's color at its root, times the param's (white), in the exports as the viewport has it.
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        from groomlab import graph as gm
        groomlab.add_map(self.groom, "tint", kind="vertices", color=(0.125, 0.75, 0.25, 1.0))
        model = gm.GraphModel.from_groom(self.groom)
        color = model.add_node("HairColor", name="color", params={"mode": "color", "color": [1.0, 1.0, 1.0]})
        model.insert_before("output", color)
        model.save(self.groom)
        self.assertTrue(gm.linkable(model.param_spec("color", "color")))
        self.assertTrue(gm.value_port(model.param_spec("color", "mode")))  # a choice: one value for the node
        node = groomlab.use_map(self.groom, "color", "color", "tint", kind="vertices", channel="rgb")
        model = gm.GraphModel.from_groom(self.groom)
        self.assertEqual(model.output_of("color", "color"), "rgb")
        self.assertEqual(model.node_type(node), "Map")
        self.assertEqual(groomlab.errors(self.groom), [])
        usd = os.path.join(tempfile.mkdtemp(prefix="groomlab_rgb_map_"), "tinted.usda")
        groomlab.export_usd(self.groom, usd, density=0.5)
        with open(usd) as f:
            block = re.search(r"primvars:displayColor\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", f.read()).group(1)
        colors = [tuple(float(v) for v in c.split(",")) for c in re.findall(r"\(([^)]*)\)", block)]
        self.assertGreater(len(colors), 50)
        shown = groomlab_core.hair_appearance_color([0.125, 0.75, 0.25])  # displayColor: the appearance
        for c in colors:
            for got, want in zip(c, shown):
                self.assertAlmostEqual(got, want, places=3)
        # A white map does nothing: the param's own color then.
        groomlab.fill_map(self.groom, "tint", color=(1.0, 1.0, 1.0))
        groomlab.set_param(self.groom, "color", "color", [0.5, 0.25, 0.1])
        groomlab.export_usd(self.groom, usd, density=0.5)
        with open(usd) as f:
            block = re.search(r"primvars:displayColor\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", f.read()).group(1)
        first = tuple(float(v) for v in re.findall(r"\(([^)]*)\)", block)[0].split(","))
        for got, want in zip(first, groomlab_core.hair_appearance_color([0.5, 0.25, 0.1])):
            self.assertAlmostEqual(got, want, places=3)

    def test_painted_hair_color_reaches_the_exports(self):
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        # HairColor.map names a color map: its color at each root is the hair's, in the viewport
        # and every exporter (USD displayColor, Alembic Cd: as the hair looks, its appearance).
        from groomlab import graph as gm
        import array
        groomlab.add_map(self.groom, "dye", kind="vertices", color=(0.125, 0.75, 0.25, 1.0))
        model = gm.GraphModel.from_groom(self.groom)
        color = model.add_node("HairColor", name="color",
                               params={"mode": "color", "map": "dye", "clumpShare": 0.0})
        model.insert_before("output", color)
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(groomlab.hair_color_node(self.groom), "color")
        folder = tempfile.mkdtemp(prefix="groomlab_dye_")
        usd = os.path.join(folder, "dyed.usda")
        groomlab.export_usd(self.groom, usd, density=0.5)
        with open(usd) as f:
            text = f.read()
        block = re.search(r"primvars:displayColor\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", text).group(1)
        colors = [tuple(float(v) for v in c.split(",")) for c in re.findall(r"\(([^)]*)\)", block)]
        self.assertGreater(len(colors), 50)
        shown = groomlab_core.hair_appearance_color([0.125, 0.75, 0.25])
        for c in colors:
            for got, want in zip(c, shown):
                self.assertAlmostEqual(got, want, places=3)
        if hasattr(cmds, "groomLabExportAlembic"):
            abc = os.path.join(folder, "dyed.abc")
            groomlab.export_alembic(self.groom, abc, density=0.5)
            with open(abc, "rb") as f:
                data = f.read()
            found = 0  # the Cd samples: float triples of the painted color, wherever Ogawa put them
            for offset in range(4):
                floats = array.array("f", data[offset:offset + (len(data) - offset) // 4 * 4])
                found = max(found, sum(1 for i in range(len(floats) - 2)
                                       if abs(floats[i] - shown[0]) < 1e-3 and abs(floats[i + 1] - shown[1]) < 1e-3 and
                                       abs(floats[i + 2] - shown[2]) < 1e-3))
            self.assertGreaterEqual(found, len(colors))
        # Mode melanin: the map dyes the pigment's color; the map repainted, the hair follows.
        groomlab.set_param(self.groom, "color", "mode", "melanin")
        groomlab.set_param(self.groom, "color", "melaninRandom", 0.0)
        groomlab.export_usd(self.groom, usd, density=0.5)
        with open(usd) as f:
            block = re.search(r"primvars:displayColor\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", f.read()).group(1)
        multiplied = [tuple(float(v) for v in c.split(",")) for c in re.findall(r"\(([^)]*)\)", block)]
        self.assertLess(multiplied[0][1], shown[1] - 0.05)
        groomlab.set_param(self.groom, "color", "mode", "color")
        groomlab.fill_map(self.groom, "dye", color=(1.0, 0.0, 0.0))
        groomlab.export_usd(self.groom, usd, density=0.5)
        with open(usd) as f:
            block = re.search(r"primvars:displayColor\.timeSamples = \{\s*[\d.-]+: \[([^\]]*)\]", f.read()).group(1)
        red = [tuple(float(v) for v in c.split(",")) for c in re.findall(r"\(([^)]*)\)", block)]
        for got, want in zip(red[0], groomlab_core.hair_appearance_color([1.0, 0.0, 0.0])):
            self.assertAlmostEqual(got, want, places=3)
        self.assertGreater(red[0][0], 10 * red[0][1])  # red
        # A missing map is an error on the node.
        groomlab.set_param(self.groom, "color", "map", "nowhere")
        self.assertTrue(any("nowhere" in e for e in groomlab.errors(self.groom)))

    def test_asset_folder_beside_the_scene(self):
        # GroomLab's files live in a groomlab/<scene name> folder beside the scene file: masks,
        # card bakes and exports go there by default (a folder per groom inside), and the groom
        # stores them relative to the scene, so the scene and that folder move together.
        import shutil
        home = tempfile.mkdtemp(prefix="groomlab_assets_")
        scene = os.path.join(home, "shot", "hero.ma")
        os.makedirs(os.path.dirname(scene))
        cmds.file(rename=scene)
        cmds.file(save=True, type="mayaAscii", force=True)
        folder = groomlab.asset_folder("masks")
        self.assertEqual(folder, os.path.join(home, "shot", "groomlab", "hero", "masks").replace("\\", "/"))
        self.assertTrue(os.path.isdir(folder))
        groomlab.add_map(self.groom, "crown", 0.0, kind="texture", resolution=64)
        groomlab.use_map(self.groom, "length", "keep", "crown")
        stored = next(m["texture"] for m in groomlab.maps(self.groom))
        self.assertEqual(stored, "groomlab/hero/masks/groomLab1/crown.png")
        self.assertTrue(os.path.exists(os.path.join(folder, "groomLab1", "crown.png")))
        self.assertEqual(groomlab.resolve_path(stored).replace("\\", "/"), folder + "/groomLab1/crown.png")
        self.assertEqual(groomlab.errors(self.groom), [])
        groomlab.fill_map(self.groom, "crown", 1.0)  # the stored path resolved for the rewrite
        self.assertEqual(groomlab.errors(self.groom), [])
        groomlab.create_cards(self.groom, max_cards=12)
        paths = groomlab.bake_cards(self.groom, resolution=64)
        self.assertTrue(all(p.startswith(os.path.join(home, "shot", "groomlab", "hero", "cards", "groomLab1").replace(
            "\\", "/")) for p in paths))
        maps = cmds.getAttr(self.groom + ".cardMaps")
        self.assertIn("Color=groomlab/hero/cards/groomLab1/groomLab1_Color.tga", maps)
        self.assertIn("Atlas=groomlab/hero/cards/groomLab1/groomLab1_Atlas.json", maps)
        self.assertTrue(groomlab.card_stats(self.groom)["baked"])
        cmds.file(save=True, type="mayaAscii", force=True)
        # The scene and its folder sent elsewhere: everything still found.
        moved = os.path.join(home, "sent")
        shutil.copytree(os.path.join(home, "shot"), moved)
        shutil.rmtree(os.path.join(home, "shot"))
        cmds.file(os.path.join(moved, "hero.ma"), open=True, force=True)
        groom = cmds.ls(type=groomlab.GROOM_TYPE)[0]
        self.assertEqual(groomlab.errors(groom), [])
        self.assertEqual(groomlab.resolve_path("groomlab/hero/masks/groomLab1/crown.png").replace("\\", "/"),
                         os.path.join(moved, "groomlab", "hero", "masks", "groomLab1", "crown.png").replace("\\", "/"))
        self.assertTrue(groomlab.card_stats(groom)["baked"])
        self.assertFalse(groomlab.card_stats(groom)["stale"])
        # A new file in the moved scene's folder, and one outside it (and the project: the
        # machine's, every host's rule for a scene, paths.h), stored as they should be.
        self.assertEqual(groomlab.portable_path(os.path.join(moved, "groomlab", "exports", "x.fbx")), "groomlab/exports/x.fbx")
        self.assertEqual(groomlab.portable_path(home.replace("\\", "/") + "/other.png"), home.replace("\\", "/") + "/other.png")
        # Save Scene As elsewhere: the files the groom names beside the scene follow it, into
        # the new scene's own folder, and the groom points there (in the file saved, too).
        elsewhere = os.path.join(home, "elsewhere")
        os.makedirs(elsewhere)
        cmds.file(rename=os.path.join(elsewhere, "hero2.ma"))
        cmds.file(save=True, type="mayaAscii", force=True)  # the plugin's save callbacks copy the files
        for name in ("masks/groomLab1/crown.png", "cards/groomLab1/groomLab1_Atlas.json",
                     "cards/groomLab1/groomLab1_Depth.tga"):
            self.assertTrue(os.path.exists(os.path.join(elsewhere, "groomlab", "hero2", name)), name)
        self.assertEqual(next(m["texture"] for m in groomlab.maps(groom)), "groomlab/hero2/masks/groomLab1/crown.png")
        self.assertIn("Color=groomlab/hero2/cards/groomLab1/groomLab1_Color.tga", cmds.getAttr(groom + ".cardMaps"))
        with open(os.path.join(elsewhere, "hero2.ma")) as f:
            self.assertIn("groomlab/hero2/masks/groomLab1/crown.png", f.read())
        self.assertEqual(groomlab.errors(groom), [])
        self.assertTrue(groomlab.card_stats(groom)["baked"])
        self.assertEqual(groomlab.follow_scene_files(), [])  # once


    def test_save_as_takes_the_graphs_files_and_simulate_bakes_along(self):
        # The files the graph reads (an image, a UDIM set) and a Simulate node's bake, beside the
        # scene: a Save As elsewhere copies them into the new scene's folder and points there.
        from groomlab import graph as gm
        home = tempfile.mkdtemp(prefix="groomlab_save_as_").replace("\\", "/")
        cmds.workspace(home, openWorkspace=True)
        os.makedirs(home + "/shot")
        cmds.file(rename=home + "/shot/hero.ma")
        cmds.file(save=True, type="mayaAscii", force=True)
        caches = home + "/shot/groomlab/hero/caches"
        os.makedirs(caches)
        for name in ("tint.png", "root.1001.png", "root.1002.png"):
            with open(caches + "/" + name, "wb") as f:
                f.write(name.encode())
        with open(caches + "/sim.abc", "wb") as f:
            f.write(b"abc")
        model = gm.GraphModel.from_groom(self.groom, gm.OperatorCatalog.from_maya())
        model.add_node("HairColor", name="tinted", params={"map": "groomlab/hero/caches/tint.png"})
        model.insert_before("output", "tinted")
        model.set_param("length", "keep", groomlab.texture_map("groomlab/hero/caches/root.<UDIM>.png"))
        model.save(self.groom)
        cmds.setAttr(self.groom + ".simCaches", json.dumps(
            {"simulate": {"file": "groomlab/hero/caches/sim.abc", "object": "", "stamp": "1"}}), type="string")
        cmds.file(save=True, type="mayaAscii", force=True)
        elsewhere = home + "/elsewhere"
        os.makedirs(elsewhere)
        cmds.file(rename=elsewhere + "/hero2.ma")
        cmds.file(save=True, type="mayaAscii", force=True)  # the plugin's save callbacks copy the files
        for name in ("tint.png", "root.1001.png", "root.1002.png", "sim.abc"):
            self.assertTrue(os.path.isfile(elsewhere + "/groomlab/hero2/caches/" + name), name)
        files = {(f["kind"], f["key"]): f["path"] for f in groomlab.file_dependencies(self.groom)}
        self.assertEqual(files[("param", "tinted.map")], "groomlab/hero2/caches/tint.png")
        self.assertEqual(files[("param", "length.keep")], "groomlab/hero2/caches/root.<UDIM>.png")
        self.assertEqual(files[("simCache", "simulate")], "groomlab/hero2/caches/sim.abc")
        with open(elsewhere + "/hero2.ma") as f:
            saved = f.read()
        self.assertIn("groomlab/hero2/caches/tint.png", saved)  # in the file saved, too
        self.assertIn("groomlab/hero2/caches/sim.abc", saved)
        self.assertEqual(groomlab.follow_scene_files(), [])  # once

    def test_cornrows_ponytail_and_braid_cards(self):
        # Two cornrows over the crown (feed: strands picked up along the rows, within
        # feed_distance, over the first feed_until of each row), a ponytail braid gathering a
        # wide region over the scalp, and cards: one card set per braid.
        def on_sphere(points):
            return [tuple(c / math.sqrt(sum(v * v for v in p)) * 1.01 for c in p) for p in points]
        rows = []
        for z in (-0.25, 0.25):
            rows.append(cmds.curve(degree=1, point=on_sphere([(-0.7, 0.7, z), (-0.3, 0.95, z), (0.3, 0.95, z), (0.7, 0.7, z)])))
        node = groomlab.add_braid(self.groom, rows, style="cornrow", width=0.12, region=0.15, feed_distance=0.12,
                                  feed_until=0.8)
        params = {n["name"]: n.get("params", {}) for n in groomlab.get_graph(self.groom)["nodes"]}[node]
        self.assertTrue(params["feed"])
        self.assertAlmostEqual(params["feedDistance"], 0.12)
        self.assertAlmostEqual(params["feedUntil"], 0.8)
        self.assertEqual(groomlab.errors(self.groom), [])
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertIn("braidId", stats["channels"].get(node, []))
        self.assertIn("braidFlat", stats["channels"].get(node, []))
        # The ponytail: a path from the back of the head hanging down, gathering a wide region
        # over the scalp (along_scalp), not through it.
        tail = cmds.curve(degree=2, point=[(0.0, 0.3, -0.95), (0.0, -0.2, -1.3), (0.0, -1.0, -1.4)])
        pony = groomlab.add_braid(self.groom, [tail], style="three", group="ponytail", width=0.2, region=0.8,
                                  feed=False, along_scalp=True)
        params = {n["name"]: n.get("params", {}) for n in groomlab.get_graph(self.groom)["nodes"]}[pony]
        self.assertFalse(params["feed"])
        self.assertTrue(params["alongScalp"])
        self.assertEqual(groomlab.errors(self.groom), [])
        # Cards: braids make card sets of their own (Cards.braids, on by default); off, the
        # braided strands are carded by clump like the rest. Both evaluate and draw.
        cards = groomlab.create_cards(self.groom, max_cards=12)
        self.assertEqual(groomlab.errors(self.groom), [])
        with_braids = groomlab.card_stats(self.groom)
        self.assertGreater(with_braids["cards"], 0)
        groomlab.set_param(self.groom, cards, "braids", False)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(groomlab.card_stats(self.groom)["cards"], 0)
        if hasattr(cmds, "ogsRender"):
            folder = tempfile.mkdtemp(prefix="groomlab_braidcards_")
            cmds.workspace(folder, openWorkspace=True)
            groomlab.set_param(self.groom, cards, "braids", True)
            groomlab.compare_cards(self.groom, True)
            try:
                cmds.ogsRender(width=64, height=64)
            except RuntimeError as e:
                self.skipTest("Viewport 2.0 is not available offscreen: %s" % e)
            self.assertEqual(groomlab.errors(self.groom), [])

    def test_export_for_unreal(self):
        path = os.path.join(tempfile.gettempdir(), "groomlab_unreal.abc")
        self.assertEqual(groomlab.export_unreal_groom(self.groom, path), 1)
        self.assertGreater(os.path.getsize(path), 1000)
        with self.assertRaises(RuntimeError):
            cmds.groomLabExportAlembic(self.groom, file=path, unreal=True, guideGroup="primary")
        # In Unreal's space (Z up, left-handed, cm) from the scene's, as Unreal's FBX importer
        # turns this scene's meshes: Y up (x, y, z) -> (x, z, y), Z up -> (x, -y, z).
        import groomlab_core
        plain = os.path.join(tempfile.gettempdir(), "groomlab_unreal_plain.abc")
        cmds.groomLabExportAlembic(self.groom, file=plain)
        tip = groomlab_core.read_alembic_curves(plain)[0].strands.strand_points(0)[-1]
        x, y, z = tip.x, tip.y, tip.z
        try:
            for axis, want in (("y", (x, z, y)), ("z", (x, -y, z))):
                cmds.upAxis(axis=axis, rotateView=True)
                cmds.groomLabExportAlembic(self.groom, file=path, unreal=True)
                got = groomlab_core.read_alembic_curves(path)[0].strands.strand_points(0)[-1]
                for a, b in zip((got.x, got.y, got.z), want):
                    self.assertAlmostEqual(a, b, places=4, msg=axis)
        finally:
            cmds.upAxis(axis="y", rotateView=True)
        os.remove(plain)
        os.remove(path)

    def test_lock(self):
        groomlab.lock_groom(self.groom)
        with self.assertRaises(RuntimeError):
            groomlab.brush_stroke(self.groom, "comb", [(0, 1.2, 0), (0.5, 1.2, 0)], radius=5.0)
        groomlab.lock_groom(self.groom, False)
        self.assertIn(self.groom, groomlab.list_grooms())

    def roots(self):
        return {g: pts[0] for g, pts in guide_points(self.groom).items()}

    def test_transfer_to_a_new_scalp(self):
        groomlab.add_map(self.groom, "fur", 0.5)
        part = cmds.curve(degree=1, point=[(0.0, math.cos(a), math.sin(a)) for a in (-0.4, 0.0, 0.4)])
        groomlab.add_part(self.groom, [part], name="crown", delete_curves=True)
        groomlab.flow_stroke(self.groom, [(0.3, 0.9, 0.3), (0.3, 0.5, 0.8)])
        groomlab.flow_sink(self.groom, (0.0, 1.0, 0.0), radius=0.3)
        arrows = len(groomlab.flow_arrows(self.groom))
        self.assertGreater(arrows, 0)
        before = self.roots()
        sculpt = cmds.polySphere(radius=1.2, subdivisionsX=24, subdivisionsY=12)[0]

        self.assertEqual(groomlab.transfer_groom(self.groom, sculpt), [])
        # Flow arrows and sinks follow, re-rooted on the new mesh's faces.
        self.assertEqual(len(groomlab.flow_arrows(self.groom)), arrows)
        sinks = groomlab.flow_sinks(self.groom)
        self.assertEqual(len(sinks), 1)
        self.assertAlmostEqual(sinks[0][4], 0.3, places=5)
        self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True), cmds.listRelatives(sculpt, shapes=True))
        after = self.roots()
        self.assertEqual(set(after), set(before))  # same guides, same IDs
        for g, p in after.items():
            self.assertAlmostEqual(math.sqrt(sum(c * c for c in p)), 1.2, delta=0.02)
        self.assertEqual(groomlab.list_maps(self.groom), ["fur"])
        self.assertEqual(groomlab.list_parts(self.groom), ["crown"])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)

        cmds.undo()  # one step
        self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True), cmds.listRelatives(self.scalp, shapes=True))
        for g, p in self.roots().items():
            self.assertLess(moved([p], [before[g]]), 1e-4)

        # A scalp elsewhere and twice the size: fit first.
        far = cmds.polySphere(radius=2.0, subdivisionsX=30, subdivisionsY=15)[0]
        cmds.move(10, 0, 0, far)
        groomlab.transfer_groom(self.groom, far, fit=True)
        for g, p in self.roots().items():
            expected = (10 + 2 * before[g][0], 2 * before[g][1], 2 * before[g][2])
            self.assertLess(moved([p], [expected]), 0.1)

        # Vertex maps live on the groom, one value per vertex: projected onto the new scalp's
        # vertices by default; left as they are without (the new scalp has another count, so the
        # map the graph reads is reported).
        groomlab.paint_scalp_map(self.groom, "mask", paint=False)
        self.assertEqual(groomlab.transfer_groom(self.groom, sculpt, maps=False), ["mask"])
        cmds.undo()
        self.assertEqual(groomlab.transfer_groom(self.groom, sculpt), [])
        self.assertEqual(len(cmds.groomLabScalpMaps(self.groom, values="mask")), cmds.polyEvaluate(sculpt, vertex=True))
        with self.assertRaises(RuntimeError):
            cmds.groomLabTransfer(self.groom, scalp=sculpt)  # already there

    def test_parts_from_curves(self):
        groomlab.clear_guides(self.groom)
        # Two guides either side of x = 0 near the top, leaning apart.
        left = cmds.curve(degree=1, point=[(-0.15, 0.99, 0.1), (-0.8, 1.4, 0.1)])
        right = cmds.curve(degree=1, point=[(0.15, 0.99, 0.1), (0.8, 1.4, 0.1)])
        groomlab.guides_from_curves(self.groom, [left, right])
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        mixed = self.strand_extent()

        points = [(0.0, math.cos(a), math.sin(a)) for a in [0.05 * i - 0.5 for i in range(21)]]
        part = cmds.curve(degree=1, point=points)
        self.assertEqual(groomlab.add_part(self.groom, [part], name="crown"), ["crown"])
        self.assertEqual(groomlab.list_parts(self.groom), ["crown"])
        cmds.undo()
        self.assertEqual(groomlab.list_parts(self.groom), [])
        cmds.redo()
        self.assertEqual(groomlab.list_parts(self.groom), ["crown"])
        cmds.delete(part)  # the part is stored on the groom; the curve is no longer needed
        self.assertEqual(groomlab.errors(self.groom), [])
        parted = self.strand_extent()
        self.assertNotEqual(parted, mixed)  # strands no longer blend both guides across the part

        before = self.strand_points()
        node = groomlab.add_part_node(self.groom, width=0.3)
        self.assertEqual({n["name"]: n["type"] for n in groomlab.get_graph(self.groom)["nodes"]}[node], "Part")
        after = self.strand_points()  # the part opens: strands beside it lean away
        self.assertEqual(len(after), len(before))
        moved = sum(1 for p, q in zip(before, after) if max(abs(p[i] - q[i]) for i in range(3)) > 1e-5)
        self.assertGreater(moved, 50)
        self.assertLess(moved, len(before) // 2)  # only the strands near the part

        path = os.path.join(tempfile.gettempdir(), "groomlab_parts.groom")
        groomlab.export_groom(self.groom, path)
        with open(path) as f:
            self.assertIn('"crown"', f.read())

        groomlab.remove_part(self.groom, "crown")
        self.assertEqual(groomlab.list_parts(self.groom), [])

        # Drawn freehand (here scripted): the part tool's strokes, with symmetry and undo.
        stroke = [(0.3 * math.sin(a), math.cos(a), 0.3 * math.sin(a) + 0.5) for a in [0.05 * i - 0.4 for i in range(17)]]
        groomlab.brush_stroke(self.groom, "part", stroke, part_name="side", symmetry="x")
        self.assertEqual(groomlab.list_parts(self.groom), ["side", "side1"])
        cmds.undo()
        self.assertEqual(groomlab.list_parts(self.groom), [])
        with self.assertRaises(RuntimeError):
            groomlab.remove_part(self.groom, "nope")

    def test_children_can_be_simulated(self):
        # Generated children have 64-bit IDs; sim curves carry them as text.
        self.assertTrue(any(int(c) >= 2 ** 31 for c in self.children))
        before = cmds.exactWorldBoundingBox(self.groom)
        made = sim.create_sim_curves(self.groom, "children", dynamic=False)
        self.assertEqual(len(made["curves"]), 18)
        big = [c for c in self.children if int(c) >= 2 ** 31][0]
        cmds.xform(made["curves"][int(big)], worldSpace=True, pivots=(0, 1, 0))
        cmds.setAttr(made["curves"][int(big)] + ".rotateZ", -60)
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)
        sim.remove_sim(self.groom, made)

    def test_bake_node_output_to_guides(self):
        from groomlab import bake
        # Arrows across the top of the head, a Flow node laying the scattered strands
        # along them, and a ToGuides node picking some of those strands: baked, the
        # picked strands become primary guides of their own group.
        groomlab.flow_stroke(self.groom, [(-0.6 + 0.1 * i, 0.9, 0.3) for i in range(13)], radius=0.2)
        graph = groomlab.get_graph(self.groom, expand_default=True)
        graph["nodes"] += [{"name": "flow", "type": "Flow", "params": {}},
                           {"name": "toGuides", "type": "ToGuides", "params": {"fraction": 0.05}}]
        graph["connections"] += [{"from": "scatter", "to": "flow", "input": "strands"},
                                 {"from": "flow", "to": "toGuides", "input": "strands"}]
        groomlab.set_graph(self.groom, graph)
        self.assertEqual(groomlab.errors(self.groom), [])
        before = len(groomlab.get_hierarchy(self.groom)["guides"])
        ids = bake.bake_guides(self.groom, "toGuides", group="baked")
        self.assertGreater(len(ids), 0)
        self.assertEqual(sorted(groomlab.guide_ids(self.groom, "baked")), sorted(ids))
        self.assertIn("baked", [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertEqual(len(groomlab.get_hierarchy(self.groom)["guides"]), before + len(ids))
        # Rooted on the scalp (a unit sphere), and the groom still evaluates.
        root = guide_points(self.groom)[ids[0]][0]
        self.assertAlmostEqual(math.sqrt(sum(c * c for c in root)), 1.0, delta=0.05)
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        self.assertEqual(groomlab.errors(self.groom), [])
        # One undo step.
        cmds.undo()
        self.assertEqual(len(groomlab.get_hierarchy(self.groom)["guides"]), before)
        cmds.redo()
        self.assertEqual(sorted(groomlab.guide_ids(self.groom, "baked")), sorted(ids))
        # Only nodes with guides or strands bake.
        with self.assertRaises(RuntimeError):
            bake.bake_guides(self.groom, "nope")
        with self.assertRaises(RuntimeError):
            bake.bake_guides(self.groom, "scalp")

    def test_rest_mesh_with_another_vertex_count_is_reported(self):
        # A rest mesh that is not the scalp undeformed is dropped, and the groom says so
        # (else strands re-scatter every frame with no hint why).
        cube = cmds.polyCube()[0]
        cube_shape = cmds.listRelatives(cube, shapes=True, fullPath=True)[0]
        cmds.connectAttr(cube_shape + ".worldMesh[0]", self.groom + ".restMesh")
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        self.assertTrue(any("restMesh" in e for e in groomlab.errors(self.groom)), groomlab.errors(self.groom))
        cmds.disconnectAttr(cube_shape + ".worldMesh[0]", self.groom + ".restMesh")
        cmds.getAttr(self.groom + ".outStrandCount")
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_rest_mesh_is_the_same_mesh_undeformed(self):
        # Two halves combined with history leave the halves as intermediate meshes: they are not
        # the scalp's rest pose, so a new groom gets none (and no "restMesh: ignored" warning).
        left = cmds.polySphere(subdivisionsX=12, subdivisionsY=8)[0]
        right = cmds.polySphere(subdivisionsX=12, subdivisionsY=8)[0]
        cmds.move(3, 0, 0, right)
        combined = cmds.polyUnite(left, right, constructionHistory=True)[0]
        groom = groomlab.create_groom(combined, name="combinedGroom")
        self.assertIsNone(groomlab.rest_mesh(groom))
        cmds.getAttr(groom + ".outStrandCount")
        self.assertFalse(any("restMesh" in e for e in groomlab.errors(groom)), groomlab.errors(groom))
        # Deformed later (a cluster), its own Orig shape is the rest pose, at the same vertex count.
        cmds.cluster(combined)
        groomlab.set_rest_mesh(groom)
        rest = groomlab.rest_mesh(groom)
        self.assertIsNotNone(rest)
        self.assertEqual(cmds.polyEvaluate(rest, vertex=True), cmds.polyEvaluate(combined, vertex=True))
        cmds.getAttr(groom + ".outStrandCount")
        self.assertFalse(any("restMesh" in e for e in groomlab.errors(groom)), groomlab.errors(groom))

    def test_static_groom_keeps_its_strands_across_frames(self):
        # Nothing animated: another frame is a cache hit and the strands stay put.
        cmds.currentTime(1)
        count = cmds.getAttr(self.groom + ".outStrandCount")
        box = cmds.exactWorldBoundingBox(self.groom)
        cmds.currentTime(25)
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), count)
        self.assertEqual(cmds.exactWorldBoundingBox(self.groom), box)

    def test_brushes_shape_parts_and_flow_with_targets(self):
        # A part over the crown (x = 0), then a comb stroke toward +x with -targets parts:
        # the points under the brush move, the guides do not, and it is one undo step.
        stroke = [(0.0, math.cos(a), math.sin(a)) for a in [0.05 * i - 0.5 for i in range(21)]]
        groomlab.brush_stroke(self.groom, "part", stroke, part_name="crown")
        before_parts = cmds.getAttr(self.groom + ".partData")
        before_guides = cmds.getAttr(self.groom + ".guideData")
        before_flow = cmds.getAttr(self.groom + ".flowData")
        part_count = len(json.loads(before_parts)["parts"][0]["points"])
        cmds.groomLabBrushStroke(self.groom, mode="comb", radius=0.3, strength=1.0, targets="parts",
                                 point=[(0.0, 1.0, 0.0), (0.3, 1.0, 0.0)])
        after_parts = cmds.getAttr(self.groom + ".partData")
        self.assertNotEqual(after_parts, before_parts)
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before_guides)  # guides left alone
        self.assertEqual(cmds.getAttr(self.groom + ".flowData"), before_flow)
        part = json.loads(after_parts)["parts"][0]
        self.assertEqual(part["name"], "crown")
        self.assertEqual(len(part["points"]), part_count)  # slid, not cut
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.undo()  # one step
        self.assertEqual(cmds.getAttr(self.groom + ".partData"), before_parts)
        cmds.redo()
        self.assertEqual(cmds.getAttr(self.groom + ".partData"), after_parts)
        cmds.undo()  # the straight part again
        # Cut splits it in two where the brush touches; erase removes what is under it.
        cmds.groomLabBrushStroke(self.groom, mode="cut", radius=0.15, strength=1.0, targets="parts",
                                 point=[(0.0, 1.0, 0.0)])
        self.assertEqual(sorted(groomlab.list_parts(self.groom)), ["crown", "crown1"])
        cmds.groomLabBrushStroke(self.groom, mode="erase", radius=0.4, strength=1.0, targets="parts",
                                 point=[(0.0, 0.9, 0.45)])
        self.assertEqual(groomlab.list_parts(self.groom), ["crown"])
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before_guides)  # erase with -targets parts spares guides
        cmds.undo()
        self.assertEqual(sorted(groomlab.list_parts(self.groom)), ["crown", "crown1"])

        # Flow: arrows across the top, pointing +x; a comb stroke toward +z slides the ones
        # under it and turns them along the stroke. The rest, and the guides, stay put.
        groomlab.flow_stroke(self.groom, [(-0.6 + 0.1 * i, 0.9, 0.3) for i in range(13)], radius=0.2)
        before_flow = cmds.getAttr(self.groom + ".flowData")
        pins = json.loads(before_flow)["pins"]
        self.assertGreater(len(pins), 4)
        cmds.groomLabBrushStroke(self.groom, mode="comb", radius=0.25, strength=1.0, targets="flow",
                                 point=[(0.0, 0.95, 0.3), (0.0, 0.85, 0.5)])
        after_flow = cmds.getAttr(self.groom + ".flowData")
        self.assertNotEqual(after_flow, before_flow)
        after_pins = json.loads(after_flow)["pins"]
        self.assertEqual(len(after_pins), len(pins))  # moved, none added or removed
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), before_guides)
        moved_pins = sum(1 for a, b in zip(pins, after_pins) if (a["face"], a["u"], a["v"]) != (b["face"], b["u"], b["v"]))
        self.assertGreater(moved_pins, 0)
        self.assertLess(moved_pins, len(pins))  # the far ends of the line stayed
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".flowData"), before_flow)
        cmds.redo()
        self.assertEqual(cmds.getAttr(self.groom + ".flowData"), after_flow)
        # Twist turns a whorl's angle; the tool context takes and reports the targets.
        groomlab.flow_sink(self.groom, (0.3, 1.0, 0.0), kind="whorl", radius=0.3)
        cmds.groomLabBrushStroke(self.groom, mode="twist", radius=0.2, strength=1.0, targets="flow",
                                 point=[(0.3, 1.0, 0.0)])
        self.assertGreater(groomlab.flow_sinks(self.groom)[0][3], 70.0)
        with self.assertRaises(RuntimeError):
            cmds.groomLabBrushStroke(self.groom, mode="comb", targets="hats", point=[(0, 1, 0)])
        # -targets none: a stroke that touches nothing.
        before = [cmds.getAttr(self.groom + "." + a) for a in ("guideData", "partData", "flowData")]
        cmds.groomLabBrushStroke(self.groom, mode="comb", radius=0.3, targets="none",
                                 point=[(0.0, 1.0, 0.0), (0.3, 1.0, 0.0)])
        self.assertEqual([cmds.getAttr(self.groom + "." + a) for a in ("guideData", "partData", "flowData")], before)
        # The interactive tool takes the same flag (context edits and queries only round-trip in the Maya UI).
        ctx = groomlab.brush_tool("comb", activate=False)
        cmds.groomLabBrushCtx(ctx, edit=True, targets="guides,flow")

    def test_brush_alphas_library_and_tool_flags(self):
        # The alphas shipped with the module, then images added: copied into the user's folder
        # (GROOMLAB_USER_ALPHAS here), listed first. The tool takes an alpha by its file.
        user = tempfile.mkdtemp(prefix="groomlab_alphas_")  # never the user's own folder
        old = os.environ.get("GROOMLAB_USER_ALPHAS")
        os.environ["GROOMLAB_USER_ALPHAS"] = user
        try:
            shipped = groomlab.brush_alphas()
            names = [a["name"] for a in shipped]
            for name in ("Cells", "Clouds", "Grain", "Noise", "Speckle", "Spots", "Square", "Streaks"):
                self.assertIn(name, names)
            self.assertTrue(all(a["shipped"] for a in shipped))
            speckle = next(a["path"] for a in shipped if a["name"] == "Speckle")
            outside = os.path.join(tempfile.mkdtemp(prefix="groomlab_alpha_src_"), "Speckle.png")
            shutil.copyfile(speckle, outside)
            added = groomlab.add_brush_alpha(outside)
            self.assertEqual(os.path.basename(added), "Speckle 2.png")  # a shipped one's name: numbered
            self.assertEqual(groomlab.brush_alphas()[0]["name"], "Speckle 2")
            self.assertEqual(os.path.normcase(os.path.normpath(groomlab.brush_alpha_folder())), os.path.normcase(os.path.normpath(user)))
            with self.assertRaises(ValueError):
                groomlab.add_brush_alpha(__file__)
        finally:
            if old is None:
                os.environ.pop("GROOMLAB_USER_ALPHAS", None)
            else:
                os.environ["GROOMLAB_USER_ALPHAS"] = old
        # The tool takes the flags (they round-trip in the Maya UI only: the GUI cases check them).
        groomlab.add_map(self.groom, "crown", 0.0, kind="vertices")
        groomlab.brush_tool("paint", attribute="crown", paint_kind="vertices", alpha=speckle, alpha_rotation="random",
                            alpha_spacing=0.5, activate=False)


    def test_card_textures_toggle(self):
        # The cards view draws the cards with their baked maps (displayCardTextures, on by default)
        # or flat, to look at the geometry. A display attribute: toggling it, and cardMaps pointing
        # anywhere (missing files, rewritten files), never upsets the evaluation.
        self.assertTrue(cmds.attributeQuery("displayCardTextures", node=self.groom, exists=True))
        self.assertTrue(cmds.getAttr(self.groom + ".displayCardTextures"))
        self.assertEqual(groomlab.card_display(self.groom), {"textures": True, "baked": False, "stale": False})
        groomlab.create_cards(self.groom, max_cards=12)
        count = cmds.getAttr(self.groom + ".outStrandCount")
        self.assertGreater(count, 0)
        self.assertEqual(groomlab.card_display(self.groom, textures=False)["textures"], False)
        self.assertFalse(cmds.getAttr(self.groom + ".displayCardTextures"))
        cmds.setAttr(self.groom + ".cardMaps", "Color=/nowhere/x_Color.tga;Coverage=/nowhere/x_Coverage.tga;Bake=1",
                     type="string")
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), count)
        folder = tempfile.mkdtemp(prefix="groomlab_cardtex_")
        groomlab.bake_cards(self.groom, folder, resolution=64, name="tex")
        self.assertTrue(groomlab.card_display(self.groom, textures=True)["baked"])
        self.assertTrue(cmds.getAttr(self.groom + ".displayCardTextures"))
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount"), count)
        self.assertGreater(json.loads(cmds.groomLabGraphStats(self.groom))["cards"], 0)
        cmds.undo()  # the toggle is undoable like any attribute
        self.assertFalse(cmds.getAttr(self.groom + ".displayCardTextures"))
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_cards_view_draws_through_every_toggle(self):
        # Drawn, not only evaluated: the viewport (offscreen here) with the cards view and its
        # textures switched on and off in every order. Turning the textured cards off once
        # crashed Maya inside the shader (a null texture assignment) while the textures were
        # given back.
        if not hasattr(cmds, "ogsRender"):
            self.skipTest("no Viewport 2.0 offscreen render")
        folder = tempfile.mkdtemp(prefix="groomlab_cardview_")
        cmds.workspace(folder, openWorkspace=True)  # the frames land here, not in the user's project
        groomlab.create_cards(self.groom, max_cards=12)
        groomlab.bake_cards(self.groom, folder, resolution=64, name="tex")

        def draw(step):
            try:
                cmds.ogsRender(width=64, height=64)
            except RuntimeError as e:
                self.skipTest("Viewport 2.0 is not available offscreen: %s" % e)
            self.assertEqual(groomlab.errors(self.groom), [], step)

        draw("strands")
        groomlab.compare_cards(self.groom, True)  # textured cards
        draw("cards textured")
        groomlab.card_display(self.groom, textures=False)
        draw("cards flat")
        groomlab.card_display(self.groom, textures=True)
        draw("cards textured again")
        groomlab.compare_cards(self.groom, False)  # textures given back while the shader is still around
        draw("strands again")
        groomlab.compare_cards(self.groom, True)
        draw("cards once more")
        cmds.setAttr(self.groom + ".cardMaps", "", type="string")  # maps gone while shown
        draw("cards without maps")
        # Guides, parts and flow over the hair (the overlay effect's pull), and depth-tested.
        self.assertTrue(cmds.getAttr(self.groom + ".displayOverlaysOnTop"))
        cmds.setAttr(self.groom + ".displayGuides", True)
        draw("guides over the cards")
        cmds.setAttr(self.groom + ".displayOverlaysOnTop", False)
        draw("guides depth-tested")
        cmds.setAttr(self.groom + ".displayOverlaysOnTop", True)
        groomlab.compare_cards(self.groom, False)
        draw("guides over the strands")
        # Flow arrows and part lines: overlay lines in the item's own color (no colors per
        # vertex), which once crashed the draw (the colors stream freed before Maya read it).
        groomlab.flow_stroke(self.groom, [(-0.6 + 0.1 * i, 0.9, 0.3) for i in range(13)], radius=0.2)
        self.assertGreater(len(groomlab.flow_arrows(self.groom)), 4)
        draw("flow arrows over the strands")
        groomlab.brush_stroke(self.groom, "part", [(0.0, math.cos(a), math.sin(a)) for a in [0.05 * i - 0.5 for i in range(21)]],
                              part_name="crown")
        self.assertEqual(groomlab.errors(self.groom), [])
        draw("part lines over the strands")
        cmds.setAttr(self.groom + ".displayOverlaysOnTop", False)
        draw("flow and parts depth-tested")
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)


def strand_length_stats(groom):
    """(count, points) of the groom as evaluated."""
    stats = json.loads(cmds.groomLabGraphStats(groom))
    return stats.get("strands", 0), stats.get("points", 0)



class ControlsAndExamplesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)

    def test_a_control_is_a_keyable_attribute(self):
        scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]
        groom = groomlab.create_groom(scalp)
        attr = groomlab.add_control(groom, "scatter", "density", name="thickness", default=100.0, max=400.0)
        self.assertEqual(attr, groom + ".ctl_thickness")
        self.assertTrue(cmds.getAttr(attr, keyable=True))
        base = cmds.getAttr(groom + ".outStrandCount")
        cmds.setAttr(attr, 300.0)
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 2 * base)
        # Keyed: the count follows the time.
        cmds.setKeyframe(attr, time=1, value=50.0)
        cmds.setKeyframe(attr, time=10, value=400.0)
        cmds.currentTime(1)
        low = cmds.getAttr(groom + ".outStrandCount")
        cmds.currentTime(10)
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 4 * low)
        self.assertEqual(groomlab.list_controls(groom), {"thickness": 400.0})
        # Removing the node drops the attribute unless it is keyed.
        graph = groomlab.get_graph(groom)
        graph["nodes"] = [n for n in graph["nodes"] if n["type"] != "Control"]
        graph["connections"] = [c for c in graph["connections"] if c["from"] != "thicknessControl"]
        for n in graph["nodes"]:
            n.pop("paramInputs", None)
        groomlab.set_graph(groom, graph)
        self.assertTrue(cmds.attributeQuery("ctl_thickness", node=groom, exists=True))  # keyed: kept
        cmds.cutKey(attr)
        groomlab.sync_controls(groom)
        self.assertFalse(cmds.attributeQuery("ctl_thickness", node=groom, exists=True))

    def test_a_keyed_blend_mixes_the_color_of_two_looks(self):
        # Interpolate, then two branches colored apart (dark and light), mixed by a Blend whose
        # amount is a keyed Control: the strands' color follows the key.
        from groomlab import utils
        scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]
        guides = [cmds.curve(degree=1, point=[(0, 1, 0), (0.3, 1.6, 0), (0.5, 2.2, 0)]),
                  cmds.curve(degree=1, point=[(1, 0, 0), (1.6, 0.3, 0), (2.2, 0.5, 0)])]
        groom = groomlab.create_groom(scalp, guides)
        plain = {"melaninRandom": 0.0}
        groomlab.set_graph(groom, {
            "nodes": [
                {"name": "scalp", "type": "Scalp"},
                {"name": "guides", "type": "Guides"},
                {"name": "scatter", "type": "Scatter", "params": {"density": 40.0}},
                {"name": "interpolate", "type": "Interpolate"},
                {"name": "dark", "type": "HairColor", "params": dict(plain, melanin=0.95)},
                {"name": "light", "type": "HairColor", "params": dict(plain, melanin=0.05)},
                {"name": "blend", "type": "Blend", "params": {"mix": 1.0}},  # times the Control
                {"name": "output", "type": "Output"},
            ],
            "connections": [
                {"from": "scalp", "to": "scatter", "input": "scalp"},
                {"from": "scatter", "to": "interpolate", "input": "strands"},
                {"from": "guides", "to": "interpolate", "input": "guides"},
                {"from": "interpolate", "to": "dark", "input": "strands"},
                {"from": "interpolate", "to": "light", "input": "strands"},
                {"from": "dark", "to": "blend", "input": "a"},
                {"from": "light", "to": "blend", "input": "b"},
                {"from": "blend", "to": "output", "input": "strands"},
            ],
            "output": "output",
        })
        self.assertEqual(groomlab.errors(groom), [])
        attr = groomlab.add_control(groom, "blend", "mix", name="wet", default=0.0)
        cmds.setKeyframe(attr, time=1, value=0.0)
        cmds.setKeyframe(attr, time=11, value=1.0)

        def reds():
            made = utils.strands_to_curves(groom, max_curves=20)
            out = {}
            for curve in made["curves"]:
                shape = cmds.listRelatives(curve, shapes=True, fullPath=True)[0]
                out[cmds.getAttr(shape + ".glStrandId")] = cmds.getAttr(shape + ".glColorR")
            cmds.delete(made["group"])
            return out

        cmds.currentTime(1)
        dry = reds()
        cmds.currentTime(6)
        half = reds()
        cmds.currentTime(11)
        wet = reds()
        self.assertEqual(set(dry), set(wet))
        self.assertGreater(len(dry), 5)
        for strand in dry:
            self.assertGreater(wet[strand], dry[strand] + 0.2)  # light, from dark
            self.assertAlmostEqual(half[strand], 0.5 * (dry[strand] + wet[strand]), places=4)
        # The Arnold hair shader must read the mixed RGB (create_hair_shader: graph stats' colorBlends).
        stats = json.loads(cmds.groomLabGraphStats(groom))
        self.assertEqual(stats["colorBlends"], ["blend"])

        # Shape only: the color stays a's, and nothing mixes colors.
        groomlab.set_param(groom, "blend", "include", "shape")
        self.assertEqual(reds(), dry)
        self.assertEqual(json.loads(cmds.groomLabGraphStats(groom))["colorBlends"], [])

    def test_scalp_and_inputs(self):
        head = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]
        other = cmds.polySphere(radius=1.2, subdivisionsX=30, subdivisionsY=15)[0]
        groom = groomlab.create_groom()
        self.assertIsNone(groomlab.scalp_mesh(groom))
        groomlab.set_scalp(groom, head)  # an empty groom: just connected
        self.assertEqual(groomlab.scalp_mesh(groom), cmds.listRelatives(head, shapes=True)[0])
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)])])
        summary = groomlab.inputs_summary(groom)
        self.assertEqual((summary["guides"], summary["groups"], summary["rest"]), (1, 1, None))
        groomlab.set_scalp(groom, other)  # with guides: transferred onto the new mesh
        self.assertEqual(groomlab.scalp_mesh(groom), cmds.listRelatives(other, shapes=True)[0])
        root = cmds.groomLabGuides(groom, guidePoints=groomlab.guide_ids(groom, "primary")[0])[:3]
        self.assertAlmostEqual(math.sqrt(sum(c * c for c in root)), 1.2, delta=0.05)
        self.assertEqual(groomlab.errors(groom), [])
        # The same mesh again, by its transform or its full path: nothing to move.
        transfers = []
        transfer = groomlab.transfer_groom
        # set_scalp finds it in its module, groomlab.transfer.
        groomlab.transfer.transfer_groom = lambda *args, **kwargs: transfers.append(args) or transfer(*args, **kwargs)
        try:
            for name in (other, cmds.ls(cmds.listRelatives(other, shapes=True)[0], long=True)[0]):
                self.assertEqual(groomlab.set_scalp(groom, name), [])
        finally:
            groomlab.transfer.transfer_groom = transfer
        self.assertEqual(transfers, [])

    def test_undo_chunk_is_one_step_however_it_ends(self):
        ball = cmds.polySphere()[0]
        translate = lambda: tuple(round(v, 6) for v in cmds.getAttr(ball + ".translate")[0])
        with groomlab.undo_chunk("two moves"):
            cmds.move(1, 0, 0, ball, relative=True)
            cmds.move(0, 2, 0, ball, relative=True)
        cmds.undo()
        self.assertEqual(translate(), (0, 0, 0))
        with self.assertRaises(ValueError):
            with groomlab.undo_chunk("fails"):
                cmds.move(3, 0, 0, ball, relative=True)
                raise ValueError("stop")
        cmds.move(0, 0, 4, ball, relative=True)  # the chunk closed anyway: this is a step of its own
        cmds.undo()
        self.assertEqual(translate(), (3, 0, 0))
        cmds.undo()
        self.assertEqual(translate(), (0, 0, 0))

    def test_a_turntable_render_is_waited_for_no_longer_than_asked(self):
        import time
        from groomlab import review
        folder = tempfile.mkdtemp(prefix="groomlab_turntable_")
        job = review.TurntableJob([sys.executable, "-c", "import time; time.sleep(30)"], folder, frames=2)
        try:
            start = time.time()
            self.assertEqual(job.wait(0.5), (0, 2, False))  # still rendering: its progress, not an error
            self.assertLess(time.time() - start, 10)
        finally:
            job.cancel()
            job.process.wait()

    def test_themes(self):
        from groomlab.ui import theme
        folder = tempfile.mkdtemp()
        os.environ["GROOMLAB_USER_THEMES"] = folder
        try:
            self.assertTrue({"playful", "studio", "polar", "charcoal"} <= set(theme.list_themes()))
            # Charcoal's file sets every key Playful's does (nothing falls back to Playful's colors),
            # and every color in every shipped theme resolves.
            def keys(data, prefix=""):
                out = set()
                for key, value in data.items():
                    out.add(prefix + key)
                    if isinstance(value, dict):
                        out |= keys(value, prefix + key + ".")
                return out

            def raw(name):
                with open(os.path.join(theme.SHIPPED_DIR, name + ".json"), encoding="utf-8") as f:
                    return json.load(f)
            self.assertEqual(keys(raw("playful")) - keys(raw("charcoal")), set())
            for name in (f[:-5] for f in os.listdir(theme.SHIPPED_DIR) if f.endswith(".json")):
                t = theme.Theme(name, theme.load(name))
                for path in ("ui.background", "ui.panel", "ui.text", "ui.accent", "graph.node", "graph.selected",
                             "viewport.selected", "viewport.parts", "icons.create"):
                    self.assertNotEqual(t.color(path), "#FF00FF", "%s: %s" % (name, path))
                for section in ("graph.categories", "graph.ports"):
                    for key in t.get(section):
                        self.assertNotEqual(t.color(section + "." + key), "#FF00FF", "%s: %s.%s" % (name, section, key))
                self.assertTrue(theme.stylesheet(t))
            charcoal = theme.Theme("charcoal", theme.load("charcoal"))
            self.assertIs(charcoal.get("ui.glass"), False)
            self.assertEqual(charcoal.get("graph.headerStyle"), "minimal")
            self.assertIs(charcoal.get("graph.shadow"), False)
            self.assertIn("solid %s" % charcoal.color("ui.outline"), theme.stylesheet(charcoal))
            polar = theme.Theme("polar", theme.load("polar"))
            self.assertEqual(polar.hex("accent"), "#3619CC")          # Ether
            playful = theme.Theme("playful", theme.load("playful"))
            self.assertEqual(playful.hex("blue"), "#2F7BF6")          # palette names resolve
            self.assertEqual(playful.hex("accent"), "#2F7BF6")        # ui.accent -> blue
            self.assertEqual(playful.color("graph.categories.Look"), "#FFC53D")
            self.assertAlmostEqual(playful.linear("#FFFFFF")[0], 1.0)
            # A partial custom theme: the rest comes from what it extends.
            with open(os.path.join(folder, "mine.json"), "w") as f:
                json.dump({"extends": "studio", "palette": {"blue": "#112233"}}, f)
            mine = theme.Theme("mine", theme.load("mine"))
            self.assertEqual(mine.hex("blue"), "#112233")
            self.assertEqual(mine.get("ui.radius"), 4)                # from studio
            # Radii: controls the theme's, small parts 2px less (never under 2), panels 2px more.
            with open(os.path.join(folder, "round.json"), "w") as f:
                json.dump({"extends": "studio", "ui": {"radius": 6}}, f)
            sheet = theme.stylesheet(theme.Theme("round", theme.load("round")))
            for radius in (6, 4, 8):
                self.assertIn("border-radius: %dpx" % theme.px(radius), sheet)
            # textMuted and rowHeight: the quietest text and one row's height where a theme sets
            # them; without them, textDim and the controls' own heights.
            dim = theme.Theme("studio", theme.load("studio")).color("ui.textDim")
            with open(os.path.join(folder, "dense.json"), "w") as f:
                json.dump({"extends": "studio", "ui": {"textMuted": "#737373", "rowHeight": 26}}, f)
            sheet = theme.stylesheet(theme.Theme("dense", theme.load("dense")))
            self.assertIn("border-radius: 0; color: #737373", sheet)  # the section headers
            self.assertIn("min-height: %dpx; padding-top: 0" % (theme.px(26) - 2 * theme.px(1)), sheet)
            with open(os.path.join(folder, "loose.json"), "w") as f:
                json.dump({"extends": "studio", "ui": {"textMuted": None, "rowHeight": None}}, f)
            sheet = theme.stylesheet(theme.Theme("loose", theme.load("loose")))
            self.assertIn("border-radius: 0; color: %s" % dim, sheet)
            self.assertNotIn("padding-top: 0; padding-bottom: 0", sheet)
            # Who never picked a theme gets Charcoal; every theme still extends Playful.
            cmds.optionVar(remove=theme.THEME_VAR)
            theme._current = None
            self.assertEqual((theme.DEFAULT, theme.BASE, theme.current().key), ("charcoal", "playful", "charcoal"))
            path = theme.create_theme("copy", base="playful")
            self.assertTrue(os.path.isfile(path))
            with self.assertRaises(ValueError):
                theme.create_theme("copy")
            with self.assertRaises(ValueError):
                theme.load("nope")
            theme.set_theme("studio")
            self.assertEqual(cmds.optionVar(query=theme.THEME_VAR), "studio")
            self.assertIn("border-radius: %dpx" % theme.px(4), theme.stylesheet())  # at Maya's UI scale
            self.assertEqual([n for n, _, active in theme.menu_entries() if active], ["studio"])
            self.assertIn(("charcoal", "Charcoal", False), theme.menu_entries())
            theme.set_theme("playful")
            # The viewport colors: the command takes linear RGB.
            cmds.groomLabDisplayTheme(frozen="1 0 0", pointSize=3.0, selected="1 1 0")
            cmds.groomLabDisplayTheme(reset=True)
        finally:
            del os.environ["GROOMLAB_USER_THEMES"]
            theme._current = None
            cmds.optionVar(remove=theme.THEME_VAR)

    def test_flow_with_guides_preset(self):
        # Fur along the flow (Blend's a) and the guides, clumped (b), blended by the guideAreas map:
        # made black on apply, so all fur, even with a guide drawn; painted white around the guide,
        # the strands there take the guide's length.
        from groomlab import presets, utils
        groom = groomlab.create_groom(cmds.polySphere(radius=10, subdivisionsX=40, subdivisionsY=20)[0])
        presets.apply_preset(groom, "flow_with_guides")
        self.assertEqual(groomlab.map_kind(groom, "guideAreas"), "vertices")
        self.assertEqual(max(cmds.groomLabScalpMaps(groom, values="guideAreas")), 0.0)
        cmds.setAttr(groom + ".previewDensity", 0.05)
        self.assertEqual(groomlab.errors(groom), [])

        def tallest():
            # The highest strand point (the groom's own box holds its guides too), a few thousand strands.
            made = utils.strands_to_curves(groom, density=0.004, max_curves=5000, attributes=False)
            try:
                return cmds.exactWorldBoundingBox(made["group"])[4]
            finally:
                cmds.delete(made["group"])

        self.assertLess(tallest(), 11.0)  # a short coat: 2.5 at 100 units, 0.5 here
        groomlab.brush_stroke(groom, "draw", [(0, 10, 0), (0, 14, 0), (0, 18, 0)], group="accents", segments=8)
        self.assertEqual(groomlab.errors(groom), [])
        self.assertLess(tallest(), 11.0)  # the map black: still the fur's length by the guide
        groomlab.brush_stroke(groom, "paint", [(0, 10, 0)], radius=3.0, strength=1.0, attribute="guideAreas", value=1.0,
                              paint_kind="vertices", hardness=1.0)
        self.assertEqual(groomlab.errors(groom), [])
        self.assertGreater(tallest(), 15.0)  # painted white: the guide's length there
        nodes = {n["name"]: n for n in groomlab.get_graph(groom)["nodes"]}
        self.assertEqual(nodes["interpolate"]["params"].get("limitDistance", 0.0), 0.0)  # every strand: the map picks
        self.assertAlmostEqual(nodes["clump"]["params"]["size"], 0.24, places=5)  # 1.2 at 100 units
        # Applied again: the painted map is kept.
        presets.apply_preset(groom, "flow_with_guides")
        self.assertGreater(max(cmds.groomLabScalpMaps(groom, values="guideAreas")), 0.99)
        self.assertGreater(tallest(), 15.0)

    def test_presets_fit_the_scalp_size(self):
        # The flow preset was made on a 100-unit body: on a 2-unit sphere its sizes shrink fiftyfold
        # and its density grows 2500-fold; it works before and after a guide.
        from groomlab import presets
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0])
        self.assertAlmostEqual(presets.scalp_size(groom), 2.0, places=3)
        presets.apply_preset(groom, "flow_with_guides")
        nodes = {n["name"]: n for n in groomlab.get_graph(groom)["nodes"]}
        self.assertAlmostEqual(nodes["clump"]["params"]["size"], 0.024, places=6)
        self.assertAlmostEqual(nodes["scatter"]["params"]["length"], 0.05, places=5)
        self.assertAlmostEqual(nodes["scatter"]["params"]["density"], 62500.0, places=0)
        self.assertAlmostEqual(nodes["width"]["params"]["width"], 0.00016, places=8)
        self.assertEqual(nodes["clump"]["params"]["strength"], 0.7)  # no units: unchanged
        self.assertNotIn("size", groomlab.get_graph(groom))
        cmds.setAttr(groom + ".previewDensity", 0.05)
        self.assertEqual(groomlab.errors(groom), [])  # no guides yet: the guideAreas map black, fur only
        flow_box = cmds.exactWorldBoundingBox(groom)
        groomlab.brush_stroke(groom, "draw", [(0, 1, 0), (0.05, 1.4, 0), (0.1, 1.8, 0)], group="accents", segments=8)
        groomlab.fill_map(groom, "guideAreas", 1.0)  # the guides everywhere
        self.assertEqual(groomlab.errors(groom), [])
        self.assertGreater(cmds.exactWorldBoundingBox(groom)[4], flow_box[4] + 0.2)
        # Without fitting, the preset's own numbers.
        presets.apply_preset(groom, "flow_with_guides", fit=False)
        nodes = {n["name"]: n for n in groomlab.get_graph(groom)["nodes"]}
        self.assertEqual(nodes["clump"]["params"]["size"], 1.2)
        # Saved presets record the scalp size.
        os.environ["GROOMLAB_USER_PRESETS"] = self.tmp if hasattr(self, "tmp") else os.path.join(
            os.environ.get("TEMP", "."), "groomlab_fit_test")
        try:
            path = presets.save_preset(groom, "fitTest", overwrite=True)
            with open(path) as f:
                self.assertAlmostEqual(json.load(f)["size"], 2.0, places=3)
        finally:
            os.environ.pop("GROOMLAB_USER_PRESETS", None)

    def test_scale_and_units(self):
        # The scalp's size is in centimeters (Maya's internal unit, the groom's) whatever the UI's
        # linear unit: presets and new nodes fit the same in m or mm.
        from groomlab import graph as gm
        from groomlab import presets
        sphere = cmds.polySphere(radius=10, subdivisionsX=24, subdivisionsY=12)[0]
        groom = groomlab.create_groom(sphere)
        self.assertIn(cmds.getAttr(groom + ".graph") or "{}", ("{}", ""), "no fit asked: the default graph as authored")
        try:
            for unit in ("m", "mm", "cm"):
                cmds.currentUnit(linear=unit)
                self.assertAlmostEqual(presets.scalp_size(groom), 20.0, places=3)
                model = gm.GraphModel.from_groom(groom)
                self.assertAlmostEqual(model.groom_size, 20.0, places=3)
                made = model.add_node("Width")
                self.assertAlmostEqual(model.param(made, "width"), 0.007, places=9)  # a head: as authored
        finally:
            cmds.currentUnit(linear="cm")
        # Scaling the scalp's transform: the groom scales with it (scaleSource Surface Area, the default):
        # as many strands on the bigger skin, and presets and new nodes fit the size the params
        # are at (the groom's 1x).
        graph = {"version": 1, "output": "output",
                 "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "scatter", "type": "Scatter", "params": {"density": 4.0}},
                           {"name": "output", "type": "Output"}],
                 "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                                 {"from": "scatter", "to": "output", "input": "strands"}]}
        groomlab.set_graph(groom, graph)
        cmds.setAttr(groom + ".previewDensity", 1.0)
        area = cmds.polyEvaluate(sphere, worldArea=True)
        strands = cmds.getAttr(groom + ".outStrandCount")
        self.assertAlmostEqual(strands / area, 4.0, delta=0.1)
        cmds.setAttr(sphere + ".scale", 3, 3, 3)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 3.0, places=3)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), strands)
        self.assertAlmostEqual(presets.scalp_size(groom), 20.0, places=2)
        # Off: the hair grows on the scaled skin at the same density per unit area seen (world
        # space), and the fitted size follows the scalp.
        groomlab.set_scale_with_scalp(groom, False)
        self.assertEqual(cmds.getAttr(groom + ".groomScale"), 1.0)
        self.assertAlmostEqual(cmds.getAttr(groom + ".outStrandCount") / cmds.polyEvaluate(sphere, worldArea=True), 4.0, delta=0.1)
        self.assertAlmostEqual(presets.scalp_size(groom), 60.0, places=2)
        # Create Groom fits the default graph to the scalp: a 2 m creature's strands 10x as long.
        big = cmds.polySphere(radius=100, subdivisionsX=24, subdivisionsY=12)[0]
        fitted = groomlab.create_groom(big, fit=True)
        nodes = {n["name"]: n for n in json.loads(cmds.getAttr(fitted + ".graph"))["nodes"]}
        self.assertAlmostEqual(nodes["scatter"]["params"]["length"], 10.0, places=4)
        self.assertAlmostEqual(nodes["scatter"]["params"]["density"], 0.8, places=4)
        self.assertAlmostEqual(nodes["width"]["params"]["width"], 0.07, places=6)

    def test_parts_on_a_guideless_graph(self):
        # Creature fur has no Interpolate: the Part tool still gets a Part node, after Flow.
        from groomlab import presets
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0])
        cmds.setAttr(groom + ".previewDensity", 0.01)  # made on a wolf-sized body: dense on this sphere
        presets.apply_preset(groom, "creature_fur")
        node = groomlab.add_part_node(groom, if_missing=True)
        graph = groomlab.get_graph(groom)
        self.assertIn({"from": "flow", "to": node, "input": "strands"}, graph["connections"])
        self.assertEqual(groomlab.errors(groom), [])



class ScaleWithScalpTest(unittest.TestCase):
    """Scale with the scalp (scaleSource): the groom grows with its scalp like a child object.
    The scalp's world surface area at the start frame against the area it was groomed at gives the
    scale k; the result is the groom at 1x scaled by k about the scalp's pivot, as many strands."""

    GRAPH = {"version": 1, "output": "output",
             "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
                       {"name": "scatter", "type": "Scatter", "params": {"density": 3.0, "length": 2.0}},
                       {"name": "interpolate", "type": "Interpolate"},
                       {"name": "clump", "type": "Clump", "params": {"size": 0.8}},
                       {"name": "width", "type": "Width", "params": {"width": 0.02}},
                       {"name": "output", "type": "Output"}],
             "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                             {"from": "scatter", "to": "interpolate", "input": "strands"},
                             {"from": "guides", "to": "interpolate", "input": "guides"},
                             {"from": "scalp", "to": "interpolate", "input": "scalp"},
                             {"from": "interpolate", "to": "clump", "input": "strands"},
                             {"from": "clump", "to": "width", "input": "strands"},
                             {"from": "width", "to": "output", "input": "strands"}]}

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentTime(1)

    def make(self, scalp):
        """A groom on `scalp` (at the origin) with two guides and GRAPH, every strand shown."""
        groom = groomlab.create_groom(scalp)
        curves = [cmds.curve(point=[(0, 4.9, 0), (0.3, 6.5, 0.2), (0.8, 8.0, 0.5)], degree=1),
                  cmds.curve(point=[(4.9, 0, 0), (6.5, 0.3, 0.1), (8.0, -0.5, 0.4)], degree=1)]
        groomlab.guides_from_curves(groom, curves)
        cmds.delete(curves)
        groomlab.set_graph(groom, self.GRAPH)
        cmds.setAttr(groom + ".previewDensity", 1.0)
        return groom

    def sphere(self, name="head"):
        return cmds.polySphere(radius=5, subdivisionsX=24, subdivisionsY=12, name=name)[0]

    def assertScaledBy(self, groom, base, k, what):
        from groomlab.exports import viewport_points
        now = viewport_points(groom)
        self.assertEqual(len(now), len(base), what + ": as many strand points")
        worst = max(_dist(q, tuple(c * k for c in p)) for p, q in zip(base, now))
        self.assertLess(worst, 1e-3 * 10 * k, what + ": the 1x groom scaled by %g (off by %g)" % (k, worst))

    def test_a_scaled_transform_scales_the_groom_and_freezing_it_does_not_pop(self):
        from groomlab.exports import viewport_points
        head = self.sphere()
        groom = self.make(head)
        base = viewport_points(groom)
        guides = guide_points(groom)
        stored = cmds.getAttr(groom + ".guideData")
        strands = cmds.getAttr(groom + ".outStrandCount")
        self.assertGreater(strands, 100)
        self.assertEqual(cmds.getAttr(groom + ".groomScale"), 1.0)
        for k in (0.5, 2.0):
            cmds.setAttr(head + ".scale", k, k, k)
            self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), k, places=3)
            self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), strands)
            self.assertScaledBy(groom, base, k, "transform x%g" % k)
            # The guides as the tools see them are scaled too; what is stored stays.
            for gid, points in guide_points(groom).items():
                self.assertLess(max(_dist(q, tuple(c * k for c in p)) for p, q in zip(guides[gid], points)), 1e-3)
            self.assertEqual(cmds.getAttr(groom + ".guideData"), stored)
        # Freeze transforms: the scale goes into the points, the transform back to 1: no pop.
        frozen = viewport_points(groom)
        cmds.makeIdentity(head, apply=True, scale=True)
        self.assertEqual(cmds.getAttr(head + ".scale")[0], (1.0, 1.0, 1.0))
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        self.assertLess(max(_dist(p, q) for p, q in zip(frozen, viewport_points(groom))), 1e-3, "frozen: the same groom")
        # Set Scale Reference: the size now is 1x (the params' sizes in scene units again).
        groomlab.set_scale_reference(groom)
        self.assertEqual(cmds.getAttr(groom + ".groomScale"), 1.0)
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 3 * strands, "the same density on 4x the skin")
        # Off: today's absolute sizes whatever the scalp's size.
        cmds.setAttr(groom + ".scaleReference", 0.0)
        groomlab.set_scale_with_scalp(groom, False)
        self.assertEqual(cmds.getAttr(groom + ".groomScale"), 1.0)

    def test_a_guide_added_on_the_scaled_groom_is_stored_at_1x(self):
        head = self.sphere()
        groom = self.make(head)
        cmds.setAttr(head + ".scale", 2, 2, 2)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        curve = cmds.curve(point=[(0, -9.9, 0), (0.2, -12.0, 0.3), (0.4, -14.0, 0.6)], degree=1)
        added = groomlab.guides_from_curves(groom, [curve])
        cmds.delete(curve)
        length = lambda pts: sum(_dist(a, b) for a, b in zip(pts, pts[1:]))
        at2 = length(guide_points(groom)[str(added[0])])
        self.assertAlmostEqual(at2, 4.13, delta=0.1, msg="shown as drawn")
        cmds.setAttr(head + ".scale", 1, 1, 1)
        self.assertAlmostEqual(length(guide_points(groom)[str(added[0])]), at2 / 2.0, delta=0.01)

    def test_another_resolution_of_the_same_surface_keeps_1x(self):
        coarse = cmds.polyPlane(width=10, height=10, subdivisionsX=4, subdivisionsY=4)[0]
        fine = cmds.polyPlane(width=10, height=10, subdivisionsX=20, subdivisionsY=20)[0]
        groom = groomlab.create_groom(coarse)
        cmds.getAttr(groom + ".outStrandCount")
        cmds.connectAttr(fine + ".worldMesh[0]", groom + ".inMesh", force=True)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4)

    def test_a_squash_deformer_keeps_one_scale_on_every_frame(self):
        head = self.sphere()
        squash, _ = cmds.nonLinear(head, type="squash")
        cmds.setKeyframe(squash, attribute="factor", time=1, value=0.0)
        cmds.setKeyframe(squash, attribute="factor", time=10, value=-0.6)
        groom = self.make(head)
        self.assertTrue(groomlab.rest_mesh(groom), "a deformed scalp gets its rest mesh")
        strands = cmds.getAttr(groom + ".outStrandCount")
        for frame in (1, 4, 10, 6, 1):
            cmds.currentTime(frame)
            self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4, msg="frame %d" % frame)
            self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), strands, "frame %d" % frame)

    def test_a_rig_scaled_at_its_root_scales_the_groom(self):
        # A skinned scalp: the rig's root joint scales the bound mesh, its original shape (the rest
        # mesh) stays as it was. Measured on the bound mesh at the start frame, the groom scales.
        from groomlab.exports import viewport_points
        head = self.sphere()
        cmds.select(clear=True)
        root = cmds.joint(position=(0, 0, 0), name="root")
        cmds.skinCluster(root, head, toSelectedBones=False)
        groom = self.make(head)
        self.assertTrue(groomlab.rest_mesh(groom))
        base = viewport_points(groom)
        strands = cmds.getAttr(groom + ".outStrandCount")
        cmds.setAttr(root + ".scale", 2, 2, 2)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), strands)
        self.assertScaledBy(groom, base, 2.0, "root joint x2")
        # Scaled at another frame (the time standing): measured at the start frame all the same.
        cmds.currentTime(5)
        cmds.setAttr(root + ".scale", 1.5, 1.5, 1.5)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.5, places=3)
        self.assertScaledBy(groom, base, 1.5, "root joint x1.5 at frame 5")

    def test_a_save_stores_the_reference_and_a_groom_file_keeps_it(self):
        head = self.sphere()
        groom = self.make(head)
        cmds.getAttr(groom + ".outStrandCount")
        self.assertEqual(cmds.getAttr(groom + ".scaleReference"), 0.0, "the first size measured is 1x")
        cmds.setAttr(head + ".scale", 2, 2, 2)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        folder = tempfile.mkdtemp(prefix="groomlab_scale_")
        path = os.path.join(folder, "scaled.ma").replace("\\", "/")
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)  # the plugin's before-save stores the reference
        area = cmds.polyEvaluate(head, worldArea=True) / 4.0
        self.assertAlmostEqual(cmds.getAttr(groom + ".scaleReference") / area, 1.0, delta=0.01)
        cmds.file(path, open=True, force=True)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        groom_path = os.path.join(folder, "scaled.groom").replace("\\", "/")
        groomlab.export_groom(groom, groom_path)
        with open(groom_path) as f:
            data = json.load(f)
        self.assertAlmostEqual(data["scaleReference"] / area, 1.0, delta=0.01)
        self.assertNotIn("scaleSource", data, "written only when not area")

    def test_transform_only_follows_the_transform_and_leaves_a_remodel(self):
        head = self.sphere()
        groom = self.make(head)
        groomlab.set_scale_source(groom, "transform")
        self.assertEqual(groomlab.scale_source(groom), "transform")
        self.assertEqual(cmds.getAttr(groom + ".groomScale"), 1.0)
        # A remodel: the points 20% more area (the transform as it was): Transform Only leaves the groom...
        grow = math.sqrt(1.2)
        cmds.scale(grow, grow, grow, head + ".vtx[*]", pivot=(0, 0, 0))
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4)
        # ...Surface Area follows it (and says so once).
        groomlab.set_scale_source(groom, "area")
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), grow, places=3)
        groomlab.set_scale_source(groom, "transform")
        # The transform scaled 2x: k 2. Frozen: the transform's scale is 1 again, so is k (Transform
        # Only does not survive Freeze Transformations; Surface Area does).
        cmds.setAttr(head + ".scale", 2, 2, 2)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        cmds.makeIdentity(head, apply=True, scale=True)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=3)
        groomlab.set_scale_source(groom, "area")
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0 * grow, places=3)
        # Set Scale Reference takes both: 1x at the size and the transform now.
        groomlab.set_scale_reference(groom)
        groomlab.set_scale_source(groom, "transform")
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4)
        groomlab.set_scale_source(groom, "off")
        self.assertEqual(cmds.getAttr(groom + ".groomScale"), 1.0)

    def test_transform_only_follows_a_rigs_root_joint(self):
        head = self.sphere()
        cmds.select(clear=True)
        root = cmds.joint(position=(0, 0, 0), name="root")
        cmds.skinCluster(root, head, toSelectedBones=False)
        groom = self.make(head)
        groomlab.set_scale_source(groom, "transform")
        cmds.getAttr(groom + ".outStrandCount")
        cmds.setAttr(root + ".scale", 2, 2, 2)
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)

    def test_a_transfer_fitting_the_size_matches_the_groom_scaled(self):
        from groomlab.exports import viewport_points
        # The groom on a 1x head moved onto a copy twice the size (frozen; by UV: the same place on
        # it): fitted, it is the groom scaled 2x; not, it keeps its sizes.
        head = self.sphere()
        groom = self.make(head)
        base = viewport_points(groom)
        strands = cmds.getAttr(groom + ".outStrandCount")
        big = cmds.duplicate(head, name="bigHead")[0]
        cmds.setAttr(big + ".scale", 2, 2, 2)
        cmds.makeIdentity(big, apply=True, scale=True)
        groomlab.transfer_groom(groom, big, fit_size=True, maps=False, method="uv")
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 2.0, places=3)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), strands)
        self.assertScaledBy(groom, base, 2.0, "fitted to a head twice the size")
        cmds.undo()
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4)
        groomlab.transfer_groom(groom, big, maps=False, method="uv")
        self.assertAlmostEqual(cmds.getAttr(groom + ".groomScale"), 1.0, places=4)
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 3 * strands, "its sizes kept: 4x the skin")


class AssetFilesTest(unittest.TestCase):
    """The files GroomLab writes for grooms (texture maps, bakes, exports): named per groom and
    per scene, never shared by accident between grooms or scenes, found again through
    references."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.home = tempfile.mkdtemp(prefix="groomlab_files_").replace("\\", "/")
        cmds.workspace(self.home, openWorkspace=True)  # untitled scenes keep their files under it
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]

    def tearDown(self):
        cmds.namespace(set=":")

    def save_as(self, *parts):
        path = os.path.join(self.home, *parts).replace("\\", "/")
        if not os.path.isdir(os.path.dirname(path)):
            os.makedirs(os.path.dirname(path))
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        return path

    def groom_under(self, parent, name="hair", scalp=None):
        """A groom named `name` under a transform `parent` (None: the world); its shape's full path."""
        shape = groomlab.create_groom(scalp or self.scalp, name=name)
        transform = cmds.listRelatives(shape, parent=True, fullPath=True)[0]
        if parent:
            if not cmds.objExists("|" + parent):
                cmds.group(empty=True, name=parent, world=True)
            transform = cmds.parent(transform, "|" + parent)[0]
            transform = cmds.ls(transform, long=True)[0]
            if transform.rsplit("|", 1)[-1] != name:
                transform = cmds.ls(cmds.rename(transform, name), long=True)[0]
        return cmds.listRelatives(transform, shapes=True, fullPath=True, type=groomlab.GROOM_TYPE)[0]

    def density_map(self, groom, value=1.0):
        """A texture map "density" feeding the scatter's density; returns its file."""
        groomlab.add_map(groom, "density", value, kind="texture", resolution=32)
        groomlab.use_map(groom, "scatter", "density", "density")
        return groomlab.map_texture(groom, "density")

    def count(self, groom):
        return cmds.getAttr(groom + ".outStrandCount")

    def test_same_short_name_under_two_parents_keeps_files_apart(self):
        a = self.groom_under("charA")
        b = self.groom_under("charB")
        self.assertEqual(cmds.listRelatives(a, parent=True, fullPath=True)[0], "|charA|hair")
        self.assertEqual(groomlab.asset_name(a), "charA_hair")
        self.assertEqual(groomlab.asset_name(b), "charB_hair")
        file_a, file_b = self.density_map(a, 1.0), self.density_map(b, 1.0)
        self.assertTrue(file_a.endswith("groomlab/untitled/masks/charA_hair/density.png"), file_a)
        self.assertTrue(file_b.endswith("groomlab/untitled/masks/charB_hair/density.png"), file_b)
        before_a, count_a = md5(file_a), self.count(a)
        groomlab.fill_map(b, "density", 0.1)
        self.assertEqual(md5(file_a), before_a)
        self.assertEqual(self.count(a), count_a)
        self.assertLess(self.count(b), count_a)
        # Card bakes: a folder per groom, named after it; neither touches the other's.
        for g in (a, b):
            groomlab.create_cards(g, max_cards=8)
        paths_a = groomlab.bake_cards(a, resolution=32)
        color_a = next(p for p in paths_a if p.endswith("_Color.tga"))
        self.assertTrue(color_a.endswith("cards/charA_hair/charA_hair_Color.tga"), color_a)
        baked_a = md5(color_a)
        paths_b = groomlab.bake_cards(b, resolution=32)
        self.assertTrue(all("/cards/charB_hair/charB_hair_" in p for p in paths_b), paths_b)
        self.assertEqual(md5(color_a), baked_a)
        self.assertNotEqual(cmds.getAttr(a + ".cardMaps").split(";")[0], cmds.getAttr(b + ".cardMaps").split(";")[0])
        # A default name elsewhere: the export dialogs start from the asset name too.
        from groomlab.ui import menu
        self.assertTrue(menu.default_file(a, "exports", ".abc").endswith("/exports/charA_hair.abc"))
        # A default bake never writes over a file the groom doesn't own: a groom made later
        # under the same name (the first one deleted) takes a free name beside the old files.
        cmds.delete(cmds.listRelatives(b, parent=True, fullPath=True))
        c = self.groom_under("charB")
        groomlab.create_cards(c, max_cards=8)
        paths_c = groomlab.bake_cards(c, resolution=32)
        self.assertTrue(all("/cards/charB_hair/charB_hair_2_" in p for p in paths_c), paths_c)
        self.assertEqual(groomlab.add_map(c, "density", 0.5, kind="texture", resolution=32), "density")
        self.assertTrue(groomlab.map_texture(c, "density").endswith("masks/charB_hair/density_2.png"))

    def test_namespaced_groom_writes_no_colons(self):
        cmds.namespace(add="charA")
        cmds.namespace(set="charA")
        try:
            groom = groomlab.create_groom(self.scalp, name="hair")
        finally:
            cmds.namespace(set=":")
        groom = cmds.ls(groom, long=True)[0]
        self.assertEqual(groom, "|charA:hair|charA:hairShape")  # the namespace once
        self.assertEqual(groomlab.asset_name(groom), "charA_hair")
        written = [self.density_map(groom)]
        groomlab.fill_map(groom, "density", 0.8)
        groomlab.create_cards(groom, max_cards=8)
        written += groomlab.bake_cards(groom, resolution=32)
        written += groomlab.bake_scalp(groom, resolution=32)
        result = groomlab.export_cards_unreal(groom, resolution=32)
        written += [result["fbx"], result["manifest"], result["scalpCap"]] + list(result["maps"])
        self.assertTrue(result["fbx"].endswith("/exports/charA_hair_cards.fbx"), result["fbx"])
        from groomlab.ui import menu
        for ext, export in ((".abc", groomlab.export_alembic), (".usda", groomlab.export_usd)):
            path = menu.default_file(groom, "exports", ext)
            export(groom, path, start=1, end=1)
            written.append(path)
        written += groomlab.export_groom(groom, menu.default_file(groom, "exports", ".groom"))
        for path in written:
            self.assertNotIn(":", os.path.splitdrive(path)[1], path)
            self.assertNotIn("|", path)
            self.assertTrue(os.path.isfile(path) and os.path.getsize(path) > 0, path)
        self.assertEqual(groomlab.errors(groom), [])

    def test_a_duplicate_forks_the_file_it_shares_before_writing(self):
        groom = self.groom_under(None)
        original = self.density_map(groom, 1.0)
        count = self.count(groom)
        copy = cmds.duplicate(cmds.listRelatives(groom, parent=True, fullPath=True)[0])[0]
        copy = cmds.listRelatives(copy, shapes=True, fullPath=True, type=groomlab.GROOM_TYPE)[0]
        cmds.connectAttr(self.scalp + ".worldMesh[0]", copy + ".inMesh")
        cmds.connectAttr("time1.outTime", copy + ".time")
        self.assertEqual(groomlab.map_texture(copy, "density"), original)  # duplicated: the same file
        before = md5(original)
        # A fill on the copy: the copy gets its own file first (one undo step with the fill).
        groomlab.fill_map(copy, "density", 0.1)
        forked = groomlab.map_texture(copy, "density")
        self.assertNotEqual(forked, original)
        self.assertTrue(forked.endswith("masks/%s/density.png" % groomlab.asset_name(copy)), forked)
        self.assertEqual(md5(original), before)
        self.assertEqual(self.count(groom), count)
        self.assertLess(self.count(copy), count)
        cmds.undo()
        self.assertEqual(groomlab.map_texture(copy, "density"), original)
        self.assertEqual(md5(original), before)
        # A paint stroke on the copy forks the same way (and undoes back to the shared file).
        groomlab.brush_stroke(copy, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="density",
                              value=0.0, paint_kind="texture", paint_texture=original)
        stroked = groomlab.map_texture(copy, "density")
        self.assertNotEqual(stroked, original)
        self.assertEqual(md5(original), before)
        self.assertEqual(self.count(groom), count)
        cmds.undo()
        self.assertEqual(groomlab.map_texture(copy, "density"), original)
        # Renaming the shared map on the copy copies the file: the original keeps its own.
        groomlab.rename_map(copy, "density", "thick")
        renamed = groomlab.map_texture(copy, "thick")
        self.assertTrue(renamed.endswith("/thick.png"), renamed)
        self.assertTrue(os.path.exists(original) and os.path.exists(renamed))
        self.assertEqual(md5(original), before)
        self.assertEqual(groomlab.errors(groom), [])
        cmds.undo()
        self.assertFalse(os.path.exists(renamed))
        self.assertTrue(os.path.exists(original))

    def test_removing_a_texture_map_can_delete_its_file(self):
        groom = self.groom_under(None)
        made = self.density_map(groom, 1.0)
        before = md5(made)
        groomlab.remove_map(groom, "density")  # by default the file stays
        self.assertTrue(os.path.exists(made))
        cmds.undo()
        groomlab.remove_map(groom, "density", delete_file=True)
        self.assertFalse(os.path.exists(made))
        cmds.undo()  # the map and its file back, the same bytes
        self.assertEqual(groomlab.map_texture(groom, "density"), made)
        self.assertEqual(md5(made), before)
        cmds.redo()
        self.assertFalse(os.path.exists(made))
        cmds.undo()
        # A file another groom reads stays.
        copy = cmds.duplicate(cmds.listRelatives(groom, parent=True, fullPath=True)[0])[0]
        copy = cmds.listRelatives(copy, shapes=True, fullPath=True, type=groomlab.GROOM_TYPE)[0]
        groomlab.remove_map(copy, "density", delete_file=True)
        self.assertTrue(os.path.exists(made))
        # A PNG of the user's linked in place (outside the masks folder) stays.
        mine = os.path.join(self.home, "mine.png").replace("\\", "/")
        shutil.copyfile(made, mine)
        groomlab.add_map(groom, "own", kind="vertices")
        groomlab.set_map_texture(groom, "own", mine)
        groomlab.remove_map(groom, "own", delete_file=True)
        self.assertTrue(os.path.exists(mine))

    def test_paint_goes_to_the_painted_grooms_own_file(self):
        # The tool's -paintTexture still names groom A's file (set while A was active); the
        # stroke is on B: B's own map file changes, A's does not.
        a = self.groom_under("charA")
        b = self.groom_under("charB")
        file_a, file_b = self.density_map(a, 1.0), self.density_map(b, 1.0)
        before_a, before_b = md5(file_a), md5(file_b)
        groomlab.brush_stroke(b, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute="density",
                              value=0.0, paint_kind="texture", paint_texture=file_a)
        self.assertEqual(md5(file_a), before_a)
        self.assertNotEqual(md5(file_b), before_b)
        self.assertEqual(groomlab.map_texture(b, "density"), file_b)
        # A groom without that texture map refuses the stroke instead of painting another's file.
        c = self.groom_under("charC")
        with self.assertRaises(RuntimeError):
            groomlab.brush_stroke(c, "paint", [(0.0, 1.0, 0.0)], radius=1.5, attribute="density", value=0.0,
                                  paint_kind="texture", paint_texture=file_a)
        self.assertEqual(md5(file_a), before_a)

    def test_scenes_in_one_folder_keep_their_own_files(self):
        groom = self.groom_under(None)
        untitled = self.density_map(groom, 0.5)
        self.assertTrue(untitled.startswith(self.home + "/groomlab/untitled/masks/"), untitled)
        v1 = self.save_as("shots", "shot_v001.ma")  # the first save: into the scene's own folder
        file_v1 = groomlab.map_texture(groom, "density")
        self.assertEqual(file_v1, self.home + "/shots/groomlab/shot_v001/masks/hair/density.png")
        self.assertEqual(groomlab.maps(groom)[0]["texture"], "groomlab/shot_v001/masks/hair/density.png")
        groomlab.create_cards(groom, max_cards=8)
        mesh = groomlab.create_card_mesh(groom)
        groomlab.bake_cards(groom, resolution=32)
        self.assertIn("Color=groomlab/shot_v001/cards/hair/hair_Color.tga", cmds.getAttr(groom + ".cardMaps"))
        textures = cmds.ls(type="file")
        self.assertTrue(textures)
        cmds.file(save=True, type="mayaAscii", force=True)
        # Saved as v002 beside it: v002 gets copies in its own folder; editing it leaves v001's alone.
        self.save_as("shots", "shot_v002.ma")
        file_v2 = groomlab.map_texture(groom, "density")
        self.assertEqual(file_v2, self.home + "/shots/groomlab/shot_v002/masks/hair/density.png")
        self.assertIn("Color=groomlab/shot_v002/cards/hair/hair_Color.tga", cmds.getAttr(groom + ".cardMaps"))
        self.assertTrue(groomlab.card_stats(groom)["baked"])
        for texture in textures:  # the card mesh's material follows the bake
            path = cmds.getAttr(texture + ".fileTextureName")
            self.assertIn("/shots/groomlab/shot_v002/cards/hair/", path)
            self.assertTrue(os.path.exists(path), path)
        self.assertEqual(groomlab.card_mesh(groom), mesh)
        before = md5(file_v1)
        groomlab.fill_map(groom, "density", 1.0)
        self.assertEqual(md5(file_v1), before)
        self.assertNotEqual(md5(file_v2), before)
        # v001 still opens with its own files.
        cmds.file(v1, open=True, force=True)
        groom = cmds.ls(type=groomlab.GROOM_TYPE, long=True)[0]
        self.assertEqual(groomlab.map_texture(groom, "density"), file_v1)
        self.assertEqual(groomlab.errors(groom), [])
        # Another scene in the folder, its groom of the same name: its own files too.
        cmds.file(new=True, force=True)
        self.save_as("shots", "other.ma")
        other = self.groom_under(None, scalp=cmds.polySphere()[0])
        self.assertEqual(self.density_map(other), self.home + "/shots/groomlab/other/masks/hair/density.png")

    def test_save_as_never_adopts_another_scenes_file(self):
        groom = self.groom_under(None)
        self.save_as("a", "hero.ma")
        self.density_map(groom, 0.5)
        # Another hero.ma's files already sit where this one's would go.
        squatter = self.home + "/b/groomlab/hero/masks/hair/density.png"
        os.makedirs(os.path.dirname(squatter))
        with open(squatter, "wb") as f:
            f.write(b"another scene's mask")
        self.save_as("b", "hero.ma")
        stored = groomlab.maps(groom)[0]["texture"]
        self.assertEqual(stored, "groomlab/hero/masks/hair/density_2.png")
        with open(squatter, "rb") as f:
            self.assertEqual(f.read(), b"another scene's mask")
        self.assertEqual(md5(groomlab.map_texture(groom, "density")), md5(self.home + "/a/groomlab/hero/masks/hair/density.png"))
        self.assertEqual(groomlab.errors(groom), [])

    def test_a_referenced_groom_finds_its_files_beside_its_own_scene(self):
        groom = self.groom_under(None)
        self.save_as("assets", "rig.ma")
        self.density_map(groom, 1.0)
        count = self.count(groom)
        cmds.file(save=True, type="mayaAscii", force=True)
        rig = cmds.file(query=True, sceneName=True)
        mask = self.home + "/assets/groomlab/rig/masks/hair/density.png"
        cmds.file(new=True, force=True)
        self.save_as("shots", "host.ma")
        cmds.file(rig, reference=True, namespace="r1")
        cmds.file(rig, reference=True, namespace="r2")
        grooms = sorted(cmds.ls(type=groomlab.GROOM_TYPE, long=True))
        self.assertEqual(len(grooms), 2)
        for g in grooms:
            self.assertEqual(groomlab.map_texture(g, "density"), mask)
            self.assertEqual(groomlab.errors(g), [])
            self.assertEqual(self.count(g), count)
        # The two references share the file: a fill on one forks it into the host scene's folder.
        before = md5(mask)
        groomlab.fill_map(grooms[0], "density", 0.1)
        forked = groomlab.map_texture(grooms[0], "density")
        self.assertTrue(forked.startswith(self.home + "/shots/groomlab/host/masks/r1_hair/"), forked)
        self.assertEqual(md5(mask), before)
        self.assertEqual(self.count(grooms[1]), count)
        self.assertLess(self.count(grooms[0]), count)

    def test_unreal_export_leaves_the_grooms_bake_alone(self):
        groom = self.groom_under(None)
        self.save_as("game", "hero.ma")
        groomlab.create_cards(groom, max_cards=8)
        mesh = groomlab.create_card_mesh(groom)
        groomlab.bake_cards(groom, resolution=32)
        maps = cmds.getAttr(groom + ".cardMaps")
        files = {n: cmds.getAttr(n + ".fileTextureName") for n in cmds.ls(type="file")}
        self.assertTrue(files)
        low = groomlab.export_cards_unreal(groom, resolution=32, lod="low")
        hero = groomlab.export_cards_unreal(groom, resolution=32, lod="hero")
        self.assertTrue(low["fbx"].endswith("/game/groomlab/hero/exports/hair_low_cards.fbx"), low["fbx"])
        self.assertTrue(hero["fbx"].endswith("/exports/hair_hero_cards.fbx"), hero["fbx"])
        self.assertTrue(os.path.exists(low["fbx"]) and os.path.exists(hero["fbx"]))
        self.assertEqual(cmds.getAttr(groom + ".cardMaps"), maps)
        self.assertEqual({n: cmds.getAttr(n + ".fileTextureName") for n in cmds.ls(type="file")}, files)
        self.assertEqual(groomlab.card_mesh(groom), mesh)


def write_image(path, gray=None, color=None, size=16):
    """A flat image of `size` square in the format its extension names (PNG, JPEG...): `gray`
    (0-255) or `color` (r, g, b)."""
    from PySide2 import QtGui
    image = QtGui.QImage(size, size, QtGui.QImage.Format_RGB32)
    image.fill(QtGui.QColor(*(color or (gray, gray, gray))))
    folder = os.path.dirname(path)
    if folder and not os.path.isdir(folder):
        os.makedirs(folder)
    assert image.save(path, quality=100) if path.lower().endswith((".jpg", ".jpeg")) else image.save(path), path
    return path.replace("\\", "/")


class MapImagesTest(unittest.TestCase):
    """A map's image: relinked to another file (a PNG in place, any other format as a PNG copy),
    read again after an edit outside Maya, and found again when it went missing."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.home = tempfile.mkdtemp(prefix="groomlab_relink_").replace("\\", "/")
        cmds.workspace(self.home, openWorkspace=True)  # the untitled scene's files go under it
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]
        self.groom = groomlab.create_groom(self.scalp)
        cmds.setAttr(self.groom + ".previewDensity", 0.5)

    def count(self):
        return cmds.getAttr(self.groom + ".outStrandCount")

    def density_map(self, value=0.0, name="density"):
        """A texture map feeding the scatter's density, `value` everywhere; its file."""
        groomlab.add_map(self.groom, name, value, kind="texture", resolution=32)
        groomlab.use_map(self.groom, "scatter", "density", name)
        return groomlab.map_texture(self.groom, name)

    def stroke(self, name, value):
        groomlab.brush_stroke(self.groom, "paint", [(0.0, 1.0, 0.0)], radius=1.5, strength=1.0, attribute=name,
                              value=value, paint_kind="texture")

    def test_a_png_is_linked_in_place_and_painted_there(self):
        original = self.density_map(0.0)
        self.assertEqual(self.count(), 0)
        png = write_image(self.home + "/textures/white.png", gray=255)
        stored = groomlab.set_map_texture(self.groom, "density", png)
        self.assertEqual(stored, "textures/white.png")  # stored relative, as GroomLab stores its files
        self.assertEqual(groomlab.map_texture(self.groom, "density"), png)
        self.assertEqual(groomlab.map_kind(self.groom, "density"), "texture")
        self.assertGreater(self.count(), 0)  # the Map node follows by name
        before = md5(png)
        self.stroke("density", 0.0)
        self.assertNotEqual(md5(png), before)  # painted in place
        cmds.undo()
        self.assertEqual(md5(png), before)
        cmds.undo()  # the relink
        self.assertEqual(groomlab.map_texture(self.groom, "density"), original)
        self.assertEqual(self.count(), 0)
        cmds.redo()
        self.assertEqual(groomlab.map_texture(self.groom, "density"), png)
        with self.assertRaises(ValueError):
            groomlab.set_map_texture(self.groom, "density", self.home + "/textures/nothing.png")
        with self.assertRaises(ValueError):
            groomlab.set_map_texture(self.groom, "nope", png)
        with self.assertRaises(ValueError):  # a texture map's own file is a PNG
            groomlab.add_map(self.groom, "other", kind="texture", texture=self.home + "/textures/other.jpg")

    def test_other_formats_are_copied_as_png_and_the_source_never_painted(self):
        original = self.density_map(0.0)
        jpeg = write_image(self.home + "/photos/white.jpg", gray=255)
        before = md5(jpeg)
        with self.assertRaises(ValueError):
            groomlab.set_map_texture(self.groom, "density", jpeg, copy=False)
        stored = groomlab.set_map_texture(self.groom, "density", jpeg)
        asset = groomlab.asset_name(self.groom)
        self.assertEqual(stored, "groomlab/untitled/masks/%s/density_2.png" % asset)  # density.png is taken
        copy = groomlab.map_texture(self.groom, "density")
        with open(copy, "rb") as f:
            width, height, kind = png_info(f.read())
        self.assertEqual((width, height, kind), (16, 16, 0))  # a gray image: a gray PNG
        self.assertGreater(self.count(), 0)
        self.stroke("density", 0.0)
        self.assertEqual(md5(jpeg), before)  # the source is never painted
        self.assertTrue(os.path.exists(original))
        cmds.undo()
        cmds.undo()  # the relink: the copy goes, the map reads its old file
        self.assertFalse(os.path.exists(copy))
        self.assertEqual(groomlab.map_texture(self.groom, "density"), original)
        cmds.redo()
        self.assertTrue(os.path.exists(copy))
        self.assertEqual(groomlab.map_texture(self.groom, "density"), copy)
        # A PNG copied on request; a map linked by hand to another format refuses to be painted.
        png = write_image(self.home + "/textures/gray.png", gray=128)
        copied = groomlab.set_map_texture(self.groom, "density", png, copy=True)
        self.assertEqual(copied, "groomlab/untitled/masks/%s/density_3.png" % asset)
        self.assertEqual(md5(groomlab.map_texture(self.groom, "density")), md5(png))
        data = json.loads(cmds.getAttr(self.groom + ".scalpMaps"))
        data["density"]["texture"] = jpeg
        cmds.setAttr(self.groom + ".scalpMaps", json.dumps(data), type="string")
        with self.assertRaises(RuntimeError):
            self.stroke("density", 1.0)
        with self.assertRaises(RuntimeError):
            groomlab.fill_map(self.groom, "density", 1.0)
        self.assertEqual(md5(jpeg), before)

    def test_guide_and_vertex_maps_become_texture_maps_keeping_their_channels(self):
        cmds.setAttr(self.groom + ".previewDensity", 0.5)
        curve = cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.5, 0.0)])
        groomlab.guides_from_curves(self.groom, [curve])
        groomlab.add_map(self.groom, "grow", 0.0, kind="guides")
        groomlab.use_map(self.groom, "scatter", "density", "grow")
        self.assertEqual(self.count(), 0)
        white = write_image(self.home + "/textures/white.png", gray=255)
        groomlab.set_map_texture(self.groom, "grow", white)
        self.assertEqual(groomlab.map_kind(self.groom, "grow"), "texture")
        self.assertNotIn("grow", cmds.groomLabGuides(self.groom, listAttributes=True) or [])  # the layer goes
        self.assertEqual(groomlab.map_channels(self.groom, "grow"), 1)
        self.assertGreater(self.count(), 0)  # its Map node reads the image now
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.undo()
        self.assertEqual(groomlab.map_kind(self.groom, "grow"), "guides")
        self.assertEqual(self.count(), 0)
        # A color vertex map stays color; a gray map reading a color image reads its luminance.
        groomlab.add_map(self.groom, "tint", kind="vertices", color=(0.0, 0.0, 0.0, 1.0))
        red = write_image(self.home + "/textures/red.png", color=(255, 0, 0))
        groomlab.set_map_texture(self.groom, "tint", red)
        self.assertEqual((groomlab.map_kind(self.groom, "tint"), groomlab.map_channels(self.groom, "tint")), ("texture", 4))
        groomlab.set_map_texture(self.groom, "grow", red)
        self.assertEqual(groomlab.map_channels(self.groom, "grow"), 1)

    def test_import_map_makes_a_texture_map_from_an_image_in_one_step(self):
        white = write_image(self.home + "/textures/white.png", gray=255)
        red = write_image(self.home + "/textures/3d red-mask.png", color=(255, 0, 0))
        before = set(groomlab.list_maps(self.groom))
        name = groomlab.import_map(self.groom, white)
        self.assertEqual(name, "white")
        self.assertEqual((groomlab.map_kind(self.groom, name), groomlab.map_channels(self.groom, name)), ("texture", 1))
        self.assertEqual(groomlab.map_texture(self.groom, name), white)  # a PNG is used in place
        self.assertEqual(groomlab.import_map(self.groom, white), "white_2")  # a free name
        colored = groomlab.import_map(self.groom, red)
        self.assertEqual(colored, "map_3d_red_mask")  # letters, digits and _, not starting with a digit
        self.assertEqual(groomlab.map_channels(self.groom, colored), 4)  # a color image: a color map
        groomlab.use_map(self.groom, "scatter", "density", "white")
        self.assertGreater(self.count(), 0)
        self.assertEqual(groomlab.errors(self.groom), [])
        for _ in range(2):
            cmds.undo()  # use_map, then the last import as one step
        self.assertNotIn(colored, groomlab.list_maps(self.groom))
        cmds.undo()
        cmds.undo()
        self.assertEqual(set(groomlab.list_maps(self.groom)), before)
        with self.assertRaises(ValueError):
            groomlab.import_map(self.groom, self.home + "/textures/none.png")

    def test_reload_map_reads_an_image_edited_outside_maya(self):
        self.density_map(0.0)
        png = write_image(self.home + "/textures/mask.png", gray=0)
        groomlab.set_map_texture(self.groom, "density", png)
        self.assertEqual(self.count(), 0)
        time.sleep(0.05)
        write_image(png, gray=255, size=24)  # an image editor saves over it
        self.assertEqual(self.count(), 0)  # nothing tells Maya
        steps = cmds.undoInfo(query=True, undoName=True)
        self.assertEqual(groomlab.reload_map(self.groom, "density"), [png])
        self.assertGreater(self.count(), 0)
        self.assertEqual(cmds.undoInfo(query=True, undoName=True), steps)  # not an undo step
        self.assertEqual(groomlab.reload_map(self.groom), [png])  # every texture map
        groomlab.add_map(self.groom, "flat", kind="vertices")
        with self.assertRaises(ValueError):
            groomlab.reload_map(self.groom, "flat")

    def test_missing_files_are_reported_and_found_in_a_folder(self):
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        from groomlab import graph as gm
        from groomlab.ui import menu
        original = self.density_map(1.0)
        count = self.count()
        self.assertEqual(groomlab.missing_files(self.groom), {"maps": {}, "params": {}, "attributes": {}})
        catalog = gm.OperatorCatalog.from_maya()
        model = gm.GraphModel.from_groom(self.groom, catalog)
        model.add_node("HairColor", name="color", params={"map": "textures/tint.png"})
        model.insert_before("output", "color")
        model.set_param("length", "keep", groomlab.texture_map("textures/gone.png"))
        model.save(self.groom)
        cmds.setAttr(self.groom + ".groomFile", "renders/hair.groom", type="string")
        # A botched copy: the mask is not where the groom looks, nor the other files.
        recovered = self.home + "/recovered"
        moved = recovered + "/groomlab/untitled/masks/%s/Density.png" % groomlab.asset_name(self.groom)
        os.makedirs(os.path.dirname(moved))
        os.rename(original, moved)
        write_image(recovered + "/decoy/density.png", gray=0)  # same name, other folders
        write_image(recovered + "/a/tint.png", gray=100)
        write_image(recovered + "/b/tint.png", gray=200)
        os.makedirs(recovered + "/renders")
        with open(recovered + "/renders/hair.groom", "wb") as f:
            f.write(b"groom")
        missing = groomlab.missing_files(self.groom)
        stored = "groomlab/untitled/masks/%s/density.png" % groomlab.asset_name(self.groom)
        self.assertEqual(missing, {"maps": {"density": stored},
                                   "params": {"color.map": "textures/tint.png", "length.keep": "textures/gone.png"},
                                   "attributes": {"groomFile": "renders/hair.groom"}})
        self.assertEqual(self.count(), 0)
        graph_before = cmds.getAttr(self.groom + ".graph")
        settle = cmds.undoInfo(query=True, undoName=True)
        report = groomlab.find_missing_files(self.groom, recovered)
        g = self.groom
        self.assertEqual(report["relinked"], {g: {
            "maps": {"density": "recovered/groomlab/untitled/masks/%s/Density.png" % groomlab.asset_name(g)},
            "attributes": {"groomFile": "recovered/renders/hair.groom"}}})
        self.assertEqual(report["ambiguous"], {g: {"params": {"color.map": [recovered + "/a/tint.png",
                                                                                recovered + "/b/tint.png"]}}})
        self.assertEqual(report["still_missing"], {g: {"params": {"length.keep": "textures/gone.png"}}})
        self.assertEqual(groomlab.map_texture(g, "density"), moved)
        self.count()
        self.assertFalse([e for e in groomlab.errors(g) if "density" in e])  # the map reads its image again
        cmds.undo()  # one step
        self.assertEqual(cmds.undoInfo(query=True, undoName=True), settle)
        self.assertEqual(groomlab.missing_files(g)["maps"], {"density": stored})
        self.assertEqual(cmds.getAttr(g + ".groomFile"), "renders/hair.groom")
        self.assertEqual(cmds.getAttr(g + ".graph"), graph_before)
        # A unique name relinks a graph param; the menu's action takes the folder as an argument.
        os.remove(recovered + "/b/tint.png")
        report = menu.find_missing_files_dialog(folder=recovered, grooms=[g])
        self.assertEqual(report["relinked"][g]["params"], {"color.map": "recovered/a/tint.png"})
        color = next(n for n in groomlab.get_graph(g)["nodes"] if n["name"] == "color")
        self.assertEqual(color["params"]["map"], "recovered/a/tint.png")
        self.assertEqual(groomlab.missing_files(g)["params"], {"length.keep": "textures/gone.png"})
        cmds.setAttr(g + ".groomFile", "", type="string")
        # Nothing missing: nothing asked.
        model = gm.GraphModel.from_groom(g, catalog)
        model.set_param("length", "keep", 1.0)
        model.save(g)
        self.assertEqual(menu.find_missing_files_dialog(grooms=[g]), {"relinked": {}, "still_missing": {}, "ambiguous": {}})
        self.assertEqual(self.count(), count)
    def test_files_are_listed_for_pipelines_and_the_file_path_editor(self):
        # file_dependencies lists every file the groom uses; a groomLabFile node mirrors each in
        # Maya's File Path Editor (no undo step); a repath there goes back to the groom, relative.
        from groomlab import dependencies
        from groomlab import graph as gm
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        original = self.density_map(1.0)
        model = gm.GraphModel.from_groom(self.groom, gm.OperatorCatalog.from_maya())
        model.add_node("HairColor", name="color", params={"map": "textures/tint.png"})
        model.insert_before("output", "color")
        model.save(self.groom)
        cmds.setAttr(self.groom + ".groomFile", "renders/hair.groom", type="string")
        files = {(f["kind"], f["key"]): f for f in groomlab.file_dependencies(self.groom)}
        self.assertEqual(set(files), {("map", "density"), ("param", "color.map"), ("attribute", "groomFile")})
        self.assertTrue(os.path.samefile(files[("map", "density")]["resolved"], original))
        self.assertTrue(files[("map", "density")]["exists"])
        self.assertFalse(files[("param", "color.map")]["exists"])
        self.assertEqual(files[("attribute", "groomFile")]["path"], "renders/hair.groom")
        settle = cmds.undoInfo(query=True, undoName=True)
        dependencies.sync(self.groom)
        nodes = dependencies.file_nodes(self.groom)
        self.assertEqual(set(nodes), set(files))
        self.assertEqual(cmds.undoInfo(query=True, undoName=True), settle, "the nodes' upkeep is no undo step")
        self.assertIn("groomLabFile.fileName", cmds.filePathEditor(query=True, listRegisteredTypes=True))
        listed = cmds.filePathEditor(query=True, listFiles=os.path.dirname(original).replace("\\", "/"), withAttribute=True) or []
        self.assertIn(nodes[("map", "density")] + ".fileName", listed)
        # The editor's Repath: the groom reads the moved map, stored relative.
        moved = self.home + "/moved"
        os.makedirs(moved)
        shutil.copy(original, moved + "/" + os.path.basename(original))
        cmds.filePathEditor(nodes[("map", "density")] + ".fileName", repath=moved, force=True)
        dependencies.file_node_changed(nodes[("map", "density")])  # (on idle in an interactive Maya)
        stored = next(m["texture"] for m in groomlab.maps(self.groom) if m["name"] == "density")
        self.assertEqual(stored, "moved/" + os.path.basename(original))
        self.assertFalse([e for e in groomlab.errors(self.groom) if "density" in e])  # the map reads the moved image
        # The pipeline's own repath, and a file the groom stops using takes its node along.
        self.assertEqual(groomlab.repath_file(self.groom, "attribute", "groomFile", self.home + "/out/hair.groom"),
                         "out/hair.groom")
        with self.assertRaises(ValueError):
            groomlab.repath_file(self.groom, "simCache", "simulate", self.home + "/x.abc")
        cmds.setAttr(self.groom + ".groomFile", "", type="string")
        dependencies.sync(self.groom)
        self.assertNotIn(("attribute", "groomFile"), dependencies.file_nodes(self.groom))
        # A groom deleted: its nodes go before the save.
        other = groomlab.create_groom(cmds.polySphere()[0])
        cmds.setAttr(other + ".groomFile", "renders/other.groom", type="string")
        dependencies.sync(other)
        made = list(dependencies.file_nodes(other).values())
        self.assertEqual(len(made), 1)
        cmds.delete(cmds.listRelatives(other, parent=True))
        groomlab.sync_file_nodes()
        self.assertFalse(cmds.objExists(made[0]))

    def test_collect_files_copies_what_lies_outside_the_scene_into_its_folder(self):
        # A map and a graph image outside the scene's folder are copied into its groomlab folder
        # and the groom points at the copies (relative); a file inside stays; a missing one is
        # reported. One undo step.
        from groomlab import graph as gm
        from groomlab.ui import menu
        scene = self.home + "/shot"
        os.makedirs(scene)
        cmds.file(rename=scene + "/shot.ma")
        cmds.file(save=True, type="mayaAscii")
        with self.assertRaises(ValueError):
            cmds.file(new=True, force=True)
            groomlab.collect_files()
        cmds.file(scene + "/shot.ma", open=True, force=True)
        self.groom = cmds.ls(type="groomLabShape", long=True)[0]
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        elsewhere = tempfile.mkdtemp(prefix="groomlab_elsewhere_").replace("\\", "/")
        outside = write_image(elsewhere + "/maps/density.png", gray=255)
        groomlab.import_map(self.groom, outside, name="density")
        tint = write_image(elsewhere + "/tex/tint.png", gray=120)
        write_image(scene + "/textures/keep.png", gray=60)
        model = gm.GraphModel.from_groom(self.groom, gm.OperatorCatalog.from_maya())
        model.add_node("HairColor", name="color", params={"map": tint})
        model.set_param("length", "keep", groomlab.texture_map("textures/keep.png"))  # (relative: inside the scene)
        model.insert_before("output", "color")
        model.save(self.groom)
        cmds.setAttr(self.groom + ".guideCache", elsewhere + "/gone.abc", type="string")
        settle = cmds.undoInfo(query=True, undoName=True)
        report = menu.collect_files_report()
        asset = groomlab.asset_name(self.groom)
        self.assertEqual(report["copied"], {self.groom: {
            "map": {"density": "groomlab/shot/masks/%s/density.png" % asset},
            "param": {"color.map": "groomlab/shot/masks/%s/tint.png" % asset}}})
        self.assertEqual(report["missing"], {self.groom: {"attribute": {"guideCache": elsewhere + "/gone.abc"}}})
        self.assertEqual(report["inside"], 1)
        self.assertTrue(os.path.isfile(scene + "/groomlab/shot/masks/%s/density.png" % asset))
        files = {(f["kind"], f["key"]): f for f in groomlab.file_dependencies(self.groom)}
        self.assertEqual(files[("param", "length.keep")]["path"], "textures/keep.png")  # inside: untouched
        self.assertTrue(files[("map", "density")]["exists"])
        self.assertGreater(self.count(), 0)
        cmds.undo()  # one step: the paths back (the copies stay)
        self.assertEqual(cmds.undoInfo(query=True, undoName=True), settle)
        self.assertTrue(os.path.samefile(groomlab.map_texture(self.groom, "density"), outside))
        # Again: the copies there already are reused, not duplicated.
        again = groomlab.collect_files()
        self.assertEqual(again["copied"][self.groom]["map"]["density"], "groomlab/shot/masks/%s/density.png" % asset)
        self.assertEqual(sorted(os.listdir(scene + "/groomlab/shot/masks/%s" % asset)), ["density.png", "tint.png"])
        # A UDIM set of one tile keeps its token; a card bake made outside comes whole, under one prefix.
        write_image(elsewhere + "/udim/root.1001.png", gray=90)
        model = gm.GraphModel.from_groom(self.groom, gm.OperatorCatalog.from_maya())
        model.set_param("length", "keep", groomlab.texture_map(elsewhere + "/udim/root.<UDIM>.png"))
        model.save(self.groom)
        groomlab.create_cards(self.groom, max_cards=8)
        groomlab.bake_cards(self.groom, elsewhere + "/bake", resolution=32, name="lock")
        report = groomlab.collect_files()
        self.assertEqual(report["copied"][self.groom]["param"]["length.keep"],
                         "groomlab/shot/masks/%s/root.<UDIM>.png" % asset)
        self.assertTrue(os.path.isfile(scene + "/groomlab/shot/masks/%s/root.1001.png" % asset))
        prefix = groomlab_core.card_maps_prefix(cmds.getAttr(self.groom + ".cardMaps"))
        self.assertEqual(prefix, "groomlab/shot/cards/%s/lock_" % asset)
        bakes = [f for f in groomlab.file_dependencies(self.groom) if f["kind"] == "cardBake"]
        self.assertGreater(len(bakes), 3)
        self.assertTrue(all(f["exists"] and f["path"].startswith(prefix) for f in bakes))

    def test_a_moved_scene_keeps_its_paths_and_its_file_nodes_follow(self):
        # The file nodes save absolute paths; opened from another folder, the grooms keep their
        # relative paths and the nodes show where the files are now.
        from groomlab import dependencies
        scene = self.home + "/shot"
        os.makedirs(scene)
        cmds.file(rename=scene + "/shot.ma")
        cmds.file(save=True, type="mayaAscii")
        self.groom = cmds.ls(type="groomLabShape", long=True)[0]
        write_image(scene + "/textures/density.png", gray=255)
        groomlab.import_map(self.groom, scene + "/textures/density.png", name="density")
        cmds.file(save=True, type="mayaAscii")  # (the nodes made before the save)
        node = dependencies.file_nodes(self.groom)[("map", "density")]
        self.assertTrue(os.path.samefile(cmds.getAttr(node + ".fileName"), scene + "/textures/density.png"))
        moved = tempfile.mkdtemp(prefix="groomlab_moved_").replace("\\", "/") + "/shot"
        shutil.copytree(scene, moved)
        cmds.file(moved + "/shot.ma", open=True, force=True)
        groom = cmds.ls(type="groomLabShape", long=True)[0]
        stored = next(m["texture"] for m in groomlab.maps(groom) if m["name"] == "density")
        self.assertEqual(stored, "textures/density.png")
        node = dependencies.file_nodes(groom)[("map", "density")]
        self.assertTrue(os.path.samefile(cmds.getAttr(node + ".fileName"), moved + "/textures/density.png"))
        dependencies.file_node_changed(node)  # (nothing to take: the node is the groom's)
        self.assertEqual(next(m["texture"] for m in groomlab.maps(groom) if m["name"] == "density"), "textures/density.png")


def write_photo(path, width, height, spot=None):
    """A test photo: dark gray, with a white square (x, y, size: pixels from the top-left)."""
    from PySide2 import QtGui
    image = QtGui.QImage(width, height, QtGui.QImage.Format_RGB32)
    image.fill(QtGui.QColor(40, 40, 40))
    if spot:
        x, y, size = spot
        for i in range(x - size // 2, x + size // 2):
            for j in range(y - size // 2, y + size // 2):
                image.setPixelColor(i, j, QtGui.QColor(255, 255, 255))
    folder = os.path.dirname(path)
    if folder and not os.path.isdir(folder):
        os.makedirs(folder)
    assert image.save(path), path
    return path.replace("\\", "/")


def maya_projection(camera, points, width, height):
    """Where Maya's camera sees world `points` on a photo of width x height pixels filling its film
    gate (pixels from the top-left): the camera's own viewing frustum for the gate's aspect."""
    from maya.api import OpenMaya as om
    path = om.MSelectionList().add(camera).getDagPath(0)
    inverse = path.inclusiveMatrixInverse()
    fn = om.MFnCamera(om.MDagPath(path).extendToShape())
    near = fn.nearClippingPlane
    left, right, bottom, top = [v / near for v in fn.getViewingFrustum(
        fn.horizontalFilmAperture / fn.verticalFilmAperture, False, False, False)]
    out = []
    for p in points:
        c = om.MPoint(*p) * inverse
        x, y = c.x / -c.z, c.y / -c.z
        out.append(((x - left) / (right - left) * width, (top - y) / (top - bottom) * height))
    return out



class ReferenceTest(unittest.TestCase):
    """Reference photos as cameras (Groom Tools > Reference): import, how they show, locking,
    saving, and lining a camera up with point pairs."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        from groomlab import reference
        self.ref = reference
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.home = tempfile.mkdtemp(prefix="groomlab_refs_").replace("\\", "/")
        cmds.workspace(self.home, openWorkspace=True)
        self.scalp = cmds.polySphere(radius=10, subdivisionsX=30, subdivisionsY=15)[0]
        self.groom = groomlab.create_groom(self.scalp)
        self.photo = write_photo(self.home + "/photos/front.png", 64, 48)

    def save_as(self, *parts):
        path = os.path.join(self.home, *parts).replace("\\", "/")
        if not os.path.isdir(os.path.dirname(path)):
            os.makedirs(os.path.dirname(path))
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        return path

    def sphere_points(self, count, step=23):
        xyz = cmds.xform(self.scalp + ".vtx[*]", query=True, worldSpace=True, translation=True)
        points = [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]
        return points[5::step][:count]

    def test_import_makes_a_camera_with_the_photos_aspect_and_copies_it(self):
        self.save_as("shots", "shot.ma")
        cam = self.ref.import_reference(self.photo, groom=self.groom)
        self.assertEqual(cam, "front_ref")
        self.assertEqual(cmds.listRelatives(cam, parent=True), [self.ref.GROUP])
        self.assertEqual(self.ref.list_references(), [cam])
        shape = cmds.listRelatives(cam, shapes=True, type="camera")[0]
        plane = self.ref.image_plane(cam)
        self.assertEqual(cmds.nodeType(plane), "imagePlane")
        # The film back and the image plane take the photo's aspect, and the photo fills the gate.
        hfa, vfa = cmds.getAttr(shape + ".horizontalFilmAperture"), cmds.getAttr(shape + ".verticalFilmAperture")
        self.assertAlmostEqual(hfa / vfa, 64.0 / 48.0, places=4)
        self.assertEqual(cmds.getAttr(plane + ".fit"), 4)  # To Size
        self.assertAlmostEqual(cmds.getAttr(plane + ".sizeX"), hfa, places=5)
        self.assertAlmostEqual(cmds.getAttr(plane + ".sizeY"), vfa, places=5)
        self.assertTrue(cmds.getAttr(plane + ".displayOnlyIfCurrent"))
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength"), 35.0)
        # The photo is copied beside the scene and stored relative to it.
        copy = self.home + "/shots/groomlab/shot/references/front.png"
        self.assertTrue(os.path.isfile(copy))
        self.assertEqual(cmds.getAttr(cam + ".glRefImage"), "groomlab/shot/references/front.png")
        self.assertTrue(groomlab._same_file(cmds.getAttr(plane + ".imageName"), copy))
        info = self.ref.reference_info(cam)
        self.assertEqual((info["width"], info["height"], info["depth"], info["locked"]), (64, 48, "front", False))
        # Aimed at the scalp from the front: its center in the middle of the photo, all of it inside.
        box = cmds.exactWorldBoundingBox(self.scalp)
        center = [0.5 * (box[i] + box[i + 3]) for i in range(3)]
        (u, v), = maya_projection(cam, [center], 64, 48)
        self.assertAlmostEqual(u, 32.0, places=3)
        self.assertAlmostEqual(v, 24.0, places=3)
        for u, v in maya_projection(cam, self.sphere_points(40, 11), 64, 48):
            self.assertTrue(0 <= u <= 64 and 0 <= v <= 48, (u, v))
        # The same photo again shares the copy; another photo of the same name gets its own.
        again = self.ref.import_reference(self.photo)
        self.assertEqual(again, "front_ref2")
        self.assertEqual(self.ref.image_path(again), self.ref.image_path(cam))
        other = self.ref.import_reference(write_photo(self.home + "/elsewhere/front.png", 30, 60))
        self.assertTrue(self.ref.image_path(other).endswith("/references/front_2.png"), self.ref.image_path(other))
        self.assertEqual((cmds.getAttr(other + ".glRefWidth"), cmds.getAttr(other + ".glRefHeight")), (30, 60))
        # copy=False keeps the photo where it is.
        kept = self.ref.import_reference(self.photo, name="kept", copy=False)
        self.assertEqual(kept, "kept")
        self.assertTrue(groomlab._same_file(self.ref.image_path(kept), self.photo))
        # One undo step each.
        cmds.undo()
        self.assertFalse(cmds.objExists("kept"))
        with self.assertRaises(ValueError):
            self.ref.import_reference(self.home + "/missing.png")

    def test_opacity_visibility_depth_and_lock(self):
        cam = self.ref.import_reference(self.photo, groom=self.groom)
        plane = self.ref.image_plane(cam)
        self.ref.set_reference_opacity(cam, 0.25)
        self.assertAlmostEqual(cmds.getAttr(plane + ".alphaGain"), 0.25)
        self.ref.set_reference_opacity(cam, 3.0)
        self.assertAlmostEqual(cmds.getAttr(plane + ".alphaGain"), 1.0)
        self.ref.set_reference_visible(cam, False)
        self.assertFalse(cmds.getAttr(cmds.listRelatives(plane, parent=True, fullPath=True)[0] + ".visibility"))
        self.assertFalse(self.ref.reference_visible(cam))
        self.ref.set_reference_visible(cam, True)
        # In front: the image plane sits between the camera and the scalp; behind: past it.
        eye = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        near_side = math.sqrt(sum(c * c for c in eye)) - 10.0
        self.assertLess(cmds.getAttr(plane + ".depth"), near_side)
        self.ref.set_reference_depth(cam, "behind")
        self.assertGreater(cmds.getAttr(plane + ".depth"), near_side + 20.0)
        self.assertEqual(self.ref.reference_depth(cam), "behind")
        with self.assertRaises(ValueError):
            self.ref.set_reference_depth(cam, "sideways")
        # The focal length by hand (the Photos tab's Focal Length): kept within 2.5 to 3500 mm.
        shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
        self.ref.set_reference_focal(cam, 85.0)
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength"), 85.0)
        self.assertAlmostEqual(self.ref.reference_info(cam)["focal"], 85.0)
        self.ref.set_reference_focal(cam, 0.5)
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength"), 2.5)
        self.ref.set_reference_focal(cam, 50.0)
        # Locked: the transform, the lens and the photo's placement; tumbling through it does nothing.
        self.ref.lock_reference(cam)
        with self.assertRaises(RuntimeError):  # a locked camera keeps its focal length
            self.ref.set_reference_focal(cam, 70.0)
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength"), 50.0)
        self.assertTrue(self.ref.is_locked(cam))
        shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
        for plug in (cam + ".translateX", cam + ".rotateY", cam + ".scaleZ", shape + ".focalLength",
                     shape + ".horizontalFilmAperture", shape + ".verticalFilmAperture", plane + ".sizeX",
                     plane + ".fit"):
            self.assertTrue(cmds.getAttr(plug, lock=True), plug)
        before = cmds.xform(cam, query=True, worldSpace=True, matrix=True)
        cmds.tumble(cam, rotationAngles=(15, 25))
        cmds.dolly(cam, distance=5)
        self.assertEqual(cmds.xform(cam, query=True, worldSpace=True, matrix=True), before)
        # Still: the photo's opacity and side change on a locked reference.
        self.ref.set_reference_depth(cam, "front")
        self.ref.set_reference_opacity(cam, 0.5)
        with self.assertRaises(ValueError):  # a locked camera is not re-aligned
            self.ref.solve(cam)
        self.ref.lock_reference(cam, False)
        self.assertFalse(cmds.getAttr(shape + ".focalLength", lock=True))
        cmds.tumble(cam, rotationAngles=(15, 25))
        self.assertNotEqual(cmds.xform(cam, query=True, worldSpace=True, matrix=True), before)

    def test_save_reopen_keeps_the_settings_and_save_as_takes_the_photo(self):
        path = self.save_as("a", "hero.ma")
        cam = self.ref.import_reference(self.photo, groom=self.groom)
        self.ref.set_pair(cam, 0, image=(10, 12), mesh=(0, 10, 0))
        self.ref.set_reference_depth(cam, "behind")
        self.ref.set_reference_opacity(cam, 0.3)
        self.ref.lock_reference(cam)
        cmds.file(save=True, type="mayaAscii", force=True)
        cmds.file(new=True, force=True)
        cmds.file(path, open=True, force=True)
        self.assertEqual(self.ref.list_references(), [cam])
        info = self.ref.reference_info(cam)
        self.assertEqual(info["pairs"], [{"image": [10.0, 12.0], "mesh": [0.0, 10.0, 0.0], "error": None}])
        self.assertEqual((info["depth"], info["locked"], round(info["opacity"], 3)), ("behind", True, 0.3))
        self.assertEqual(cmds.getAttr(cam + ".glRefImage"), "groomlab/hero/references/front.png")
        # Save As: the photo goes into the new scene's own folder, the camera follows it.
        self.save_as("b", "hero_v2.ma")
        moved = self.home + "/b/groomlab/hero_v2/references/front.png"
        self.assertTrue(os.path.isfile(moved))
        self.assertEqual(cmds.getAttr(cam + ".glRefImage"), "groomlab/hero_v2/references/front.png")
        self.assertTrue(groomlab._same_file(cmds.getAttr(self.ref.image_plane(cam) + ".imageName"), moved))
        # A scene opened where its absolute image path is gone relinks to its groomlab folder.
        plane = self.ref.image_plane(cam)
        cmds.setAttr(plane + ".imageName", lock=False)
        cmds.setAttr(plane + ".imageName", self.home + "/nowhere/front.png", type="string")
        self.assertEqual(self.ref.relink(), [cam])
        self.assertTrue(groomlab._same_file(cmds.getAttr(plane + ".imageName"), moved))

    def test_a_solve_recovers_a_camera_maya_projected_the_points_with(self):
        cam = self.ref.import_reference(self.photo, groom=self.groom)
        shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
        # A known camera: Maya projects mesh points onto the photo with it ...
        cmds.xform(cam, worldSpace=True, translation=(14, 7, 38), rotation=(-9, 21, 4))
        cmds.setAttr(shape + ".focalLength", 55)
        truth_t = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        truth_r = cmds.xform(cam, query=True, worldSpace=True, rotation=True)
        points = [p for p in self.sphere_points(60, 7) if p[2] > 2.0][:10]  # the side facing it
        for (u, v), p in zip(maya_projection(cam, points, 64, 48), points):
            self.ref.set_pair(cam, len(self.ref.pairs(cam)), image=(u, v), mesh=p)
        # ... then the camera is moved away and the lens changed; the solve brings both back.
        cmds.xform(cam, worldSpace=True, translation=(0, 0, 60), rotation=(0, 0, 0))
        cmds.setAttr(shape + ".focalLength", 35)
        result = self.ref.solve(cam)
        self.assertLess(result["rms"], 1e-3)
        now = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        self.assertLess(max(abs(a - b) for a, b in zip(now, truth_t)), 0.1)  # a millimeter (cm scene)
        rot = cmds.xform(cam, query=True, worldSpace=True, rotation=True)
        self.assertLess(max(abs(a - b) for a, b in zip(rot, truth_r)), 0.01)
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength") / 55.0, 1.0, delta=0.01)
        self.assertEqual(len(result["errors"]), 10)
        self.assertTrue(all(p["error"] is not None and p["error"] < 1e-3 for p in self.ref.pairs(cam)))
        self.assertAlmostEqual(cmds.getAttr(cam + ".glRefRms"), result["rms"])
        # The film back stays; the solve is one undo step.
        self.assertAlmostEqual(cmds.getAttr(shape + ".horizontalFilmAperture") /
                               cmds.getAttr(shape + ".verticalFilmAperture"), 64.0 / 48.0, places=4)
        cmds.undo()
        self.assertEqual([round(c, 6) for c in cmds.xform(cam, query=True, worldSpace=True, translation=True)],
                         [0.0, 0.0, 60.0])
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength"), 35.0)

    def test_four_pairs_solve_the_pose_with_the_focal_length_kept(self):
        cam = self.ref.import_reference(self.photo, groom=self.groom)
        shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
        cmds.xform(cam, worldSpace=True, translation=(-20, 12, 35), rotation=(-15, -30, 0))
        cmds.setAttr(shape + ".focalLength", 45)
        truth = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        points = [p for p in self.sphere_points(80, 5) if p[2] > 1.0 and p[0] < 0][:4]
        self.assertEqual(len(points), 4)
        for i, ((u, v), p) in enumerate(zip(maya_projection(cam, points, 64, 48), points)):
            self.ref.set_pair(cam, i, image=(u, v), mesh=p)
        cmds.xform(cam, worldSpace=True, translation=(0, 0, 50), rotation=(0, 0, 0))
        with self.assertRaises(ValueError) as caught:  # four pairs don't fix the focal length too
            self.ref.solve(cam)
        self.assertIn("6 complete pairs", str(caught.exception))
        self.assertFalse(self.ref.keep_focal(cam))
        self.ref.set_keep_focal(cam, True)  # kept on the camera: the solve keeps the focal length
        self.assertTrue(self.ref.reference_info(cam)["keepFocal"])
        result = self.ref.solve(cam)
        self.assertLess(result["rms"], 1e-3)
        self.assertAlmostEqual(result["focal"], 45.0, places=6)
        now = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        self.assertLess(max(abs(a - b) for a, b in zip(now, truth)), 0.1)

    def test_pairs_edit_in_order_and_the_solve_says_what_is_wrong(self):
        cam = self.ref.import_reference(self.photo)
        self.ref.set_pair(cam, 0, image=(1, 2), mesh=(0, 0, 10))
        self.ref.set_pair(cam, 1, image=(3, 4))
        self.ref.set_pair(cam, 2, mesh=(1, 1, 10))
        self.assertEqual(len(self.ref.complete_pairs(cam)), 1)
        self.ref.set_pair(cam, 0, image=(5, 6))
        self.assertEqual(self.ref.pairs(cam)[0]["image"], [5.0, 6.0])
        with self.assertRaises(ValueError):
            self.ref.set_pair(cam, 5, image=(1, 2))  # past the end
        self.ref.remove_pair(cam, 1)
        self.assertEqual([p["mesh"] for p in self.ref.pairs(cam)], [[0.0, 0.0, 10.0], [1.0, 1.0, 10.0]])
        self.ref.clear_pairs(cam)
        self.assertEqual(self.ref.pairs(cam), [])
        # The command itself: its errors name the problem.
        with self.assertRaises(RuntimeError):
            cmds.groomLabCameraSolve(imagePoint=[(1, 2)], worldPoint=[(0, 0, 0), (1, 1, 1)], imageSize=(64, 48))
        flat = [(x, y, 0.0) for x in (-5, 0, 5) for y in (-5, 5)]
        with self.assertRaises(RuntimeError):  # on one plane, the focal length can't be solved
            cmds.groomLabCameraSolve(imagePoint=[(10 + i, 20 + 2 * i) for i in range(6)], worldPoint=flat,
                                     imageSize=(64, 48))
        help_text = cmds.help("groomLabCameraSolve")
        for flag in ("-imagePoint", "-worldPoint", "-imageSize", "-filmBack", "-focal", "-fixFocal", "-camera"):
            self.assertIn(flag, help_text)

    def test_remove_and_rename(self):
        self.save_as("r", "shot.ma")
        cam = self.ref.import_reference(self.photo)
        other = self.ref.import_reference(self.photo)
        shared = self.ref.image_path(cam)
        self.assertEqual(self.ref.rename_reference(cam, "closeup"), "closeup")
        with self.assertRaises(ValueError):
            self.ref.rename_reference("closeup", "9lives")
        self.ref.remove_reference("closeup", delete_file=True)  # another reference shows it: it stays
        self.assertTrue(os.path.isfile(shared))
        self.assertEqual(self.ref.list_references(), [other])
        self.ref.remove_reference(other, delete_file=True)
        self.assertFalse(os.path.isfile(shared))
        self.assertFalse(cmds.objExists(self.ref.GROUP))  # nothing left under it
        self.assertTrue(os.path.isfile(self.photo))  # the original is never touched
        with self.assertRaises(ValueError):
            self.ref.lock_reference(self.scalp)  # not a reference

    def test_guides_from_reference_grow_replace_undo_and_look(self):
        """Guides from Reference (experimental) as the Reference tab runs it: flow strokes and a
        mask kept on the groom, Generate into fromReference (again: replaced; one undo step each),
        the fitted look applied to HairColor, a camera with an image plane adopted as a view."""
        from PySide2 import QtGui
        from groomlab import from_reference as fr
        # A light backdrop, dark vertical strands where the ball shows (the camera Import Photo
        # aims at the groom sees it fill the middle of the photo).
        image = QtGui.QImage(160, 120, QtGui.QImage.Format_RGB32)
        image.fill(QtGui.QColor(230, 230, 235))
        for x in range(40, 120):
            for y in range(15, 115):
                image.setPixelColor(x, y, QtGui.QColor(70, 45, 30) if x % 3 == 0 else QtGui.QColor(115, 80, 55))
        photo = os.path.join(self.home, "strands.png").replace("\\", "/")
        self.assertTrue(image.save(photo))
        cam = self.ref.import_reference(photo, groom=self.groom, copy=False)
        self.assertEqual(fr.reference_hints(self.groom, cam), [])
        fr.set_reference_hints(self.groom, cam, [[(80, 30), (80, 100)]])
        fr.add_reference_stroke(self.groom, cam, [(60, 30), (60, 100)])
        self.assertEqual(len(fr.reference_hints(self.groom, cam)), 2)
        cmds.undo()  # one stroke, one undo step
        self.assertEqual(len(fr.reference_hints(self.groom, cam)), 1)
        self.assertEqual(fr.erase_reference_strokes(self.groom, cam, [(81, 60)]), 1)
        fr.set_reference_hints(self.groom, cam, [[(80, 30), (80, 100)]])
        self.assertIsNone(fr.view_mask(self.groom, cam))

        def groups():
            return {g["name"]: g["count"] for g in groomlab.list_guide_groups(self.groom)}
        first = fr.generate(self.groom, guide_count=120, symmetry="x", max_length=8.0)
        self.assertGreater(len(first["guide_ids"]), 10)
        self.assertEqual(first["symmetry"], "x")
        self.assertEqual(groups()[fr.GROUP], len(first["guide_ids"]))
        again = fr.generate(self.groom, guide_count=120, symmetry="x", max_length=8.0)
        self.assertEqual(groups()[fr.GROUP], len(again["guide_ids"]))  # replaced, not added
        self.assertEqual(len(again["guide_ids"]), len(first["guide_ids"]))  # the same input, the same guides
        cmds.undo()
        self.assertEqual(groups()[fr.GROUP], len(first["guide_ids"]))
        cmds.undo()
        self.assertNotIn(fr.GROUP, groups())
        cmds.redo()
        # The measured look (dark strands on a light backdrop: dark hair), previewed against the
        # graph, then applied: the measured values plus every node it can add, in the usual
        # chain order, and the density map; one undo step.
        look = fr.measured_look(self.groom)
        self.assertTrue(look and look["HairColor"]["mode"] == "melanin", look)
        self.assertGreater(look["HairColor"]["melanin"], 0.25)  # brown strands
        rows = fr.measured_look_preview(self.groom)
        melanin = next(r for r in rows if r["type"] == "HairColor" and r["param"] == "melanin")
        self.assertEqual((melanin["node"], melanin["value"]), ("color", look["HairColor"]["melanin"]))
        vertices = cmds.polyEvaluate(self.scalp, vertex=True)
        full = dict(look, Clump={"clumpBy": "size", "size": 0.8, "strength": 0.7}, Curl={"radius": 0.2, "frequency": 0.5},
                    Noise={"fraction": 0.2, "amplitude": 0.05, "frequency": 10.0, "octaves": 1}, Flyaway={"fraction": 0.05},
                    Scatter={"densityMap": [0.2 if i % 2 else 1.0 for i in range(vertices)]})
        rows = fr.measured_look_preview(self.groom, full)
        self.assertEqual({r["type"] for r in rows if r["node"] is None}, {"Clump", "Curl", "Noise", "Flyaway"})
        before = json.dumps(groomlab.get_graph(self.groom), sort_keys=True)
        done = fr.apply_measured_look(self.groom, full)
        from groomlab import graph as gm
        model = gm.GraphModel.from_groom(self.groom)
        chain = [model.node_type(n) for n in fr._chain(model)]
        order = [chain.index(t) for t in ("Clump", "Curl", "Flyaway", "HairColor")]
        self.assertEqual(order, sorted(order), chain)
        self.assertEqual(model.param(done["Noise"], "frequency"), 10.0)
        self.assertGreater(chain.index("Noise", chain.index("Curl")), chain.index("Curl"), chain)  # frizz after the curls
        self.assertAlmostEqual(model.param(done["HairColor"], "melanin"), look["HairColor"]["melanin"])
        self.assertIn(fr.DENSITY_MAP, groomlab.painting.list_maps(self.groom))
        self.assertEqual([round(v, 3) for v in cmds.groomLabScalpMaps(self.groom, values=fr.DENSITY_MAP)[:2]], [1.0, 0.2])
        cmds.undo()
        self.assertEqual(json.dumps(groomlab.get_graph(self.groom), sort_keys=True), before)
        self.assertNotIn(fr.DENSITY_MAP, groomlab.painting.list_maps(self.groom))
        cmds.redo()
        # The strokes and the views stay with the scene.
        path = self.save_as("look.ma")
        cmds.file(path, open=True, force=True)
        groom = cmds.ls(type="groomLabShape")[0]
        self.assertEqual(fr.reference_hints(groom, cam), [{"points": [[80.0, 30.0], [80.0, 100.0]], "directed": True}])
        # A camera of the artist's own with a photo on an image plane becomes a view.
        own = cmds.camera()[0]
        cmds.imagePlane(camera=own, fileName=photo)
        self.assertIn(cmds.ls(own)[0], self.ref.image_plane_cameras())
        adopted = self.ref.adopt_camera(own)
        self.assertTrue(self.ref.is_reference(adopted))
        self.assertEqual((cmds.getAttr(adopted + ".glRefWidth"), cmds.getAttr(adopted + ".glRefHeight")), (160, 120))
        shape = cmds.listRelatives(adopted, shapes=True, type="camera")[0]
        self.assertAlmostEqual(cmds.getAttr(shape + ".horizontalFilmAperture") / cmds.getAttr(shape + ".verticalFilmAperture"),
                               160.0 / 120.0, places=4)


    def test_reference_window_data_landmarks_solve_strokes_masks_and_generate(self):
        """What the Reference window keeps and does without its views: a photo's landmarks (on the
        camera's pairs), flow arrows and mask touch-ups (on the groom) read into the core's editor
        and stored back, the live solve put on the camera, and Generate with the touch-ups."""
        import groomlab_core as gl
        from maya.api import OpenMaya as om
        from PySide2 import QtGui
        from groomlab import from_reference as fr
        image = QtGui.QImage(160, 120, QtGui.QImage.Format_RGB32)
        image.fill(QtGui.QColor(230, 230, 235))
        for x in range(40, 120):
            for y in range(15, 115):
                image.setPixelColor(x, y, QtGui.QColor(70, 45, 30) if x % 3 == 0 else QtGui.QColor(115, 80, 55))
        photo = os.path.join(self.home, "strands.png").replace("\\", "/")
        self.assertTrue(image.save(photo))
        cam = self.ref.import_reference(photo, groom=self.groom, copy=False)
        truth = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        focal = cmds.getAttr(self.ref._shape(cam) + ".focalLength")
        inverse = om.MMatrix(cmds.getAttr(cam + ".worldMatrix[0]")).inverse()
        f = self.ref.focal_px(cam)

        def project(p):
            c = om.MPoint(*p) * inverse
            return 80.0 + f * c.x / -c.z, 60.0 - f * c.y / -c.z
        points = [(0, 0, 10), (6, 4, 6.6), (-6, 4, 6.6), (5, -6, 6.1), (-5, -6, 6.1), (0, 8, 6), (3, 2, 9.3)]
        points = [tuple(10.0 * c / sum(x * x for x in p) ** 0.5 for c in p) for p in points]
        cmds.xform(cam, worldSpace=True, translation=(truth[0] - 5.0, truth[1] + 2.0, truth[2] + 8.0))
        editor = gl.ReferencePhotoEditor()
        editor.reset_json(fr.photo_state(self.groom, cam))
        self.assertEqual((editor.pair_count, editor.strokes, editor.mask_strokes), (0, [], []))
        for p in points:
            editor.add_landmark("mesh", p)
            editor.add_landmark("photo", project(p))
        solve = gl.solve_reference_landmarks(editor, 160, 120, self.ref.focal_px(cam), False, self.ref.camera_dict(cam))
        self.assertTrue(solve["ok"] and solve["focal_solved"], solve)
        self.assertTrue(self.ref.apply_solved_camera(cam, solve))
        moved = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        self.assertLess(max(abs(a - b) for a, b in zip(moved, truth)), 0.5, (moved, truth))
        self.assertAlmostEqual(cmds.getAttr(self.ref._shape(cam) + ".focalLength"), focal, delta=0.5)
        self.assertAlmostEqual(cmds.getAttr(cam + ".glRefRms"), solve["rms"], places=4)
        # Arrows and touch-ups, stored and read back the same; a pending landmark kept at the end.
        editor.add_stroke([(80, 30), (80, 100)])
        editor.add_mask_stroke([(122, 20), (122, 110)], 3.0, True)
        editor.add_landmark("photo", (5, 5))
        fr.store_photo_state(self.groom, cam, editor.state_json(), solve["errors"])
        self.assertEqual(len(self.ref.complete_pairs(cam)), len(points))
        self.assertEqual(len(self.ref.pairs(cam)), len(points) + 1)
        self.assertAlmostEqual(self.ref.pairs(cam)[1]["error"], solve["errors"][1], places=4)
        self.assertEqual(fr.reference_hints(self.groom, cam), [{"points": [[80.0, 30.0], [80.0, 100.0]], "directed": True}])
        self.assertEqual(fr.mask_strokes(self.groom, cam)[0]["radius"], 3.0)
        again = gl.ReferencePhotoEditor()
        again.reset_json(fr.photo_state(self.groom, cam))
        self.assertEqual(again.state_json(), editor.state_json())
        self.assertEqual(again.pending_count, 1)
        # The touch-ups reach the mask Generate works with: excluding everything leaves no hair.
        scalp = fr._mesh(groomlab.scalp_mesh(self.groom))
        preview = gl.preview_reference_view(scalp, fr._camera_view(self.groom, cam))
        hair = sum(bytearray(preview["mask"]))
        self.assertGreater(hair, 0)
        self.assertTrue(preview["lines"])
        first = fr.generate(self.groom, guide_count=120, symmetry="x", max_length=8.0)
        self.assertGreater(len(first["guide_ids"]), 10)
        fr.set_mask_strokes(self.groom, cam, [{"points": [(0, 60), (160, 60)], "radius": 90, "include": False}])
        dark = gl.preview_reference_view(scalp, fr._camera_view(self.groom, cam))
        self.assertEqual(sum(bytearray(dark["mask"])), 0)
        none = fr.generate(self.groom, guide_count=120, symmetry="x", max_length=8.0)
        self.assertLess(len(none["guide_ids"]), len(first["guide_ids"]))
        cmds.undo()  # the generate
        cmds.undo()  # the touch-ups: one undo step
        self.assertEqual(fr.mask_strokes(self.groom, cam)[0]["radius"], 3.0)
        # A locked camera stays where it is.
        self.ref.lock_reference(cam)
        self.assertFalse(self.ref.apply_solved_camera(cam, dict(solve, position=(0, 0, 100))))
        self.assertLess(max(abs(a - b) for a, b in zip(cmds.xform(cam, query=True, worldSpace=True, translation=True),
                                                        truth)), 0.5)


class MotionTest(unittest.TestCase):
    """Wind and Jiggle: motion from the time and the scalp's last frames, no simulation cache.
    Bypassed or at rest, the groom must be exactly what it was."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.playbackOptions(minTime=1, maxTime=40)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=12)[0]
        self.groom = groomlab.create_groom(self.scalp)
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        curves = []
        for angle in (0.3, 0.9, 1.4):
            n = (math.sin(angle), math.cos(angle), 0.2)
            curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.2 * c for c in n)]))
        groomlab.guides_from_curves(self.groom, curves)
        groomlab.set_param(self.groom, "scatter", "density", 25)  # a few hundred strands: quick to compare
        cmds.currentTime(1)

    def points_at(self, frame):
        cmds.currentTime(frame)
        return groomlab.viewport_points(self.groom)

    def graph_node(self, name):
        return next(n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == name)

    def set_bypass(self, name, on):
        from groomlab import graph as gm
        model = gm.GraphModel.from_groom(self.groom)
        model.set_bypass(name, on)
        model.save(self.groom)

    def mean_x(self, points):
        return sum(p[0] for p in points) / len(points)

    def test_wind_moves_with_time_and_bypass_gives_the_groom_back(self):
        calm = {f: self.points_at(f) for f in (1, 2, 3)}
        self.assertGreater(len(calm[1]), 500)
        self.assertEqual(calm[1], calm[2])  # nothing animated: a still groom
        node = groomlab.add_wind(self.groom, direction=(1, 0, 0), strength=0.5, gust=0.8, frequency=1.0, speed=2.0)
        self.assertEqual(groomlab.errors(self.groom), [])
        graph = groomlab.get_graph(self.groom)
        self.assertIn({"from": node, "to": "output", "input": "strands"}, graph["connections"])  # before the output
        self.assertIn({"from": "resample", "to": node, "input": "strands"}, graph["connections"])
        windy = {f: self.points_at(f) for f in (1, 2, 3)}
        for f in (1, 2, 3):
            self.assertEqual(len(windy[f]), len(calm[f]))
            self.assertNotEqual(windy[f], calm[f])
        self.assertNotEqual(windy[1], windy[2])  # the wind moves from frame to frame
        self.assertNotEqual(windy[2], windy[3])
        self.assertGreater(self.mean_x(windy[2]), self.mean_x(calm[2]))  # blown toward +x
        # Disabled: the groom as it was, to the bit, on every frame.
        self.set_bypass(node, True)
        for f in (1, 2, 3):
            self.assertEqual(self.points_at(f), calm[f])
        self.set_bypass(node, False)
        self.assertEqual(self.points_at(2), windy[2])
        # No strength: no wind, and the same strands again.
        groomlab.set_param(self.groom, node, "strength", 0.0)
        self.assertEqual(self.points_at(3), calm[3])
        # Deleted (its input wired to the output again): the same again.
        from groomlab import graph as gm
        model = gm.GraphModel.from_groom(self.groom)
        model.remove_node(node)
        model.connect("resample", "output", "strands")
        model.save(self.groom)
        self.assertEqual(self.points_at(1), calm[1])
        self.assertEqual(self.points_at(2), calm[2])

    def test_follow_scalp_carries_the_groom_at_rest(self):
        # A bend deforms the scalp over the frames; the rest mesh is its undeformed shape.
        bend, _ = cmds.nonLinear(self.scalp, type="bend")
        cmds.setKeyframe(bend, attribute="curvature", time=1, value=0)
        cmds.setKeyframe(bend, attribute="curvature", time=20, value=70)
        groomlab.set_rest_mesh(self.groom)
        self.assertFalse(cmds.getAttr(self.groom + ".followScalp"))  # off by default
        rest = self.points_at(1)
        bent = self.points_at(20)
        self.assertNotEqual(rest, bent)

        groomlab.set_follow_scalp(self.groom, True)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(self.points_at(1), rest)  # at rest: the groom as it is, to the bit
        followed = self.points_at(20)
        self.assertEqual(len(followed), len(bent))
        self.assertNotEqual(followed, bent)

        # Strand by strand: the roots sit where the bent groom's do, and each strand keeps its rest
        # shape, moved rigidly (the distances between its points).
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        count = stats["strands"]
        per = stats["points"] // count
        self.assertEqual(count * per, len(rest))

        def dist(a, b):
            return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))

        for s in range(0, count, max(1, count // 40)):
            a, b, r = followed[s * per:(s + 1) * per], bent[s * per:(s + 1) * per], rest[s * per:(s + 1) * per]
            self.assertLess(dist(a[0], b[0]), 1e-4)
            self.assertAlmostEqual(dist(a[0], a[-1]), dist(r[0], r[-1]), delta=1e-4)
            self.assertAlmostEqual(dist(a[1], a[-2]), dist(r[1], r[-2]), delta=1e-4)

        # Off and on again: one undo step each way, back to the groom grown on the bent scalp.
        groomlab.set_follow_scalp(self.groom, False)
        self.assertEqual(groomlab.viewport_points(self.groom), bent)
        cmds.undo()
        self.assertTrue(cmds.getAttr(self.groom + ".followScalp"))
        self.assertEqual(groomlab.viewport_points(self.groom), followed)
        cmds.redo()
        self.assertEqual(groomlab.viewport_points(self.groom), bent)
        groomlab.set_follow_scalp(self.groom, True)

        # Groom files keep it (the CLI and the Arnold procedural follow too); exports see it.
        path = os.path.join(tempfile.mkdtemp(), "follow.groom")
        groomlab.export_groom(self.groom, path)
        with open(path) as f:
            self.assertTrue(json.load(f).get("followScalp"))
        groomlab.set_follow_scalp(self.groom, False)
        groomlab.export_groom(self.groom, path)
        with open(path) as f:
            self.assertNotIn("followScalp", json.load(f))

    def test_a_deforming_scalp_gives_the_same_groom_however_its_frames_are_reached(self):
        # Maya splits a bent quad across its other diagonal: with a rest mesh the scalp keeps the
        # rest mesh's triangles, so a frame first read on its own (a jump, a scene opened there,
        # cached playback's background fill) is the frame played to.
        bend, _ = cmds.nonLinear(self.scalp, type="bend")
        cmds.setKeyframe(bend, attribute="curvature", time=1, value=0)
        cmds.setKeyframe(bend, attribute="curvature", time=20, value=80)
        groomlab.set_rest_mesh(self.groom)
        played = {f: self.points_at(f) for f in range(1, 21)}
        cmds.dgdirty(self.groom + ".inMesh")  # the scalp read in full at frame 20
        self.assertEqual(groomlab.viewport_points(self.groom), played[20])
        self.assertEqual(self.points_at(7), played[7])
        # Another time's evaluation (the groom's background lane, as cached playback fills frames)
        # leaves the frame shown alone and counts the frame played.
        before = json.loads(cmds.groomLabGraphStats(self.groom, frameInfo=True))
        self.assertEqual(cmds.getAttr(self.groom + ".outStrandCount", time=13), cmds.getAttr(self.groom + ".outStrandCount"))
        after = json.loads(cmds.groomLabGraphStats(self.groom, frameInfo=True))
        self.assertGreater(after["backgroundEvaluations"], before["backgroundEvaluations"])
        self.assertEqual(after["evaluations"], before["evaluations"])  # the live evaluation untouched
        self.assertEqual(groomlab.viewport_points(self.groom), played[7])

    def test_motion_nodes_go_before_collide_and_cards(self):
        groomlab.add_collider(self.groom, cmds.polyCube(name="box")[0])
        node = groomlab.add_jiggle(self.groom, strength=0.5, history=6)
        graph = groomlab.get_graph(self.groom)
        self.assertIn({"from": node, "to": "collide_box", "input": "strands"}, graph["connections"])
        self.assertEqual(self.graph_node(node)["params"]["history"], 6)
        wind = groomlab.add_wind(self.groom)
        self.assertIn({"from": wind, "to": "collide_box", "input": "strands"}, groomlab.get_graph(self.groom)["connections"])
        self.assertEqual(groomlab.errors(self.groom), [])

    def key_scalp_move(self):
        """The scalp still to frame 5, sliding +x to frame 10, still after."""
        for frame, x in ((1, 0.0), (5, 0.0), (10, 1.5), (40, 1.5)):
            cmds.setKeyframe(self.scalp, attribute="translateX", time=frame, value=x)
        cmds.keyTangent(self.scalp, attribute="translateX", inTangentType="linear", outTangentType="linear")

    def test_jiggle_lags_a_moving_scalp_and_leaves_a_still_one(self):
        # A still scalp: Jiggle changes nothing, and reads the scalp's past once, not every frame.
        still = {f: self.points_at(f) for f in (1, 2, 3, 4)}
        node = groomlab.add_jiggle(self.groom, history=8)
        self.assertEqual(groomlab.errors(self.groom), [])
        for f in (1, 2, 3, 4):
            self.assertEqual(self.points_at(f), still[f])
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["scalpHistory"]["frames"], 8)
        self.assertEqual(stats["notes"], {})
        reads = stats["scalpHistory"]["reads"]
        self.points_at(5)
        self.assertLessEqual(json.loads(cmds.groomLabGraphStats(self.groom))["scalpHistory"]["reads"], reads + 1)
        # Keyed to move: the strands trail during the move, swing after it, and settle once
        # the move is older than the history.
        self.key_scalp_move()
        self.set_bypass(node, True)
        rigid = {f: self.points_at(f) for f in (4, 8, 12, 13, 30)}
        self.set_bypass(node, False)
        moving = {f: self.points_at(f) for f in (4, 8, 12, 13, 30)}
        self.assertEqual(moving[4], rigid[4])    # still until frame 5
        self.assertLess(self.mean_x(moving[8]), self.mean_x(rigid[8]) - 1e-3)  # trailing (-x) mid-move
        self.assertNotEqual(moving[12], rigid[12])
        self.assertNotEqual(moving[13], rigid[13])
        self.assertEqual(moving[30], rigid[30])  # still for longer than the history: as it was
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_jiggle_reads_a_moving_other_scalp(self):
        # Another scalp (add_scalp) keyed to move while the groom's own scalp keeps still: the hair
        # on it trails as hair on a moving scalp does (its own earlier frames are read), the head's
        # hair stays as it was.
        ear = cmds.polyPlane(width=0.8, height=0.8, subdivisionsX=6, subdivisionsY=6, name="ear")[0]
        cmds.xform(ear, translation=(1.6, 0.2, 0.0), rotation=(0, 0, 90))
        self.assertEqual(groomlab.add_scalp(self.groom, ear), "ear")
        for frame, x in ((1, 1.6), (5, 1.6), (10, 3.1), (40, 3.1)):
            cmds.setKeyframe(ear, attribute="translateX", time=frame, value=x)
        cmds.keyTangent(ear, attribute="translateX", inTangentType="linear", outTangentType="linear")
        node = groomlab.add_jiggle(self.groom, history=8)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.set_bypass(node, True)
        rigid = {f: self.points_at(f) for f in (4, 8, 30)}
        self.set_bypass(node, False)
        moving = {f: self.points_at(f) for f in (4, 8, 30)}
        self.assertEqual(moving[4], rigid[4])  # still until frame 5
        self.assertEqual(moving[30], rigid[30])  # still for longer than the history
        self.assertEqual(len(moving[8]), len(rigid[8]))
        changed = [i for i, (p, q) in enumerate(zip(moving[8], rigid[8])) if p != q]
        self.assertGreater(len(changed), 0, "the hair on the moving ear did not jiggle")
        on_ear = [rigid[8][i] for i in changed]
        self.assertTrue(all(p[0] > 1.5 for p in on_ear), "only the ear's hair trails")  # the head keeps still
        self.assertLess(self.mean_x([moving[8][i] for i in changed]), self.mean_x(on_ear) - 1e-3)  # trailing (-x)
        self.assertEqual(groomlab.errors(self.groom), [])

    def assert_export_matches_viewport(self, frames):
        viewport = {f: self.points_at(f) for f in frames}
        path = os.path.join(tempfile.mkdtemp(prefix="groomlab_jiggle_"), "jiggle.abc")
        self.assertEqual(groomlab.export_alembic(self.groom, path, start=frames[0], end=frames[-1]), frames[-1] - frames[0] + 1)
        before = set(cmds.ls(long=True))
        cmds.loadPlugin("AbcImport", quiet=True)
        cmds.AbcImport(path, mode="import")
        curves = [c for c in cmds.ls(type="nurbsCurve", long=True) if c not in before]
        self.assertGreater(len(curves), 100)
        for f in frames:
            cmds.currentTime(f)
            exported = []
            for curve in curves:
                xyz = cmds.xform(curve + ".cv[*]", query=True, worldSpace=True, translation=True)
                exported += [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]
            self.assertEqual(len(exported), len(viewport[f]), "frame %d" % f)
            worst = max(moved([p], [q]) for p, q in zip(sorted(exported), sorted(viewport[f])))
            self.assertLess(worst, 1e-4, "frame %d: the export differs from the viewport by %g" % (f, worst))
        cmds.delete([n for n in cmds.ls(long=True, assemblies=True) if n not in before])
        return viewport

    def stretch_guides_with_sim_curves(self, factor, frame):
        """Sim curves pose every guide: as authored on frame 1, scaled about the root by `factor` on `frame`."""
        curves = {}
        for gid in groomlab.guide_ids(self.groom, "primary"):
            pts = sim._guide_points(self.groom, gid)
            curve = cmds.curve(degree=1, point=pts)
            cmds.xform(curve, worldSpace=True, pivots=pts[0])
            for attr in ("scaleX", "scaleY", "scaleZ"):
                cmds.setKeyframe(curve, attribute=attr, time=1, value=1.0)
                cmds.setKeyframe(curve, attribute=attr, time=frame, value=factor)
            curves[int(gid)] = curve
        sim.connect_sim_curves(self.groom, curves)

    def viewport_instances(self, frame):
        """(the viewport's strands as point lists, each instance's origin), instance k on strand k."""
        cmds.currentTime(frame)
        m = cmds.groomLabGraphStats(self.groom, instances=True) or []
        origins = [tuple(m[i + 12:i + 15]) for i in range(0, len(m), 16)]
        points = groomlab.viewport_points(self.groom)
        self.assertGreater(len(origins), 100)
        per = len(points) // len(origins)
        self.assertEqual(per * len(origins), len(points))  # one instance per strand, equal point counts
        return [points[k * per:(k + 1) * per] for k in range(len(origins))], origins

    @staticmethod
    def length(points):
        return sum(math.sqrt(sum((a - b) ** 2 for a, b in zip(p, q))) for p, q in zip(points, points[1:]))

    @staticmethod
    def along(points, distance):
        for p, q in zip(points, points[1:]):
            seg = math.sqrt(sum((a - b) ** 2 for a, b in zip(p, q)))
            if distance <= seg:
                u = distance / seg if seg > 0 else 0.0
                return tuple(a + (b - a) * u for a, b in zip(p, q))
            distance -= seg
        return points[-1]

    def test_instances_pinned_to_rest_keep_their_distance_on_stretching_guides(self):
        # Simulated guides that stretch 1.5x by frame 10; one instance per strand, 0.8 along it.
        self.stretch_guides_with_sim_curves(1.5, 10)
        cmds.polySphere(name="flake", radius=1, subdivisionsX=6, subdivisionsY=4)
        node = groomlab.add_instances(self.groom, "flake", probability=1.0, t_range=(0.8, 0.8), scale=0.02,
                                      scale_by_width=False)
        self.assertEqual(groomlab.errors(self.groom), [])
        rest, _ = self.viewport_instances(1)
        rest_length = [self.length(s) for s in rest]

        # Not pinned: 0.8 of the length now, sliding out as the strands stretch.
        strands, origins = self.viewport_instances(10)
        self.assertGreater(sum(self.length(s) for s in strands) / sum(rest_length), 1.3)  # they did stretch
        for s, o in zip(strands, origins):
            self.assertLess(moved([o], [self.along(s, 0.8 * self.length(s))]), 1e-3)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["restEvaluations"], 0)

        # Pinned: 0.8 of the rest length from the root, on the stretched strands.
        groomlab.set_param(self.groom, node, "pinToRest", True)
        strands, pinned = self.viewport_instances(10)
        for s, o, r in zip(strands, pinned, rest_length):
            self.assertLess(moved([o], [self.along(s, 0.8 * r)]), 1e-3)
        self.assertGreater(max(moved([p], [q]) for p, q in zip(pinned, origins)), 0.05)
        self.viewport_instances(5)
        self.viewport_instances(7)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["restEvaluations"], 1)  # the rest pose: once, not per frame
        self.assertEqual(stats["instances"], len(pinned))

        # The USD export places them as the viewport does (preview density 1: the same strands).
        path = os.path.join(tempfile.mkdtemp(prefix="groomlab_pin_"), "pinned.usda")
        self.assertEqual(groomlab.export_usd(self.groom, path, start=10, end=10), 1)
        with open(path) as f:
            text = f.read()
        m = re.search(r"point3f\[\] positions\.timeSamples = \{\s*10: \[(.*?)\]", text, re.S)
        self.assertIsNotNone(m, text[:2000])
        exported = [tuple(float(v) for v in t.split(",")) for t in re.findall(r"\(([^)]*)\)", m.group(1))]
        self.assertEqual(len(exported), len(pinned))
        self.assertLess(max(moved([p], [q]) for p, q in zip(sorted(exported), sorted(pinned))), 1e-3)

    def viewport_axes(self, frame):
        """(the viewport's strands as point lists, each instance's x and y axis lengths), instance k on strand k."""
        cmds.currentTime(frame)
        m = cmds.groomLabGraphStats(self.groom, instances=True) or []
        norm = lambda v: math.sqrt(sum(c * c for c in v))
        axes = [(norm(m[i:i + 3]), norm(m[i + 4:i + 7]), norm(m[i + 8:i + 11])) for i in range(0, len(m), 16)]
        points = groomlab.viewport_points(self.groom)
        self.assertGreater(len(axes), 100)
        per = len(points) // len(axes)
        self.assertEqual(per * len(axes), len(points))
        return [points[k * per:(k + 1) * per] for k in range(len(axes))], axes

    def test_instances_fitted_to_strands_keep_their_rest_size_on_stretching_guides(self):
        # Simulated guides that stretch 1.5x by frame 10; one leaf per strand at the root, fitted:
        # 0.8 of the strand's rest length along +Y, a quarter of that across.
        self.stretch_guides_with_sim_curves(1.5, 10)
        cmds.polyPlane(name="leaf", width=1, height=1, subdivisionsX=1, subdivisionsY=1)
        node = groomlab.add_instances(self.groom, "leaf", probability=1.0, t_range=(0.0, 0.0), scale_random=0.0,
                                      scale_by_width=False, size_mode="fitStrand", fit_scale=0.8, aspect=0.25)
        self.assertEqual(groomlab.errors(self.groom), [])
        rest, axes = self.viewport_axes(1)
        rest_length = [self.length(s) for s in rest]
        for (x, y, z), r in zip(axes, rest_length):
            self.assertAlmostEqual(y, 0.8 * r, delta=1e-3)
            self.assertAlmostEqual(x, 0.25 * y, places=5)
            self.assertAlmostEqual(z, 0.25 * y, places=5)
        # Stretched: the strands are longer now, the leaves keep their rest size.
        strands, stretched = self.viewport_axes(10)
        self.assertGreater(sum(self.length(s) for s in strands) / sum(rest_length), 1.3)
        for (x, y, z), r in zip(stretched, rest_length):
            self.assertAlmostEqual(y, 0.8 * r, delta=1e-3)
            self.assertAlmostEqual(x, 0.25 * y, places=5)
        self.viewport_axes(5)
        self.viewport_axes(7)
        stats = json.loads(cmds.groomLabGraphStats(self.groom))
        self.assertEqual(stats["restEvaluations"], 1)  # fitStrand alone: the rest pose once, not per frame

        # The USD export writes the per-axis scales the viewport shows.
        path = os.path.join(tempfile.mkdtemp(prefix="groomlab_fit_"), "fitted.usda")
        self.assertEqual(groomlab.export_usd(self.groom, path, start=10, end=10), 1)
        with open(path) as f:
            text = f.read()
        m = re.search(r"float3\[\] scales\.timeSamples = \{\s*10: \[(.*?)\]", text, re.S)
        self.assertIsNotNone(m, text[:2000])
        exported = sorted(tuple(float(v) for v in t.split(",")) for t in re.findall(r"\(([^)]*)\)", m.group(1)))
        self.assertEqual(len(exported), len(stretched))
        self.assertLess(max(max(abs(a - b) for a, b in zip(p, q)) for p, q in zip(exported, sorted(stretched))), 1e-3)

        # Fixed again (the default): the size in scene units, and no rest pose to keep.
        groomlab.set_param(self.groom, node, "sizeMode", "fixed")
        _, fixed = self.viewport_axes(10)
        self.assertTrue(all(abs(x - 0.02) < 1e-6 and abs(y - 0.02) < 1e-6 for x, y, _ in fixed))

    def test_jiggle_export_matches_the_viewport(self):
        if not hasattr(cmds, "groomLabExportAlembic"):
            self.skipTest("built without Alembic")
        self.key_scalp_move()
        groomlab.add_jiggle(self.groom, history=8)
        first = self.assert_export_matches_viewport((7, 9, 11, 13))
        # The move changed (a key edited, the viewport on a frame after it): the viewport reads the
        # scalp's earlier frames again and still matches a fresh export.
        cmds.currentTime(12)
        cmds.setKeyframe(self.scalp, attribute="translateX", time=10, value=0.5)
        second = self.assert_export_matches_viewport((11, 12, 13))
        self.assertNotEqual(second[11], first[11])


class SimulateTest(unittest.TestCase):
    """The core's Simulate node in Maya: stepped as the time moves (the scalp read at the frames on
    the way), scrubbing back reads its frames, bypass gives the groom exactly, a bake is what the
    viewport showed and what exports read. nHair (groomlab.sim's other half) is untouched."""

    setUp = MotionTest.setUp
    points_at = MotionTest.points_at
    set_bypass = MotionTest.set_bypass
    mean_x = MotionTest.mean_x
    key_scalp_move = MotionTest.key_scalp_move
    assert_export_matches_viewport = MotionTest.assert_export_matches_viewport

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)

    def add(self, **params):
        node = sim.add_simulate(self.groom, gravity=9.8, collisionOffset=0.02, damping=0.2, **params)
        self.assertEqual(groomlab.errors(self.groom), [])
        return node

    def test_simulate_presets_set_the_node_in_one_step(self):
        # The core's presets (every host's): Fur sets the motion settings in one undo step, Default
        # puts back the node's defaults, both the same params; gravity is left alone.
        from groomlab import graph as gm
        node = self.add()
        self.assertEqual([p["name"] for p in sim.simulate_presets()][:2], ["default", "fur"])
        def param(name):
            return gm.GraphModel.from_groom(self.groom).param(node, name)
        fur = sim.simulate_preset(self.groom, "fur")
        self.assertIs(param("groomedUnderGravity"), True)
        self.assertAlmostEqual(param("bend"), 0.8)
        self.assertAlmostEqual(param("gravity"), 9.8)
        self.assertEqual(set(fur), set(sim.simulate_preset_params("default")))
        cmds.undo()
        self.assertIs(param("groomedUnderGravity"), True)
        self.assertAlmostEqual(param("bend"), 0.5)
        sim.simulate_preset(self.groom, "fur", node=node)
        sim.simulate_preset(self.groom, "default", node=node)
        self.assertAlmostEqual(param("bend"), 0.5)
        self.assertIs(param("groomedUnderGravity"), True)
        with self.assertRaises(ValueError):
            sim.simulate_preset(self.groom, "nope")
        self.assertEqual(groomlab.errors(self.groom), [])
        sim.simulate(self.groom, 6)

    def test_simulate_range_drives_the_main_progress_bar(self):
        # The menu's Simulate Range steps the frames one by one with Maya's main progress bar
        # (begin, a step a frame, end; cancelled: it stops after the frame being made) and says
        # how many it simulated. The bar is mocked: a mayapy session has none.
        from groomlab.ui import menu
        self.add()
        calls = []
        cancel_after = [None]

        def progress_bar(bar, **kwargs):
            calls.append((bar, kwargs))
            if kwargs.get("query") and kwargs.get("isCancelled"):
                steps = sum(1 for _, k in calls if k.get("step"))
                return cancel_after[0] is not None and steps >= cancel_after[0]
            return None
        real_bar, real_progress = menu._main_progress_bar, cmds.progressBar
        menu._main_progress_bar = lambda: "fakeBar"
        cmds.progressBar = progress_bar
        cmds.select(self.groom)
        try:
            self.assertEqual(menu.simulate_range(), 40)
            begins = [k for _, k in calls if k.get("beginProgress")]
            self.assertEqual(len(begins), 1)
            self.assertEqual((begins[0]["maxValue"], begins[0]["isInterruptable"]), (40, True))
            self.assertEqual(sum(1 for _, k in calls if k.get("step")), 40)
            self.assertEqual(sum(1 for _, k in calls if k.get("endProgress")), 1)
            self.assertEqual(sim.simulation_status(self.groom)[0]["ranges"], [[1, 40]])
            # Cancelled after three frames: three stepped, the bar ended all the same.
            sim.invalidate_simulation(self.groom)
            del calls[:]
            cancel_after[0] = 3
            self.assertEqual(menu.simulate_range(), 3)
            self.assertEqual(sum(1 for _, k in calls if k.get("endProgress")), 1)
        finally:
            menu._main_progress_bar, cmds.progressBar = real_bar, real_progress

    def test_it_steps_with_the_timeline_scrubs_back_and_bypass_gives_the_groom(self):
        calm = {f: self.points_at(f) for f in (1, 6, 12)}
        node = self.add()
        graph = groomlab.get_graph(self.groom)
        self.assertIn({"from": "guides", "to": node, "input": "guides"}, graph["connections"])
        self.assertIn({"from": node, "to": "interpolate", "input": "guides"}, graph["connections"])
        self.assertEqual(sim.simulate_nodes(self.groom), [node])
        played = {f: self.points_at(f) for f in range(1, 13)}
        self.assertEqual(played[1], calm[1])  # the start frame: the groom at rest
        self.assertNotEqual(played[12], calm[12])  # gravity pulled the guides, the strands followed
        status = sim.simulation_status(self.groom)
        self.assertEqual(status[0]["node"], node)
        self.assertEqual(status[0]["ranges"], [[1, 12]])
        # Scrubbing back reads the frames: the same strands, nothing stepped.
        for f in (6, 3, 12):
            self.assertEqual(self.points_at(f), played[f])
        self.assertEqual(sim.simulation_status(self.groom)[0]["frames"], 12)
        # Bypassed: the groom as it was, to the bit.
        self.set_bypass(node, True)
        for f in (1, 6, 12):
            self.assertEqual(self.points_at(f), calm[f])
        self.set_bypass(node, False)
        self.assertEqual(self.points_at(12), played[12])
        # A param change starts it again; Clear drops the frames (the frame shown simulates again).
        groomlab.set_param(self.groom, node, "bend", 0.9)
        self.assertNotEqual(self.points_at(12), played[12])
        cmds.currentTime(2)
        sim.invalidate_simulation(self.groom)
        self.assertEqual(sim.simulation_status(self.groom)[0]["ranges"], [[1, 2]])

    def test_a_moving_scalp_swings_the_hair_and_a_jump_steps_the_frames_on_the_way(self):
        self.key_scalp_move()
        node = self.add(rootStiffnessProfile=0.0)
        self.set_bypass(node, True)
        rigid = {f: self.points_at(f) for f in (8, 30)}
        self.set_bypass(node, False)
        cmds.currentTime(1)
        played = {f: self.points_at(f) for f in range(1, 31)}
        self.assertLess(self.mean_x(played[8]), self.mean_x(rigid[8]) - 1e-3)  # trailing the move (-x)
        # A fresh start jumping straight to frame 20: the frames on the way read the scalp then.
        sim.invalidate_simulation(self.groom)
        self.assertEqual(self.points_at(20), played[20])
        self.assertEqual(sim.simulation_status(self.groom)[0]["ranges"], [[1, 20]])

    def test_simulate_range_meets_a_moving_collider_where_playing_met_it(self):
        # A ball sweeping through the hair over frames 2-10 into the Simulate node. Played, then
        # simulated again with Simulate Range from frame 1 (what a bake runs when the frames are
        # gone): the same frames. The frames on the way read the collider then; they met it where
        # it stood at the frame shown, so the bake was not what had played.
        from groomlab import graph as gm
        ball = cmds.polySphere(radius=0.6, subdivisionsX=16, subdivisionsY=8)[0]
        cmds.setAttr(ball + ".translateY", 1.7)
        for frame, x in ((1, -2.5), (2, -2.5), (10, 2.5), (40, 2.5)):
            cmds.setKeyframe(ball, attribute="translateX", time=frame, value=x)
        cmds.keyTangent(ball, attribute="translateX", inTangentType="linear", outTangentType="linear")
        collider = groomlab.add_collider(self.groom, ball, wire=False)
        node = self.add()
        model = gm.GraphModel.from_groom(self.groom)
        model.connect(collider, node, "collider")
        model.save(self.groom)
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.currentTime(1)
        played = {f: self.points_at(f) for f in range(1, 15)}
        sim.invalidate_simulation(self.groom)
        cmds.currentTime(1)
        self.points_at(1)
        sim.simulate(self.groom, 14)
        self.assertEqual(sim.simulation_status(self.groom)[0]["ranges"], [[1, 14]])
        for f in (6, 10, 14):
            self.assertEqual(self.points_at(f), played[f], "frame %d" % f)
        # (The sweep mattered: the ball held at its first place moves the hair otherwise.)
        cmds.cutKey(ball, attribute="translateX")
        cmds.setAttr(ball + ".translateX", -2.5)
        sim.invalidate_simulation(self.groom)
        cmds.currentTime(1)
        held = {f: self.points_at(f) for f in range(1, 11)}
        self.assertNotEqual(held[10], played[10])

    def test_bake_and_exports_read_what_the_viewport_showed(self):
        if not hasattr(cmds, "groomLabExportAlembic"):
            self.skipTest("built without Alembic")
        self.key_scalp_move()
        node = self.add(cacheMode="cached")
        self.assertEqual(sim.simulate(self.groom, 20), 20)
        # The bake: a guide cache in the scene's caches folder, named in simCaches with its stamp;
        # one undo step.
        written = sim.bake_simulation(self.groom, end=20)
        self.assertTrue(os.path.exists(written[node]))
        bakes = sim.baked_simulations(self.groom)
        self.assertEqual(sorted(bakes), [node])
        self.assertEqual(len(bakes[node]["stamp"]), 16)
        status = sim.simulation_status(self.groom)[0]
        self.assertTrue(status["baked"])
        self.assertEqual((status["bakedFirst"], status["bakedLast"]), (1, 20))
        cmds.undo()
        self.assertEqual(cmds.getAttr(self.groom + ".simCaches") or "", "")
        cmds.redo()
        self.assertEqual(sorted(sim.baked_simulations(self.groom)), [node])
        # Groom files name the bake (relative to the file): the CLI and the procedural read it.
        groom_path = os.path.join(tempfile.mkdtemp(prefix="groomlab_sim_"), "baked.groom")
        groomlab.export_groom(self.groom, groom_path)
        with open(groom_path) as f:
            sims = json.load(f)["simCaches"]
        self.assertEqual(sorted(sims), [node])
        self.assertEqual(sims[node]["stamp"], bakes[node]["stamp"])
        self.assertTrue(os.path.exists(os.path.join(os.path.dirname(groom_path), sims[node]["file"])))
        # Exports evaluate their own session: it reads the bake, as the viewport shows it.
        self.assert_export_matches_viewport((14, 16, 18))
        # Other settings since the bake: it is stale (ignored) and the node simulates again.
        groomlab.set_param(self.groom, node, "damping", 0.5)
        self.points_at(18)
        status = sim.simulation_status(self.groom)[0]
        self.assertFalse(status["baked"])
        self.assertTrue(status["bakeStale"])
        sim.clear_baked_simulation(self.groom)
        self.assertEqual(sim.baked_simulations(self.groom), {})


class UtilitiesTest(unittest.TestCase):
    """GroomLab > Utilities (groomlab.utils and the menu's actions, given their choices: no dialogs)."""

    @classmethod
    def setUpClass(cls):
        if not cmds.pluginInfo("GroomLab", query=True, loaded=True):
            cmds.loadPlugin(PLUGIN_PATH)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        self.groom = groomlab.create_groom(self.scalp)
        cmds.setAttr(self.groom + ".previewDensity", 0.5)
        curves = []
        for angle in (0.35, 0.8, 1.2):
            n = (math.sin(angle), math.cos(angle), 0.2)
            curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.1 * c for c in n)]))
        groomlab.guides_from_curves(self.groom, curves)
        cmds.delete(curves)
        groomlab.mirror_group(self.groom, "primary", "x")
        groomlab.generate_children(self.groom, "primary", name="children", count=3)
        self.primaries = groomlab.guide_ids(self.groom, "primary")
        self.children = groomlab.guide_ids(self.groom, "children")

    def cvs(self, curve):
        xyz = cmds.xform(curve + ".cv[*]", query=True, worldSpace=True, translation=True)
        return [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]

    def set_density(self, groom, density):
        graph = groomlab.get_graph(groom, expand_default=True)
        for node in graph["nodes"]:
            if node["type"] == "Scatter":
                node.setdefault("params", {})["density"] = float(density)
        groomlab.set_graph(groom, graph)

    def curve_shape(self, curve):
        return cmds.listRelatives(curve, shapes=True, fullPath=True)[0]

    def test_guides_to_curves_one_tagged_curve_per_guide(self):
        from groomlab import utils
        points = guide_points(self.groom)
        made = utils.guides_to_curves(self.groom)
        self.assertEqual(made["count"], 24)
        self.assertEqual(len(made["curves"]), 24)
        self.assertEqual(made["group"], "|groomLab1_guideCurves")
        # A transform per guide group, nested as the groups are.
        self.assertEqual(cmds.listRelatives(made["group"], children=True), ["groomLab1_primary"])
        primary = made["group"] + "|groomLab1_primary"
        self.assertIn("groomLab1_children", cmds.listRelatives(primary, children=True))
        ids = set()
        for curve in made["curves"]:
            guide, group = cmds.getAttr(curve + ".glGuideId"), cmds.getAttr(curve + ".glGuideGroup")
            ids.add(guide)
            self.assertEqual(group, "primary" if guide in self.primaries else "children")
            self.assertEqual(cmds.listRelatives(curve, parent=True)[0], "groomLab1_" + group)
            self.assertEqual(cmds.getAttr(self.curve_shape(curve) + ".degree"), 1)
            self.assertEqual(len(self.cvs(curve)), len(points[guide]))  # degree 1: the guide's points, exactly
            self.assertLess(moved(self.cvs(curve), points[guide]), 1e-5)
        self.assertEqual(ids, set(self.primaries) | set(self.children))
        cmds.undo()  # one step
        self.assertFalse(cmds.objExists(made["group"]))
        cmds.redo()
        self.assertEqual(len(cmds.listRelatives(made["group"], allDescendents=True, type="nurbsCurve")), 24)

        # One group, flat; the selected guides; a cubic through the points.
        children = utils.guides_to_curves(self.groom, group="children", keep_hierarchy=False, name="kids")
        self.assertEqual(children["count"], 18)
        self.assertEqual(len(cmds.listRelatives(children["group"], children=True)), 18)
        self.assertEqual(children["group"], "|kids")
        groomlab.select_guides(self.groom, guides=self.primaries[:2])
        picked = utils.guides_to_curves(self.groom, selection=True)
        self.assertEqual(sorted(cmds.getAttr(c + ".glGuideId") for c in picked["curves"]), sorted(self.primaries[:2]))
        cubic = utils.guides_to_curves(self.groom, group="primary", degree=3)
        self.assertEqual(cmds.getAttr(self.curve_shape(cubic["curves"][0]) + ".degree"), 3)
        # Its edit points are the guides' points: brought back unchanged, nothing moves.
        self.assertEqual(utils.curves_to_guides(self.groom, cubic["group"])["updated"], [])
        groomlab.clear_guide_selection(self.groom)
        with self.assertRaises(ValueError):
            utils.guides_to_curves(self.groom, selection=True)

    def test_curves_round_trip_update_guides_in_place(self):
        from groomlab import utils
        before = guide_points(self.groom)
        data = cmds.getAttr(self.groom + ".guideData")
        made = utils.guides_to_curves(self.groom)
        edited = next(c for c in made["curves"] if cmds.getAttr(c + ".glGuideId") == self.primaries[0])
        cmds.move(0.25, 0.1, 0.0, edited + ".cv[1:]", relative=True, worldSpace=True)
        loose = cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.5, 0.1), (0.0, 2.0, 0.2)])
        stray = cmds.curve(degree=1, point=[(0.0, -1.0, 0.0), (0.0, -1.5, 0.1), (0.0, -2.0, 0.2)])
        cmds.addAttr(stray, longName="glGuideId", dataType="string")
        cmds.setAttr(stray + ".glGuideId", "999999", type="string")  # a guide this groom does not have

        result = utils.curves_to_guides(self.groom, [made["group"], loose, stray])
        self.assertEqual(result["updated"], [self.primaries[0]])
        self.assertEqual(len(result["unchanged"]), 23)
        self.assertEqual(len(result["added"]), 2)
        after = guide_points(self.groom)
        self.assertEqual(set(after), set(before) | set(result["added"]))  # same IDs, two new
        self.assertLess(moved(after[self.primaries[0]], self.cvs(edited)), 1e-4)  # the curve's shape, in place
        self.assertGreater(moved(after[self.primaries[0]], before[self.primaries[0]]), 0.1)
        # Its children followed it; the other guides did not move.
        kids = [str(g["id"]) for g in groomlab.get_hierarchy(self.groom)["guides"]
                if g.get("parent") == int(self.primaries[0])]
        self.assertEqual(len(kids), 3)
        for kid in kids:
            self.assertGreater(moved(after[kid], before[kid]), 0.05)
        for guide in self.primaries[1:]:
            self.assertLess(moved(after[guide], before[guide]), 1e-5)
        self.assertEqual(groomlab.errors(self.groom), [])

        cmds.undo()  # one step: every guide as it was
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), data)
        cmds.redo()
        self.assertEqual(len(guide_points(self.groom)), 26)
        # Without update, every curve is new.
        again = utils.curves_to_guides(self.groom, [edited], update=False, group="extra")
        self.assertEqual((again["updated"], len(again["added"])), ([], 1))
        self.assertEqual(groomlab.guide_ids(self.groom, "extra"), again["added"])

    def test_strands_to_curves_capped_by_strand_id(self):
        from groomlab import utils
        groomlab.paint_scalp_map(self.groom, "mask", paint=False)  # a strand channel "mask"
        few = utils.strands_to_curves(self.groom, max_curves=50)
        self.assertEqual(few["count"], 50)
        self.assertTrue(few["capped"])
        self.assertGreater(few["total"], 50)
        shape = self.curve_shape(few["curves"][0])
        attrs = cmds.listAttr(shape, userDefined=True)
        for attr in ("glStrandId", "glWidth", "glMask"):
            self.assertIn(attr, attrs)
        self.assertEqual(len(cmds.getAttr(shape + ".glWidth")), len(self.cvs(few["curves"][0])))
        ids = lambda made: {cmds.getAttr(self.curve_shape(c) + ".glStrandId") for c in made["curves"]}
        first = ids(few)
        cmds.undo()
        self.assertFalse(cmds.objExists(few["group"]))
        # Deterministic: the same strands again, and a smaller cap keeps a subset.
        self.assertEqual(ids(utils.strands_to_curves(self.groom, max_curves=50)), first)
        self.assertTrue(ids(utils.strands_to_curves(self.groom, max_curves=20)) <= first)
        bare = utils.strands_to_curves(self.groom, max_curves=5, attributes=False, degree=3)
        self.assertEqual(cmds.listAttr(self.curve_shape(bare["curves"][0]), userDefined=True), None)
        self.assertEqual(cmds.getAttr(self.curve_shape(bare["curves"][0]) + ".degree"), 3)
        # Any node's output: the scatter's strands, before Interpolate grows them.
        scattered = utils.strands_to_curves(self.groom, node="scatter", max_curves=10)
        self.assertEqual(scattered["count"], 10)
        with self.assertRaises(RuntimeError):
            utils.strands_to_curves(self.groom, node="nope")

        # 20,000 curves in one go.
        self.set_density(self.groom, 3000.0)
        start = time.time()
        big = utils.strands_to_curves(self.groom, max_curves=20000)
        took = time.time() - start
        sys.stderr.write("\nstrands_to_curves: %d of %d strands in %.2f s\n" % (big["count"], big["total"], took))
        self.assertEqual(big["count"], 20000)
        self.assertEqual(len(big["curves"]), 20000)
        self.assertLess(took, 60.0)
        start = time.time()
        cmds.undo()
        sys.stderr.write("undo of 20,000 curves: %.2f s\n" % (time.time() - start))
        self.assertFalse(cmds.objExists(big["group"]))

    def test_strands_to_mesh_tubes_and_ribbons(self):
        from maya.api import OpenMaya as om
        from groomlab import utils
        points = [cmds.getAttr(self.curve_shape(c) + ".spans") + 1
                  for c in utils.strands_to_curves(self.groom, max_curves=40, attributes=False)["curves"]]
        tube = utils.strands_to_mesh(self.groom, sides=4, max_strands=40)
        self.assertTrue(tube["capped"])
        self.assertEqual(tube["strands"], 40)
        self.assertEqual(tube["vertices"], sum(p * 4 + 1 for p in points))  # a ring per point and the tip
        self.assertEqual(tube["faces"], sum((p - 1) * 4 + 4 for p in points))
        mesh = cmds.listRelatives(tube["mesh"], shapes=True, fullPath=True)[0]
        self.assertIn("initialShadingGroup", cmds.listConnections(mesh, type="shadingEngine") or [])
        sel = om.MSelectionList()
        sel.add(mesh)
        fn = om.MFnMesh(sel.getDagPath(0))
        us, vs = fn.getUVs()
        self.assertEqual((min(us), max(us), min(vs), max(vs)), (0.0, 1.0, 0.0, 1.0))
        self.assertEqual(list(fn.getUVSetNames()), ["map1"])
        cmds.undo()  # one step
        self.assertFalse(cmds.objExists(tube["mesh"]))
        cmds.redo()
        self.assertTrue(cmds.objExists(tube["mesh"]))

        ribbon = utils.strands_to_mesh(self.groom, style="ribbon", max_strands=40, strand_uvs=True, width_scale=2.0)
        self.assertEqual(ribbon["vertices"], sum(p * 2 for p in points))
        self.assertEqual(ribbon["faces"], sum(p - 1 for p in points))
        sel = om.MSelectionList()
        sel.add(cmds.listRelatives(ribbon["mesh"], shapes=True, fullPath=True)[0])
        fn = om.MFnMesh(sel.getDagPath(0))
        self.assertEqual(list(fn.getUVSetNames()), ["map1", "strandUVs"])
        us, vs = fn.getUVs("strandUVs")
        self.assertTrue(0.0 < min(us) and max(us) < 1.0 and 0.0 < min(vs) and max(vs) < 1.0)
        with self.assertRaises(ValueError):
            utils.strands_to_mesh(self.groom, style="cards")

        self.set_density(self.groom, 3000.0)
        start = time.time()
        big = utils.strands_to_mesh(self.groom, max_strands=20000)
        sys.stderr.write("\nstrands_to_mesh: %d strands, %d faces in %.2f s\n"
                         % (big["strands"], big["faces"], time.time() - start))
        self.assertEqual(big["strands"], 20000)

    def test_bake_node_to_guides_from_a_scatter(self):
        from groomlab import utils
        from groomlab.ui import menu
        self.assertIn("scatter", utils.bake_nodes(self.groom))
        self.assertNotIn("scalp", utils.bake_nodes(self.groom))
        count = len(groomlab.get_hierarchy(self.groom)["guides"])
        ids = utils.bake_guides(self.groom, "scatter", group="baked", density=0.05)
        self.assertGreater(len(ids), 5)
        self.assertEqual(sorted(groomlab.guide_ids(self.groom, "baked")), sorted(ids))
        cmds.undo()
        self.assertEqual(len(groomlab.get_hierarchy(self.groom)["guides"]), count)
        # The menu's action, given its choices.
        again = menu.bake_node_dialog(node="scatter", group="scattered", density=0.05, groom=self.groom)
        self.assertEqual(sorted(groomlab.guide_ids(self.groom, "scattered")), sorted(again))
        # The groom's output bakes too ("(output)", as the other Utilities offer it), whatever the
        # output node is named; the node list never shows the output node twice.
        final = menu.bake_node_dialog(node="(output)", group="final", density=0.05, groom=self.groom)
        self.assertGreater(len(final), 5)
        self.assertEqual(menu._node_choices(self.groom)[0], "(output)")
        model = groomlab.graph.GraphModel.from_groom(self.groom)
        model.rename_node(model.output, "final")
        model.save(self.groom)
        choices = menu._node_choices(self.groom)
        self.assertNotIn("final", choices)
        self.assertIn("(output)", choices)
        self.assertGreater(len(menu.bake_node_dialog(node="(output)", group="final2", density=0.05, groom=self.groom)), 5)

    def test_menu_actions_call_through(self):
        from groomlab.ui import menu
        made = menu.guides_to_curves_action(groom=self.groom)
        self.assertEqual(made["count"], 24)
        back = menu.curves_to_guides_dialog(group="primary", curves=[made["group"]], groom=self.groom)
        self.assertEqual((back["updated"], back["added"]), ([], []))
        curves = menu.strands_to_curves_dialog(node="(output)", density=1.0, max_curves=12, attributes=True, degree=1,
                                               groom=self.groom)
        self.assertEqual(curves["count"], 12)
        mesh = menu.strands_to_mesh_dialog(node="scatter", style="ribbon", sides=4, width_scale=1.0, max_strands=12,
                                           strand_uvs=False, groom=self.groom)
        self.assertEqual(mesh["strands"], 12)
        other = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0], name="other")
        report = menu.copy_graph_dialog(guides="keep", maps=False, source=self.groom, targets=[other])
        self.assertEqual(list(report), [other])

    def test_create_groom_from_the_menu_on_an_untitled_scene_opens_no_dialog(self):
        # An untitled scene goes on (its files move beside the scene on its first save): no
        # confirm or prompt dialog, a note over the viewport instead.
        from groomlab.ui import menu
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10)[0]
        cmds.select(scalp)

        def no_dialog(*_, **__):
            raise AssertionError("a modal dialog opened")
        confirm, prompt, ask = cmds.confirmDialog, cmds.promptDialog, menu.ASK_TO_SAVE
        cmds.confirmDialog = cmds.promptDialog = no_dialog
        menu.ASK_TO_SAVE = True
        try:
            self.assertFalse(groomlab.scene_folder())
            self.assertTrue(menu.ensure_scene_saved("this groom's files"))
            menu.create_groom_from_selection()
        finally:
            cmds.confirmDialog, cmds.promptDialog, menu.ASK_TO_SAVE = confirm, prompt, ask
        groom = cmds.ls(selection=True, dag=True, type=groomlab.GROOM_TYPE)
        self.assertEqual(len(groom), 1)
        self.assertEqual(groomlab.scalp_mesh(groom[0]), cmds.listRelatives(scalp, shapes=True, fullPath=False)[0])
        # Generate Children takes its count inline, never from a prompt.
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0, 1.5, 0.2), (0, 2, 0.4)])
        groomlab.guides_from_curves(groom[0], [curve])
        cmds.promptDialog = no_dialog
        try:
            self.assertEqual(menu.generate_children_dialog(count=2, group="primary", groom=groom[0]), 2)
            menu.CHILDREN_COUNT = 3
            self.assertEqual(menu.generate_children_dialog(group="primary", groom=groom[0]), 3)  # the inline count
        finally:
            cmds.promptDialog = prompt
            menu.CHILDREN_COUNT = 5

    def test_add_submenu_actions_call_the_apis(self):
        from groomlab.ui import menu

        def types():
            return [n["type"] for n in groomlab.get_graph(self.groom, expand_default=True)["nodes"]]
        self.assertEqual(menu.add_wind_action(groom=self.groom), "wind")
        self.assertIn("Wind", types())
        self.assertEqual(menu.add_jiggle_action(groom=self.groom), "jiggle")
        self.assertIn("Jiggle", types())
        cards = menu.add_cards_action("low", groom=self.groom)
        self.assertIn("Cards", types())
        self.assertTrue(cards)
        other = cmds.polySphere(radius=0.5, subdivisionsX=12, subdivisionsY=8)[0]
        cmds.move(0, -2, 0, other)
        name = menu.add_collider_action(groom=self.groom, mesh=cmds.listRelatives(other, shapes=True, fullPath=True)[0])
        self.assertIn("ColliderMesh", types())
        self.assertIn("Collide", types())
        self.assertTrue(name)
        path = cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.4, 0.4), (0.0, 1.6, 0.9)])
        cmds.select(path)
        braid = menu.add_braid_dialog(style="rope", groom=self.groom)
        nodes = {n["name"]: n for n in groomlab.get_graph(self.groom, expand_default=True)["nodes"]}
        self.assertEqual(nodes[braid]["type"], "Braid")
        self.assertEqual(nodes[braid]["params"]["style"], "rope")
        cmds.select(self.groom)
        made = menu.add_feathers_dialog(design="contour", source="scatter", groom=self.groom)
        self.assertIn("shafts", made)
        self.assertIn("Compound", types())

    def test_transfer_to_a_second_scalp_from_the_menu(self):
        from groomlab.ui import menu
        groomlab.paint_scalp_map(self.groom, "mask", paint=False)  # a vertex map
        part = cmds.curve(degree=1, point=[(0.0, math.cos(a), math.sin(a)) for a in (-0.4, 0.0, 0.4)])
        groomlab.add_part(self.groom, [part], name="crown", delete_curves=True)
        head = cmds.polySphere(radius=2.0, subdivisionsX=24, subdivisionsY=12)[0]
        cmds.move(6, 0, 0, head)
        # Painted maps not projected: the vertex map stays with the old mesh's vertices.
        result = menu.transfer_dialog(scalp=head, fit=True, groom=self.groom, maps=False, color_sets=False)
        self.assertEqual((result["guides"], result["parts"], result["missing"]), (24, 1, ["mask"]))
        self.assertEqual(result["projected"], [])
        self.assertIn("moved 24 guides, 1 part and 0 flow arrows", result["message"])
        self.assertIn("mask", result["message"])
        self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True), cmds.listRelatives(head, shapes=True))
        for pts in guide_points(self.groom).values():
            self.assertAlmostEqual(math.sqrt((pts[0][0] - 6) ** 2 + pts[0][1] ** 2 + pts[0][2] ** 2), 2.0, delta=0.05)
        cmds.undo()  # one step
        self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True),
                         cmds.listRelatives(self.scalp, shapes=True))
        # The dialog's default: projected onto the new scalp.
        result = menu.transfer_dialog(scalp=head, fit=True, groom=self.groom, maps=True, color_sets=False)
        self.assertEqual((result["missing"], result["projected"]), ([], ["mask"]))
        self.assertIn("projected 1 painted map: mask", result["message"])
        self.assertEqual(len(cmds.groomLabScalpMaps(self.groom, values="mask")), cmds.polyEvaluate(head, vertex=True))
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_transfer_by_markers_onto_a_moved_turned_scaled_copy(self):
        from maya.api import OpenMaya as om
        from groomlab import utils, transfer
        from groomlab.ui import menu
        head = cmds.duplicate(self.scalp, name="newHead")[0]
        cmds.xform(head, rotation=(30, 50, 10), scale=(1.5, 1.5, 1.5), translation=(8, 2, -3), worldSpace=True)
        matrix = om.MMatrix(cmds.xform(head, query=True, matrix=True, worldSpace=True))

        def place(p):
            q = om.MPoint(*p) * matrix
            return (q.x, q.y, q.z)

        before = guide_points(self.groom)
        picks = [45, 170, 333, 500, 610, 761]
        pairs = [("%s.vtx[%d]" % (self.scalp, i), "%s.vtx[%d]" % (head, i)) for i in picks]

        result = utils.transfer_to_scalp(self.groom, head, method="markersAlign", markers=pairs[:4], maps=False)
        self.assertEqual(result["method"], "markersAlign")
        self.assertIn("markers: align", result["message"])
        after = guide_points(self.groom)
        for g in self.primaries:
            self.assertLess(moved([after[g][0]], [place(before[g][0])]), 0.03, "root of %s where it belongs" % g)
            self.assertLess(moved(after[g], [place(p) for p in before[g]]), 0.06, "the shape turned and scaled")
        # Stored on the groom, each side snapped: a face and its point.
        stored = transfer.get_transfer_markers(self.groom)
        self.assertEqual(len(stored), 4)
        for pair in stored:
            self.assertIn("face", pair["old"])
            self.assertIn("face", pair["new"])
        self.assertLess(moved([stored[1]["new"]["point"]],
                              [cmds.pointPosition("%s.vtx[%d]" % (head, picks[1]), world=True)]), 1e-3)

        # One undo step: the old scalp and no markers again.
        cmds.undo()
        self.assertEqual(transfer.get_transfer_markers(self.groom), [])
        self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True), cmds.listRelatives(self.scalp, shapes=True))

        # Closest point shows why: on a turned copy the guides land elsewhere.
        utils.transfer_to_scalp(self.groom, head, maps=False)
        wrong = guide_points(self.groom)
        self.assertGreater(max(moved([wrong[g][0]], [place(before[g][0])]) for g in self.primaries), 0.5)
        cmds.undo()

        # Markers stored beforehand are reused (a transfer again, the dialog's way).
        transfer.set_transfer_markers(self.groom, pairs)
        self.assertEqual(len(transfer.get_transfer_markers(self.groom)), 6)
        result = menu.transfer_dialog(scalp=head, method="markersWarp", groom=self.groom, maps=False, color_sets=False)
        self.assertEqual(result["method"], "markersWarp")
        after = guide_points(self.groom)
        for g in self.primaries:
            self.assertLess(moved([after[g][0]], [place(before[g][0])]), 0.05)
        cmds.undo()

        # keep_lengths: turned, not scaled.
        utils.transfer_to_scalp(self.groom, head, method="markersAlign", markers=pairs, keep_lengths=True, maps=False)
        after = guide_points(self.groom)
        length = lambda pts: sum(moved([a], [b]) for a, b in zip(pts, pts[1:]))
        for g in self.primaries:
            self.assertAlmostEqual(length(after[g]), length(before[g]), delta=0.02)
        cmds.undo()

        # Too few markers: a clear error, nothing changed.
        with self.assertRaises(RuntimeError):
            utils.transfer_to_scalp(self.groom, head, method="markersAlign", markers=pairs[:2], maps=False)
        self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True), cmds.listRelatives(self.scalp, shapes=True))
        with self.assertRaises(ValueError):
            utils.transfer_to_scalp(self.groom, head, method="nearest")

        # Add Pair From Selection: a component on each mesh, in either order.
        cmds.select(["%s.vtx[%d]" % (head, 333), "%s.f[100]" % self.scalp])
        pair = transfer.pair_from_selection(self.groom, head)
        self.assertLess(moved([pair["new"]["point"]], [cmds.pointPosition("%s.vtx[333]" % head, world=True)]), 1e-3)
        self.assertGreaterEqual(pair["old"]["face"], 0)
        cmds.select("%s.vtx[3]" % head)
        with self.assertRaises(ValueError):
            transfer.pair_from_selection(self.groom, head)
        self.assertIn("1: old face", transfer.describe_marker(0, pair))
        group = transfer.show_markers(self.groom, head, [pair, stored[0]])
        self.assertEqual(len(cmds.listRelatives(group, allDescendents=True, type="locator")), 4)
        transfer.clear_markers()
        self.assertFalse(cmds.objExists(transfer.MARKER_GROUP))

        # The marker views' picking: a ray against the mesh (face and barycentrics), the closest
        # point (a symmetry mirror snapped on), the bounds the mirror plane comes from.
        top = cmds.pointPosition("%s.vtx[333]" % head, world=True)
        hit = transfer.pick_mesh(head, (top[0] * 3 - 10, top[1] * 3, top[2] * 3),
                                 [top[i] - (top[0] * 3 - 10, top[1] * 3, top[2] * 3)[i] for i in range(3)])
        self.assertIsNotNone(hit)
        self.assertGreaterEqual(hit["face"], 0)
        self.assertGreaterEqual(hit["triangle"], 0)
        self.assertIsNone(transfer.pick_mesh(head, (100, 100, 100), (1, 0, 0)))
        center, size = transfer.mesh_bounds(head)
        self.assertGreater(size, 0)
        snapped = transfer.closest_on_mesh(head, [center[0], center[1] + 100, center[2]])
        self.assertLess(moved([snapped], [center]), 100)

    # Painted maps carried onto another scalp: shaded by position on the old head, they must be
    # shaded the same by position on the new one.

    @staticmethod
    def directions(mesh, center=(0.0, 0.0, 0.0)):
        """Each vertex's direction from `center`."""
        flat = cmds.xform(mesh + ".vtx[*]", query=True, worldSpace=True, translation=True)
        out = []
        for i in range(0, len(flat), 3):
            d = [flat[i] - center[0], flat[i + 1] - center[1], flat[i + 2] - center[2]]
            n = math.sqrt(sum(c * c for c in d)) or 1.0
            out.append([c / n for c in d])
        return out

    @staticmethod
    def shade(d):
        """A color by direction: r a gradient up the head, g its inverse, b across, a front to back."""
        f = 0.5 + 0.35 * d[1] + 0.1 * d[0]
        return [f, 1.0 - f, 0.5 + 0.4 * d[2], 0.6 + 0.3 * d[0]]

    def paint_by_position(self, groom, mesh):
        """Vertex maps grad (gray) and tint (color), and texture maps gradTex and tintTex made
        from the same, each shaded by position on `mesh`."""
        cmds.workspace(tempfile.mkdtemp(prefix="groomlab_transfer_").replace("\\", "/"), openWorkspace=True)
        dirs = self.directions(mesh)
        gray = [round(self.shade(d)[0], 4) for d in dirs]
        color = [round(c, 4) for d in dirs for c in self.shade(d)]
        entries = {"grad": {"kind": "vertices", "values": gray},
                   "tint": {"kind": "vertices", "channels": 4, "values": color},
                   "gradTex": {"kind": "vertices", "values": gray},
                   "tintTex": {"kind": "vertices", "channels": 4, "values": color}}
        cmds.setAttr(groom + ".scalpMaps", json.dumps(entries), type="string")
        groomlab.convert_map(groom, "gradTex", "texture", resolution=128)
        groomlab.convert_map(groom, "tintTex", "texture", resolution=128)

    def assert_shaded(self, groom, mesh, center=(0.0, 0.0, 0.0)):
        """The four maps follow the shading by position on `mesh` (the textures read back at its
        vertices, the conversion undone)."""
        dirs = self.directions(mesh, center)
        want = [self.shade(d) for d in dirs]
        for name, channels, kind in (("grad", 1, "vertices"), ("tint", 4, "vertices"), ("gradTex", 1, "texture"),
                                     ("tintTex", 4, "texture")):
            self.assertEqual(groomlab.map_channels(groom, name), channels)
            self.assertEqual(groomlab.map_kind(groom, name), kind)
            if kind == "texture":
                groomlab.convert_map(groom, name, "vertices")
            values = cmds.groomLabScalpMaps(groom, values=name)
            if kind == "texture":
                cmds.undo()
                self.assertEqual(groomlab.map_kind(groom, name), "texture")
            self.assertEqual(len(values), len(dirs) * channels)
            errors = [abs(values[i * channels + c] - want[i][c]) for i in range(len(dirs)) for c in range(channels)]
            self.assertLess(sum(errors) / len(errors), 0.015, name)
            self.assertLess(max(errors), 0.08, name)

    def test_transfer_projects_painted_maps(self):
        self.paint_by_position(self.groom, self.scalp)
        old = {n: groomlab.map_texture(self.groom, n) for n in ("gradTex", "tintTex")}
        old_md5 = {n: md5(p) for n, p in old.items()}
        self.assert_shaded(self.groom, self.scalp)
        stored = cmds.getAttr(self.groom + ".scalpMaps")
        # Another head: elsewhere, twice the size, another resolution (fitted).
        head = cmds.polySphere(radius=2.0, subdivisionsX=24, subdivisionsY=12)[0]
        cmds.move(6, 0, 0, head)
        report = {}
        self.assertEqual(groomlab.transfer_groom(self.groom, head, fit=True, report=report), [])
        self.assertEqual(report["projected"], ["grad", "gradTex", "tint", "tintTex"])
        new = {n: groomlab.map_texture(self.groom, n) for n in ("gradTex", "tintTex")}
        for n in new:
            # A new file in the groom's masks folder; the old one untouched (others may read it).
            self.assertNotEqual(os.path.normcase(new[n]), os.path.normcase(old[n]))
            self.assertTrue(os.path.exists(new[n]))
            self.assertIn("/masks/%s/" % groomlab.asset_name(self.groom), new[n])
            self.assertEqual(md5(old[n]), old_md5[n])
        with open(new["tintTex"], "rb") as f:
            self.assertEqual(png_info(f.read())[2], 6)  # still RGBA
        with open(new["gradTex"], "rb") as f:
            self.assertEqual(png_info(f.read())[:2], (128, 128))  # the source's size, gray
        self.assert_shaded(self.groom, head, center=(6.0, 0.0, 0.0))
        self.assertEqual(groomlab.errors(self.groom), [])
        cmds.undo()  # one step: the maps as they were, the new files gone
        self.assertEqual(cmds.getAttr(self.groom + ".scalpMaps"), stored)
        for n in new:
            self.assertFalse(os.path.exists(new[n]))
            self.assertEqual(md5(old[n]), old_md5[n])
        cmds.redo()
        for n in new:
            self.assertTrue(os.path.exists(new[n]))
            self.assertEqual(groomlab.map_texture(self.groom, n), new[n])
        cmds.undo()
        # Without projection: the maps stay as they were, and the vertex map the graph reads
        # (another count of values than the new scalp's vertices) is reported.
        groomlab.use_map(self.groom, "scatter", "density", "grad")
        stored = cmds.getAttr(self.groom + ".scalpMaps")
        report = {}
        self.assertEqual(groomlab.transfer_groom(self.groom, head, fit=True, maps=False, report=report), ["grad"])
        self.assertEqual(report["projected"], [])
        self.assertEqual(cmds.getAttr(self.groom + ".scalpMaps"), stored)
        self.assertEqual(groomlab.map_texture(self.groom, "gradTex"), old["gradTex"])

    def test_transfer_reports_maps_it_cannot_carry(self):
        from groomlab import utils
        self.paint_by_position(self.groom, self.scalp)
        os.remove(groomlab.map_texture(self.groom, "gradTex"))  # an image gone missing
        entries = json.loads(cmds.getAttr(self.groom + ".scalpMaps"))
        entries["grad"]["values"] = entries["grad"]["values"][:10]  # values that fit no scalp
        cmds.setAttr(self.groom + ".scalpMaps", json.dumps(entries), type="string")
        head = cmds.polySphere(radius=1.0, subdivisionsX=24, subdivisionsY=12)[0]
        result = utils.transfer_to_scalp(self.groom, head)
        self.assertEqual(result["projected"], ["tint", "tintTex"])
        self.assertEqual(result["missing"], ["grad", "gradTex"])
        self.assertIn("not carried", result["message"])
        self.assertEqual(json.loads(cmds.getAttr(self.groom + ".scalpMaps"))["grad"]["values"],
                         entries["grad"]["values"])

    def test_transfer_copies_the_color_sets_the_graph_reads(self):
        # A color set painted on the old scalp: red on the upper half. The graph reads it.
        mesh = cmds.listRelatives(self.scalp, shapes=True)[0]
        cmds.polyColorSet(mesh, create=True, colorSet="thick", representation="RGBA")
        cmds.polyColorSet(mesh, currentColorSet=True, colorSet="thick")
        cmds.polyColorPerVertex(self.scalp + ".vtx[*]", rgb=(0, 0, 0), alpha=1)
        dirs = self.directions(self.scalp)
        upper = ["%s.vtx[%d]" % (self.scalp, i) for i, d in enumerate(dirs) if d[1] > 0.0]
        cmds.polyColorPerVertex(upper, rgb=(1, 0, 0), alpha=1)
        cmds.polyColorSet(mesh, create=True, colorSet="unused", representation="RGBA")
        groomlab.set_param(self.groom, "scatter", "density", groomlab.color_set_map("thick"))
        head = cmds.polySphere(radius=2.0, subdivisionsX=24, subdivisionsY=12)[0]
        cmds.move(-5, 0, 0, head)
        head_mesh = cmds.listRelatives(head, shapes=True)[0]
        # Left behind by default: reported.
        self.assertEqual(groomlab.transfer_groom(self.groom, head, fit=True), ["thick"])
        cmds.undo()
        report = {}
        self.assertEqual(groomlab.transfer_groom(self.groom, head, fit=True, color_sets=True, report=report), [])
        self.assertEqual(report["colorSets"], ["thick"])  # only what the graph reads
        self.assertEqual(cmds.polyColorSet(head_mesh, query=True, allColorSets=True), ["thick"])
        self.assertEqual(cmds.ls(cmds.listHistory(head_mesh) or [], type="transferAttributes"), [])  # nothing live
        import maya.api.OpenMaya as om
        colors = om.MFnMesh(om.MSelectionList().add(head_mesh).getDagPath(0)).getVertexColors("thick")
        for i, d in enumerate(self.directions(head, (-5.0, 0.0, 0.0))):
            if abs(d[1]) > 0.3:
                self.assertAlmostEqual(colors[i].r, 1.0 if d[1] > 0 else 0.0, delta=0.05)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.ls("groomLabColorSource*"), [])  # the moved copy is cleaned up
        cmds.undo()  # one step
        self.assertFalse(cmds.polyColorSet(head_mesh, query=True, allColorSets=True))
        self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True), [mesh])

    def transfer_state(self, head_mesh):
        """What a transfer changes: the scalp and rest mesh connections, the guides (points and
        hierarchy), parts, flow, painted maps, stored markers and the new mesh's color sets."""
        return {"scalp": cmds.listConnections(self.groom + ".inMesh", shapes=True),
                "rest": groomlab.rest_mesh(self.groom),
                "guideData": cmds.getAttr(self.groom + ".guideData"),
                "guides": guide_points(self.groom),
                "hierarchy": json.dumps(groomlab.get_hierarchy(self.groom), sort_keys=True),
                "parts": groomlab.list_parts(self.groom),
                "flow": cmds.getAttr(self.groom + ".flowData"),
                "maps": cmds.getAttr(self.groom + ".scalpMaps"),
                "markers": cmds.getAttr(self.groom + ".transferMarkers"),
                "colorSets": cmds.polyColorSet(head_mesh, query=True, allColorSets=True) or [],
                "history": sorted(cmds.listHistory(head_mesh) or []),
                "nodes": sorted(cmds.ls(dependencyNodes=True))}

    def assert_same_state(self, got, want):
        for key in want:
            if key == "guides":
                self.assertEqual(sorted(got[key]), sorted(want[key]))
                for g in want[key]:
                    self.assertLess(moved(got[key][g], want[key][g]), 1e-5, "guide %s" % g)
            else:
                self.assertEqual(got[key], want[key], key)

    def test_transfer_undoes_in_one_step(self):
        """A transfer by markers (warp) with maps projected and color sets copied: one undo puts
        back everything it changed, one redo does it all again."""
        from groomlab import utils
        part = cmds.curve(degree=1, point=[(0.0, math.cos(a), math.sin(a)) for a in (-0.4, 0.0, 0.4)])
        groomlab.add_part(self.groom, [part], name="crown", delete_curves=True)
        groomlab.flow_sink(self.groom, (0.3, 0.9, 0.3), kind="whorl", radius=0.4)
        groomlab.set_rest_mesh(self.groom)
        head = cmds.duplicate(self.scalp, name="newHead")[0]
        cmds.xform(head, rotation=(10, 40, 0), scale=(1.2, 1.1, 1.2), translation=(6, 1, 0), worldSpace=True)
        head_mesh = cmds.listRelatives(head, shapes=True)[0]
        self.paint_by_position(self.groom, self.scalp)
        mesh = cmds.listRelatives(self.scalp, shapes=True)[0]
        cmds.polyColorSet(mesh, create=True, colorSet="thick", representation="RGBA")
        cmds.polyColorSet(mesh, currentColorSet=True, colorSet="thick")
        cmds.polyColorPerVertex(self.scalp + ".vtx[*]", rgb=(1, 0, 0), alpha=1)
        groomlab.set_param(self.groom, "scatter", "density", groomlab.color_set_map("thick"))
        pairs = [("%s.vtx[%d]" % (self.scalp, i), "%s.vtx[%d]" % (head, i)) for i in (45, 170, 333, 500, 610, 761)]
        cmds.flushUndo()  # the undo queue holds this test's setup no more: one undo can only be the transfer's
        before = self.transfer_state(head_mesh)
        self.assertTrue(before["parts"])
        self.assertTrue(before["flow"])
        self.assertIsNotNone(before["rest"])

        result = utils.transfer_to_scalp(self.groom, head, method="markersWarp", markers=pairs, maps=True,
                                         color_sets=True)
        self.assertEqual(result["method"], "markersWarp")
        self.assertEqual(result["colorSets"], ["thick"])
        self.assertEqual(result["projected"], ["gradTex", "tintTex"])  # new images (the vertex maps fit as they are)
        after = self.transfer_state(head_mesh)
        self.assertEqual(after["scalp"], [head_mesh])
        self.assertNotEqual(after["maps"], before["maps"])
        self.assertIn("thick", after["colorSets"])
        self.assertTrue(after["markers"])

        cmds.undo()
        self.assert_same_state(self.transfer_state(head_mesh), before)
        self.assertEqual(cmds.undoInfo(query=True, undoName=True) or "", "")  # nothing else to undo: one step
        cmds.redo()
        self.assert_same_state(self.transfer_state(head_mesh), after)
        cmds.undo()
        self.assert_same_state(self.transfer_state(head_mesh), before)

    def test_copy_graph_to_other_grooms(self):
        from groomlab import utils
        source = self.groom
        twin_scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        cmds.move(5, 0, 0, twin_scalp)
        twin = groomlab.create_groom(twin_scalp, name="twin")
        big_scalp = cmds.polySphere(radius=2, subdivisionsX=24, subdivisionsY=12)[0]
        cmds.move(-6, 0, 0, big_scalp)
        big = groomlab.create_groom(big_scalp, name="big")
        for groom, center, radius in ((twin, 5.0, 1.0), (big, -6.0, 2.0)):
            curve = cmds.curve(degree=1, point=[(center, radius, 0.0), (center, radius + 1, 0.0)])
            groomlab.guides_from_curves(groom, [curve])
        groomlab.set_param(source, "scatter", "density", 55.0)
        attr = groomlab.add_control(source, "length", "keep", name="len", min=0.0, max=10.0)
        cmds.setAttr(attr, 3.5)
        twin_guides = cmds.getAttr(twin + ".guideData")

        report = utils.copy_graph(source, [twin, big])
        self.assertEqual(report[twin]["guides"], "kept")
        for target in (twin, big):
            self.assertEqual(groomlab.get_graph(target), groomlab.get_graph(source))
            self.assertAlmostEqual(cmds.getAttr(target + ".ctl_len"), 3.5)
            self.assertEqual(groomlab.errors(target), [])
        self.assertEqual(cmds.getAttr(twin + ".guideData"), twin_guides)
        cmds.undo()  # one step, every target
        self.assertEqual(groomlab.get_graph(twin), {})
        self.assertEqual(groomlab.get_graph(big), {})

        # Guides too: copied onto the same topology (bound to its faces: on the twin head), moved
        # across with the transfer onto another (bounding boxes fitted).
        report = utils.copy_graph(source, [twin, big], guides="replace")
        self.assertEqual((report[twin]["guides"], report[big]["guides"]), ("copied", "transferred"))
        for target, center, radius in ((twin, 5.0, 1.0), (big, -6.0, 2.0)):
            points = guide_points(target)
            self.assertEqual(set(points), set(guide_points(source)))
            for pts in points.values():
                root = math.sqrt((pts[0][0] - center) ** 2 + pts[0][1] ** 2 + pts[0][2] ** 2)
                self.assertAlmostEqual(root, radius, delta=0.06)
        self.assertEqual(cmds.listConnections(big + ".inMesh", shapes=True), cmds.listRelatives(big_scalp, shapes=True))
        cmds.undo()
        self.assertEqual(cmds.getAttr(twin + ".guideData"), twin_guides)
        self.assertEqual(cmds.listConnections(big + ".inMesh", shapes=True), cmds.listRelatives(big_scalp, shapes=True))

        # Painted maps the graph reads: reported, or with maps=True copied onto the same topology
        # and projected by position onto another.
        groomlab.paint_scalp_map(source, "mask", paint=False)
        groomlab.use_map(source, "scatter", "density", "tex", kind="texture", resolution=16)
        report = utils.copy_graph(source, [twin, big])
        self.assertEqual(sorted(report[twin]["missing"]), ["mask", "tex"])
        cmds.undo()
        own_md5 = md5(groomlab.map_texture(source, "tex"))
        report = utils.copy_graph(source, [twin, big], guides="replace", maps=True)
        self.assertEqual((sorted(report[twin]["copied"]), report[twin]["projected"]), (["mask", "tex"], []))
        self.assertEqual((report[big]["copied"], report[big]["projected"]), ([], ["mask", "tex"]))
        self.assertEqual(report[big]["missing"], {})
        own = groomlab.map_texture(source, "tex")
        for target in (twin, big):
            theirs = groomlab.map_texture(target, "tex")
            self.assertNotEqual(os.path.normcase(own), os.path.normcase(theirs))  # a file of its own
            self.assertIn("/masks/%s/" % groomlab.asset_name(target), theirs)
            self.assertTrue(os.path.exists(theirs))
            self.assertEqual(groomlab.map_kind(target, "mask"), "vertices")
            self.assertEqual(len(cmds.groomLabScalpMaps(target, values="mask")),
                             cmds.polyEvaluate(groomlab.scalp_mesh(target), vertex=True))
            self.assertEqual(groomlab.errors(target), [])
        self.assertEqual(md5(own), own_md5)
        big_tex = groomlab.map_texture(big, "tex")
        cmds.undo()  # one step: the projected map's file goes with it
        self.assertFalse(os.path.exists(big_tex))
        self.assertEqual(groomlab.map_kind(big, "mask"), "")
        with self.assertRaises(ValueError):
            utils.copy_graph(source, [source])
        with self.assertRaises(ValueError):
            utils.copy_graph(source, [twin], guides="merge")


class KeyedParamsTest(unittest.TestCase):
    """Set Key on a graph param: its own attribute on the groom (ctl_<node>_<param>), keyed like any other."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        cmds.autoKeyframe(state=False)
        cmds.playbackOptions(minTime=1, maxTime=20)
        scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]
        self.groom = groomlab.create_groom(scalp)
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        groomlab.set_param(self.groom, "scatter", "density", 20.0)
        cmds.currentTime(1)

    def lengths(self):
        """Each viewport strand's length (a strand: segments + 1 points from the Resample)."""
        points = groomlab.viewport_points(self.groom)
        n = groomlab.get_graph(self.groom, expand_default=True)
        per = next(x.get("params", {}).get("segments", 8) for x in n["nodes"] if x["type"] == "Resample") + 1
        out = []
        for i in range(0, len(points) - per + 1, per):
            out.append(sum(math.sqrt(sum((a - b) ** 2 for a, b in zip(points[k], points[k + 1])))
                           for k in range(i, i + per - 1)))
        return out

    def links(self, node):
        return next(n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == node).get("paramControls", {})

    def test_the_first_key_links_the_param_in_one_undo_step(self):
        groom = self.groom
        groomlab.set_graph(groom, CLASSIC_GRAPH)
        attr = groomlab.set_param_key(groom, "length", "keep", value=0.3)
        self.assertEqual(attr, groom + ".ctl_length_keep")
        self.assertTrue(cmds.getAttr(attr, keyable=True))
        self.assertEqual(self.links("length"), {"keep": "length_keep"})
        self.assertEqual(cmds.keyframe(attr, query=True), [1.0])
        self.assertEqual(groomlab.param_control(groom, "length", "keep"), attr)
        cmds.undo()  # attribute, link and key: one step
        self.assertFalse(cmds.attributeQuery("ctl_length_keep", node=groom, exists=True))
        self.assertEqual(self.links("length") if groomlab.get_graph(groom) else {}, {})
        self.assertEqual(cmds.ls(type="animCurve"), [])
        cmds.redo()
        self.assertEqual(cmds.keyframe(attr, query=True), [1.0])

        # Keys at two frames: the strands follow the time, their count stays.
        groomlab.set_param_key(groom, "length", "keep", value=1.0, time=10)
        self.assertEqual(groomlab.param_key_state(groom, "length", "keep"), "keyed")
        short = self.lengths()
        cmds.currentTime(10)
        long_ = self.lengths()
        self.assertEqual(len(short), len(long_))
        self.assertGreater(len(short), 20)
        self.assertGreater(sum(long_), 2.5 * sum(short))
        self.assertEqual(groomlab.errors(groom), [])
        cmds.currentTime(5)
        self.assertEqual(groomlab.param_key_state(groom, "length", "keep"), "animated")
        self.assertGreater(groomlab.param_value(groom, "length", "keep"), 0.3)
        # The graph keeps the param's own value; the attribute holds the animation.
        self.assertEqual(next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == "length")
                         .get("params", {}).get("keep", 1.0), 1.0)

        # With Auto Key on, an edit keys it; off, it holds until the time changes (Maya's way).
        groomlab.set_param_value(groom, "length", "keep", 0.8)
        self.assertAlmostEqual(cmds.getAttr(attr), 0.8)
        cmds.currentTime(6)
        self.assertNotAlmostEqual(cmds.getAttr(attr), 0.8)
        cmds.autoKeyframe(state=True)
        try:
            groomlab.set_param_value(groom, "length", "keep", 0.8)
        finally:
            cmds.autoKeyframe(state=False)
        self.assertIn(6.0, cmds.keyframe(attr, query=True))
        self.assertEqual(groomlab.select_param_attribute(groom, "length", "keep"), attr)
        self.assertEqual(cmds.ls(selection=True), [groom])

    def test_remove_animation_and_delete_key_give_a_plain_param(self):
        groom = self.groom
        groomlab.set_param_key(groom, "length", "keep", value=0.4, time=1)
        groomlab.set_param_key(groom, "length", "keep", value=0.9, time=10)
        cmds.currentTime(10)
        groomlab.remove_param_animation(groom, "length", "keep")
        self.assertEqual(self.links("length"), {})
        self.assertFalse(cmds.attributeQuery("ctl_length_keep", node=groom, exists=True))
        self.assertEqual(cmds.ls(type="animCurve"), [])
        value = next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == "length")["params"]["keep"]
        self.assertAlmostEqual(value, 0.9, places=5)  # the value then, baked in
        self.assertIsNone(groomlab.param_key_state(groom, "length", "keep"))
        cmds.undo()
        self.assertEqual(self.links("length"), {"keep": "length_keep"})
        self.assertEqual(len(cmds.keyframe(groom + ".ctl_length_keep", query=True)), 2)

        # Deleting the keys one by one: the last one gone, the param is plain again.
        groomlab.delete_param_key(groom, "length", "keep", time=1)
        self.assertEqual(cmds.keyframe(groom + ".ctl_length_keep", query=True), [10.0])
        groomlab.delete_param_key(groom, "length", "keep", time=10)
        self.assertEqual(self.links("length"), {})
        self.assertFalse(cmds.attributeQuery("ctl_length_keep", node=groom, exists=True))
        self.assertEqual(groomlab.errors(groom), [])

    def test_counts_segments_and_seeds_cannot_be_keyed(self):
        groom = self.groom
        for node, param in (("scatter", "density"), ("scatter", "seed"), ("resample", "segments")):
            with self.assertRaises(ValueError) as caught:
                groomlab.set_param_key(groom, node, param)
            self.assertIn("cannot be keyed", str(caught.exception))
        self.assertFalse([a for a in cmds.listAttr(groom, userDefined=True) or [] if a.startswith("ctl_")])
        # Written by hand, the graph refuses it.
        graph = groomlab.get_graph(groom, expand_default=True)
        for n in graph["nodes"]:
            if n["name"] == "scatter":
                n["paramControls"] = {"density": "d"}
        groomlab.set_graph(groom, graph)
        self.assertTrue(any("cannot be keyed" in e for e in groomlab.errors(groom)), groomlab.errors(groom))

    def test_vectors_and_colors_key_as_three_channels(self):
        from groomlab import graph as gm
        groom = self.groom
        wind = groomlab.add_wind(groom, strength=1.0, gust=0.0)
        model = gm.GraphModel.from_groom(groom)
        color = model.add_node("HairColor")
        model.insert_before("output", color)
        model.save(groom)
        cmds.currentTime(10)
        attr = groomlab.set_param_key(groom, color, "tipColor", value=[1.0, 0.0, 0.0])
        self.assertTrue(cmds.attributeQuery(attr.split(".")[1], node=groom, usedAsColor=True))
        self.assertEqual(cmds.attributeQuery(attr.split(".")[1], node=groom, listChildren=True),
                         [attr.split(".")[1] + c for c in "RGB"])
        self.assertEqual(self.links(color), {"tipColor": [attr.split(".")[1][4:] + c for c in "RGB"]})
        self.assertEqual(groomlab.param_value(groom, color, "tipColor"), [1.0, 0.0, 0.0])
        vec = groomlab.set_param_key(groom, wind, "direction", value=[1.0, 0.0, 0.0])
        self.assertFalse(cmds.attributeQuery(vec.split(".")[1], node=groom, usedAsColor=True))
        self.assertEqual(cmds.attributeQuery(vec.split(".")[1], node=groom, listChildren=True),
                         [vec.split(".")[1] + c for c in "XYZ"])
        east = groomlab.viewport_points(groom)
        groomlab.set_param_key(groom, wind, "direction", value=[-1.0, 0.0, 0.0])  # the key at 10 replaced
        west = groomlab.viewport_points(groom)
        self.assertEqual(len(east), len(west))
        self.assertGreater(sum(p[0] for p in east) - sum(p[0] for p in west), 0.1 * len(east) / 9.0)
        self.assertEqual(groomlab.errors(groom), [])
        self.assertEqual(sorted(groomlab.list_controls(groom)),
                         sorted([attr.split(".")[1][4:], vec.split(".")[1][4:]]))
        # Removed, a vector goes back into the graph as three numbers.
        groomlab.remove_param_animation(groom, wind, "direction")
        self.assertEqual(next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == wind)["params"]["direction"],
                         [-1.0, 0.0, 0.0])
        self.assertFalse(cmds.attributeQuery(vec.split(".")[1], node=groom, exists=True))

    def test_a_switch_keys_stepped(self):
        from groomlab import graph as gm
        groom = self.groom
        model = gm.GraphModel.from_groom(groom)
        switch = model.add_node("Switch")
        model.insert_before("output", switch, input_name="strands")
        model.connect("length", switch, "b")
        model.save(groom)
        attr = groomlab.set_param_key(groom, switch, "index", value=0, time=1)
        groomlab.set_param_key(groom, switch, "index", value=1, time=10)
        self.assertEqual(cmds.getAttr(attr, type=True), "long")
        self.assertEqual(set(cmds.keyTangent(attr, query=True, outTangentType=True)), {"step"})
        cmds.currentTime(9)
        self.assertEqual(groomlab.param_value(groom, switch, "index"), 0)
        cmds.currentTime(10)
        self.assertEqual(groomlab.param_value(groom, switch, "index"), 1)
        self.assertEqual(groomlab.errors(groom), [])
        # Blend's amount keys too.
        model = gm.GraphModel.from_groom(groom)
        self.assertIsNone(model.keyable(switch, "index"))

    def test_presets_and_copied_graphs_keep_the_links(self):
        from groomlab import presets, utils
        groom = self.groom
        groomlab.set_param_key(groom, "length", "keep", value=0.5, time=1)
        groomlab.set_param_key(groom, "length", "keep", value=1.0, time=10)
        other_scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]
        cmds.move(4, 0, 0, other_scalp)
        other = groomlab.create_groom(other_scalp, name="other")
        utils.copy_graph(groom, [other])
        self.assertTrue(cmds.attributeQuery("ctl_length_keep", node=other, exists=True))
        self.assertEqual(next(n for n in groomlab.get_graph(other)["nodes"] if n["name"] == "length")["paramControls"],
                         {"keep": "length_keep"})
        self.assertAlmostEqual(cmds.getAttr(other + ".ctl_length_keep"), 0.5)  # its value then (keys stay)
        path = presets.save_preset(groom, "keyedParamsTest", overwrite=True)
        try:
            with open(path) as f:
                saved = json.load(f)
            self.assertEqual(next(n for n in saved["nodes"] if n["name"] == "length")["paramControls"],
                             {"keep": "length_keep"})
            self.assertEqual(presets.check(presets.read_preset("keyedParamsTest")), [])
            third = groomlab.create_groom(cmds.polySphere(radius=1)[0], name="third")
            presets.apply_preset(third, "keyedParamsTest")
            self.assertTrue(cmds.attributeQuery("ctl_length_keep", node=third, exists=True))
            self.assertEqual(groomlab.errors(third), [])
        finally:
            os.remove(path)

    def test_a_slider_driven_param_reads_as_driven(self):
        groom = self.groom
        groomlab.add_control(groom, "length", "keep", name="len")
        self.assertEqual(groomlab.param_key_state(groom, "length", "keep"), "driven")
        self.assertIsNone(groomlab.param_key_state(groom, "width", "width"))


def points_key(groom):
    """The viewport's strand points as (point count, hash of them rounded): pulls the evaluation."""
    values = cmds.groomLabGraphStats(groom, points=True) or []
    text = ",".join("%.5f" % v for v in values)
    return len(values) // 3, hashlib.md5(text.encode()).hexdigest()


class StoredInputsTest(unittest.TestCase):
    """The groom reads its stored texts (graph, guideData, parts, flow, correctives, maps) only when
    their plugs were dirtied, its guide curves' CVs only when they were dirtied, and its control
    attributes once per change of them: every way of changing them must still reach the groom,
    in the DG and in the Evaluation Manager."""

    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH, quiet=True)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.evaluationManager(mode="off")
        cmds.undoInfo(state=True, infinity=True)
        cmds.autoKeyframe(state=False)
        cmds.playbackOptions(minTime=1, maxTime=20)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15)[0]
        self.curve = cmds.curve(degree=1, point=[(0, 1, 0), (0.3, 1.5, 0.1), (0.8, 1.8, 0.2)])
        self.groom = groomlab.create_groom(self.scalp, [self.curve])
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        groomlab.set_param(self.groom, "scatter", "density", 30.0)
        cmds.currentTime(1)

    def tearDown(self):
        cmds.evaluationManager(mode="off")

    def test_edits_undo_redo_and_unevaluated_edits_reach_the_groom(self):
        groom = self.groom
        start = points_key(groom)
        groomlab.set_param(groom, "scatter", "density", 60.0)
        denser = points_key(groom)
        self.assertGreater(denser[0], start[0])
        cmds.undo()
        self.assertEqual(points_key(groom), start)
        cmds.redo()
        self.assertEqual(points_key(groom), denser)
        # Two edits with no evaluation between: the last one counts.
        graph = groomlab.get_graph(groom, expand_default=True)
        for density in (10.0, 60.0):
            next(n for n in graph["nodes"] if n["name"] == "scatter")["params"]["density"] = density
            cmds.setAttr(groom + ".graph", json.dumps(graph), type="string")
        self.assertEqual(points_key(groom), denser)
        # Each stored text, broken and undone: reported, then gone.
        groomlab.set_param(groom, "length", "keep", 0.5)
        before = points_key(groom)
        self.assertEqual(groomlab.errors(groom), [])
        for attr in ("guideData", "partData", "flowData", "correctiveData", "scalpMaps"):
            cmds.setAttr(groom + "." + attr, "{not json", type="string")
            points_key(groom)
            self.assertTrue(groomlab.errors(groom), attr)
            cmds.undo()
            self.assertEqual(points_key(groom), before, attr)
            self.assertEqual(groomlab.errors(groom), [], attr)
            cmds.redo()
            self.assertTrue(groomlab.errors(groom), attr)
            cmds.undo()
            self.assertEqual(groomlab.errors(groom), [], attr)

    def test_a_connected_text_is_read_on_every_change(self):
        groom = self.groom
        start = points_key(groom)
        holder = cmds.createNode("transform", name="graphHolder")
        cmds.addAttr(holder, longName="text", dataType="string")
        graph = groomlab.get_graph(groom, expand_default=True)
        next(n for n in graph["nodes"] if n["name"] == "scatter")["params"]["density"] = 60.0
        cmds.setAttr(holder + ".text", json.dumps(graph), type="string")
        cmds.connectAttr(holder + ".text", groom + ".graph", force=True)
        denser = points_key(groom)
        self.assertGreater(denser[0], start[0])
        next(n for n in graph["nodes"] if n["name"] == "scatter")["params"]["density"] = 30.0
        cmds.setAttr(holder + ".text", json.dumps(graph), type="string")
        self.assertEqual(points_key(groom), start)

    def test_controls_added_set_keyed_and_removed(self):
        groom = self.groom
        start = points_key(groom)
        attr = groomlab.add_control(groom, "length", "keep", name="len", default=1.0, min=0.0, max=3.0)
        self.assertEqual(points_key(groom), start)  # at the param's own value
        cmds.setAttr(attr, 0.5)
        longer = points_key(groom)  # (shorter: the guide curve's length is the most)
        self.assertNotEqual(longer, start)
        cmds.undo()
        self.assertEqual(points_key(groom), start)
        cmds.redo()
        self.assertEqual(points_key(groom), longer)
        # Keyed: the frames follow the keys, in the DG and in the Evaluation Manager.
        cmds.setKeyframe(attr, time=1, value=1.0)
        cmds.setKeyframe(attr, time=10, value=0.5)
        frames = {}
        for mode in ("off", "parallel"):
            cmds.evaluationManager(mode=mode)
            for frame in (1, 10, 5, 1):
                cmds.currentTime(frame)
                frames.setdefault(frame, set()).add(points_key(groom))
        self.assertEqual(frames[1], {start})
        self.assertEqual(frames[10], {longer})
        self.assertEqual(len(frames[5]), 1)  # the same in both modes
        self.assertNotIn(start, frames[5])
        cmds.evaluationManager(mode="off")
        # The attribute removed (and the Control node with it): the param's own value again.
        cmds.cutKey(attr)
        cmds.currentTime(1)
        cmds.setAttr(attr, 0.5)
        self.assertEqual(points_key(groom), longer)
        graph = groomlab.get_graph(groom)
        graph["nodes"] = [n for n in graph["nodes"] if n["type"] != "Control"]
        graph["connections"] = [c for c in graph["connections"] if not c["from"].startswith("lenControl")]
        cmds.deleteAttr(attr)
        cmds.setAttr(groom + ".graph", json.dumps(graph), type="string")
        self.assertEqual(points_key(groom), start)

    def test_animated_guide_curves_follow_in_both_evaluation_modes(self):
        groom = self.groom
        groomlab.set_graph(groom, groomlab.get_graph(groom, expand_default=True))  # the guide curves drive it
        shape = cmds.listRelatives(self.curve, shapes=True)[0]
        cmds.setKeyframe(shape + ".controlPoints[2].xValue", time=1, value=0.8)
        cmds.setKeyframe(shape + ".controlPoints[2].xValue", time=10, value=1.6)
        seen = {}
        for mode in ("off", "parallel", "off"):
            cmds.evaluationManager(mode=mode)
            for frame in (1, 10, 4, 10, 1):
                cmds.currentTime(frame)
                seen.setdefault(frame, set()).add(points_key(groom))
        for frame in (1, 4, 10):
            self.assertEqual(len(seen[frame]), 1, frame)
        self.assertNotEqual(seen[1], seen[10])
        self.assertNotEqual(seen[4], seen[10])
        # An edit of the curve itself (not a key): the groom follows.
        cmds.cutKey(shape)
        cmds.currentTime(1)
        before = points_key(groom)
        cmds.move(0.0, 0.3, 0.0, self.curve + ".cv[2]", relative=True)
        self.assertNotEqual(points_key(groom), before)
        cmds.undo()
        self.assertEqual(points_key(groom), before)

    def test_reopened_and_referenced_grooms_read_their_stored_inputs(self):
        groom = self.groom
        groomlab.set_param(groom, "scatter", "density", 45.0)
        groomlab.set_param(groom, "length", "keep", 0.6)
        expected = points_key(groom)
        stored_graph = cmds.getAttr(groom + ".graph")
        folder = tempfile.mkdtemp(prefix="groomlab_stored_")
        path = os.path.join(folder, "asset.ma").replace("\\", "/")
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        cmds.file(path, open=True, force=True)
        self.assertEqual(points_key(groom), expected)
        groomlab.set_param(groom, "length", "keep", 1.0)  # edits after opening reach it too
        self.assertNotEqual(points_key(groom), expected)
        cmds.file(new=True, force=True)
        cmds.file(path, reference=True, namespace="asset")
        referenced = "asset:" + groom
        self.assertEqual(points_key(referenced), expected)
        cmds.setAttr(referenced + ".previewDensity", 0.5)  # a reference edit
        self.assertNotEqual(points_key(referenced), expected)
        cmds.setAttr(referenced + ".previewDensity", 1.0)
        self.assertEqual(points_key(referenced), expected)
        graph = groomlab.get_graph(referenced, expand_default=True)
        next(n for n in graph["nodes"] if n["name"] == "length")["params"]["keep"] = 1.0
        cmds.setAttr(referenced + ".graph", json.dumps(graph), type="string")
        self.assertNotEqual(points_key(referenced), expected)
        # Unloaded and loaded again: the reference edit is applied to the file's groom.
        reference_node = cmds.referenceQuery(path, referenceNode=True)
        cmds.file(unloadReference=reference_node)
        cmds.file(loadReference=reference_node)
        changed = points_key(referenced)
        self.assertNotEqual(changed, expected)
        cmds.setAttr(referenced + ".graph", stored_graph, type="string")
        self.assertEqual(points_key(referenced), expected)


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
