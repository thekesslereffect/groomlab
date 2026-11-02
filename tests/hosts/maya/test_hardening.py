# SPDX-License-Identifier: Apache-2.0
"""1.0 hardening in Maya: errors on the node, referencing and moving projects, scene-open time.

Usage: mayapy test_hardening.py <GroomLab.mll> <maya/scripts dir> <groomlab-cli> <work dir> <texture.pgm>
"""

import os
import shutil
import subprocess
import sys
import time
import unittest

PLUGIN_PATH = os.path.abspath(sys.argv[1])
SCRIPTS_DIR = os.path.abspath(sys.argv[2])
CLI = os.path.abspath(sys.argv[3])
WORK_DIR = os.path.abspath(sys.argv[4])
TEXTURE = os.path.abspath(sys.argv[5])
sys.argv = sys.argv[:1]
sys.path.insert(0, SCRIPTS_DIR)

import maya.standalone  # noqa: E402

maya.standalone.initialize(name="python")

from maya import cmds  # noqa: E402

import groomlab  # noqa: E402


def cli(*args):
    result = subprocess.run([CLI] + [str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True)
    if result.returncode != 0:
        raise AssertionError("groomlab-cli %s failed:\n%s" % (" ".join(map(str, args)), result.stdout))
    return result.stdout


def make_project(root):
    """A Maya project with the test texture in sourceimages."""
    os.makedirs(os.path.join(root, "sourceimages"))
    os.makedirs(os.path.join(root, "scenes"))
    shutil.copy(TEXTURE, os.path.join(root, "sourceimages", "density.pgm"))
    cmds.workspace(root, openWorkspace=True)
    cmds.groomLabPaths()
    return root


class HardeningTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmds.loadPlugin(PLUGIN_PATH)
        shutil.rmtree(WORK_DIR, ignore_errors=True)
        os.makedirs(WORK_DIR)

    def setUp(self):
        cmds.file(new=True, force=True)
        cmds.currentUnit(time="film")

    def test_errors_show_on_the_node(self):
        scalp = cmds.polySphere()[0]
        groom = groomlab.create_groom(scalp)
        cmds.setAttr(groom + ".previewDensity", 1.0)
        self.assertEqual(groomlab.errors(groom), [])
        good = cmds.getAttr(groom + ".graph")

        cmds.setAttr(groom + ".graph", '{"version": 1, "nodes": [{"name": "x", "type": "Bogus"}]}', type="string")
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), 0)
        self.assertTrue(any("Bogus" in e for e in groomlab.errors(groom)), groomlab.errors(groom))

        cmds.setAttr(groom + ".graph", '{"version": 99}', type="string")
        self.assertTrue(any("newer" in e for e in groomlab.errors(groom)), groomlab.errors(groom))

        cmds.setAttr(groom + ".graph", good, type="string")
        groomlab.set_param(groom, "scatter", "density", groomlab.texture_map("no/such/map.png", max=100))
        self.assertTrue(any("no/such/map.png" in e.replace("\\", "/") for e in groomlab.errors(groom)))
        groomlab.set_param(groom, "scatter", "density", 100.0)
        self.assertEqual(groomlab.errors(groom), [])
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 0)

        cmds.setAttr(groom + ".guideData", "{broken", type="string")
        self.assertTrue(any(e.startswith("guideData") for e in groomlab.errors(groom)))

    def test_a_dense_uv_grid_survives_length_and_width_edits(self):
        # A tester's crash report: Scatter's uvGrid at 1000 x 1000 (a million roots), then Scatter's
        # length and the Width node's width changed. Every step evaluates (thinned preview and in
        # full), past 2M strands the Scatter notes it is heavy, past 50M the node shows an error:
        # never a crash, never a dialog.
        import json
        scalp = cmds.polySphere(subdivisionsX=40, subdivisionsY=40)[0]
        groom = groomlab.create_groom(scalp, fit=True)
        groomlab.set_param(groom, "scatter", "layout", "uvGrid")
        groomlab.set_param(groom, "scatter", "gridRows", 1000)
        groomlab.set_param(groom, "scatter", "gridColumns", 1000)
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 50000)
        groomlab.set_param(groom, "scatter", "length", 2.0)
        for width in (0.02, 0.0, 0.5):
            groomlab.set_param(groom, "width", "width", width)
            self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 50000)
            self.assertEqual(groomlab.errors(groom), [])
        cmds.setAttr(groom + ".previewDensity", 1.0)
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 500000)
        groomlab.set_param(groom, "width", "width", 0.01)
        self.assertEqual(groomlab.errors(groom), [])
        cmds.setAttr(groom + ".previewDensity", 0.02)
        groomlab.set_param(groom, "scatter", "gridRows", 2000)
        groomlab.set_param(groom, "scatter", "gridColumns", 1500)
        self.assertGreater(cmds.getAttr(groom + ".outStrandCount"), 10000)
        notes = json.loads(cmds.groomLabGraphStats(groom)).get("notes") or {}
        self.assertIn("strands: heavy", notes.get("scatter", ""), notes)
        groomlab.set_param(groom, "scatter", "gridRows", 20000)
        groomlab.set_param(groom, "scatter", "gridColumns", 20000)
        self.assertEqual(cmds.getAttr(groom + ".outStrandCount"), 0)
        self.assertTrue(any("50M" in e for e in groomlab.errors(groom)), groomlab.errors(groom))

    def test_referenced_groom_survives_moving_the_project(self):
        first = make_project(os.path.join(WORK_DIR, "projectA"))
        scalp = cmds.polySphere(name="head")[0]
        groom = groomlab.create_groom(scalp, name="hair")
        map_ = groomlab.texture_map(os.path.join(first, "sourceimages", "density.pgm"), component="luminance", max=400)
        self.assertEqual(map_["texture"], "sourceimages/density.pgm")  # stored relative to the project
        groomlab.set_param(groom, "scatter", "density", map_)
        cmds.setAttr(groom + ".previewDensity", 1.0)
        count = cmds.getAttr(groom + ".outStrandCount")
        self.assertGreater(count, 100)
        self.assertEqual(groomlab.errors(groom), [])
        self.assertEqual(groomlab.absolute_paths(groom), [])
        cmds.file(rename=os.path.join(first, "scenes", "asset.ma"))
        cmds.file(save=True, type="mayaAscii")

        # Move the whole project; nothing may still point at the old one.
        second = os.path.join(WORK_DIR, "projectB")
        cmds.file(new=True, force=True)
        shutil.copytree(first, second)
        shutil.rmtree(first)
        cmds.workspace(second, openWorkspace=True)
        cmds.groomLabPaths()

        cmds.file(os.path.join(second, "scenes", "asset.ma"), reference=True, namespace="hero")
        hero = "hero:hairShape"
        self.assertTrue(cmds.objExists(hero))
        cmds.setAttr(hero + ".previewDensity", 1.0)
        self.assertEqual(cmds.getAttr(hero + ".outStrandCount"), count)
        self.assertEqual(groomlab.errors(hero), [])
        self.assertTrue(groomlab.resolve_path("sourceimages/density.pgm").replace("\\", "/").startswith(
            second.replace("\\", "/")))

        # The groom file finds the map through its search paths.
        path = os.path.join(second, "cache", "hero.groom")
        os.makedirs(os.path.dirname(path))
        groomlab.export_groom(hero, path)
        out = os.path.join(second, "cache", "hero.abc")
        self.assertIn("frame 1: %d strands" % count, cli("eval", path, "-o", out, "-f", "1"))

        # Helpers name new nodes outside the read-only reference namespace.
        from groomlab import sim
        made = sim.create_sim_curves(hero, "primary", dynamic=False) if groomlab.list_guide_groups(hero) else None
        if made:
            sim.remove_sim(hero, made)

    def test_scene_open_time_with_three_grooms(self):
        # Spec: < 3 s added load time for a scene with 3 grooms (drawn at preview density).
        for i in range(3):
            scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=40, name="head%d" % i)[0]
            cmds.move(3 * i, 0, 0, scalp)
            groom = groomlab.create_groom(scalp, name="hair%d" % i)
            curve = cmds.curve(degree=1, point=[(3 * i, 1, 0), (3 * i + 0.2, 1.6, 0.1), (3 * i + 0.5, 2.1, 0.2)])
            groomlab.guides_from_curves(groom, [curve])
            groomlab.generate_children(groom, "primary", count=8)
            groomlab.set_param(groom, "scatter", "density", 8000.0)  # 100k strands at render density
        with_grooms = os.path.join(WORK_DIR, "three_grooms.ma")
        cmds.file(rename=with_grooms)
        cmds.file(save=True, type="mayaAscii")
        for g in cmds.ls(type="groomLabShape"):
            cmds.delete(cmds.listRelatives(g, parent=True))
        without = os.path.join(WORK_DIR, "no_grooms.ma")
        cmds.file(rename=without)
        cmds.file(save=True, type="mayaAscii")

        def open_ms(path):
            cmds.file(new=True, force=True)
            t0 = time.time()
            cmds.file(path, open=True, force=True)
            counts = [cmds.getAttr(g + ".outStrandCount") for g in cmds.ls(type="groomLabShape")]  # as a draw would
            return (time.time() - t0) * 1000.0, counts

        base_ms, _ = open_ms(without)
        groom_ms, counts = open_ms(with_grooms)
        print("[ budget ] scene open: %.0f ms without grooms, %.0f ms with 3 grooms (%s preview strands)"
              % (base_ms, groom_ms, counts))
        self.assertEqual(len(counts), 3)
        self.assertTrue(all(c > 5000 for c in counts))
        self.assertLess(groom_ms - base_ms, 3000.0)


