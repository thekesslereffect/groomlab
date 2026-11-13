# SPDX-License-Identifier: Apache-2.0
"""The Card Atlas in Maya (groomlab.card_atlas, groomLabCardAtlas): blocks as grooms on root bands
side by side, laid out on one atlas, edited (the band turned or resized, the groom following),
baked into one texture set, their cards exported, and the atlas saved as a groom file and opened
again.

Usage: mayapy test_card_atlas.py <GroomLab.mll> <maya/scripts dir> <work dir>
"""

import json
import os
import shutil
import sys
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
WORK_DIR = os.path.abspath(sys.argv[3])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402
from groomlab import card_atlas as ca  # noqa: E402


def _bbox(node):
    return cmds.exactWorldBoundingBox(node)


class CardAtlasTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)
        shutil.rmtree(WORK_DIR, ignore_errors=True)
        os.makedirs(WORK_DIR)

    def setUp(self):
        cmds.file(new=True, force=True)
        self.atlas = ca.create_atlas("bangs", size=512)

    def test_blocks_side_by_side_on_one_atlas(self):
        a = ca.add_block(self.atlas, "straight", width=2.0, length=15.0)
        b = ca.add_block(self.atlas, "tilted", width=3.0, length=12.0, root_rotation=(40.0, 0.0, 0.0))
        c = ca.duplicate_block(a, "copy")
        self.assertEqual([ca.block(g)["name"] for g in ca.blocks(self.atlas)], ["straight", "tilted", "copy"])
        self.assertEqual(cmds.getAttr(c + ".graph"), cmds.getAttr(a + ".graph"))  # the duplicate's graph
        cells = [ca.block(g)["cell"] for g in ca.blocks(self.atlas)]
        for i, g in enumerate(ca.blocks(self.atlas)):
            self.assertGreater(cmds.getAttr(g + ".outStrandCount"), 100)
            self.assertEqual(groomlab.errors(g), [])
            for other in cells[i + 1:]:  # apart on the atlas
                c0 = cells[i]
                self.assertTrue(c0[2] <= other[0] or other[2] <= c0[0] or c0[3] <= other[1] or other[3] <= c0[1])
        # Side by side in the scene: their bands apart.
        bands = [ca._band_of(g) for g in ca.blocks(self.atlas)]
        for left, right in zip(bands, bands[1:]):
            self.assertLess(_bbox(left)[3], _bbox(right)[0])
        # The atlas' settings on every block.
        self.assertEqual(ca.settings(self.atlas)["size"], 512)
        ca.set_settings(self.atlas, preset="Unity HDRP Hair", name="fringe")
        for g in ca.blocks(self.atlas):
            stored = json.loads(cmds.getAttr(g + ".cardBlock"))["atlas"]
            self.assertEqual((stored["preset"], stored["name"]), ("Unity HDRP Hair", "fringe"))

    def test_a_block_edited(self):
        g = ca.add_block(self.atlas, "block", width=2.0, length=10.0)
        band = ca._band_of(g)
        before = _bbox(g)
        # Tilted back: the guides moved with the band, not turned (each hangs as it was shaped).
        def shapes():
            data = json.loads(cmds.getAttr(g + ".guideData"))["guides"]
            out = []
            for gd in data:
                f = gd["points"]  # flat x y z
                out.append([[f[i + k] - f[k] for k in range(3)] for i in range(0, len(f), 3)])
            return out
        shaped = shapes()
        ca.set_block(g, root_rotation=(50.0, 0.0, 0.0))
        tilted = shapes()
        self.assertEqual(len(tilted), len(shaped))
        for a, b in zip(shaped, tilted):
            for p, q in zip(a, b):
                for k in range(3):
                    self.assertAlmostEqual(p[k], q[k], places=3)
        ca.set_block(g, root_rotation=(0.0, 0.0, 0.0))
        # Turned: the same band moved, the groom follows it (its strands roll with the band).
        ca.set_block(g, root_rotation=(0.0, 0.0, 30.0))
        self.assertEqual(ca.block(g)["rootRotation"], [0.0, 0.0, 30.0])
        self.assertEqual(ca._band_of(g), band)
        after = _bbox(g)
        self.assertNotAlmostEqual(before[0], after[0], places=2)
        # Wider: another band (more quads across), the groom on it, its name kept.
        ca.set_block(g, width=4.0)
        self.assertEqual(ca._band_of(g).split("|")[-1], band.split("|")[-1])
        bb = _bbox(ca._band_of(g))
        self.assertGreater(bb[3] - bb[0], 3.0)
        self.assertGreater(cmds.getAttr(g + ".outStrandCount"), 100)
        # Placed by hand: kept through the layout, a new block finds room beside it.
        ca.set_block(g, cell=(0.05, 0.05, 0.25, 0.55))
        other = ca.add_block(self.atlas, "other", width=2.0, length=8.0)
        self.assertEqual([round(v, 4) for v in ca.block(g)["cell"]], [0.05, 0.05, 0.25, 0.55])
        self.assertTrue(ca.block(g)["placed"])
        o = ca.block(other)["cell"]
        self.assertTrue(o[0] >= 0.25 or o[2] <= 0.05 or o[1] >= 0.55)
        # Auto Layout: every block packed again.
        ca.update_layout(self.atlas, pack=True)
        self.assertFalse(ca.block(g)["placed"])
        # Removed: its groom and band go, the layout follows.
        ca.remove_block(other)
        self.assertEqual(len(ca.blocks(self.atlas)), 1)
        cmds.undo()
        self.assertEqual(len(ca.blocks(self.atlas)), 2)

    def test_the_card_fits_the_hair(self):
        g = ca.add_block(self.atlas, "swept", width=2.0, length=10.0)
        other = ca.add_block(self.atlas, "straight", width=2.0, length=10.0)
        self.assertAlmostEqual(ca.card_size(g)[0], 2.0, delta=0.4)  # fitted on adding: about its width
        # Its guides combed 3 across toward their tips: the hair past the card's edge.
        data = json.loads(cmds.getAttr(g + ".guideData"))
        for gd in data["guides"]:
            f = gd["points"]
            n = len(f) // 3
            for i in range(n):
                f[3 * i] += 3.0 * i / float(n - 1)
        cmds.setAttr(g + ".guideData", json.dumps(data), type="string")
        self.assertTrue(ca.fit(self.atlas))
        width, length = ca.card_size(g)
        self.assertGreater(width, 4.5)
        self.assertGreater(length, 8.0)
        self.assertFalse(ca.fit(self.atlas))  # nothing more to fit
        a, b = ca.block(g)["cell"], ca.block(other)["cell"]
        self.assertTrue(a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])  # still apart
        self.assertAlmostEqual((a[2] - a[0]) / (a[3] - a[1]), width / length, delta=0.02)
        # The bake takes it (the preview fits first), the card shapes are its size.
        self.assertIn("Alpha", ca.preview(self.atlas, os.path.join(WORK_DIR, "fit_preview"), resolution=128))
        # A block edit keeps it fitted (the band narrower: the swept hair still wider than it).
        ca.set_block(g, width=1.5)
        self.assertGreater(ca.card_size(g)[0], 4.0)

    def test_cards_to_scene_and_the_fbx_beside_the_blocks(self):
        a = ca.add_block(self.atlas, "fringe", width=2.0, length=10.0)
        b = ca.add_block(self.atlas, "wisp", width=1.5, length=8.0)
        maps = ca.preview(self.atlas, os.path.join(WORK_DIR, "scene_preview"), resolution=64)
        group = ca.cards_to_scene(self.atlas, segments=4, maps=maps)
        cards = sorted(n.split("|")[-1] for n in cmds.listRelatives(group, children=True) or [])
        self.assertEqual(cards, ["fringe_card", "wisp_card"])
        for card, groom in (("fringe_card", a), ("wisp_card", b)):
            node = group + "|" + card
            self.assertEqual(cmds.polyEvaluate(node, face=True), 4)
            cell = ca.block(groom)["cell"]
            us, vs = cmds.polyEditUV(node + ".map[*]", query=True)[0::2], cmds.polyEditUV(node + ".map[*]", query=True)[1::2]
            self.assertAlmostEqual(min(us), cell[0], places=4)  # the block's rectangle of the atlas
            self.assertAlmostEqual(max(vs), cell[3], places=4)
        self.assertGreater(cmds.getAttr(group + ".translateX"), 0.0)  # beside the blocks
        self.assertTrue(cmds.listConnections(self.atlas.split("|")[-1] + "_mat.color"))  # textured with the preview
        # Made again: replaced, not added; undone: the first ones back.
        again = ca.cards_to_scene(self.atlas, segments=2)
        self.assertEqual(len(cmds.ls("fringe_card", long=True)), 1)
        self.assertEqual(cmds.polyEvaluate(again + "|fringe_card", face=True), 2)
        cmds.undo()
        self.assertEqual(cmds.polyEvaluate(ca.scene_cards(self.atlas) + "|fringe_card", face=True), 4)
        cmds.delete(ca.scene_cards(self.atlas))
        # The FBX dropped into the scene (Maya's default import merges by name): new cards beside the
        # blocks, the blocks' grooms and the atlas untouched.
        path = ca.export_shapes(self.atlas, os.path.join(WORK_DIR, "cards.fbx"), segments=4)
        try:
            cmds.loadPlugin("fbxmaya", quiet=True)
        except RuntimeError:
            self.skipTest("no FBX plugin")
        cmds.file(path, i=True, type="FBX", ignoreVersion=True)
        self.assertTrue(cmds.objExists("fringe_card") and cmds.objExists("wisp_card"))
        self.assertEqual(cmds.nodeType(cmds.listRelatives("fringe_card", shapes=True, fullPath=True)[0]), "mesh")
        self.assertEqual(len(ca.blocks(self.atlas)), 2)
        self.assertEqual(cmds.nodeType(ca.blocks(self.atlas)[0]), "groomLabShape")

    def test_baked_exported_saved_and_opened(self):
        ca.add_block(self.atlas, "a", width=2.0, length=12.0)
        ca.add_block(self.atlas, "b", width=1.5, length=9.0, root_rotation=(30.0, 0.0, 0.0))
        maps = ca.export_maps(self.atlas, os.path.join(WORK_DIR, "maps"), resolution=256)
        self.assertTrue(any(p.endswith("bangs_Color.tga") for p in maps), maps)
        for p in maps:
            self.assertTrue(os.path.isfile(p))
        preview = ca.preview(self.atlas, os.path.join(WORK_DIR, "preview"), resolution=128)
        self.assertIn("Albedo", preview)
        self.assertIn("Alpha", preview)
        fbx = ca.export_shapes(self.atlas, os.path.join(WORK_DIR, "cards.fbx"), segments=4, bend=15.0)
        self.assertGreater(os.path.getsize(fbx), 1000)
        before = json.loads(ca.atlas_json(self.atlas))

        path = os.path.join(WORK_DIR, "bangs.groom")
        ca.save_atlas(self.atlas, path)
        cmds.file(new=True, force=True)
        opened = ca.open_atlas(path)
        after = json.loads(ca.atlas_json(opened))
        self.assertEqual(after["settings"], before["settings"])
        self.assertEqual([b["cell"] for b in after["blocks"]], [b["cell"] for b in before["blocks"]])
        self.assertEqual([b["rootRotation"] if "rootRotation" in b else None for b in after["blocks"]],
                         [b["rootRotation"] if "rootRotation" in b else None for b in before["blocks"]])
        for g in ca.blocks(opened):
            self.assertGreater(cmds.getAttr(g + ".outStrandCount"), 100)
        self.assertTrue(ca.export_maps(opened, os.path.join(WORK_DIR, "maps2"), resolution=128))

    def test_the_command_says_what_is_wrong(self):
        plain = groomlab.create_groom(cmds.polyPlane()[0])
        with self.assertRaises(RuntimeError):
            cmds.groomLabCardAtlas(plain, maps=WORK_DIR)  # no block
        g = ca.add_block(self.atlas, "a")
        with self.assertRaises(RuntimeError):
            cmds.groomLabCardAtlas(g)  # nothing asked


if __name__ == "__main__":
    unittest.main(verbosity=2)