# Maya quits without finalizing Python, so shiboken2's static cleanup walks every wrapper still
# alive at DLL unload. A widget whose style() was called under a style sheet keeps a wrapper of
# Qt's QStyleSheetStyle ("__style__"), and the walk crashed (now and then in a Maya session; with
# fifty such widgets, every time) when it freed that wrapper before reaching the widget through its
# parents. os._exit ends the process the way Maya does: no Python finalization, DLLs unloaded.
QUIT_SCRIPT = r'''
import os, sys
sys.path.insert(0, %r)
from groomlab.ui.qt import QtWidgets
from groomlab.ui import widgets
app = QtWidgets.QApplication([])
keep = []
for i in range(50):
    top = QtWidgets.QWidget()
    top.setStyleSheet('QPushButton[done="true"] { color: #888888; }')
    card = QtWidgets.QFrame(QtWidgets.QWidget(QtWidgets.QScrollArea(top)))
    button = QtWidgets.QPushButton("step", card)
    button.setProperty("done", True)
    widgets.repolish(button)
    keep.append(top)
sys.stdout.flush()
os._exit(0)
'''


class QuitTest(unittest.TestCase):
    def test_repolished_widgets_alive_at_exit_quit_cleanly(self):
        os.makedirs(WORK_DIR, exist_ok=True)
        script = os.path.join(WORK_DIR, "quit_with_widgets.py")
        with open(script, "w") as f:
            f.write(QUIT_SCRIPT % SCRIPTS_DIR)
        result = subprocess.run([sys.executable, script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, timeout=120)
        self.assertEqual(result.returncode, 0, "the process crashed at exit (%#x):\n%s"
                         % (result.returncode & 0xFFFFFFFF, result.stdout))


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    maya.standalone.uninitialize()
    sys.exit(0 if result.wasSuccessful() else 1)
