# SPDX-License-Identifier: Apache-2.0
"""Graph editor widget tests. Need a QApplication, i.e. a Maya UI session;
run by gui_graph_editor.py. Expects the plugin loaded and `groomlab` importable.

They never move the real mouse or need the OS focus: hover(), drag() and activate() (gui_support)
and press() below send the events to the widgets. Tests that count undo steps call settle_undo()
first: Maya's deferred idle work (File New's callbacks, selection mask resets) then cannot slip a
step of its own between the test's steps.
"""

import json
import math
import os
import unittest

from maya import cmds

import groomlab
from groomlab.ui import menu as _menu

from gui_support import CLASSIC_GRAPH, activate, drag, hover, settle_undo

_menu.ASK_TO_SAVE = False  # no save prompt in tests (scenes stay untitled here)


def classic_links():
    return sorted((c["from"], c["to"], c["input"]) for c in CLASSIC_GRAPH["connections"])


def wait(ms):
    """Let Qt run events and timers for `ms` milliseconds (PySide2 has no QTest.qWait)."""
    import time
    from PySide2 import QtWidgets
    end = time.time() + ms / 1000.0
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.005)
    QtWidgets.QApplication.processEvents()


def press(widget, key, modifiers=None):
    """A key pressed on widget as a user would: its window active and widget focused (activate:
    Qt's own, whatever window the OS has in front, so shortcuts reach it)."""
    from PySide2 import QtCore, QtTest
    assert activate(widget), "no keyboard focus for %s" % widget
    QtTest.QTest.keyClick(widget, key, QtCore.Qt.NoModifier if modifiers is None else modifiers)


def at_frame(t):
    """Go to a frame and let Maya's idle work run (scriptJobs fire from its idle queue), then Qt's."""
    cmds.currentTime(t)
    cmds.flushIdleQueue()
    wait(30)


class GraphEditorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide2 import QtWidgets
        cls.app = QtWidgets.QApplication.instance()

    def setUp(self):
        from groomlab.ui import graph_editor
        self.ge = graph_editor
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        # File New's deferred callbacks (HumanIK's records an undoable command) run now, not between
        # a test's steps, where a test's Ctrl+Z would undo them instead.
        settle_undo()
        scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        self.groom = groomlab.create_groom(scalp)
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)
        cmds.setAttr(self.groom + ".previewDensity", 1.0)
        self.editor = graph_editor.GraphEditor(self.groom)

    def tearDown(self):
        self.editor.deleteLater()

    def test_shows_the_groom_graph(self):
        model = self.editor.model
        self.assertEqual(set(self.editor.items), set(model.order))
        edges = [i for i in self.editor.scene.items() if isinstance(i, self.ge.EdgeItem)]
        self.assertEqual(len(edges), len(model.connections))
        self.assertIn(self.groom, [self.editor.groom_box.itemText(i) for i in range(self.editor.groom_box.count())])

    def test_motion_nodes_come_from_the_add_menu_and_notes_badge_them(self):
        cats = dict(self.ge.categories(self.editor.catalog))
        self.assertEqual(cats["Motion"], ["Wind", "Jiggle", "Simulate"])
        self.editor.items["resample"].setSelected(True)
        name = self.editor.add_node("Jiggle")
        self.assertEqual(self.editor.model.source_of(name, "strands"), "resample")
        self.assertEqual(self.editor.model.source_of("output", "strands"), name)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.editor.refresh_status()
        self.assertNotIn(name, self.editor.node_warnings)  # the scalp is here: history, no note
        # A node's note (not an error: a Jiggle without scalp motion) shows as its warning badge.
        real = cmds.groomLabGraphStats

        def stats(groom, **flags):
            if flags:
                return real(groom, **flags)
            data = json.loads(real(groom))
            data["notes"] = {name: "no scalp motion history here"}
            return json.dumps(data)

        cmds.groomLabGraphStats = stats
        try:
            self.editor.refresh_status()
        finally:
            cmds.groomLabGraphStats = real
        self.assertEqual(self.editor.node_notes.get(name), "no scalp motion history here")
        self.assertIn("no scalp motion", self.editor.items[name].toolTip())

    def test_add_node_after_selection_applies_live(self):
        self.editor.items["length"].setSelected(True)
        name = self.editor.add_node("Curl")
        model = self.editor.model
        self.assertEqual(model.source_of(name, "strands"), "length")
        self.assertEqual(model.source_of("width", "strands"), name)
        stored = groomlab.get_graph(self.groom)
        self.assertIn(name, [n["name"] for n in stored["nodes"]])  # Live applied it
        self.assertIn("strands", self.editor.status.text())

        cmds.undo()  # the whole edit is one undo step
        self.assertNotIn(name, [n["name"] for n in groomlab.get_graph(self.groom, expand_default=True)["nodes"]])

    def test_param_panel_follows_selection_and_edits(self):
        self.editor.items["scatter"].setSelected(True)
        self.assertEqual(self.editor.params.node, "scatter")
        # density is mappable: a MappableEditor switching to a color set.
        editors = self.editor.params.findChildren(self.ge.MappableEditor)
        self.assertEqual(len(editors), 1)
        self.editor.set_param("scatter", "density", 50.0)
        self.assertEqual(groomlab.get_graph(self.groom)["nodes"][2]["params"]["density"], 50.0)

    def test_sliders_stretch_preview_and_commit_once(self):
        from groomlab.ui import widgets
        widgets.forget_range("Scatter.density")
        self.editor.items["scatter"].setSelected(True)
        slider = self.editor.params.findChildren(widgets.SoftSlider)[0]
        self.assertEqual((slider.lo, slider.hi), (0.0, 400.0))  # density: centered on real hair's 200/cm²
        try:
            slider.field.setValue(750)
            slider.field.editingFinished.emit()
            self.assertEqual((slider.lo, slider.hi), (0.0, 1000.0))  # stretched to reach the typed value
            self.assertEqual(groomlab.get_graph(self.groom)["nodes"][2]["params"]["density"], 750)
            # Only this slider: other Scatter nodes still start from the typical range.
            self.assertIsNone(widgets.remembered_range("Scatter.density"))
            other = widgets.SoftSlider(200.0, 0.0, 400.0, key="Scatter.density", default=200.0)
            self.assertEqual((other.lo, other.hi), (0.0, 400.0))
            other.deleteLater()

            # Dragging previews outside undo; releasing commits one undo step.
            settle_undo()
            slider.slider.setSliderDown(True)
            slider.slider.setValue(300)
            wait(80)  # previews are coalesced: written within 40 ms
            self.assertEqual(groomlab.get_graph(self.groom)["nodes"][2]["params"]["density"], 300)
            slider.slider.setValue(400)
            slider.slider.setSliderDown(False)
            self.assertEqual(groomlab.get_graph(self.groom)["nodes"][2]["params"]["density"], 400)
            cmds.undo()
            self.assertEqual(groomlab.get_graph(self.groom)["nodes"][2]["params"]["density"], 750)
        finally:
            widgets.forget_range("Scatter.density")

    def test_dragging_a_link_onto_a_port_connects_without_selecting(self):
        from PySide2 import QtCore
        self.editor.unlink("interpolate", "guides")
        self.editor.resize(1200, 600)
        self.editor.show()
        QtWidgets = __import__("PySide2.QtWidgets", fromlist=["QtWidgets"])
        QtWidgets.QApplication.processEvents()
        view = self.editor.view
        src = view.mapFromScene(self.editor.items["guides"].output.scene_center())
        # Drop a few pixels off the port: the link snaps to it.
        dst = view.mapFromScene(self.editor.items["interpolate"].inputs["guides"].scene_center()) + QtCore.QPoint(6, 5)
        drag(view.viewport(), src, dst)
        self.assertEqual(self.editor.model.source_of("interpolate", "guides"), "guides")
        self.assertEqual(self.editor.scene.selectedItems(), [])  # no rubber band from the port drag
        self.editor.hide()

    def test_alt_shift_drag_cuts_the_links_it_crosses(self):
        # Across length -> width: the link goes; the output left unconnected, the groom keeps its
        # graph (not applied), and Ctrl+Z puts the editor back to it.
        QtCore, QtWidgets = self.ge.QtCore, self.ge.QtWidgets
        self.editor.resize(1200, 600)
        self.editor.show()
        QtWidgets.QApplication.processEvents()
        try:
            view = self.editor.view
            middle = self.edge("length", "width").path().pointAtPercent(0.5)
            start = view.mapFromScene(middle - QtCore.QPointF(0, 30))
            end = view.mapFromScene(middle + QtCore.QPointF(0, 30))
            settle_undo()
            before = cmds.undoInfo(query=True, undoName=True)
            drag(view.viewport(), start, end, steps=4, modifiers=QtCore.Qt.AltModifier | QtCore.Qt.ShiftModifier)
            self.assertIsNone(self.editor.model.source_of("width", "strands"))
            self.assertFalse(any(e.cutting for e in self.editor.scene.items() if isinstance(e, self.ge.EdgeItem)))
            self.assertIsNone(self.editor.scene._cut_line)
            self.assertEqual(self.stored_links(), classic_links())  # (the groom's graph as it was)
            self.assertTrue(self.editor.undo())
            self.assertEqual(self.editor.model.source_of("width", "strands"), "length")
            self.assertEqual(cmds.undoInfo(query=True, undoName=True), before, "Maya's history untouched")
        finally:
            self.editor.hide()

    def test_pins_route_links_and_snap_puts_them_on_the_grid(self):
        # Double-click a link: a pin there, the link through it; no undo step (layout), the groom's
        # graph keeps it. Delete takes it out. Snap lands a dragged node on the grid.
        from PySide2 import QtTest
        QtCore, QtWidgets = self.ge.QtCore, self.ge.QtWidgets
        self.editor.resize(1200, 600)
        self.editor.show()
        QtWidgets.QApplication.processEvents()
        try:
            view = self.editor.view
            middle = self.edge("length", "width").path().pointAtPercent(0.5)
            settle_undo()
            before = cmds.undoInfo(query=True, undoName=True)
            QtTest.QTest.mouseDClick(view.viewport(), QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, view.mapFromScene(middle))
            wait(50)
            pins = self.editor.model.route("width", "strands")
            self.assertEqual(len(pins), 1)
            pin = self.editor.pin_items[pins[0]]
            self.assertTrue(pin.isSelected())
            self.assertEqual(self.edge("length", "width").pins, [pin])
            self.assertEqual(cmds.undoInfo(query=True, undoName=True), before, "a pin is layout: no undo step")
            self.assertEqual(groomlab.get_graph(self.groom)["pins"]["routes"][0]["to"], "width")
            # Moved, the link follows it.
            pin.setPos(pin.pos() + QtCore.QPointF(0, 120))
            path = self.edge("length", "width").path()
            self.assertTrue(any((path.pointAtPercent(i / 64.0) - pin.scenePos()).manhattanLength() < 2 for i in range(65)))
            self.editor.scene.clearSelection()
            pin.setSelected(True)
            self.editor.delete_selected()
            self.assertEqual(self.editor.model.route("width", "strands"), [])
            self.assertEqual(self.editor.model.source_of("width", "strands"), "length", "the link stays")
            self.assertNotIn("pins", groomlab.get_graph(self.groom))
            # Snap: a node dragged lands on the grid's dots.
            self.editor.snap_box.setChecked(True)
            node = self.editor.items["width"]
            start = view.mapFromScene(node.scenePos() + QtCore.QPointF(20, 8))
            drag(view.viewport(), start, start + QtCore.QPoint(37, 29), steps=3)
            grid = self.ge.GRID
            self.assertEqual((node.pos().x() % grid, node.pos().y() % grid), (0.0, 0.0))
            self.assertEqual(self.editor.model.position("width"), (node.pos().x(), node.pos().y()))
        finally:
            self.editor.snap_box.setChecked(False)
            self.editor.hide()

    def test_links_drag_out_of_pins_and_into_them(self):
        # Blueprints' reroute nodes: a link dragged from a pin's nub carries its signal, through the
        # pins up to it; a link from an output dropped on a pin feeds every link through it.
        QtCore, QtWidgets = self.ge.QtCore, self.ge.QtWidgets
        self.editor.resize(1200, 600)
        self.editor.show()
        QtWidgets.QApplication.processEvents()
        try:
            view, model = self.editor.view, self.editor.model
            width = self.editor.items["width"]
            under = width.scenePos() + QtCore.QPointF(-60, width.height + 80)
            pin = model.add_pin("width", "strands", under.x(), under.y())
            self.editor.rebuild()
            QtWidgets.QApplication.processEvents()
            item = self.editor.pin_items[pin]
            nub = view.mapFromScene(item.scenePos() + QtCore.QPointF(self.ge.PinItem.NUB, 0))
            port = view.mapFromScene(self.editor.items["resample"].inputs["strands"].scene_center())
            drag(view.viewport(), nub, port, steps=4)
            self.assertEqual(model.source_of("resample", "strands"), "length")
            self.assertEqual(model.route("resample", "strands"), [pin])
            self.assertEqual(groomlab.get_graph(self.groom)["pins"]["routes"][0]["via"], [pin])
            # The interpolate's output dropped on the pin: both links come from it now, through the pin.
            item = self.editor.pin_items[pin]
            out = view.mapFromScene(self.editor.items["interpolate"].output.scene_center())
            drag(view.viewport(), out, view.mapFromScene(item.scenePos() + QtCore.QPointF(-2, 0)), steps=4)
            self.assertEqual(model.source_of("width", "strands"), "interpolate")
            self.assertEqual(model.source_of("resample", "strands"), "interpolate")
            self.assertEqual((model.route("width", "strands"), model.route("resample", "strands")), ([pin], [pin]))
            stored = {(c["from"], c["to"], c["input"]) for c in groomlab.get_graph(self.groom)["connections"]}
            self.assertIn(("interpolate", "resample", "strands"), stored)
            self.assertTrue(self.editor.undo())  # the feed: one step
            wait(50)
            self.assertEqual(self.editor.model.source_of("resample", "strands"), "length")
        finally:
            self.editor.hide()

    def test_a_cut_is_one_edit(self):
        # Two links at once, the output left unconnected: one Ctrl+Z puts both back.
        settle_undo()
        self.editor.cut_links([self.edge("length", "width").link(), self.edge("width", "resample").link()])
        self.assertIsNone(self.editor.model.source_of("width", "strands"))
        self.assertIsNone(self.editor.model.source_of("resample", "strands"))
        self.assertTrue(self.editor.undo())
        self.assertEqual(self.editor.model.source_of("width", "strands"), "length")
        self.assertEqual(self.editor.model.source_of("resample", "strands"), "width")
        # An optional link (Interpolate's scalp): applied, one step of Maya's history.
        self.editor.cut_links([self.edge("scalp", "interpolate").link()])
        self.assertNotIn(("scalp", "interpolate", "scalp"), self.stored_links())
        self.assertIn("cut 1 link", self.editor.status.text())
        self.assertTrue(self.editor.undo())
        wait(50)
        self.assertEqual(self.stored_links(), classic_links())
        self.assertEqual(self.editor.model.source_of("interpolate", "scalp"), "scalp")

    def stored_links(self):
        return sorted((c["from"], c["to"], c["input"]) for c in groomlab.get_graph(self.groom)["connections"])

    def test_tab_added_nodes_wire_after_the_selection(self):
        self.editor.items["length"].setSelected(True)
        search = self.ge.NodeSearch(self.editor, (321.0, 654.0))
        search.choose("Curl")
        name = [n for n in self.editor.model.order if self.editor.model.node_type(n) == "Curl"][0]
        self.assertEqual(self.editor.model.source_of(name, "strands"), "length")
        self.assertEqual(self.editor.model.source_of("width", "strands"), name)
        self.assertEqual(self.editor.model.position(name), (321.0, 654.0))  # where Tab was pressed
        self.assertIn("after length", self.editor.status.text())

    def edge(self, src, dst):
        return next(i for i in self.editor.scene.items() if isinstance(i, self.ge.EdgeItem)
                    and i.src_item.name == src and i.dst_item.name == dst)

    def test_tab_on_a_wire_inserts_the_node_there(self):
        # Over the length -> width wire, nothing selected: the new node goes into that wire.
        search = self.ge.NodeSearch(self.editor, (321.0, 654.0), on_link=self.edge("length", "width").link())
        search.choose("Curl")
        name = [n for n in self.editor.model.order if self.editor.model.node_type(n) == "Curl"][0]
        self.assertEqual(self.editor.model.source_of(name, "strands"), "length")
        self.assertEqual(self.editor.model.source_of("width", "strands"), name)
        # A node that can't take what the wire carries is added unlinked, and says why.
        search = self.ge.NodeSearch(self.editor, (0.0, 0.0), on_link=self.edge("guides", "interpolate").link())
        search.choose("Value")
        value = [n for n in self.editor.model.order if self.editor.model.node_type(n) == "Value"][0]
        self.assertEqual(self.editor.model.source_of("interpolate", "guides"), "guides")
        self.assertFalse([c for c in self.editor.model.connections if value in (c["from"], c["to"])])
        self.assertIn("Not inserted", self.editor.status.text())

    def test_a_node_dropped_on_a_wire_goes_into_it(self):
        from PySide2 import QtCore, QtWidgets
        self.editor.resize(1400, 700)
        self.editor.show()
        QtWidgets.QApplication.processEvents()
        try:
            view = self.editor.view
            wire = self.edge("length", "width")
            # An unwired Curl away from the graph, dragged by its title until its body (not the
            # cursor) lies over the wire's middle.
            rect = self.editor.scene.itemsBoundingRect()
            name = self.editor.add_node("Curl", at=(rect.center().x(), rect.bottom() + 120), wire=False)
            self.editor.scene.clearSelection()
            QtWidgets.QApplication.processEvents()
            item = self.editor.items[name]
            title = item.sceneBoundingRect().topLeft() + QtCore.QPointF(30, 8)
            target = self.edge("length", "width").path().pointAtPercent(0.5) - QtCore.QPointF(0, 20)
            drag(view.viewport(), view.mapFromScene(title), view.mapFromScene(target), steps=6)
            QtWidgets.QApplication.processEvents()
            wait(50)
            self.assertEqual(self.editor.model.source_of(name, "strands"), "length")
            self.assertEqual(self.editor.model.source_of("width", "strands"), name)
            self.assertIn("inserted %s" % name, self.editor.status.text())
            del wire
        finally:
            self.editor.hide()

    def test_tab_offers_the_painted_maps_as_map_nodes(self):
        groomlab.add_map(self.groom, "freckles", 0.5, kind="vertices")
        search = self.ge.NodeSearch(self.editor, (10.0, 20.0))
        for text in ("freck", "mask", "paint"):
            search.fill(text)
            keys = [search.list.item(i).data(0x0100) for i in range(search.list.count())]  # Qt.UserRole
            self.assertIn(self.ge.MAP_ENTRY + "freckles", keys, text)
        search.choose(self.ge.MAP_ENTRY + "freckles")
        maps = [n for n in self.editor.model.order if self.editor.model.node_type(n) == "Map"]
        self.assertEqual(len(maps), 1)
        self.assertEqual(self.editor.model.param(maps[0], "name"), "freckles")
        self.assertEqual(maps[0], "freckles")

    def test_an_invalid_link_drop_says_why_and_keeps_the_link(self):
        from PySide2 import QtCore, QtWidgets
        self.editor.resize(1200, 600)
        self.editor.show()
        QtWidgets.QApplication.processEvents()
        try:
            view = self.editor.view
            # Pick up interpolate's guides link and drop it on the scalp's mesh input of a Scatter
            # (Guides can't go there): it says why and the link stays.
            src = view.mapFromScene(self.editor.items["interpolate"].inputs["guides"].scene_center())
            scene = self.editor.scene
            port = self.editor.items["guides"].output
            ports = scene.ports()

            def lonely(p):  # can't take Guides, and no port nearby that could (the drop would snap there)
                return scene.incompatible_reason(port, p) and not p.is_output and not any(
                    scene.compatible(port, q) and QtCore.QLineF(q.scene_center(), p.scene_center()).length()
                    < self.ge.SNAP * 2 for q in ports)
            target = next(p for p in ports if lonely(p))
            dst = view.mapFromScene(target.scene_center())
            self.assertIn("Guides", scene.incompatible_reason(port, target))
            drag(view.viewport(), src, dst)
            self.assertEqual(self.editor.model.source_of("interpolate", "guides"), "guides")  # restored
            self.assertIn("Not linked", self.editor.status.text())
        finally:
            self.editor.hide()

    def test_a_map_node_links_from_the_output_dragged(self):
        # A Map node shows one labelled port per output (luminance, r, g, b, a) down its right
        # edge; a link dragged from g reads g, is drawn from g, and one undo takes it away.
        from PySide2 import QtGui, QtWidgets
        groomlab.add_map(self.groom, "packed", 1.0, kind="vertices", color=True)
        self.editor.reload()
        name = self.editor.add_node("Map")
        self.editor.set_param(name, "name", "packed")
        self.editor.set_param_exposed("length", "keep", True)
        self.editor.rebuild()
        item = self.editor.items[name]
        self.assertEqual(list(item.outputs), ["luminance", "r", "g", "b", "a", "rgb"])
        self.assertIs(item.output, item.outputs["luminance"])
        ys = [port.pos().y() for port in item.outputs.values()]
        self.assertEqual(ys, sorted(ys))  # stacked down the edge
        self.assertTrue(all(port.pos().x() == self.ge.NODE_W for port in item.outputs.values()))
        self.assertGreaterEqual(item.height, self.ge.HEADER_H + 5 * self.ge.ROW_H)  # grown to hold them
        self.assertEqual({p.port_type for p in item.outputs.values()}, {"Field"})
        settle_undo()  # the edits below are the undo steps on top of the queue
        # The Map node just left of Length (a layout move, in the graph outside undo), all of it in view.
        x, y = self.editor.model.position("length")
        self.editor.model.set_position(name, x - 280, y + 150)
        self.editor.silently(lambda: self.editor.model.save_layout(self.groom))
        before = groomlab.get_graph(self.groom)
        self.editor.resize(1400, 800)
        self.editor.show()
        wait(100)
        self.editor.rebuild()
        item = self.editor.items[name]
        view = self.editor.view
        view.auto_frame = False
        view.setTransform(QtGui.QTransform.fromScale(0.8, 0.8))
        view.centerOn(self.editor.items["length"])
        QtWidgets.QApplication.processEvents()
        src = view.mapFromScene(item.outputs["g"].scene_center())
        dst = view.mapFromScene(self.editor.items["length"].inputs["keep"].scene_center())
        drag(view.viewport(), src, dst)
        self.assertEqual(self.editor.model.source_of("length", "keep"), name)
        self.assertEqual(self.editor.model.output_of("length", "keep"), "g")
        stored = groomlab.get_graph(self.groom)
        self.assertIn({"from": name, "to": "length", "input": "keep", "output": "g"}, stored["connections"])
        self.assertEqual(groomlab.errors(self.groom), [])
        edge = next(e for e in self.editor.scene.items() if isinstance(e, self.ge.EdgeItem) and e.src_item.name == name)
        self.assertEqual(edge.output, "g")
        self.assertEqual(edge.path().pointAtPercent(0.0), self.editor.items[name].outputs["g"].scene_center())
        self.assertTrue(self.editor.items[name].outputs["g"].linked())
        self.assertFalse(self.editor.items[name].outputs["r"].linked())
        # Picking the link up at its input carries it from g: dropped on width's width input.
        self.editor.set_param_exposed("width", "width", True)
        self.editor.rebuild()
        QtWidgets.QApplication.processEvents()
        pick = view.mapFromScene(self.editor.items["length"].inputs["keep"].scene_center())
        drop = view.mapFromScene(self.editor.items["width"].inputs["width"].scene_center())
        drag(view.viewport(), pick, drop)
        self.assertIsNone(self.editor.model.source_of("length", "keep"))
        self.assertEqual(self.editor.model.output_of("width", "width"), "g")
        # Duplicating the Map node and its reader keeps the output (applied: the copy's strands input
        # is free, but nothing the output is made from reads it); undo takes the applied edits back,
        # one step each.
        self.editor.scene.clearSelection()
        copies = self.editor.duplicate([name, "width"])
        width_copy = next(n for n in copies if self.editor.model.node_type(n) == "Width")
        self.assertEqual(self.editor.model.source_of(width_copy, "width"), next(n for n in copies if n != width_copy))
        self.assertEqual(self.editor.model.output_of(width_copy, "width"), "g")
        for _ in range(3):  # the duplicate, the move, then exposing width's width
            self.editor.undo()
            wait(50)
        self.assertEqual(self.editor.model.output_of("length", "keep"), "g")  # back before the move
        self.assertNotIn(width_copy, self.editor.model.nodes)
        self.editor.undo()
        wait(50)
        self.assertEqual(groomlab.get_graph(self.groom), before)
        self.assertIsNone(self.editor.model.source_of("length", "keep"))
        self.editor.redo()
        wait(50)
        self.assertEqual(self.editor.model.output_of("length", "keep"), "g")
        # Feed Param on a color map's node feeds from the output picked: one submenu per output.
        self.editor.params.show_node(name)
        feed = self.editor.params.findChild(QtWidgets.QPushButton, "nodeAction_FeedParam")
        self.assertEqual([a.text() for a in feed.menu().actions()],
                         ["From luminance", "From r", "From g", "From b", "From a", "From rgb"])
        # rgb (the map's color) feeds color params only.
        from_rgb = feed.menu().actions()[5].menu()
        for node_menu in [a.menu() for a in from_rgb.actions()]:
            for action in node_menu.actions():
                self.assertTrue(self.editor.model.param_spec(node_menu.title().split(" ")[0], action.text()).get("colorField"))
        from_b = feed.menu().actions()[3].menu()
        scatter = next(a.menu() for a in from_b.actions() if a.text().startswith("scatter"))
        next(a for a in scatter.actions() if a.text() == "density").trigger()
        wait(150)
        self.assertEqual(self.editor.model.output_of("scatter", "density"), "b")
        self.assertIn({"from": name, "to": "scatter", "input": "density", "output": "b"},
                      groomlab.get_graph(self.groom)["connections"])
        self.editor.hide()

    def test_a_map_nodes_panel_fits_a_narrow_editor(self):
        # The Map node's panel on a narrow editor: its warnings and where the map lives each take
        # a full-width row above the buttons (not a column squeezed beside them), the path breaks
        # at its folders, and the name box stays wide enough to read.
        from PySide2 import QtWidgets
        groomlab.add_map(self.groom, "grow", 0.5, kind="texture", resolution=16)
        self.editor.reload()
        name = self.editor.add_node("Map")
        self.editor.set_param(name, "name", "grow")
        self.editor.resize(420, 700)
        self.editor.show()
        self.editor.params.show_node(name)
        wait(100)
        panel = self.editor.params
        hint = panel.findChild(QtWidgets.QLabel, "nodeAction_MaskUnconnected")
        info = panel.findChild(QtWidgets.QLabel, "nodeAction_MapInfo")
        feed = panel.findChild(QtWidgets.QPushButton, "nodeAction_FeedParam")
        paint = panel.findChild(QtWidgets.QPushButton, "nodeAction_Paint")
        top = lambda w: w.mapTo(panel, w.rect().topLeft()).y()
        left = lambda w: w.mapTo(panel, w.rect().topLeft()).x()
        self.assertLess(top(hint) + hint.height(), top(info) + 1)  # one row each, stacked
        self.assertLess(top(info) + info.height(), top(feed) + 1)  # the buttons below the text
        self.assertEqual(top(feed), top(paint))  # the buttons side by side
        self.assertEqual(left(info), left(hint))
        self.assertGreater(info.width(), feed.width() + paint.width())  # the row's width, not a column
        self.assertIn("/​", info.text())  # the path breaks at its folders
        self.assertIn(groomlab.map_texture(self.groom, "grow"), info.toolTip())
        box = next(c for c in panel.findChildren(QtWidgets.QComboBox) if c.currentText() == "grow")
        self.assertGreaterEqual(box.width(), box.fontMetrics().horizontalAdvance("M" * 10))
        shot = os.environ.get("GROOMLAB_UI_SHOT")  # a look at the panel, when asked for
        if shot:
            panel.grab().save(shot)
        self.editor.hide()

    def test_a_map_nodes_panel_relinks_a_missing_image(self):
        # A Map node reading a texture map whose image is missing warns, naming the file, with a
        # Relink button (its dialog opens only from the click; relink_map_image takes the path).
        import tempfile
        from PySide2 import QtWidgets
        folder = tempfile.mkdtemp(prefix="groomlab_relinkge_").replace("\\", "/")
        cmds.workspace(folder, openWorkspace=True)
        groomlab.add_map(self.groom, "grow", 0.5, kind="texture", resolution=16)
        image = groomlab.map_texture(self.groom, "grow")
        moved = folder + "/found/grow.png"
        os.makedirs(os.path.dirname(moved))
        os.rename(image, moved)
        self.editor.reload()
        name = self.editor.add_node("Map")
        self.editor.set_param(name, "name", "grow")
        panel = self.editor.params
        panel.show_node(name)
        lost = panel.findChild(QtWidgets.QLabel, "nodeAction_MapImageMissing")
        self.assertIsNotNone(lost)
        self.assertIn(image, lost.text())
        self.assertIsNotNone(panel.findChild(QtWidgets.QPushButton, "nodeAction_RelinkImage"))
        self.assertEqual(panel.relink_map_image("grow", moved), "found/grow.png")
        self.assertEqual(groomlab.map_texture(self.groom, "grow"), moved)
        self.assertIn("grow reads found/grow.png", self.editor.status.text())
        panel.show_node(name)
        self.assertIsNone(panel.findChild(QtWidgets.QLabel, "nodeAction_MapImageMissing"))
        self.assertIsNone(panel.findChild(QtWidgets.QPushButton, "nodeAction_RelinkImage"))
        self.assertIsNone(panel.relink_map_image("grow", folder + "/nothing.png"))  # reported, not raised
        self.assertIn("no image", self.editor.status.text())

    def test_expose_a_param_and_drive_it(self):
        self.editor.set_param_exposed("length", "keep", True)
        self.editor.rebuild()
        self.assertTrue(self.editor.items["length"].inputs["keep"].is_param)
        name = self.editor.add_node("FieldNoise")
        self.editor.link(name, "length", "keep")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertIn("keep", [n.get("paramInputs", [None])[0] for n in groomlab.get_graph(self.groom)["nodes"]
                                 if n["name"] == "length"])
        self.editor.scene.clearSelection()
        self.editor.items["length"].setSelected(True)
        self.assertEqual(self.editor.params.node, "length")
        labels = [w.text() for w in self.editor.params.findChildren(self.ge.QtWidgets.QLabel)]
        self.assertTrue(any("driven by" in t or "×" in t for t in labels))  # the field named beside the value

    def test_expand_inputs_button_toggles_every_connectable_param(self):
        from PySide2 import QtCore, QtWidgets
        item = self.editor.items["length"]
        params = item.mappable()
        self.assertGreater(len(params), 1)
        self.assertIn("inputs", item.toggle_rects())
        self.editor.toggle_param_inputs("length")
        self.assertEqual(self.editor.model.param_inputs("length"), params)
        x, y = self.editor.model.position("length")
        value = self.editor.add_node("Value", at=(x - 280, y + 100), params={"kind": "number", "number": 0.5})
        self.editor.resize(1200, 700)
        self.editor.show()
        QtWidgets.QApplication.processEvents()
        view = self.editor.view
        src = view.mapFromScene(self.editor.items[value].output.scene_center())
        dst = view.mapFromScene(self.editor.items["length"].inputs["keep"].scene_center())
        drag(view.viewport(), src, dst)
        self.assertEqual(self.editor.model.source_of("length", "keep"), value)
        # Collapse with the live connection: its port and wire survive; the unused ports go.
        self.editor.toggle_param_inputs("length")
        self.assertEqual(self.editor.model.param_inputs("length"), ["keep"])
        # Pick up that wire and drop it on empty space, through the real scene event path.
        pick = view.mapFromScene(self.editor.items["length"].inputs["keep"].scene_center())
        empty = view.mapFromScene(self.editor.items["length"].sceneBoundingRect().bottomLeft() + QtCore.QPointF(-80, 80))
        drag(view.viewport(), pick, empty)
        self.assertIsNone(self.editor.model.source_of("length", "keep"))
        self.assertIsNone(self.editor.scene._drag_port)
        # Expanding again recreates compatible sockets; the disconnected Value can wire back in.
        self.editor.toggle_param_inputs("length")
        self.assertEqual(self.editor.model.param_inputs("length"), params)
        src = view.mapFromScene(self.editor.items[value].output.scene_center())
        dst = view.mapFromScene(self.editor.items["length"].inputs["keep"].scene_center())
        drag(view.viewport(), src, dst)
        self.assertEqual(self.editor.model.source_of("length", "keep"), value)
        self.assertIsNone(self.editor.scene._drag_port)

    def test_search_backdrops_badges_reset(self):
        from PySide2 import QtCore, QtWidgets
        from groomlab.ui import widgets
        # Type-to-search: the best match first, Enter adds it where asked.
        self.editor.show_search(QtCore.QPointF(500, 300), QtCore.QPoint(100, 100))
        popup = self.editor._search
        popup.field.setText("noi")
        self.assertEqual(popup.list.item(0).data(QtCore.Qt.UserRole), "Noise")
        popup.accept_first()
        noise = [n for n in self.editor.model.order if self.editor.model.node_type(n) == "Noise"][0]
        self.assertEqual(self.editor.model.position(noise), (500 - self.ge.NODE_W / 2, 300 - self.ge.HEADER_H / 2))
        popup.field.setText("remap curve")  # words from descriptions count too
        self.assertIn("FieldRemap", [popup.list.item(i).data(QtCore.Qt.UserRole) for i in range(popup.list.count())])
        popup.close()
        self.editor.delete_nodes([noise])  # unconnected, it would keep the graph from applying

        # Backdrops are layout: stored in the graph's "backdrops" (as every host keeps the layout),
        # which evaluation never reads.
        self.editor.scene.clearSelection()
        self.editor.items["scalp"].setSelected(True)
        self.editor.items["scatter"].setSelected(True)
        self.editor.add_backdrop("Roots")
        self.assertEqual(groomlab.get_graph(self.groom)["backdrops"][0]["title"], "Roots")
        self.assertEqual(groomlab.errors(self.groom), [])
        backdrop = [i for i in self.editor.scene.items() if isinstance(i, self.ge.BackdropItem)][0]
        self.editor.scene.clearSelection()
        backdrop.setSelected(True)
        self.editor.delete_selected()
        self.assertEqual(self.editor.model.backdrops, [])

        # An error shows on its node.
        self.editor.set_param("scatter", "density", {"colorSet": "missing", "min": 0, "max": 100})
        self.assertIn("scatter", self.editor.node_errors)
        # Reset to default from the slider.
        self.editor.set_param("scatter", "density", 350.0)
        self.editor.scene.clearSelection()
        self.editor.items["scatter"].setSelected(True)
        slider = self.editor.params.findChildren(widgets.SoftSlider)[0]
        slider._reset_value()
        self.assertEqual(groomlab.get_graph(self.groom)["nodes"][2]["params"]["density"], 80.0)
        self.assertEqual(self.editor.node_errors, {})
        self.assertTrue(self.editor.node_times)  # timing tags

        # View a node, bypass one.
        self.editor.view_node("scatter")
        self.assertEqual(cmds.getAttr(self.groom + ".viewNode"), "scatter")
        self.editor.view_node(None)
        self.editor.set_bypass(["length"], True)
        self.assertTrue(self.editor.model.bypassed("length"))
        self.editor.set_bypass(["length"], False)

        # When it runs (the node's Evaluate menu): render only, on the groom's graph; back to always.
        def evaluation():
            return [n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == "length"][0].get("evaluation")
        self.editor.set_evaluation(["length"], "renderOnly")
        self.assertEqual(self.editor.model.evaluation("length"), "renderOnly")
        self.assertEqual(evaluation(), "renderOnly")
        self.editor.items["length"].update()  # its tag under the node
        wait(30)
        self.assertEqual(self.editor.node_errors, {})
        self.editor.set_evaluation(["length"], "always")
        self.assertIsNone(evaluation())

    def _select_node(self, name):
        self.editor.scene.clearSelection()
        self.editor.items[name].setSelected(True)

    def _action(self, label):
        from PySide2 import QtWidgets
        buttons = [b for b in self.editor.params.findChildren(QtWidgets.QPushButton)
                   if b.objectName() == "nodeAction_" + label.replace(" ", "").replace(".", "")]
        self.assertEqual(len(buttons), 1, label)
        return buttons[0]

    def test_node_actions(self):
        # Braid: selected curves become its paths (a Guides node for them is made and connected).
        braid = self.editor.add_node("Braid")
        self.editor.link("width", braid, "strands")
        curve = cmds.curve(degree=2, point=[(0.0, 1.0, 0.0), (0.0, 1.5, 0.3), (0.0, 1.6, 1.2)])
        self._select_node(braid)
        cmds.select(curve)
        self._action("Paths from Selected Curves").click()
        wait(50)  # actions run once the click is over
        self.assertIn("braids", [g["name"] for g in groomlab.list_guide_groups(self.groom)], self.editor.status.text())
        paths = self.editor.model.source_of(braid, "braids")
        self.assertEqual(self.editor.model.node_type(paths), "Guides")

        # Instance: the selected mesh becomes the prototype.
        instance = self.editor.add_node("Instance")
        self.editor.link("width", instance, "strands")
        cube = cmds.polyCube(name="snowflake")[0]
        self._select_node(instance)
        cmds.select(cube)
        self._action("Prototype from Selection").click()
        wait(50)
        self.assertEqual(self.editor.model.param(instance, "prototype"), "snowflake")

        # HairColor and Cards carry their buttons too.
        color = self.editor.add_node("HairColor")
        self._select_node(color)
        self._action("Create Hair Shader")
        cards = self.editor.add_node("Cards")
        self._select_node(cards)
        self._action("Bake Maps")
        from PySide2 import QtWidgets as _qw
        show = [b for b in self.editor.params.findChildren(_qw.QCheckBox) if b.objectName() == "nodeAction_ShowCards"]
        self.assertEqual(len(show), 1)
        show[0].setChecked(True)  # the cards view, as A/B
        self.assertTrue(cmds.getAttr(self.groom + ".displayCards"))
        show[0].setChecked(False)
        self.assertFalse(cmds.getAttr(self.groom + ".displayCards"))
        textures = [b for b in self.editor.params.findChildren(_qw.QCheckBox) if b.objectName() == "nodeAction_ShowCardTextures"]
        self.assertEqual(len(textures), 1)
        self.assertTrue(textures[0].isChecked())  # on by default, like the attribute
        textures[0].setChecked(False)  # flat cards
        self.assertFalse(cmds.getAttr(self.groom + ".displayCardTextures"))
        textures[0].setChecked(True)
        self.assertTrue(cmds.getAttr(self.groom + ".displayCardTextures"))
        self._action("Export Unreal Cards...")
        self._action("Export Unity Cards...")  # present only: a click opens a file dialog
        self._action("Export glTF Cards...")
        self._action("Export Maps...")
        self._action("Bake Scalp")
        # Pinned over the params (graph_ui.json's nodeActions and Maya's own): in view however far the
        # params scroll, not in the scrolled body.
        panel = self.editor.params
        self.assertIsNotNone(panel.pinned)
        self.assertTrue(panel.pinned.isVisibleTo(panel))
        self.assertIs(self._action("Bake Maps").window(), panel.window())
        self.assertFalse(panel.widget().isAncestorOf(self._action("Bake Maps")))
        self.assertTrue(panel.pinned.isAncestorOf(self._action("Export Unreal Cards...")))
        self.assertGreaterEqual(panel.viewport().geometry().top(), panel.pinned.geometry().bottom())
        panel.verticalScrollBar().setValue(panel.verticalScrollBar().maximum())
        self.assertTrue(panel.pinned.isVisibleTo(panel))
        # Simulate: its range, bake and clear (the GroomLab > Simulate menu's) on the node itself.
        simulate = self.editor.add_node("Simulate")
        self._select_node(simulate)
        for label in ("Simulate Range", "Bake Simulation", "Clear Simulation"):
            self.assertTrue(panel.pinned.isAncestorOf(self._action(label)), label)
        self._action("Clear Simulation").click()
        wait(50)
        # StrandMesh: its mesh shows (displayStrandMesh); Mesh in Scene puts a Maya mesh on outStrandMesh.
        strand_mesh = self.editor.add_node("StrandMesh")
        self._select_node(strand_mesh)
        shown = [b for b in self.editor.params.findChildren(_qw.QCheckBox) if b.objectName() == "nodeAction_ShowStrandMesh"]
        self.assertEqual(len(shown), 1)
        self.assertTrue(shown[0].isChecked())
        self._action("Mesh in Scene").click()
        wait(50)
        self.assertTrue(cmds.listConnections(self.groom + ".outStrandMesh", destination=True, source=False))
        self._select_node("length")
        wait(10)  # (the last node's pinned actions go once the event is over)
        from PySide2 import QtWidgets
        self.assertFalse([b for b in self.editor.params.findChildren(QtWidgets.QPushButton)
                          if b.objectName().startswith("nodeAction_") and b.isVisibleTo(self.editor.params)])
        self.assertIsNone(self.editor.params.pinned)
        self.assertEqual(self.editor.params.viewportMargins().top(), 0)

    def test_selecting_a_sculpt_node_brushes_its_guides(self):
        # ToGuides' dense guides -> Sculpt -> Interpolate: selecting the Sculpt node makes it what
        # brushes shape, and the guides the viewport draws are its output.
        import tempfile
        import groomlab_core
        from groomlab import sculpts
        from PySide2 import QtWidgets
        model = self.editor.model
        self.editor.scene.clearSelection()
        to_guides = self.editor.add_node("ToGuides")
        self.editor.link("interpolate", to_guides, "strands")
        self.editor.scene.clearSelection()
        sculpt = self.editor.add_node("Sculpt")
        self.editor.link(to_guides, sculpt, "guides")
        self.editor.link("scalp", sculpt, "scalp")
        self.editor.scene.clearSelection()
        grown = self.editor.add_node("Interpolate")
        self.editor.link("scatter", grown, "strands")
        self.editor.link(sculpt, grown, "guides")
        self.editor.link(grown, "length", "strands")
        self.assertTrue(self.editor.apply(), self.editor.status.text())
        self.assertEqual(model.source_of(sculpt, "guides"), to_guides)
        self._select_node(sculpt)
        self.assertEqual(sculpts.target(self.groom), sculpt)
        self.assertIn("Brushes shape this node's guides", " ".join(
            l.text() for l in self.editor.params.findChildren(QtWidgets.QLabel)))
        self._action("Clear Sculpt Edits")
        # A stroke: the node's edits, not the guides'.
        guide_data = cmds.getAttr(self.groom + ".guideData")
        groomlab.brush_stroke(self.groom, "comb", [(-0.4, 1.4, 0.0), (0.4, 1.5, 0.4)], radius=0.8)
        self.assertGreater(sculpts.edited_guides(self.groom, sculpt), 0)
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), guide_data)
        # The drawn guides (the shape's evaluated hierarchy) are the node's output.
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        path = os.path.join(tempfile.mkdtemp(), "sculpt.groom")
        groomlab.export_groom(self.groom, path)
        session = groomlab_core.GroomSession(groomlab_core.read_groom_file(path).data)
        session.evaluate()
        out = session.evaluate_node(sculpt).guides
        self.assertTrue(out)
        for g in out[:5]:
            shown = cmds.groomLabGuides(self.groom, guidePoints=str(g.id), evaluated=True)
            self.assertEqual(len(shown), 3 * len(g.points))
            for k, p in enumerate(g.points):
                for i in range(3):
                    self.assertAlmostEqual(shown[3 * k + i], p[i], delta=1e-3)
        # The node's menu turns brushing it off: the base groom's guides again.
        self.editor.target_sculpt(None)
        self.assertIsNone(sculpts.target(self.groom))

    def test_a_direction_takes_an_expression(self):
        # Gravity's direction: three numbers, or (f(x)) an expression giving a vector per strand.
        from PySide2 import QtWidgets
        gravity = self.editor.add_node("Gravity")
        self.editor.link("width", gravity, "strands")
        self._select_node(gravity)
        wait(30)

        def child(kind, name):
            found = [w for w in self.editor.params.findChildren(kind) if w.objectName() == name]
            self.assertEqual(len(found), 1, name)
            return found[0]
        child(QtWidgets.QToolButton, "vectorExpressionToggle").setChecked(True)
        wait(50)  # applied once the click is over; the panel is built again
        self.assertEqual(self.editor.model.param(gravity, "direction"), {"expression": "[0, -1, 0]"})
        field = child(QtWidgets.QLineEdit, "vectorExpression")
        field.setText("normalize(N * -1 + [0, -1, 0])")
        field.editingFinished.emit()
        wait(30)
        self.assertEqual(self.editor.model.param(gravity, "direction"), {"expression": "normalize(N * -1 + [0, -1, 0])"})
        self.assertEqual(self.editor.node_errors, {})
        child(QtWidgets.QToolButton, "vectorExpressionToggle").setChecked(False)
        wait(50)
        self.assertEqual(self.editor.model.param(gravity, "direction"), [0.0, -1.0, 0.0])

    def test_pivot_picks_the_selection(self):
        # A world Transform's pivot is a world position: Pick beside it takes the selected locator's
        # (the editor stays on its groom while the locator is selected). Its translate is no world
        # position: no Pick.
        from PySide2 import QtWidgets
        tie = self.editor.add_node("Transform")
        self.editor.link("width", tie, "strands")
        self.editor.model.set_param(tie, "space", "world")
        self._select_node(tie)
        wait(30)
        pickers = [w for w in self.editor.params.findChildren(QtWidgets.QToolButton) if w.objectName() == "positionPick"]
        self.assertEqual(len(pickers), 1)
        locator = cmds.spaceLocator(name="tieHere")[0]
        try:
            cmds.xform(locator, worldSpace=True, translation=(0.5, 1.0, -2.0))
            cmds.select(locator)
            wait(30)
            pickers[0].click()
            wait(50)
            self.assertEqual([round(c, 4) for c in self.editor.model.param(tie, "pivot")], [0.5, 1.0, -2.0])
            self.assertEqual(self.editor.node_errors, {})
        finally:
            cmds.delete(locator)
            cmds.select(self.groom)

    def test_export_card_maps_window(self):
        # The Cards node's Export Maps... opens the window (never modal); a preset fills the packing
        # and the format, an edit turns it Custom, Export writes the files, Save as Preset keeps the
        # packing (its name typed beside the button), Delete removes it.
        import tempfile
        from groomlab.ui import card_maps_window
        presets = tempfile.mkdtemp(prefix="groomlab_card_presets_")
        os.environ["GROOMLAB_USER_CARD_PRESETS"] = presets
        try:
            cards = groomlab.create_cards(self.groom, max_cards=12)
            self.editor.reload()
            self._select_node(cards)
            self._action("Export Maps...").click()
            wait(50)
            panel = card_maps_window.window()
            self.assertIsNotNone(panel, "the window opened")
            try:
                self.assertEqual(panel.groom, self.groom)
                self.assertEqual(panel.preset.currentText(), "Unreal Hair Cards")
                self.assertEqual(panel.table.rowCount(), 7)
                self.assertEqual(panel.format.currentData(), "tga")
                panel.preset.setCurrentIndex(panel.preset.findText("Unity HDRP Hair"))
                self.assertEqual(panel.table.rowCount(), 5)
                self.assertEqual(panel.format.currentData(), "png")
                self.assertEqual(panel.normal.currentData(), "opengl")
                # Edited: Custom.
                cell = panel.table.cellWidget(2, 4)  # Mask's A
                cell.set_value("1-ao")
                self.assertEqual(panel.preset.currentText(), "Custom")
                self.assertEqual(panel.current_preset()["files"][2]["channels"], ["0", "ao", "root", "1-ao"])
                # Export at 64 px into a folder of our own.
                folder = tempfile.mkdtemp(prefix="groomlab_card_maps_ui_")
                panel.resolution.setCurrentText("64")
                panel.folder.setText(folder)
                panel.name.setText("ui")
                paths = panel.export()
                self.assertEqual(sorted(os.path.basename(p) for p in paths),
                                 sorted("ui_%s.png" % m for m in ("BaseColor", "Normal", "Mask", "Tangent", "Data")))
                self.assertTrue(panel.preview.isVisibleTo(panel), "the maps as thumbnails")
                self.assertIn("Wrote 5 maps", panel.status.text())
                # Saved as a preset, then deleted.
                panel.save_name.setText("UI Packed")
                self.assertTrue(panel.save_preset())
                self.assertEqual(panel.preset.currentText(), "UI Packed")
                self.assertTrue(panel.delete_button.isEnabled())
                self.assertTrue(panel.delete_preset())
                self.assertNotIn("UI Packed", [p["name"] for p in groomlab.card_map_presets()])
                self.assertEqual(panel.preset.currentText(), "Unreal Hair Cards")
                self.assertFalse(panel.delete_button.isEnabled(), "built-in presets stay")
                # A bad packing says why instead of exporting.
                panel.table.cellWidget(0, 0).setText("")
                panel.table.changed.emit()
                self.assertIsNone(panel.export())
                self.assertIn("Can't export", panel.status.text())
            finally:
                panel.close()
        finally:
            os.environ.pop("GROOMLAB_USER_CARD_PRESETS", None)

    def test_only_cards_nodes_whose_cards_changed_get_the_stale_badge(self):
        # Two Cards nodes, baked; the strands between them change: the inner node's maps are out
        # of date, the outer node's still fit. Each node's panel shows its own numbers.
        import tempfile
        from PySide2 import QtWidgets
        from groomlab import graph as gm
        outer = groomlab.create_cards(self.groom, max_cards=12)
        model = gm.GraphModel.from_groom(self.groom)
        model.set_param(outer, "clumpChannel", "none")
        frizz = model.add_node("Noise", name="frizz", params={"amplitude": 0.02})
        model.insert_before("output", frizz)
        inner = model.add_node("Cards", name="inner", params={"clumpChannel": "none", "maxCards": 20})
        model.insert_before("output", inner)
        model.save(self.groom)
        groomlab.bake_cards(self.groom, tempfile.mkdtemp(prefix="groomlab_badge_"), resolution=64, name="badge")
        self.editor.reload()
        self.editor.refresh_status()
        self.assertEqual(self.editor.node_warnings, {})
        groomlab.set_param(self.groom, frizz, "amplitude", 0.08)
        self.editor.reload()
        self.editor.refresh_status()
        self.assertEqual(set(self.editor.node_warnings), {inner})

        def panel(node):
            self._select_node(node)
            labels = {w.objectName(): w.text() for w in self.editor.params.findChildren(QtWidgets.QLabel)
                      if w.objectName().startswith("nodeAction_Card")}
            return labels

        mine = groomlab.card_stats(self.groom, node=inner)
        labels = panel(inner)
        self.assertTrue(labels["nodeAction_CardStats"].startswith("%d cards" % mine["cards"]), labels)
        self.assertIn("nodeAction_CardsStale", labels)
        theirs = groomlab.card_stats(self.groom, node=outer)
        self.assertNotEqual(theirs["cards"], mine["cards"])
        labels = panel(outer)
        self.assertTrue(labels["nodeAction_CardStats"].startswith("%d cards" % theirs["cards"]), labels)
        self.assertNotIn("nodeAction_CardsStale", labels)

    def test_paint_a_param_from_its_menu(self):
        menu = self.editor.params.build_param_menu("scatter", "density")
        paint = next((a.menu() for a in menu.actions() if a.text() == "Paint" and a.menu()), None)
        self.assertIsNotNone(paint)
        self.assertEqual([a.text() for a in paint.actions()], ["On Guides", "On Scalp Vertices", "As Texture"])
        # The row's yellow Paint chip (the app's pill) offers the same choices.
        from PySide2 import QtWidgets
        self.editor.params.show_node("scatter")
        chips = self.editor.params.widget().findChildren(QtWidgets.QToolButton, "paintChip")
        self.assertTrue(chips)
        self.assertEqual([a.text() for a in chips[0].menu().actions()], ["On Guides", "On Scalp Vertices", "As Texture"])
        before = self.editor.model.param("scatter", "density")
        self.editor.params.paint_param("scatter", "density", "guides")
        self.assertIn("no guides", self.editor.status.text())  # nothing to paint on yet
        groomlab.guides_from_curves(self.groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)])])
        self.editor.reload()
        self.editor.params.paint_param("scatter", "density", "guides")
        # The param keeps its value; a Map node reading the (white) map feeds its exposed input.
        self.assertEqual(self.editor.model.param("scatter", "density"), before)
        self.assertIn("density", self.editor.model.param_inputs("scatter"))
        self.assertEqual(self.editor.model.node_type("scatter_density"), "Map")
        self.assertEqual(groomlab.map_kind(self.groom, "scatter_density"), "guides")
        self.assertIn("scatter_density", groomlab.list_maps(self.groom), self.editor.status.text())
        self.assertEqual(groomlab.errors(self.groom), [])
        # On the scalp's vertices for another param: a second map and Map node.
        self.editor.params.paint_param("length", "keep", "vertices")
        self.assertEqual(groomlab.map_kind(self.groom, "length_keep"), "vertices")
        self.assertEqual(self.editor.model.node_type("length_keep"), "Map")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertNotIn("Paint", [a.text() for a in self.editor.params.build_param_menu("output", "strands").actions()]
                         if self.editor.model.param_spec("output", "strands") else ["x"])

    def test_any_param_exposed_and_set_by_a_value(self):
        from PySide2 import QtWidgets
        # A seed (no field can drive it) offers Expose as Input too: a Value port a Value node sets.
        menu = self.editor.params.build_param_menu("scatter", "seed")
        expose = next((a for a in menu.actions() if a.text() == "Expose as Input"), None)
        self.assertIsNotNone(expose)
        expose.trigger()
        wait(50)
        self.assertIn("seed", self.editor.model.param_inputs("scatter"))
        port = self.editor.items["scatter"].inputs["seed"]
        self.assertEqual((port.port_type, port.value_kind), ("Value", "integer"))
        value = self.editor.add_node("Value")
        self.editor.set_param(value, "kind", "integer")
        self.editor.rebuild()
        out = self.editor.items[value].output
        self.assertEqual((out.port_type, out.value_kind), ("Value", "integer"))
        self.assertTrue(self.editor.scene.compatible(out, port))
        self.assertFalse(self.editor.scene.compatible(self.editor.items["interpolate"].output, port))
        self.editor.link(value, "scatter", "seed")
        self.assertEqual(self.editor.model.driver_of("scatter", "seed"), value)
        self.editor.params.show_node("scatter")
        texts = [w.text() for w in self.editor.params.widget().findChildren(QtWidgets.QLabel)]
        self.assertTrue(any("driven by" in t and value in t for t in texts), texts)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_real_units(self):
        from PySide2 import QtWidgets
        from groomlab import units
        from groomlab.ui import widgets
        units.set_unit("width", "um")
        units.set_unit("length", "mm")
        try:
            self.editor.scene.clearSelection()
            self.editor.items["width"].setSelected(True)
            sliders = self.editor.params.findChildren(widgets.SoftSlider)
            root = self.editor.model.param("width", "width")
            root = root if isinstance(root, (int, float)) else 0.01
            shown = [s for s in sliders if abs(s.value() - root * 1e4) < 1e-6]
            self.assertTrue(shown, [s.value() for s in sliders])  # widths in micrometers
            shown[0].committed.emit(60.0)
            self.assertAlmostEqual(self.editor.model.param("width", "width"), 0.006, places=9)
            labels = [l.text() for l in self.editor.params.findChildren(QtWidgets.QLabel)]
            self.assertIn(u"µm", labels)
            self.assertAlmostEqual(units.to_display("length", 1.5), 15.0)
            self.assertAlmostEqual(units.from_display("density", 200.0), 200.0)  # per cm2 is per scene unit2
            with self.assertRaises(ValueError):
                units.set_unit("width", "furlong")
        finally:
            units.set_unit("width", "um")
            units.set_unit("length", "cm")

    def _param_label(self, param):
        from PySide2 import QtWidgets
        body = self.editor.params.widget()
        return next(w for w in body.findChildren(QtWidgets.QLabel) if w.property("glParam") == param)

    def _shown(self, param):
        return self._param_label(param).isVisibleTo(self.editor.params.widget())

    def test_param_sections_show_when_units_and_upgrade(self):
        from PySide2 import QtWidgets
        from groomlab.ui import widgets
        panel = self.editor.params
        self.ge.ParamPanel.folded.clear()
        try:
            self.editor.resize(1200, 700)
            self.editor.show()
            wait(30)
            # Groups under headers that fold, per node type for the session; Advanced folded, last.
            self.editor.scene.clearSelection()
            self.editor.items["width"].setSelected(True)
            color = self.editor.add_node("HairColor")  # wired after width: the graph stays valid (applied live)
            self.editor.scene.clearSelection()
            self.editor.items[color].setSelected(True)
            body = panel.widget()
            headers = [h.objectName() for h in body.findChildren(QtWidgets.QPushButton)
                       if h.objectName().startswith("paramSection_")]
            self.assertIn("paramSection_Base", headers)
            self.assertIn("paramSection_Random", headers)
            self.assertEqual(headers[-1], "paramSection_Advanced")
            self.assertTrue(self._shown("melanin"))
            self.assertFalse(self._shown("seed"))  # advanced: folded away
            self.assertFalse(self._shown("color"))  # showWhen mode color
            body.findChild(QtWidgets.QPushButton, "paramSection_Random").click()
            self.assertFalse(self._shown("melaninRandom"))
            body.findChild(QtWidgets.QPushButton, "paramSection_Advanced").click()
            self.assertTrue(self._shown("seed"))
            panel.show_node(color)  # built again: the folds stay
            self.assertFalse(self._shown("melaninRandom"))
            self.assertTrue(self._shown("seed"))
            # S over a param inside a group: the hovered param is still its own.
            panel.ensureWidgetVisible(self._param_label("melanin"))
            wait(30)
            self.assertTrue(hover(self._param_label("melanin")))
            wait(30)
            self.assertEqual(panel.hovered_param(), "melanin")
            # showWhen follows an edit without a rebuild.
            body = panel.widget()
            self.editor.set_param(color, "mode", "color")
            self.assertIs(panel.widget(), body)
            self.assertTrue(self._shown("color"))
            self.assertFalse(self._shown("melanin"))
            self.editor.set_param(color, "mode", "melanin")
            self.assertIn("%", [l.text() for l in body.findChildren(QtWidgets.QLabel)])

            # One group or none: no group headers (Advanced alone); a fraction shows as a percentage.
            self.editor.scene.clearSelection()
            self.editor.items["length"].setSelected(True)
            body = panel.widget()
            headers = [h.objectName() for h in body.findChildren(QtWidgets.QPushButton)
                       if h.objectName().startswith("paramSection_")]
            self.assertEqual(headers, ["paramSection_Advanced"])
            trim = panel.key_widgets["randomness"]
            self.assertAlmostEqual(trim.value(), self.editor.model.param("length", "randomness") * 100.0)
            trim.committed.emit(25.0)
            self.assertAlmostEqual(self.editor.model.param("length", "randomness"), 0.25)

            # Resample: maxAngle (degrees) only in curvature mode, minSegments in length or curvature.
            self.editor.scene.clearSelection()
            self.editor.items["resample"].setSelected(True)
            self.assertFalse(self._shown("maxAngle"))
            self.assertFalse(self._shown("minSegments"))
            self.editor.set_param("resample", "mode", "length")
            self.assertTrue(self._shown("minSegments"))
            self.assertFalse(self._shown("maxAngle"))
            self.editor.set_param("resample", "mode", "curvature")
            self.assertTrue(self._shown("maxAngle"))
            self.assertIn(u"°", [l.text() for l in panel.widget().findChildren(QtWidgets.QLabel)])

            # A node saved with an older behaviour: Upgrade in the header raises it, the badge goes.
            self.assertIsNone(panel.widget().findChild(QtWidgets.QToolButton, "upgradeNode"))
            self.editor.model.nodes["length"]["version"] = 0  # older than any operator's (1 and up)
            self.editor.scene.clearSelection()
            self.editor.items["length"].setSelected(True)
            upgrade = panel.widget().findChild(QtWidgets.QToolButton, "upgradeNode")
            self.assertIsNotNone(upgrade)
            self.assertIn("version", upgrade.toolTip())
            upgrade.click()
            self.assertEqual(self.editor.model.node_version("length"), (1, 1))
            self.assertIsNone(panel.widget().findChild(QtWidgets.QToolButton, "upgradeNode"))
            stored = json.loads(cmds.getAttr(self.groom + ".graph"))
            self.assertEqual(next(n for n in stored["nodes"] if n["name"] == "length").get("version"), 1)
            self.assertTrue(widgets.alive(panel.widget()))
        finally:
            self.ge.ParamPanel.folded.clear()
            self.editor.hide()

    def test_node_toggles_and_curves(self):
        from PySide2 import QtCore, QtWidgets
        from groomlab.ui import widgets
        self.editor.resize(1200, 600)
        self.editor.show()
        QtWidgets.QApplication.processEvents()
        # Header toggles: Focus and Bypass, clicked like buttons.
        item = self.editor.items["length"]
        rects = item.toggle_rects()
        view = self.editor.view
        from PySide2 import QtTest
        for kind in ("focus", "disable"):
            point = view.mapFromScene(item.mapToScene(rects[kind].center()))
            QtTest.QTest.mouseClick(view.viewport(), QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, point)
            wait(20)  # the toggle runs once the click's event is over
            item = self.editor.items["length"]  # rebuilt
        self.assertEqual(self.editor.viewed, "length")
        self.assertTrue(self.editor.model.bypassed("length"))
        self.assertNotIn("focus", self.editor.items["guides"].toggle_rects())  # guides nodes show no focus
        self.editor.view_node(None)
        self.editor.set_bypass(["length"], False)
        self.editor.hide()

        # A Curve node drives Clump's profile; the curve editor edits ramps.
        clump = self.editor.add_node("Clump")
        self.editor.link("interpolate", clump, "strands")
        self.assertEqual(self.editor.model.param(clump, "centersFrom"), "pick")
        self.editor.link("guides", clump, "centers")  # drawn by hand: the Clump takes the guides' curves
        self.assertEqual(self.editor.model.param(clump, "centersFrom"), "inputCurves")
        self.editor.link(clump, "length", "strands")
        self.editor.scene.clearSelection()
        self.editor.items[clump].setSelected(True)
        self.assertTrue(self._shown("centersFrom"))
        self.assertFalse(self._shown("clumpBy"))  # not picking: hidden by the dropdown
        self.assertFalse(self._shown("count"))    # and with it what clumpBy shows
        self.assertTrue(self._shown("clumpChannel"))
        ramps = self.editor.params.findChildren(widgets.RampEditor)
        self.assertTrue(ramps)
        ramps[0].setValue([[0, 0], [0.5, 1], [1, 0.2]])
        ramps[0].committed.emit(ramps[0].value())
        self.assertEqual(self.editor.model.param(clump, "profile"), [[0, 0], [0.5, 1], [1, 0.2]])
        # Drawn as the core evaluates it: smooth through the keys, or straight when Linear.
        self.assertAlmostEqual(ramps[0].evaluate(0.25), 0.625, places=5)
        ramps[0].linear = True
        self.assertAlmostEqual(ramps[0].evaluate(0.25), 0.5, places=5)
        ramps[0].committed.emit(ramps[0].value())
        self.assertEqual(self.editor.model.param(clump, "profile"),
                         {"keys": [[0, 0], [0.5, 1], [1, 0.2]], "linear": True})
        self.assertEqual(groomlab.errors(self.groom), [])
        self.editor.set_param_exposed(clump, "profile", True)
        self.editor.rebuild()
        self.assertEqual(self.editor.items[clump].inputs["profile"].port_type, "Curve")
        curve = self.editor.add_node("Curve")
        self.editor.link(curve, clump, "profile")
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_feather_nodes(self):
        # Propagate and Plume sit under Feathers in the Add Node menu (Split under Utility);
        # Propagate's profiles edit with the curve editor; Split shows its four outputs, one feeding
        # Propagate.
        from groomlab.ui import widgets
        categories = dict(self.ge.categories(self.editor.catalog))
        self.assertEqual(categories["Feathers"], ["Propagate", "Plume"])
        self.assertIn("Split", categories["Utility"])
        self.editor.scene.clearSelection()
        split = self.editor.add_node("Split")
        self.editor.link("interpolate", split, "strands")
        self.editor.scene.clearSelection()
        propagate = self.editor.add_node("Propagate")
        self.editor.link(split, propagate, "shafts", output="b")
        self.editor.link(propagate, "length", "strands")
        self.editor.rebuild()
        self.assertEqual(list(self.editor.items[split].outputs), ["a", "b", "c", "d"])
        self.assertEqual(self.editor.model.output_of(propagate, "shafts"), "b")
        self.editor.scene.clearSelection()
        self.editor.items[propagate].setSelected(True)
        ramps = self.editor.params.findChildren(widgets.RampEditor)
        self.assertEqual(len(ramps), 4)  # lengthProfile, angleProfile, curlProfile, shaftWidthProfile
        ramps[0].setValue([[0, 0.2], [0.5, 1], [1, 0.1]])
        ramps[0].committed.emit(ramps[0].value())
        self.assertEqual(self.editor.model.param(propagate, "lengthProfile"), [[0, 0.2], [0.5, 1], [1, 0.1]])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)

    def test_invalid_graphs_are_not_applied(self):
        applied = cmds.getAttr(self.groom + ".graph")
        self.editor.delete_nodes(["output"])
        self.assertIn("no output node", self.editor.status.text())
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), applied)

    def test_connect_rejects_type_mismatch(self):
        self.editor.link("scalp", "width", "strands")
        self.assertIn("cannot connect", self.editor.status.text())

    def test_presets_load_into_the_editor(self):
        cmds.setAttr(self.groom + ".previewDensity", 0.05)  # presets are made on a head: dense on this sphere
        self.editor.load_preset("hair_basics")
        self.assertIn("clump", self.editor.items)
        self.assertIn("Clump", [n["type"] for n in groomlab.get_graph(self.groom)["nodes"]])
        # Where the preset put them, with its backdrops; the maps it reads made.
        from groomlab import presets
        self.editor.load_preset("blend_two_looks")
        preset = presets.read_preset("blend_two_looks")
        for node in preset["nodes"]:
            self.assertEqual(self.editor.model.position(node["name"]), (node["ui"]["x"], node["ui"]["y"]))
        self.assertEqual([b["title"] for b in self.editor.model.backdrops], [b["title"] for b in preset["backdrops"]])
        self.assertEqual(groomlab.get_graph(self.groom)["backdrops"], preset["backdrops"])
        self.editor.load_preset("painted_maps")
        self.assertIn("groomMasks", groomlab.list_maps(self.groom))
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_hand_placed_feathers_preset_and_add_feathers_draw_into_their_group(self):
        # A preset that requires a guide group: made empty, and the Draw tool draws into it, so
        # every guide drawn is a flight feather.
        cmds.select(self.groom)
        self.editor.load_preset("hand_placed_feathers")
        self.assertIn("feathers", [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(cmds.currentCtx(), groomlab.BRUSH_CONTEXT)
        self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True), "draw")
        self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, group=True), "feathers")
        self.assertIn("draws into feathers", self.editor.status.text())
        # Add > Feathers, one per guide in a new group with no guides yet: the Draw tool on it.
        from groomlab.ui import menu
        cmds.setToolTo("selectSuperContext")
        cmds.select(self.groom)
        menu.add_feathers_dialog(design="flight", source="guides", group="tail", groom=self.groom)
        self.assertEqual(cmds.currentCtx(), groomlab.BRUSH_CONTEXT)
        self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True), "draw")
        self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, group=True), "tail")
        self.assertEqual(groomlab.feather_guide_groups(self.groom), ["feathers", "tail"])
        cmds.setToolTo("selectSuperContext")

    def test_the_presets_menu_has_a_submenu_per_category(self):
        from groomlab import presets
        user = os.environ.get("GROOMLAB_USER_PRESETS")
        import tempfile
        folder = tempfile.mkdtemp(prefix="groomlab_menu_presets_")
        os.environ["GROOMLAB_USER_PRESETS"] = folder
        try:
            with open(os.path.join(folder, "my_look.json"), "w") as f:  # no "about": the top level
                json.dump({"version": 1, "nodes": [], "connections": []}, f)
            self.editor.fill_preset_menu()
            actions = self.editor.preset_menu.actions()
            subs = [a.menu() for a in actions if a.menu() and not a.data()]
            self.assertEqual([m.title() for m in subs], list(presets.CATEGORIES))
            hair = next(m for m in subs if m.title() == "Hair")
            basics = next(a for a in hair.actions() if a.data() == "hair_basics")
            info = presets.preset_info("hair_basics")
            self.assertEqual(basics.text(), info["title"])
            self.assertEqual(basics.toolTip(), info["description"])
            self.assertEqual(basics.statusTip(), info["description"])
            self.assertTrue(hair.toolTipsVisible())
            # Each preset: Apply beside Insert After Selected Node.
            self.assertEqual([a.text() for a in basics.menu().actions()], ["Apply", "Insert After Selected Node"])
            loose = [a for a in actions if a.data() == "my_look"]
            self.assertEqual([a.text() for a in loose], ["My look"])
            self.assertIn("Add-ons", [m.title() for m in subs])
            self.assertEqual([a.text() for a in actions if a.text() and not a.menu()][-3:],
                             ["Save as Preset...", "Copy Graph", "Paste Graph"])
            techniques = next(m for m in subs if m.title() == "Techniques")
            self.assertIn("blend_two_looks", [a.data() for a in techniques.actions()])
        finally:
            if user is None:
                os.environ.pop("GROOMLAB_USER_PRESETS", None)
            else:
                os.environ["GROOMLAB_USER_PRESETS"] = user
            import shutil
            shutil.rmtree(folder, ignore_errors=True)

    def test_insert_a_preset_after_the_selected_node(self):
        from groomlab import presets
        self.editor.load_preset("hair_basics")
        nodes_before = list(self.editor.model.order)
        clump = next(n for n in nodes_before if self.editor.model.nodes[n]["type"] == "Clump")
        self.editor.scene.clearSelection()
        self.editor.items[clump].setSelected(True)
        self.assertEqual(self.editor.insert_preset("frizz"), "frizz")
        model = self.editor.model
        self.assertEqual(model.nodes["frizz"]["type"], "Compound")
        self.assertEqual(len(model.order), len(nodes_before) + 1)
        fed = [c["to"] for c in model.connections if c["from"] == clump]
        self.assertEqual(fed, ["frizz"])  # the Compound takes over what the node fed
        self.assertIn({"from": clump, "to": "frizz", "input": "strands"}, model.connections)
        inner = model.nodes["frizz"]["params"]["definition"]["graph"]
        self.assertEqual(sorted(n["name"] for n in inner["nodes"]), ["frizzNoise", "strands"])
        self.assertEqual([i.name for i in self.editor.scene.selectedItems() if isinstance(i, self.ge.NodeItem)], ["frizz"])
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertIn("inserted frizz", self.editor.status.text())
        # One undo step takes it back; nothing selected inserts before the Output.
        cmds.undo()
        self.editor.reload()
        self.assertEqual(list(self.editor.model.order), nodes_before)
        self.editor.scene.clearSelection()
        self.assertEqual(self.editor.insert_preset("wet"), "wet")
        model = self.editor.model
        self.assertIn({"from": "wet", "to": model.output, "input": "strands"}, model.connections)
        self.assertEqual(groomlab.errors(self.groom), [])
        # Every add-on inserts into this groom.
        import groomlab_core
        for name in ("flyaways", "cards_mid", "gravity"):
            self.editor.scene.clearSelection()
            self.assertEqual(self.editor.insert_preset(name), groomlab_core.preset_node_name(name), name)
            self.assertEqual(groomlab.errors(self.groom), [], name)
        self.assertIn("cardsMid", self.editor.model.nodes)

    def test_an_inserted_presets_compound_shows_its_key_params(self):
        # The preset's "exposed" params on the Compound's own panel, grouped as the inner params
        # are; an edit there changes the groom and is stored with it.
        from PySide2 import QtWidgets
        self.editor.load_preset("hair_basics")
        self.editor.scene.clearSelection()
        self.assertEqual(self.editor.insert_preset("creature_fur"), "creatureFur")
        model = self.editor.model
        expose = model.nodes["creatureFur"]["params"]["definition"]["expose"]
        self.assertEqual(expose["tuftStrength"], "tufts.strength")
        self.assertEqual(expose["tipColor"], "color.tipColor")
        self.assertEqual(len(expose), 7)
        self.assertAlmostEqual(model.param("creatureFur", "tuftStrength"), 0.4)
        self.editor.scene.clearSelection()
        self.editor.items["creatureFur"].setSelected(True)
        panel = self.editor.params
        for param in expose:
            self._param_label(param)  # a row each
        body = panel.widget()
        headers = [h.objectName() for h in body.findChildren(QtWidgets.QPushButton)
                   if h.objectName().startswith("paramSection_")]
        self.assertIn("paramSection_Base", headers)  # the HairColor's groups
        self.assertIn("paramSection_Tip", headers)
        before = cmds.exactWorldBoundingBox(self.groom)
        panel.key_widgets["tuftStrength"].committed.emit(0.95)
        self.assertAlmostEqual(self.editor.model.param("creatureFur", "tuftStrength"), 0.95)
        self.assertNotEqual(cmds.exactWorldBoundingBox(self.groom), before)
        stored = next(n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == "creatureFur")
        self.assertAlmostEqual(stored["params"]["tuftStrength"], 0.95)
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_copy_and_paste_the_whole_graph_through_the_clipboard(self):
        from PySide2 import QtWidgets
        from groomlab import presets
        clipboard = QtWidgets.QApplication.clipboard()
        users = clipboard.text()  # the user's clipboard, put back after
        self.addCleanup(lambda: clipboard.setText(users))
        cmds.setAttr(self.groom + ".previewDensity", 0.05)
        presets.apply_preset(self.groom, "blend_two_looks")
        cmds.setAttr(self.groom + ".ctl_curls", 0.6)
        self.editor.set_groom(self.groom)
        # Both in the ... menu and the Presets menu.
        self.editor.fill_preset_menu()
        for menu in (self.editor.edit_menu, self.editor.preset_menu):
            texts = [a.text() for a in menu.actions()]
            self.assertIn("Copy Graph", texts)
            self.assertIn("Paste Graph", texts)
        action = {a.text(): a for a in self.editor.edit_menu.actions()}
        clipboard.setText("")
        action["Copy Graph"].trigger()
        text = clipboard.text()
        self.assertEqual(json.loads(text)["controls"]["ctl_curls"], 0.6)
        self.assertIn("copied", self.editor.status.text())
        # Onto another groom on a bigger scalp, from the real clipboard.
        other = groomlab.create_groom(cmds.polySphere(radius=2, subdivisionsX=20, subdivisionsY=10)[0])
        cmds.setAttr(other + ".previewDensity", 0.02)
        self.editor.set_groom(other)
        settle_undo()
        action["Paste Graph"].trigger()
        wait(30)
        self.assertIn("curlsControl", self.editor.items)
        self.assertEqual(sorted(n["name"] for n in groomlab.get_graph(other)["nodes"]),
                         sorted(n["name"] for n in groomlab.get_graph(self.groom)["nodes"]))
        self.assertAlmostEqual(cmds.getAttr(other + ".ctl_curls"), 0.6, places=5)
        self.assertIn("pasted", self.editor.status.text())
        self.assertIn("x2", self.editor.status.text())  # fitted to the bigger scalp
        # Text that is no graph: said in the status bar, nothing changed, no dialog.
        stored = cmds.getAttr(other + ".graph")
        clipboard.setText("hello, this is not a groom")
        action["Paste Graph"].trigger()
        self.assertIn("cannot paste", self.editor.status.text())
        self.assertEqual(self.editor.status.property("role"), "status")
        self.assertEqual(cmds.getAttr(other + ".graph"), stored)
        self.assertIsNone(QtWidgets.QApplication.activeModalWidget())
        # One undo step takes the paste back; the editor follows.
        cmds.undo()
        wait(30)
        self.assertNotIn("curlsControl", self.editor.items)

    def test_follows_graph_changes_made_elsewhere(self):
        # A preset applied from the menu (or a script, or undo) shows without Reload.
        from groomlab import presets
        cmds.setAttr(self.groom + ".previewDensity", 0.05)  # presets are made on a head: dense on this sphere
        self.editor.show()
        settle_undo()
        presets.apply_preset(self.groom, "hair_basics")
        for _ in range(5):
            self.app.processEvents()
        self.assertIn("clump", self.editor.items)
        cmds.undo()
        for _ in range(5):
            self.app.processEvents()
        self.assertNotIn("clump", self.editor.items)
        self.editor.close()

    def test_follows_a_groom_created_while_open(self):
        # Create Groom from Selection with the editor open shows the new groom's graph, whether
        # the editor showed another groom or none, and with another groom pinned: the new groom
        # takes the pin.
        from groomlab.ui import menu
        groomlab.pin_groom(self.groom)
        self.addCleanup(groomlab.pin_groom, None)
        empty = self.ge.GraphEditor()
        for editor in (self.editor, empty):
            editor.resize(900, 500)
            editor.show()
        wait(30)
        mesh = cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=12)[0]
        cmds.select(mesh)
        cmds.flushIdleQueue()
        wait(30)
        menu.create_groom_from_selection()
        cmds.flushIdleQueue()
        wait(60)
        made = cmds.ls(selection=True, dag=True, type="groomLabShape")[0]
        self.assertEqual(groomlab.pinned_groom(), made)
        try:
            for editor in (self.editor, empty):
                self.assertEqual(editor.groom, made)
                self.assertEqual(editor.groom_box.currentText(), made)
                self.assertEqual(set(editor.items), set(editor.model.order))
                self.assertEqual(editor.model.to_json(), self.ge.load_model(made, editor.catalog).to_json())
        finally:
            empty.close()
            empty.deleteLater()
            self.editor.close()

    def test_a_new_scene_clears_and_an_opened_one_shows_its_groom(self):
        # File > New empties the editor (testers saw the closed scene's nodes stay); File > Open shows
        # the opened scene's groom, and the Groom Tools pick it too rather than "Start a groom".
        import tempfile
        from groomlab.ui import palette
        groomlab.pin_groom(None)
        self.addCleanup(groomlab.pin_groom, None)
        path = os.path.join(tempfile.mkdtemp(), "groomed.ma").replace("\\", "/")
        cmds.file(rename=path)
        cmds.file(save=True, type="mayaAscii", force=True)
        self.editor.resize(900, 500)
        self.editor.show()
        wait(30)
        self.assertTrue(self.editor.items)
        cmds.file(new=True, force=True)
        cmds.flushIdleQueue()
        wait(30)
        try:
            self.assertIsNone(self.editor.groom)
            self.assertIsNone(self.editor.model)
            self.assertEqual(self.editor.items, {})
            self.assertEqual(self.editor.scene.items(), [])
            self.assertEqual(self.editor.groom_box.count(), 0)
            tools = palette.show()
            try:
                self.assertTrue(tools.empty.isVisibleTo(tools))  # an empty scene: Start a groom
                cmds.file(path, open=True, force=True)
                cmds.flushIdleQueue()
                wait(60)
                (groom,) = cmds.ls(type="groomLabShape")
                self.assertEqual(groomlab.adopted_groom(), groom)
                self.assertIsNone(groomlab.pinned_groom())  # not a pin: selecting another groom still leads
                self.assertEqual(self.editor.groom, groom)
                self.assertEqual(set(self.editor.items), set(self.editor.model.order))
                self.assertEqual(tools.grooms.currentData(), groom)
                self.assertFalse(tools.empty.isVisibleTo(tools))
            finally:
                tools.close()
        finally:
            self.editor.close()

    def test_scene_grooms_are_adopted_only_when_none_is_active(self):
        other = groomlab.create_groom(cmds.polySphere()[0])
        groomlab.pin_groom(None)
        self.addCleanup(groomlab.pin_groom, None)
        cmds.select(clear=True)
        cmds.optionVar(stringValue=(groomlab.ACTIVE_VAR, other))  # the groom last worked on: picked again
        self.assertEqual(groomlab.adopt_scene_groom(), other)
        self.assertEqual((groomlab.adopted_groom(), groomlab.active_groom()), (other, other))
        self.assertIsNone(groomlab.pinned_groom())
        cmds.select(self.groom)  # a groom selected leads, and stays current with nothing selected
        self.assertEqual(groomlab.active_groom(), self.groom)
        cmds.select(clear=True)
        self.assertEqual(groomlab.active_groom(), self.groom)
        groomlab.pin_groom(None)
        cmds.optionVar(stringValue=(groomlab.ACTIVE_VAR, "noSuchGroom"))
        self.assertEqual(groomlab.adopt_scene_groom(), sorted([self.groom, other])[0])  # else the first
        groomlab.pin_groom(None)
        cmds.select(other)
        self.assertEqual(groomlab.adopt_scene_groom(), other)  # one selected: followed, nothing adopted
        self.assertIsNone(groomlab.adopted_groom())

    def test_nodes_pan_from_their_cached_pictures(self):
        # Testers found Alt+drag panning sluggish: nodes and ports paint once into pixmaps a pan
        # moves; an Alt+right-drag zoom scales one picture of the view (at the screen's resolution:
        # item-coordinate caches drew 1.5x too large on a 150% screen), sharp again on release.
        from PySide2 import QtCore, QtGui, QtWidgets
        device = QtWidgets.QGraphicsItem.DeviceCoordinateCache
        items = [i for i in self.editor.scene.items() if isinstance(i, (self.ge.NodeItem, self.ge.PortItem))]
        self.assertTrue(items)
        self.assertEqual({i.cacheMode() for i in items}, {device})
        self.assertGreaterEqual(QtGui.QPixmapCache.cacheLimit(), self.ge.PIXMAP_CACHE_KB)
        self.editor.resize(900, 500)
        self.editor.show()
        wait(30)
        view = self.editor.view.viewport()
        center = QtCore.QPointF(view.width() / 2.0, view.height() / 2.0)
        right, alt = QtCore.Qt.RightButton, QtCore.Qt.AltModifier
        try:
            self.app.sendEvent(view, QtGui.QMouseEvent(QtCore.QEvent.MouseButtonPress, center, right, right, alt))
            snapshot = self.editor.view._snapshot
            self.assertIsNotNone(snapshot)
            picture, corner, span = snapshot
            # At the screen's resolution (a small graph: under the size cap), over the whole graph
            # and past the view, so zooming out mid-drag shows the rest of it.
            self.assertAlmostEqual(span, 1.0 / view.devicePixelRatioF(), places=3)
            covered = QtCore.QRectF(corner, QtCore.QSizeF(picture.width() * span, picture.height() * span))
            graph = self.editor.view.mapFromScene(self.editor.scene.itemsBoundingRect()).boundingRect()
            self.assertTrue(covered.contains(QtCore.QRectF(graph)), (covered, graph))
            self.assertTrue(covered.contains(QtCore.QRectF(view.rect().adjusted(-view.width(), 0, view.width(), 0))))
            self.assertEqual({i.cacheMode() for i in items}, {device})
            self.app.sendEvent(view, QtGui.QMouseEvent(QtCore.QEvent.MouseButtonRelease, center, right,
                                                       QtCore.Qt.NoButton, alt))
            self.assertIsNone(self.editor.view._snapshot)
            self.assertEqual({i.cacheMode() for i in items}, {device})
        finally:
            self.editor.close()

    def test_wheel_zoom_draws_what_a_fresh_paint_draws(self):
        # A tester saw the view broken while wheel-zooming (stale, larger pictures of the nodes left
        # behind): each wheel step must show what painting the view afresh shows.
        from PySide2 import QtCore, QtGui, QtWidgets
        self.editor.resize(1200, 500)
        self.editor.show()
        wait(30)
        view = self.editor.view
        port = view.viewport()

        def shown():
            """What is on the screen (not a fresh render of the view)."""
            for _ in range(3):
                QtWidgets.QApplication.processEvents()
                wait(15)
            screen = port.screen() if hasattr(port, "screen") else QtGui.QGuiApplication.primaryScreen()
            return screen.grabWindow(int(port.winId())).toImage().convertToFormat(QtGui.QImage.Format_RGB32)

        def fresh():
            view.resetCachedContent()
            for item in view.scene().items():
                item.update()
            port.update()
            return shown()
        try:
            view.frame_all()
            at = QtCore.QPointF(port.width() / 2.0, port.height() / 2.0)
            for k, up in enumerate([True] * 6 + [False] * 9):
                wheel = QtGui.QWheelEvent(at, port.mapToGlobal(at.toPoint()), QtCore.QPoint(0, 0),
                                          QtCore.QPoint(0, 120 if up else -120), QtCore.Qt.NoButton,
                                          QtCore.Qt.NoModifier, QtCore.Qt.NoScrollPhase, False)
                self.app.sendEvent(port, wheel)
                now = shown()
                again = fresh()
                differ = sum(1 for y in range(0, now.height(), 3) for x in range(0, now.width(), 3)
                             if now.pixel(x, y) != again.pixel(x, y))
                self.assertLess(differ, 20, "wheel step %d (%s): %d sampled pixels differ from a fresh paint"
                                % (k, "in" if up else "out", differ))
            # An Alt+right-drag zoom shows its picture of the view scaled about the point pressed: at
            # the zoom it started from, what was there; after the release, the view painted afresh.
            view.frame_all()
            before = shown()
            right, alt = QtCore.Qt.RightButton, QtCore.Qt.AltModifier
            self.app.sendEvent(port, QtGui.QMouseEvent(QtCore.QEvent.MouseButtonPress, at, right, right, alt))
            try:
                for dx in (40, 120, -60, 0):
                    self.app.sendEvent(port, QtGui.QMouseEvent(QtCore.QEvent.MouseMove, at + QtCore.QPointF(dx, 0),
                                                               QtCore.Qt.NoButton, right, alt))
                now = shown()  # back where it started: the picture unscaled
                differ = sum(1 for y in range(0, now.height(), 3) for x in range(0, now.width(), 3)
                             if abs(QtGui.qGray(now.pixel(x, y)) - QtGui.qGray(before.pixel(x, y))) > 40)
                self.assertLess(differ, 40, "mid-drag at the start's zoom: %d sampled pixels moved" % differ)
                self.app.sendEvent(port, QtGui.QMouseEvent(QtCore.QEvent.MouseMove, at + QtCore.QPointF(80, 0),
                                                           QtCore.Qt.NoButton, right, alt))
            finally:
                self.app.sendEvent(port, QtGui.QMouseEvent(QtCore.QEvent.MouseButtonRelease, at, right,
                                                           QtCore.Qt.NoButton, alt))
            now, again = shown(), fresh()
            differ = sum(1 for y in range(0, now.height(), 3) for x in range(0, now.width(), 3)
                         if now.pixel(x, y) != again.pixel(x, y))
            self.assertLess(differ, 20, "after the drag zoom: %d sampled pixels differ from a fresh paint" % differ)
            # Painting the whole view each move stays quick: an Alt+drag pan, painted at once.
            import time
            left = QtCore.Qt.LeftButton
            self.app.sendEvent(port, QtGui.QMouseEvent(QtCore.QEvent.MouseButtonPress, at, left, left, alt))
            start = time.perf_counter()
            for k in range(40):
                to = at + QtCore.QPointF(3 * k, 2 * k)
                self.app.sendEvent(port, QtGui.QMouseEvent(QtCore.QEvent.MouseMove, to, QtCore.Qt.NoButton, left, alt))
                port.repaint()
            per_move = (time.perf_counter() - start) * 1000.0 / 40
            self.app.sendEvent(port, QtGui.QMouseEvent(QtCore.QEvent.MouseButtonRelease, at, left, QtCore.Qt.NoButton, alt))
            print("alt+drag pan: %.2f ms a move" % per_move)
            self.assertLess(per_move, 12.0)
        finally:
            self.editor.close()

    def test_ctrl_z_in_the_editor_is_mayas_undo(self):
        # Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z sent to the editor window undo and redo the groom's graph
        # (Maya's undo), and the editor shows the result, keeping the selection where it can.
        from PySide2 import QtCore, QtTest
        from groomlab.ui import widgets
        self.editor.resize(1200, 600)
        self.editor.show()
        wait(30)
        view = self.editor.view
        self.assertTrue(activate(view))  # the editor has the keys, whatever window the OS has active
        settle_undo()
        before = groomlab.get_graph(self.groom, expand_default=True)
        self.editor.items["length"].setSelected(True)
        name = self.editor.add_node("Curl")  # applied: one undo step
        self.assertIn(name, [n["name"] for n in groomlab.get_graph(self.groom)["nodes"]])
        self.editor.scene.clearSelection()
        self.editor.items["scatter"].setSelected(True)
        press(view, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
        wait(60)  # the editor reloads once the change settles
        self.assertEqual(groomlab.get_graph(self.groom, expand_default=True), before)
        self.assertNotIn(name, self.editor.items)  # the editor shows the restored graph
        self.assertIn("undo", self.editor.status.text())
        self.assertEqual([i.name for i in self.editor.scene.selectedItems()], ["scatter"])
        press(view, QtCore.Qt.Key_Y, QtCore.Qt.ControlModifier)
        wait(60)
        self.assertIn(name, self.editor.items)
        self.assertIn("redo", self.editor.status.text())
        press(view, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
        wait(60)
        self.assertNotIn(name, self.editor.items)
        press(view, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier | QtCore.Qt.ShiftModifier)
        wait(60)
        self.assertIn(name, self.editor.items)
        self.assertIn(name, [n["name"] for n in groomlab.get_graph(self.groom)["nodes"]])
        # The Edit menu names the steps, with the keys.
        self.editor.fill_edit_menu()
        self.assertTrue(self.editor.undo_action.text().startswith("Undo groomLab graph edit"))
        self.assertIn("Ctrl+Z", self.editor.undo_action.text())
        self.assertIn("Ctrl+Y", self.editor.redo_action.text())
        self.editor.undo_action.trigger()
        wait(60)
        self.assertNotIn(name, self.editor.items)
        self.editor.redo_action.trigger()
        wait(60)
        self.assertIn(name, self.editor.items)

        # A text field with typing to undo keeps Ctrl+Z for itself; with nothing left, it is Maya's.
        self.editor.scene.clearSelection()
        self.editor.items["length"].setSelected(True)
        field = self.editor.params.findChildren(widgets.SoftSlider)[0].field
        original = field.text()
        self.assertTrue(activate(field))
        field.selectAll()
        QtTest.QTest.keyClicks(field, "5")
        self.assertNotEqual(field.text(), original)
        press(field, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
        wait(60)
        self.assertEqual(field.text(), original)  # the typing came back
        self.assertIn(name, self.editor.items)     # and the groom was left alone
        press(field, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
        wait(60)
        self.assertNotIn(name, self.editor.items)  # nothing left to undo in the field: Maya's undo
        self.assertNotIn(name, [n["name"] for n in groomlab.get_graph(self.groom, expand_default=True)["nodes"]])

        # During a slider drag (previews outside undo) the keys do nothing. The panel was rebuilt by
        # the undo: its live slider (the old panel's may linger until Qt deletes it).
        slider = self.editor.params.key_widgets["keep"]
        slider.slider.setSliderDown(True)
        slider.slider.setValue(250 if slider.slider.value() > 500 else 750)  # a move it can make (not past its end)
        wait(80)
        self.assertTrue(self.editor.previewing())
        stored = cmds.getAttr(self.groom + ".graph")
        press(view, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
        wait(60)
        self.assertEqual(cmds.getAttr(self.groom + ".graph"), stored)
        slider.slider.setSliderDown(False)
        self.editor.hide()

    def test_undo_after_a_node_move_shows_the_undone_graph(self):
        # A node moved (its layout written to the groom outside undo), then an edit: undoing the
        # edit puts the graph back to the text the move wrote, and the editor shows it (its model,
        # canvas and panel), whether undone here (Ctrl+Z) or by Maya (Edit > Undo); before, it kept
        # showing the edit until reopened.
        from PySide2 import QtCore
        self.editor.resize(1200, 600)
        self.editor.show()
        wait(30)
        view = self.editor.view
        for how in ("editor", "maya"):
            settle_undo()
            item = self.editor.items["resample"]
            item.setPos(item.pos() + QtCore.QPointF(0, 30))
            self.editor.node_moved("resample", item.pos())
            self.editor.commit_layout()
            wait(30)
            moved = cmds.getAttr(self.groom + ".graph")
            self.editor.scene.clearSelection()
            self.editor.items["scatter"].setSelected(True)
            density = self.editor.model.param("scatter", "density")
            self.editor.set_param("scatter", "density", density * 0.5)
            self.editor.items["length"].setSelected(True)
            name = self.editor.add_node("Curl")
            self.editor.scene.clearSelection()
            self.editor.items["scatter"].setSelected(True)
            wait(30)
            for _ in range(2):  # the node, then the param
                if how == "editor":
                    press(view, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
                else:
                    cmds.undo()
                wait(60)
            self.assertEqual(cmds.getAttr(self.groom + ".graph"), moved)
            self.assertEqual(self.editor.model.to_json(), moved, how)
            self.assertNotIn(name, self.editor.items)
            self.assertEqual(self.editor.model.param("scatter", "density"), density)
            self.assertEqual(self.editor.params.node, "scatter")
            field = self.editor.params.key_widgets["density"]
            self.assertAlmostEqual(field.value() / field.unit_factor, density, places=3)
        self.editor.hide()

    def test_drive_a_param_with_a_slider(self):
        menu = self.editor.params.build_param_menu("length", "keep")
        self.assertIn("Drive with Slider", [a.text() for a in menu.actions()])
        self.editor.params.add_control("length", "keep")
        self.assertIn("Control", [self.editor.model.node_type(n) for n in self.editor.model.order])
        self.assertTrue(cmds.attributeQuery("ctl_keep", node=self.groom, exists=True))
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_set_key_from_the_menu_and_with_s_over_a_param(self):
        # Right-click > Set Key links the param to its own attribute and keys it (one undo step); S
        # over its field keys it again; the field takes the Channel Box's colors as the time moves.
        from PySide2 import QtCore, QtGui, QtTest
        cmds.autoKeyframe(state=False)
        cmds.playbackOptions(minTime=1, maxTime=20)
        cmds.currentTime(1)
        self.editor.resize(1200, 600)
        self.editor.show()
        wait(30)
        self.editor.items["length"].setSelected(True)
        settle_undo()
        panel = self.editor.params
        menu = panel.build_param_menu("length", "keep")
        actions = {a.text(): a for a in menu.actions()}
        self.assertTrue(actions["Set Key"].isEnabled())
        self.assertFalse(actions["Remove Animation"].isEnabled())
        self.assertIn("Drive with Slider", actions)  # the visible Control node stays on offer
        actions["Set Key"].trigger()
        attr = self.groom + ".ctl_length_keep"
        self.assertEqual(cmds.keyframe(attr, query=True), [1.0])
        self.assertEqual(self.editor.model.control_of("length", "keep"), "length_keep")
        self.assertEqual(sorted(self.editor.items), sorted(self.editor.model.order))  # no node on the canvas
        self.assertNotIn("Control", [self.editor.model.node_type(n) for n in self.editor.model.order])
        field = panel.key_widgets["keep"]
        self.assertEqual(field.property("keyState"), "keyed")
        keyed = QtGui.QColor(self.ge.theme.current().color(*self.ge.KEY_COLORS["keyed"])).name()
        self.assertIn(keyed, field.field.styleSheet().lower())
        cmds.undo()  # attribute, link and key: one step
        wait(60)
        self.assertFalse(cmds.attributeQuery("ctl_length_keep", node=self.groom, exists=True))
        self.assertIsNone(self.editor.model.control_of("length", "keep"))
        self.assertEqual(panel.key_widgets["keep"].property("keyState"), "")
        cmds.redo()
        wait(60)
        self.assertEqual(self.editor.model.control_of("length", "keep"), "length_keep")

        # Another frame: animated (pink); an edit holds until the time changes (Auto Key off).
        at_frame(10)
        field = panel.key_widgets["keep"]
        self.assertEqual(field.property("keyState"), "animated")
        field.field.setValue(40.0)  # keep is a share: its field shows a percentage
        field.field.editingFinished.emit()
        self.assertAlmostEqual(cmds.getAttr(attr), 0.4)
        # S over the field keys it here (the editor's view has the keyboard focus).
        self.assertTrue(hover(field.field))  # the mouse over the field (Qt's, not the real cursor)
        wait(30)
        self.assertEqual(panel.hovered_param(), "keep")
        press(self.editor.view, QtCore.Qt.Key_S)
        wait(30)
        self.assertEqual(sorted(cmds.keyframe(attr, query=True)), [1.0, 10.0])
        self.assertEqual(panel.key_widgets["keep"].property("keyState"), "keyed")
        # The field follows the time: the value and the state on each frame.
        at_frame(1)
        field = panel.key_widgets["keep"]
        self.assertAlmostEqual(field.value(), 100.0, places=2)
        self.assertEqual(field.property("keyState"), "keyed")
        at_frame(5)
        self.assertLess(field.value(), 100.0)
        self.assertGreater(field.value(), 40.0)
        self.assertEqual(field.property("keyState"), "animated")
        # Remove Animation: a plain param with its value then.
        panel.remove_animation("length", "keep")
        wait(30)
        self.assertFalse(cmds.attributeQuery("ctl_length_keep", node=self.groom, exists=True))
        self.assertEqual(panel.key_widgets["keep"].property("keyState"), "")
        self.assertLess(self.editor.model.param("length", "keep"), 1.0)

        # A param that sets the strand count: Set Key disabled, saying why; S over it only says so.
        self.editor.scene.clearSelection()
        self.editor.items["scatter"].setSelected(True)
        menu = panel.build_param_menu("scatter", "density")
        action = next(a for a in menu.actions() if a.text() == "Set Key")
        self.assertFalse(action.isEnabled())
        self.assertIn("strands", action.toolTip())
        density = panel.key_widgets["density"]
        self.assertTrue(hover(density.field))
        wait(30)
        press(self.editor.view, QtCore.Qt.Key_S)
        wait(30)
        self.assertIn("cannot be keyed", self.editor.status.text())
        self.assertEqual([a for a in cmds.listAttr(self.groom, userDefined=True) or [] if a.startswith("ctl_")], [])
        self.editor.hide()

    def test_dragging_a_keyed_slider_previews_live_and_commits_one_step(self):
        # A keyed param's slider moves the groom while it is dragged (its attribute, outside undo),
        # and the release is one undo step from the value before the drag.
        from PySide2 import QtCore, QtGui, QtTest
        from groomlab.ui import widgets
        widgets.forget_range("Length.keep")  # the default range, whatever an earlier test stretched it to
        # The CPU's drag (the attribute written as it moves): the chain ends in an even Resample, which
        # the GPU preview would take, drawing the drag itself and writing the value on release.
        gpu_was = groomlab.gpu_preview()
        groomlab.gpu_preview(False)
        self.addCleanup(groomlab.gpu_preview, gpu_was)
        cmds.autoKeyframe(state=False)
        cmds.currentTime(1)
        self.editor.resize(1200, 600)
        self.editor.show()
        wait(30)
        self.editor.items["length"].setSelected(True)
        menu = self.editor.params.build_param_menu("length", "keep")
        next(a for a in menu.actions() if a.text() == "Set Key").trigger()  # as the user keys it
        wait(60)
        attr = self.groom + ".ctl_length_keep"
        self.assertEqual(cmds.getAttr(attr), 1.0)
        self.assertTrue(self.editor.keyed("length", "keep"))
        before = groomlab.viewport_points(self.groom)
        slider = self.editor.params.key_widgets["keep"].slider
        y = slider.rect().center().y()
        start, end = QtCore.QPoint(int(slider.width() * 0.9), y), QtCore.QPoint(int(slider.width() * 0.2), y)
        keyed_step = settle_undo()  # Maya's idle work done: only the drag's steps come after this
        QtTest.QTest.mousePress(slider, QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, start)
        wait(30)
        pressed = cmds.getAttr(attr)  # a press off the handle jumps there (a step of its own)
        for k in range(1, 9):
            at = QtCore.QPoint(int(start.x() + (end.x() - start.x()) * k / 8.0), y)
            self.app.sendEvent(slider, QtGui.QMouseEvent(QtCore.QEvent.MouseMove, at, QtCore.Qt.LeftButton,
                                                         QtCore.Qt.LeftButton, QtCore.Qt.NoModifier))
            wait(20)
        wait(120)  # the coalesced preview write
        self.assertTrue(self.editor.previewing())
        dragged = cmds.getAttr(attr)
        self.assertLess(dragged, 0.6)  # the attribute follows the drag...
        self.assertNotEqual(groomlab.viewport_points(self.groom), before)  # ...and so do the strands
        undo_name = cmds.undoInfo(query=True, undoName=True)
        QtTest.QTest.mouseRelease(slider, QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, end)
        wait(60)
        self.assertFalse(self.editor.previewing())
        self.assertAlmostEqual(cmds.getAttr(attr) * 100.0, self.editor.params.key_widgets["keep"].value(), places=3)  # a percentage
        if pressed == 1.0:  # the press was on the handle (no step): the release is one
            self.assertEqual(undo_name, keyed_step)  # the drag itself made none (previews are outside undo)
            self.assertNotEqual(cmds.undoInfo(query=True, undoName=True), undo_name)
        else:  # the press jumped (a "set param" step): the release is another of that name
            self.assertNotEqual(undo_name, keyed_step)  # so undoing the release alone gives the jump's value, below
        cmds.undo()
        self.assertAlmostEqual(cmds.getAttr(attr), pressed, places=5)  # back to before the drag, in one step
        self.assertEqual(cmds.keyframe(attr, query=True, valueChange=True), [1.0])  # Auto Key off: no key
        self.editor.hide()

    def test_a_keyed_color_shows_on_its_three_boxes(self):
        cmds.currentTime(1)
        self.editor.items["width"].setSelected(True)
        name = self.editor.add_node("HairColor")
        self.editor.scene.clearSelection()
        self.editor.items[name].setSelected(True)
        panel = self.editor.params
        self.assertTrue(panel.set_key(name, "tipColor"))
        attr = "ctl_%s_tipColor" % name
        self.assertTrue(cmds.attributeQuery(attr, node=self.groom, usedAsColor=True))
        boxes = panel.key_widgets["tipColor"].boxes
        self.assertEqual(len(boxes), 3)
        cmds.setAttr(self.groom + "." + attr, 0.9, 0.1, 0.2, type="float3")
        wait(60)  # the attribute's change reaches the fields
        self.assertAlmostEqual(boxes[0].value(), 0.9, places=3)
        self.assertEqual(panel.key_widgets["tipColor"].property("keyState"), "keyed")
        self.assertTrue(all(b.styleSheet() for b in boxes))
        # Its numbers key; its seed (which strands vary how) does not.
        self.assertIsNone(self.editor.model.keyable(name, "melanin"))
        self.assertIsNotNone(self.editor.model.keyable(name, "seed"))
        # The keyed color's swatch follows its value on the frame.
        swatch = panel.key_widgets["tipColor"].swatch
        self.assertEqual([round(c, 3) for c in swatch.rgb], [0.9, 0.1, 0.2])

    def test_color_params_show_a_swatch_that_picks(self):
        # Every color param (a vec3 whose unit is color) has a swatch: the color as Maya displays it
        # (through the view transform with color management on), a click opening Maya's color
        # editor. apply() is what a pick does (no modal editor in a scripted session): the param, its
        # boxes and the swatch take the linear values picked.
        from groomlab.ui import widgets
        self.editor.items["width"].setSelected(True)
        name = self.editor.add_node("HairColor")
        self.editor.scene.clearSelection()
        self.editor.items[name].setSelected(True)
        panel = self.editor.params
        self.editor.set_param(name, "mode", "melanin")
        body = panel.widget()
        swatches = body.findChildren(widgets.ColorSwatch)
        self.assertGreaterEqual(len(swatches), 2)  # dye and tipColor (color: mode color only)
        dye = panel.key_widgets["dye"]
        self.assertIsNotNone(dye.swatch)
        self.assertEqual(dye.swatch.rgb, [1.0, 1.0, 1.0])
        self.assertEqual(dye.swatch.shown(), widgets.display_color([1.0, 1.0, 1.0]))
        dye.swatch.apply([0.2, 0.4, 0.6])
        wait(30)
        stored = self.editor.model.param(name, "dye")
        self.assertEqual([round(c, 4) for c in stored], [0.2, 0.4, 0.6])
        self.assertEqual([round(b.value(), 4) for b in panel.key_widgets["dye"].boxes], [0.2, 0.4, 0.6])
        # Typed values reach the swatch too.
        boxes = panel.key_widgets["dye"].boxes
        boxes[0].setValue(0.9)
        boxes[0].editingFinished.emit()
        wait(30)
        self.assertAlmostEqual(panel.key_widgets["dye"].swatch.rgb[0], 0.9, places=4)
        # With color management on, the swatch shows Maya's display value, not the linear one.
        if cmds.colorManagementPrefs(query=True, cmEnabled=True):
            shown = cmds.colorManagementConvert(toDisplaySpace=[0.2, 0.4, 0.6])
            self.assertEqual([round(c, 4) for c in widgets.display_color([0.2, 0.4, 0.6])],
                             [round(min(1.0, max(0.0, c)), 4) for c in shown])
        # A click asks (the picker is replaceable; Maya's color editor by default) and applies.
        swatch = widgets.ColorSwatch([0.5, 0.5, 0.5], ask=lambda rgb: [0.1, 0.2, 0.3])
        got = []
        swatch.picked.connect(lambda rgb: got.append(list(rgb)))
        swatch.click()
        self.assertEqual(got, [[0.1, 0.2, 0.3]])
        cancelled = widgets.ColorSwatch([0.5, 0.5, 0.5], ask=lambda rgb: None)
        cancelled.picked.connect(lambda rgb: got.append(list(rgb)))
        cancelled.click()
        self.assertEqual(len(got), 1)

    def test_closing_the_editor_leaves_no_script_jobs(self):
        def ours():
            return {j for j in cmds.scriptJob(listJobs=True) or [] if "GraphEditor" in j}
        before = ours()
        editor = self.ge.GraphEditor(self.groom)
        editor.show()
        wait(30)
        self.assertGreater(len(ours() - before), 0)
        editor.set_groom(self.groom)  # a groom (re)chosen: its jobs replaced, not added
        editor.items["length"].setSelected(True)
        editor.params.set_key("length", "keep")
        cmds.currentTime(cmds.currentTime(query=True) + 1)
        wait(30)
        editor.close()
        wait(30)
        self.assertEqual(ours() - before, set())
        editor.deleteLater()

    def test_scalp_node_shows_the_mesh(self):
        from PySide2 import QtWidgets
        self.editor.params.show_node("scalp")
        body = self.editor.params.widget()
        labels = [w.text() for w in body.findChildren(QtWidgets.QLabel)]
        self.assertNotIn("input", labels)  # plumbing hidden
        combo = body.findChild(QtWidgets.QComboBox, "meshField")  # the mesh it reads, in a menu of meshes
        self.assertEqual(combo.currentText(), "pSphere1")
        self.assertIsNotNone(body.findChild(QtWidgets.QToolButton, "meshPick"))
        self.assertNotIn("Rest pose", labels)  # a scalp that doesn't move needs no rest pose
        # A deformed scalp: the groom takes its undeformed shape as the rest pose, and the row says so.
        bent = cmds.polySphere(name="bentHead")[0]
        cmds.cluster(bent)
        groomlab.set_scalp(self.groom, bent)
        self.editor.params.show_node("scalp")
        labels = [w.text() for w in self.editor.params.widget().findChildren(QtWidgets.QLabel)]
        self.assertIn("Rest pose", labels)
        self.assertIn("<b>auto (the undeformed scalp)</b>", labels)
        # Another mesh from its menu (listed as it opens): the groom grows on it.
        cmds.polyCube()
        combo = self.editor.params.widget().findChild(QtWidgets.QComboBox, "meshField")
        combo.fill()
        self.editor.params.choose_mesh("scalp", combo.itemData(combo.findText("pCube1")), ask=False)
        self.assertIn("pCube", groomlab.scalp_mesh(self.groom))
        self.editor.params.show_node("guides")
        labels = [w.text() for w in self.editor.params.widget().findChildren(QtWidgets.QLabel)]
        self.assertIn("<b>no guides yet</b>", labels)

    def test_attribute_editor_sections_build(self):
        from groomlab.ui import ae
        window = cmds.window()
        cmds.columnLayout()
        groomlab.add_control(self.groom, "length", "keep")
        ball = cmds.polySphere(radius=0.3, name="aeBall")[0]
        groomlab.add_collider(self.groom, ball, wire=False, friction=0.4)
        ae.inputs_new(self.groom + ".inMesh")
        ae.controls_new(self.groom + ".graph")
        texts = [cmds.textField(c, query=True, text=True) for c in cmds.lsUI(type="textField", long=True) or []
                 if ae.INPUTS in c]
        self.assertTrue(any("pSphere" in t for t in texts), texts)
        # The collider's row shows its friction.
        frictions = [cmds.floatField(c, query=True, value=True) for c in cmds.lsUI(type="floatField", long=True) or []
                     if ae.INPUTS in c]
        self.assertEqual([round(f, 3) for f in frictions], [0.4])
        self.assertEqual(len(cmds.columnLayout(ae.CONTROLS, query=True, childArray=True) or []), 1)  # one slider
        ae.inputs_replace(self.groom + ".inMesh")
        # Edits don't multiply the sections' scriptJobs (they used to double per edit).
        ours = lambda: [j for j in cmds.scriptJob(listJobs=True) if "_watch" in j or "groomLabAE" in j]
        count = len(ours())
        self.assertGreater(count, 0)
        for i in range(8):
            groomlab.set_param(self.groom, "length", "keep", 1.0 + 0.1 * i)
            cmds.flushIdleQueue()
        self.assertEqual(len(ours()), count, ours())
        cmds.deleteUI(window)

    def test_attribute_editor_pick_takes_the_next_selected_mesh(self):
        # Pick arms: the next mesh selected (viewport or Outliner: a selection change) becomes a
        # collider through Maya's own SelectionChanged scriptJob, and the groom is selected again.
        from PySide2 import QtCore, QtGui
        from groomlab.ui import ae, pick
        window = cmds.window()
        cmds.columnLayout()
        ae.inputs_new(self.groom + ".inMesh")
        ball = cmds.polySphere(radius=0.3, name="pickBall")[0]
        cmds.select(self.groom)

        def label():
            button = ae._pick_buttons["collider"][1]
            return cmds.button(button, query=True, label=True) if cmds.button(button, exists=True) else None

        def settle():
            cmds.flushIdleQueue()  # the scriptJob, then its deferred apply
            wait(30)
            cmds.flushIdleQueue()

        try:
            self.assertEqual(ae.press_pick(self.groom, "collider"), "armed")
            self.assertEqual(label(), "Cancel")
            cmds.select(ball)
            settle()
            self.assertEqual([c["name"] for c in groomlab.colliders(self.groom)], ["pickBall"])
            self.assertTrue(pick.groom_selected(self.groom))
            self.assertIsNone(pick.armed())
            self.assertEqual(label(), "Pick")
            # An invalid pick (the scalp) cancels, and the groom comes back.
            self.assertEqual(ae.press_pick(self.groom, "collider"), "armed")
            cmds.select(groomlab.scalp_mesh(self.groom))
            settle()
            self.assertEqual(len(groomlab.colliders(self.groom)), 1)
            self.assertTrue(pick.groom_selected(self.groom))
            self.assertIsNone(pick.armed())
            # Esc cancels, the selection as it was.
            self.assertEqual(ae.press_pick(self.groom, "collider"), "armed")
            event = QtGui.QKeyEvent(QtCore.QEvent.KeyPress, QtCore.Qt.Key_Escape, QtCore.Qt.NoModifier)
            self.app.sendEvent(self.editor, event)
            wait(30)
            self.assertIsNone(pick.armed())
            self.assertEqual(label(), "Pick")
            # The button pressed again cancels too.
            self.assertEqual(ae.press_pick(self.groom, "collider"), "armed")
            self.assertEqual(ae.press_pick(self.groom, "collider"), "cancelled")
            self.assertEqual(label(), "Pick")
            self.assertEqual(len(groomlab.colliders(self.groom)), 1)
        finally:
            pick.cancel()
            cmds.deleteUI(window)

    def test_collider_node_mesh_menu_and_eyedropper(self):
        # A ColliderMesh node added in the graph: its mesh menu wires the groom's collider of its
        # name, the eyedropper swaps it for the next mesh selected, and a change from elsewhere (the
        # Attribute Editor, a script) shows in the panel.
        from PySide2 import QtWidgets
        from groomlab.ui import pick
        ball = cmds.polySphere(radius=0.3, name="ball")[0]
        box = cmds.polyCube(name="box")[0]
        name = self.editor.add_node("ColliderMesh")
        self.editor.params.show_node(name)
        collider = self.editor.model.param(name, "name")
        field = lambda: self.editor.params.widget().findChild(QtWidgets.QComboBox, "meshField")
        self.assertEqual(field().currentText(), "none")
        field().fill()
        texts = [field().itemText(i) for i in range(field().count())]
        self.assertNotIn("pSphere1", texts)  # the scalp is no collider
        field().activated.emit(field().findText("ball"))
        wait(50)
        meshes = lambda: [(c["name"], c["mesh"].split("|")[-1]) for c in groomlab.colliders(self.groom)]
        self.assertEqual(meshes(), [(collider, "ballShape")])
        self.assertEqual(field().currentText(), "ball")
        # Elsewhere: the panel follows the groom's connection.
        groomlab.add_collider(self.groom, box, name=collider, wire=False)
        wait(50)
        self.assertEqual(field().currentText(), "box")
        # The eyedropper: armed, then the next mesh selected.
        dropper = self.editor.params.widget().findChild(QtWidgets.QToolButton, "meshPick")
        dropper.click()
        self.assertTrue(pick.armed(self.groom, pick.input_key("collider", collider)))
        self.assertTrue(dropper.isChecked())
        cmds.select(ball)
        cmds.flushIdleQueue()
        wait(50)
        cmds.flushIdleQueue()
        self.assertIsNone(pick.armed())
        self.assertEqual(meshes(), [(collider, "ballShape")])
        self.assertTrue(pick.groom_selected(self.groom))
        self.assertEqual(field().currentText(), "ball")
        self.assertFalse(self.editor.params.widget().findChild(QtWidgets.QToolButton, "meshPick").isChecked())
        self.assertEqual([n for n in self.editor.model.order if self.editor.model.node_type(n) == "Collide"], [])

    def test_scalp_node_reading_another_scalp(self):
        # A Scalp node whose input names another scalp (several scalps on one groom): its mesh menu
        # wires the groom's extraScalps element of that name; the Attribute Editor lists it with
        # Select and Remove, and a Pick row adds one.
        from PySide2 import QtWidgets
        from groomlab.ui import ae
        ear = cmds.polyPlane(width=0.5, height=0.5, name="earMesh")[0]
        cmds.xform(ear, translation=(1.5, 0.0, 0.0))
        name = self.editor.add_node("Scalp")
        self.editor.model.set_param(name, "input", "ears")
        self.editor.apply()
        self.editor.params.show_node(name)
        field = lambda: self.editor.params.widget().findChild(QtWidgets.QComboBox, "meshField")
        self.assertEqual(field().currentText(), "none")
        field().fill()
        texts = [field().itemText(i) for i in range(field().count())]
        self.assertNotIn("pSphere1", texts)  # the groom's own scalp is no other scalp
        field().activated.emit(field().findText("earMesh"))
        wait(50)
        self.assertEqual([(s["name"], s["mesh"].split("|")[-1]) for s in groomlab.scalps(self.groom)],
                         [("ears", "earMeshShape")])
        self.assertEqual(field().currentText(), "earMesh")
        self.assertEqual(groomlab.errors(self.groom), [])
        window = cmds.window()
        cmds.columnLayout()
        ae.inputs_new(self.groom + ".inMesh")
        texts = [cmds.textField(c, query=True, text=True) for c in cmds.lsUI(type="textField", long=True) or []
                 if ae.INPUTS in c]
        self.assertTrue(any("ears (earMesh" in t for t in texts), texts)
        self.assertIn("extraScalp", ae._pick_buttons)
        cmds.deleteUI(window)
        self.assertTrue(groomlab.remove_scalp(self.groom, "ears"))

    def test_group_and_ungroup(self):
        editor = self.editor
        editor.scene.clearSelection()
        for name in ("length", "width"):
            editor.items[name].setSelected(True)
        compound = editor.collapse_selected()
        self.assertIsNotNone(compound)
        self.assertIn(compound, editor.items)
        self.assertEqual(list(editor.items[compound].inputs), ["strands"])
        self.assertGreater(cmds.getAttr(self.groom + ".outStrandCount"), 0)
        editor.scene.clearSelection()
        editor.items[compound].setSelected(True)
        editor.expand_selected()
        self.assertNotIn(compound, editor.items)
        self.assertEqual(sorted(editor.model.node_type(n) for n in editor.model.order if n not in
                                ("scalp", "guides", "scatter", "interpolate", "output")), ["Length", "Resample", "Width"])

    def test_group_and_ungroup_from_keys_menu_and_panel(self):
        # Ctrl+G groups the selection; a Compound offers Ungroup on right-click, in its panel and as
        # Ctrl+Shift+G, and Ctrl+Z brings the compound back.
        from PySide2 import QtCore, QtTest, QtWidgets
        editor = self.editor
        editor.resize(1200, 600)
        editor.show()
        wait(30)
        editor.scene.clearSelection()
        for name in ("length", "width"):
            editor.items[name].setSelected(True)
        press(editor.view, QtCore.Qt.Key_G, QtCore.Qt.ControlModifier)
        wait(30)
        compounds = [n for n in editor.model.order if editor.model.node_type(n) == "Compound"]
        self.assertEqual(len(compounds), 1)
        compound = compounds[0]
        # Its panel has the button; the right-click menu has the action.
        editor.scene.clearSelection()
        editor.items[compound].setSelected(True)
        wait(30)
        button = editor.params.findChild(QtWidgets.QPushButton, "nodeAction_UngroupCompound")
        self.assertIsNotNone(button)
        settle_undo()
        button.click()
        wait(60)
        self.assertNotIn(compound, editor.items)
        self.assertIn("length", editor.items)
        cmds.undo()  # the ungroup is one step
        wait(60)
        self.assertIn(compound, editor.model.order)
        # Ctrl+Shift+G with the compound selected.
        editor.scene.clearSelection()
        editor.items[compound].setSelected(True)
        press(editor.view, QtCore.Qt.Key_G, QtCore.Qt.ControlModifier | QtCore.Qt.ShiftModifier)
        wait(30)
        self.assertNotIn(compound, editor.model.order)
        self.assertEqual(groomlab.errors(self.groom), [])
        editor.hide()

    def test_save_as_node_lists_it_in_add_node_and_search(self):
        # Save as Node: the panel's form (no dialog) writes the compound to the user's node folder; Add
        # Node and Tab search list it under its category; adding it gives a node from the library;
        # Import Node copies a shared file in and adds it.
        import shutil
        import tempfile
        from PySide2 import QtWidgets
        folder = tempfile.mkdtemp(prefix="groomlab_gui_nodes_")
        old = os.environ.get("GROOMLAB_USER_NODES")
        os.environ["GROOMLAB_USER_NODES"] = folder
        try:
            editor = self.editor
            editor.resize(1200, 600)
            editor.show()
            wait(30)
            compound = editor.collapse(["length", "width"])
            self.assertTrue(editor.show_inner_param(compound, "length.keep"))  # right-click > Show Inner Param
            editor.scene.clearSelection()
            editor.items[compound].setSelected(True)
            wait(30)
            button = editor.params.findChild(QtWidgets.QPushButton, "nodeAction_SaveasNode")
            self.assertIsNotNone(button)
            button.click()
            wait(60)
            form = editor.params.findChild(QtWidgets.QWidget, "saveNodeForm")
            self.assertIsNotNone(form)
            form.fields["label"].setText("Gui Look")
            form.fields["label"].textEdited.emit("Gui Look")
            form.fields["author"].setText("Tester")
            form.fields["author"].textEdited.emit("Tester")
            form.fields["category"].setCurrentText("Look")
            self.assertEqual(form.fields["id"].text(), "tester.gui_look")
            self.assertEqual(form.fields["version"].value(), 1)
            self.assertTrue(form.fields["params"]["keep"].isChecked())
            settle_undo()
            form.fields["save"].click()
            wait(60)
            path = os.path.join(folder, "tester.gui_look.glnode")
            self.assertTrue(os.path.isfile(path), editor.status.text())
            self.assertEqual(editor.model.library_info(compound)["id"], "tester.gui_look")
            self.assertEqual(groomlab.errors(self.groom), [])
            with open(path) as f:
                self.assertEqual([p["name"] for p in json.load(f)["params"]], ["keep"])

            # Add Node lists it under Look; Tab search finds it; adding it makes a library node.
            editor.fill_add_menu(editor.add_menu)
            look = next(m for m in editor.add_menu.submenus if m.title() == "Look")
            action = next((a for a in look.actions() if a.data() == "node:tester.gui_look"), None)
            self.assertIsNotNone(action)
            self.assertEqual(action.text(), "Gui Look")
            search = self.ge.NodeSearch(editor, (0.0, 0.0))
            search.field.setText("gui look")
            self.assertEqual(search.list.item(0).data(0x0100), "node:tester.gui_look")  # Qt.UserRole
            search.close()
            editor.scene.clearSelection()
            action.trigger()
            wait(60)
            added = [n for n in editor.model.order if (editor.model.library_info(n) or {}).get("id") == "tester.gui_look"]
            self.assertEqual(len(added), 2)
            self.assertIn("guiLook", added)

            # Import Node: a shared file copied into the library and added.
            shared_dir = tempfile.mkdtemp(prefix="groomlab_gui_share_")
            shared = os.path.join(shared_dir, "wave.glnode")
            with open(path) as f:
                data = json.load(f)
            data["id"], data["label"] = "friend.wave", "Wave"
            with open(shared, "w") as f:
                json.dump(data, f)
            made = editor.import_nodes([shared], at=(0.0, 300.0))
            shutil.rmtree(shared_dir, ignore_errors=True)
            self.assertEqual(made, ["wave"])
            self.assertTrue(os.path.isfile(os.path.join(folder, "friend.wave.glnode")))
            editor.hide()
        finally:
            if old is None:
                os.environ.pop("GROOMLAB_USER_NODES", None)
            else:
                os.environ["GROOMLAB_USER_NODES"] = old
            shutil.rmtree(folder, ignore_errors=True)

    def test_script_node_editor_is_live_and_saves(self):
        # New Script Node (Add Node) opens its editor in the side panel (no dialog); a script with an
        # error lists it by line and column and the node keeps its last good script; fixed, the
        # node takes it (live, after the pause); a param added in the table reads as $name; Save to
        # Library writes the file; Done shows the node's params, the new one among them.
        import shutil
        import tempfile
        from PySide2 import QtWidgets
        folder = tempfile.mkdtemp(prefix="groomlab_gui_scripts_")
        old = os.environ.get("GROOMLAB_USER_NODES")
        os.environ["GROOMLAB_USER_NODES"] = folder
        try:
            editor = self.editor
            editor.resize(1200, 800)
            editor.show()
            wait(30)
            editor.scene.clearSelection()
            editor.items["width"].setSelected(True)
            editor.fill_add_menu(editor.add_menu)
            action = next(a for a in editor.add_menu.actions() if a.text() == "New Script Node")
            action.trigger()
            wait(60)
            name = editor.params.editing_script
            self.assertTrue(name and name.startswith("myScript"), name)
            self.assertEqual(editor.model.node_type(name), "Script")
            self.assertEqual(editor.model.source_of(name, "strands"), "width")  # wired after the selection
            panel = editor.params.findChild(QtWidgets.QWidget, "scriptNodeEditor")
            self.assertIsNotNone(panel)
            fields = panel.fields
            self.assertEqual(groomlab.errors(self.groom), [])
            good = editor.model.param(name, "definition")["script"]

            # An error: listed with its line and column, its line tinted; the node keeps its script.
            fields["script"].setPlainText("per point {\n    P += N * ;\n}")
            wait(450)  # the editor's pause (300 ms)
            errors = fields["errors"]
            self.assertTrue(errors.isVisible() or errors.count() == 1)
            self.assertEqual(errors.count(), 1)
            self.assertIn("line 2, column 14", errors.item(0).text())
            self.assertEqual(fields["script"].error_lines, {2})
            self.assertEqual(editor.model.param(name, "definition")["script"], good)

            # Fixed, with a param of its own added in the table: the node takes it.
            fields["add"].click()
            table = fields["params"]
            row = table.rowCount() - 1
            table.item(row, 0).setText("push")
            table.cellWidget(row, 1).setCurrentText("length")
            table.item(row, 2).setText("0.2")
            fields["script"].setPlainText("per point {\n    P += N * ($amount + $push) * t;\n}")
            wait(450)
            self.assertEqual(errors.count(), 0, [errors.item(i).text() for i in range(errors.count())])
            definition = editor.model.param(name, "definition")
            self.assertIn("$push", definition["script"])
            self.assertEqual([p["name"] for p in definition["params"]], ["amount", "push"])
            self.assertAlmostEqual(editor.model.param(name, "push"), 0.2)
            self.assertEqual(groomlab.errors(self.groom), [])
            self.assertIs(editor.params.widget(), panel)  # the editor stayed through the apply

            # A write the header does not list: said, with where.
            fields["script"].setPlainText("per point {\n    width = 1;\n}")
            wait(450)
            self.assertIn("width is not in the node's", errors.item(0).text())
            fields["writes"]["width"].setChecked(True)
            wait(450)
            self.assertEqual(errors.count(), 0)

            # Save to Library: the file, then Done: the params panel with the new param.
            fields["label"].setText("Gui Lift")
            fields["label"].textEdited.emit("Gui Lift")
            fields["id"].setText("tester.gui_lift")
            fields["id"].textEdited.emit("tester.gui_lift")
            fields["save"].click()
            wait(60)
            path = os.path.join(folder, "tester.gui_lift.glnode")
            self.assertTrue(os.path.isfile(path), fields["status"].text())
            with open(path) as f:
                saved = json.load(f)
            self.assertEqual(saved["writes"], ["P", "width"])
            self.assertEqual(editor.model.library_info(name)["id"], "tester.gui_lift")
            fields["save_new"].click()  # its id is the library's now: a new node needs another
            wait(30)
            self.assertIn("another Id", errors.item(0).text())
            fields["done"].click()
            wait(60)
            self.assertIsNone(editor.params.editing_script)
            self.assertIsNone(editor.params.findChild(QtWidgets.QWidget, "scriptNodeEditor"))
            rows = [w.text() for w in editor.params.findChildren(QtWidgets.QLabel)]
            self.assertTrue(any("Lift" in r for r in rows), rows)
            editor.hide()
        finally:
            if old is None:
                os.environ.pop("GROOMLAB_USER_NODES", None)
            else:
                os.environ["GROOMLAB_USER_NODES"] = old
            shutil.rmtree(folder, ignore_errors=True)

    def test_shaping_nodes_take_strands_or_guides(self):
        # ToGuides -> Noise -> Twist's axes: the Noise gives guides, shown and checked as such.
        model = self.editor.model
        centers = model.add_node("ToGuides", params={"fraction": 0.05})
        frizz = model.add_node("Noise")
        twist = model.add_node("Twist")
        self.assertEqual(model.output_type(frizz), "Curves")  # unconnected: either
        model.connect("interpolate", centers, "strands")
        model.connect(centers, frizz, "strands")
        self.assertEqual(model.output_type(frizz), "Guides")
        model.connect(frizz, twist, "axes")
        model.connect("interpolate", twist, "strands")
        with self.assertRaises(self.ge.gm.GraphError):
            model.connect(frizz, "width", "strands")  # guides can't feed a strands-only input
        model.insert_between("length", "width", twist)
        self.assertEqual(model.validate(), [])
        self.editor.rebuild()
        self.editor.changed()
        self.assertEqual(groomlab.errors(self.groom), [])
        self.assertEqual(self.editor.items[frizz].output.port_type, "Guides")
        # Rewired to strands, a guides-only link downstream is reported, not silently broken.
        model.connect("interpolate", frizz, "strands")
        self.assertEqual(model.output_type(frizz), "Strands")
        self.assertTrue(any("takes guides" in p for p in model.validate()), model.validate())

    def test_attribute_maps_offer_the_channels_that_reach_the_node(self):
        from groomlab import presets
        from PySide2 import QtWidgets
        presets.apply_preset(self.groom, "flow_with_guides")  # its Blend writes `blend` (mixChannel)
        self.editor.reload()
        self.assertIn("blend", self.editor.node_channels.get("blend", []))
        self.assertIn("blend", self.editor.channels_into("color"))
        self.assertNotIn("blend", self.editor.channels_into("blend"))  # made there, not before
        # HairColor's clumpChannel (a param that reads) offers it.
        self.editor.scene.clearSelection()
        self.editor.items["color"].setSelected(True)
        boxes = self.editor.params.findChildren(QtWidgets.QComboBox)
        offered = [b.itemText(i) for b in boxes if b.isEditable() for i in range(b.count())]
        self.assertIn("blend", [text.split(" (from ")[0] for text in offered], offered)  # "blend (from blend)"

    def test_attribute_params_say_writes_or_reads(self):
        # Clump's clumpChannel writes (a plain field, a warning when it overwrites an upstream
        # Clump's), its regionChannel and a Curl's clumpChannel read (the attributes reaching the
        # node to pick from, each with its writer; a note when the one named does not reach).
        from PySide2 import QtWidgets
        model, panel = self.editor.model, self.editor.params
        model.insert_between("interpolate", "length", model.add_node("Clump", name="clumpLarge"))
        model.insert_between("clumpLarge", "length", model.add_node("Clump", name="clumpSmall"))
        model.insert_between("clumpSmall", "length", model.add_node(
            "Curl", name="curl", params={"coherence": "clump", "clumpChannel": "lockId"}))
        self.editor.rebuild()
        self.editor.changed()

        def rows(node):
            panel.show_node(node)
            out = {}
            for chip in panel.findChildren(QtWidgets.QLabel, "attributeUse"):
                holder = chip.parentWidget()
                note = holder.note.text() if holder.note is not None else ""
                out[holder.property("glParam")] = (chip.text(), holder.field, note)
            return out

        clump = rows("clumpSmall")
        self.assertEqual(clump["clumpChannel"][0], "Writes")
        self.assertIsInstance(clump["clumpChannel"][1], QtWidgets.QLineEdit)  # a name of its own: no dropdown
        self.assertIn("overwrites clumpId from clumpLarge", clump["clumpChannel"][2])
        self.assertEqual(clump["regionChannel"][0], "Reads")
        self.assertIn("no node upstream writes region", clump["regionChannel"][2])
        self.assertNotIn("clumpSmall", self.editor.attribute_notes)  # region by default: optional, no badge
        curl = rows("curl")["clumpChannel"]
        self.assertEqual(curl[0], "Reads")
        self.assertIn("no node upstream writes lockId", curl[2])
        self.assertIn("lockId", self.editor.note_of("curl"))  # the node's grey "i"
        self.assertEqual([curl[1].itemText(i) for i in range(curl[1].count())], ["clumpId (from clumpSmall)"])
        self.assertEqual(self.editor.params.suggestions("clumpSmall", "clumpChannel")[0], [])

        # Its own name for the large clumps: no overwrite, and the Curl reads it.
        self.editor.set_param("clumpLarge", "clumpChannel", "lockId")
        self.assertEqual(rows("clumpSmall")["clumpChannel"][2], "")
        curl = rows("curl")["clumpChannel"]
        self.assertEqual(curl[2], "")
        self.assertNotIn("curl", self.editor.attribute_notes)
        self.assertEqual(sorted(curl[1].itemText(i) for i in range(curl[1].count())),
                         ["clumpId (from clumpSmall)", "lockId (from clumpLarge)"])
        self.assertEqual(curl[1].currentText(), "lockId")
        # Picking one stores its name alone.
        index = curl[1].findData("clumpId")
        curl[1].activated.emit(index)
        self.assertEqual(model.param("curl", "clumpChannel"), "clumpId")

    def test_widget_renders(self):
        from PySide2 import QtCore
        self.editor.resize(1200, 600)
        self.editor.show()
        self.editor.items["scatter"].setSelected(True)
        self.app.processEvents()
        view = self.editor.view
        from groomlab.ui import theme
        # Opened at a readable size, on the output end where look nodes go (all of it when it fits).
        self.assertGreaterEqual(view.transform().m11(), view.READABLE * theme.scale() - 1e-6)
        output = view.mapFromScene(self.editor.items["output"].sceneBoundingRect()).boundingRect()
        self.assertTrue(view.viewport().rect().contains(output))
        view.frame_all()  # A: everything that fits at a readable zoom, centered on the graph
        self.assertGreaterEqual(view.transform().m11(), view.READABLE * theme.scale() - 1e-6)
        middle = view.mapFromScene(self.editor.scene.itemsBoundingRect().center())
        self.assertTrue(view.viewport().rect().contains(middle))
        framed = view.mapFromScene(self.editor.scene.itemsBoundingRect()).boundingRect()
        if view.transform().m11() > view.READABLE * theme.scale() + 1e-6:  # it fit: all of it shows
            self.assertTrue(view.viewport().rect().contains(framed))
        # Panning isn't held to the nodes: the view goes well past them.
        far = self.editor.scene.itemsBoundingRect().bottomRight() + QtCore.QPointF(3000, 3000)
        view.centerOn(far)
        center = view.mapToScene(view.viewport().rect().center())
        self.assertLess((center - far).manhattanLength(), 50)
        view.frame()  # Frame: the selected node fills the view
        node = view.mapFromScene(self.editor.items["scatter"].sceneBoundingRect()).boundingRect()
        self.assertTrue(view.viewport().rect().contains(node))
        # Alt+right-drag zooms as in Maya's viewports (right or down: in), the pressed point staying
        # put, and opens no add menu.
        from PySide2 import QtGui, QtWidgets
        alt, right = QtCore.Qt.AltModifier, QtCore.Qt.RightButton
        start = QtCore.QPointF(view.viewport().rect().center())
        anchor = view.mapToScene(start.toPoint())
        zoom = view.transform().m11()

        def drag(kind, pos, button, buttons):
            event = QtGui.QMouseEvent(kind, pos, button, buttons, alt)
            {QtCore.QEvent.MouseButtonPress: view.mousePressEvent, QtCore.QEvent.MouseMove: view.mouseMoveEvent,
             QtCore.QEvent.MouseButtonRelease: view.mouseReleaseEvent}[kind](event)
        drag(QtCore.QEvent.MouseButtonPress, start, right, right)
        drag(QtCore.QEvent.MouseMove, start + QtCore.QPointF(60, 20), QtCore.Qt.NoButton, right)
        self.assertGreater(view.transform().m11(), zoom * 1.3)
        self.assertLess((view.mapFromScene(anchor) - start.toPoint()).manhattanLength(), 3)
        drag(QtCore.QEvent.MouseMove, start - QtCore.QPointF(60, 20), QtCore.Qt.NoButton, right)
        self.assertLess(view.transform().m11(), zoom * 0.8)
        drag(QtCore.QEvent.MouseButtonRelease, start, right, QtCore.Qt.NoButton)
        menu = QtGui.QContextMenuEvent(QtGui.QContextMenuEvent.Mouse, start.toPoint(), view.mapToGlobal(start.toPoint()))
        view.contextMenuEvent(menu)
        self.assertTrue(menu.isAccepted())
        self.assertFalse(QtWidgets.QApplication.activePopupWidget())
        view.frame_all()
        image = self.editor.grab()
        self.assertGreater(image.width(), 100)
        out = os.environ.get("GROOMLAB_EDITOR_SNAPSHOT")
        if out:
            image.save(out)

    def test_a_deleted_editor_stops_following_the_time(self):
        import shiboken2
        editor = self.ge.GraphEditor(self.groom)
        job = editor._time_job
        self.assertTrue(job is not None and cmds.scriptJob(exists=job))
        shiboken2.delete(editor)  # gone without a closeEvent, as a dock deleted with its workspace
        editor.status_soon()  # what the time job calls on the next frame: no touching the dead widget
        wait(50)
        self.assertIsNone(editor._time_job)
        self.assertFalse(cmds.scriptJob(exists=job))


class GuidesTabTest(unittest.TestCase):
    """The Groom Tools' Guides tab: where the group names in the graph come from."""

    def setUp(self):
        from groomlab.ui import palette
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        self.groom = groomlab.create_groom(scalp)
        curve = cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)])
        groomlab.guides_from_curves(self.groom, [curve])
        cmds.select(self.groom)
        self.tools = palette.ToolPalette()
        self.tools.show()
        self.tools.poll()
        self.tab = self.tools.guides_tab

    def tearDown(self):
        self.tools.close()
        self.tools.deleteLater()

    def events(self):
        from PySide2 import QtWidgets
        for _ in range(5):
            QtWidgets.QApplication.processEvents()

    def test_the_segments_slider_previews_live_and_applies_one_step(self):
        # Nothing selected: the slider shows every guide's segments and resamples them all; dragging
        # previews (the stored guides untouched, no undo step), releasing applies as one step.
        curve = cmds.curve(degree=1, point=[(0.3, 0.95, 0), (0.3, 1.6, 0.3), (0.6, 1.9, 0.6)])
        groomlab.guides_from_curves(self.groom, [curve])
        cmds.select(self.groom)  # the palette follows Maya's selection: keep it on the groom
        groomlab.clear_guide_selection(self.groom)
        groomlab.resample_guides(self.groom, 12)
        self.events()
        tab = self.tab
        self.assertEqual(tab.segments.value(), 12)
        self.assertIn("all 2", tab.segments_label.text())
        stored = cmds.getAttr(self.groom + ".guideData")
        settle_undo()
        undo_before = cmds.undoInfo(query=True, undoName=True)
        for value in (20, 31, 40):
            tab.segments_dragged(value)
            wait(60)
        self.assertTrue(tab._previewing, tab.owner.status.text())
        self.assertEqual(cmds.getAttr(self.groom + ".guideData"), stored)  # a preview only
        self.assertEqual(cmds.undoInfo(query=True, undoName=True), undo_before)
        tab.segments_committed(40)
        self.events()
        self.assertFalse(tab._previewing)
        self.assertEqual(groomlab.guide_segments(self.groom), [40, 40])
        self.assertEqual(tab.segments.value(), 40)
        cmds.undo()  # the whole drag: one step
        self.assertEqual(groomlab.guide_segments(self.groom), [12, 12])
        # A selected guide: its own count, and only it is resampled.
        first = groomlab.guide_ids(self.groom, "primary")[0]
        groomlab.select_guides(self.groom, guides=[first])
        groomlab.resample_guides(self.groom, 7)
        self.events()
        self.assertEqual(tab.segments.value(), 7)
        self.assertIn("1 selected", tab.segments_label.text())
        self.assertEqual(sorted(groomlab.guide_segments(self.groom, group="primary")), [7, 12])
        self.assertEqual(groomlab.errors(self.groom), [])

    def test_children_count_is_inline(self):
        # Edit Guides > Children takes its count from the spin box beside it: no prompt.
        from groomlab.ui import menu
        tab = self.tab
        tab.children_count.setValue(2)
        self.assertEqual(menu.CHILDREN_COUNT, 2)
        prompt = cmds.promptDialog

        def no_dialog(*_, **__):
            raise AssertionError("a modal dialog opened")
        cmds.promptDialog = no_dialog
        try:
            tab.generate_children()
        finally:
            cmds.promptDialog = prompt
            tab.children_count.setValue(5)
        groups = [g["name"] for g in groomlab.list_guide_groups(self.groom)]
        self.assertEqual(len(groups), 2)
        children = next(g for g in groups if g != "primary")
        self.assertEqual(len(groomlab.guide_ids(self.groom, children)), 2)  # one guide, two children

    def test_groups_show_where_graph_names_come_from(self):
        from PySide2 import QtCore
        groomlab.generate_children(self.groom, "primary", name="kids", count=2)
        path = cmds.curve(degree=1, point=[(0.2, 0.98, 0), (0.2, 0.5, -1.0)])
        groomlab.add_braid(self.groom, [path])
        self.events()  # the tab follows guide and graph changes
        tree = self.tab.tree
        top = {tree.topLevelItem(i).text(0): tree.topLevelItem(i) for i in range(tree.topLevelItemCount())}
        self.assertEqual(set(top), {"primary", "braids"})
        self.assertEqual(top["primary"].child(0).text(0), "kids")  # child groups nest under their parents
        self.assertEqual(top["primary"].child(0).text(3), "1")  # level
        self.assertIn("guides (exclude)", top["braids"].toolTip(0))  # the graph nodes naming it
        self.assertIn("braidPaths (group)", top["braids"].toolTip(0))
        groomlab.rename_guide_group(self.groom, "braids", "rows")  # the graph follows
        nodes = {n["name"]: n for n in groomlab.get_graph(self.groom)["nodes"]}
        self.assertEqual(nodes["braidPaths"]["params"]["group"], "rows")
        self.assertEqual(nodes["guides"]["params"]["exclude"], "rows")
        self.assertEqual(groomlab.errors(self.groom), [])
        self.tab.refresh()
        tree.setCurrentItem(tree.findItems("kids", QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)[0])
        self.tab.select_group()
        self.assertEqual(len(groomlab.selected_guides(self.groom)), 2)
        # Each group's swatch in the color it draws in (its automatic one, dashed: none of its own).
        self.assertEqual(cmds.getAttr(self.groom + ".guideColorMode"), 0)
        automatic = dict((g["name"], g["color"]) for g in groomlab.list_guide_groups(self.groom))
        kids = tree.findItems("kids", QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)[0]
        self.assertIn(automatic["kids"], tree.itemWidget(kids, 1).styleSheet())
        self.assertIn("dashed", tree.itemWidget(kids, 1).styleSheet())
        self.assertNotEqual(automatic["kids"], automatic["primary"])
        # A group's own color, from its swatch; renaming keeps it; clearing goes back.
        self.tab.set_color("kids", "#ff8800")
        self.assertEqual(groomlab.guide_group_colors(self.groom), {"kids": "#ff8800"})
        cmds.refresh(force=True)
        self.assertEqual(groomlab.errors(self.groom), [])
        self.events()
        kids = tree.findItems("kids", QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)[0]
        self.assertIn("#ff8800", tree.itemWidget(kids, 1).styleSheet())
        kids.setText(0, "fringe")  # renamed in place (double-click)
        self.events()
        self.assertIn("fringe", [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertEqual(groomlab.guide_group_colors(self.groom), {"fringe": "#ff8800"})
        groomlab.set_guide_group_color(self.groom, "fringe", None)
        self.assertEqual(groomlab.guide_group_colors(self.groom), {})

    def test_guides_nodes_pick_groups_in_the_graph_editor(self):
        from groomlab.ui import graph_editor
        from PySide2 import QtWidgets
        editor = graph_editor.GraphEditor(self.groom)
        try:
            groomlab.generate_children(self.groom, "primary", name="kids", count=2)
            picked = []
            for param in ("group", "exclude"):  # both: type names, or pick them with +
                w = editor.params.plain_editor("guides", editor.model.param_spec("guides", param), "",
                                               on_change=picked.append)
                options = [a.text() for a in w.menu.actions()]
                self.assertEqual(options, ["primary", "kids"], param)
                self.assertIn("primary", w.toolTip())
            w.menu.actions()[0].trigger()
            w.menu.actions()[1].trigger()  # several groups
            self.assertEqual(picked[-1], "primary kids")
            self.assertEqual(w.line.text(), "primary kids")
            # A fixed set of values is a dropdown; names that exist are offered.
            braid = editor.model.add_node("Braid")
            box = editor.params.plain_editor(braid, editor.model.param_spec(braid, "style"), "three")
            self.assertIsInstance(box, QtWidgets.QComboBox)
            self.assertFalse(box.isEditable())
            self.assertIn("cornrow", [box.itemText(i) for i in range(box.count())])
            twist = editor.model.add_node("Twist")
            # An attribute it reads: any name typed, and only what reaches it offered (nothing yet).
            holder = editor.params.plain_editor(twist, editor.model.param_spec(twist, "clumpChannel"), "clumpId")
            self.assertTrue(holder.field.isEditable())
            self.assertEqual(holder.field.count(), 0)
            self.assertEqual(holder.field.currentText(), "clumpId")
        finally:
            editor.deleteLater()

    def test_hidden_groups_are_not_drawn_or_brushed(self):
        from PySide2 import QtCore
        self.tab.refresh()
        item = self.tab.tree.findItems("primary", QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)[0]
        self.assertEqual(item.checkState(0), QtCore.Qt.Checked)
        item.setCheckState(0, QtCore.Qt.Unchecked)  # the eye: hide it
        self.assertEqual(groomlab.hidden_guide_groups(self.groom), ["primary"])
        cmds.refresh(force=True)
        guides = cmds.groomLabGuides(self.groom, listGuides="primary")
        points = lambda: [cmds.groomLabGuides(self.groom, guidePoints=g) for g in guides]
        before = points()
        groomlab.brush_stroke(self.groom, "comb", [(0, 1.5, 0), (0.8, 1.5, 0)], radius=3.0, level=-1)
        self.assertEqual(points(), before)  # hidden: left alone
        groomlab.rename_guide_group(self.groom, "primary", "main")  # stays hidden under its new name
        self.assertEqual(groomlab.hidden_guide_groups(self.groom), ["main"])
        groomlab.show_guide_group(self.groom, "main")
        self.assertEqual(groomlab.hidden_guide_groups(self.groom), [])
        groomlab.brush_stroke(self.groom, "comb", [(0, 1.5, 0), (0.8, 1.5, 0)], radius=3.0, level=-1)
        self.assertNotEqual(points(), before)

    def test_new_groups_take_new_guides_and_name_guides_nodes(self):
        from groomlab.ui import palette
        from PySide2 import QtCore
        self.assertEqual(palette.TABS[0], "tools")  # groups live on the Tools tab
        self.assertEqual(self.tools.current_group(), "primary")  # the first group to start
        groomlab.add_guide_group(self.groom, "bangs")
        with self.assertRaises(RuntimeError):
            groomlab.add_guide_group(self.groom, "bangs")  # names are unique
        groomlab.add_guide_group(self.groom, "flyaways", parent="bangs")
        self.tools.fill_levels(self.groom)
        labels = [self.tools.level.itemText(i) for i in range(self.tools.level.count())]
        self.assertEqual(labels[0], "All groups")
        self.assertIn("bangs", labels)
        self.assertIn("    flyaways", labels)  # children indent under their parent
        self.assertNotIn("New Group...", labels)  # made from the groups list, not the brush's group choice
        self.tools.pick_group("bangs")
        self.assertEqual(self.tools.current_group(), "bangs")
        self.tools.buttons["place"].click()  # the brush takes the group
        self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, group=True), "bangs")
        groomlab.brush_stroke(self.groom, "place", [(0.3, 0.95, 0.0)], radius=0.1, level=0, group="bangs")
        self.assertEqual(len(cmds.groomLabGuides(self.groom, listGuides="bangs")), 1)
        self.assertEqual(len(cmds.groomLabGuides(self.groom, listGuides="primary")), 1)
        self.tab.refresh()  # the groups list picks the target too
        item = self.tab.tree.findItems("primary", QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)[0]
        self.tab.tree.itemClicked.emit(item, 0)
        self.assertEqual(self.tools.current_group(), "primary")
        from groomlab import graph as gm  # a Guides node reads the group by name (made in the Graph Editor)
        graph = gm.GraphModel.from_groom(self.groom)
        graph.add_node("Guides", name="bangsGuides", params={"group": "bangs"})
        graph.save(self.groom)
        self.assertIn(("bangsGuides", "group"), groomlab.group_users(self.groom, "bangs"))
        self.tools.show_tab("guides")
        self.assertEqual(self.tools.tabs.currentIndex(), palette.TABS.index("tools"))
        self.assertTrue(self.tools.guides_section.expanded())
        self.assertTrue(self.tools.guides_section.isVisibleTo(self.tools))
        self.tools.show_tab("paint")
        self.assertFalse(self.tools.guides_section.isVisibleTo(self.tools))


class ExpressionPanelTest(unittest.TestCase):
    """An Expression node in the Graph Editor: a code block, its variables and the library."""

    def setUp(self):
        from groomlab.ui import graph_editor
        cmds.file(new=True, force=True)
        self.groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=12)[0])
        self.editor = graph_editor.GraphEditor(self.groom)
        self.node = self.editor.add_node("Expression")
        self.editor.scene.clearSelection()
        self.editor.items[self.node].setSelected(True)
        self.panel = self.editor.params.expression

    def tearDown(self):
        self.editor.deleteLater()

    def test_code_variables_and_library(self):
        import json
        from groomlab.ui import expression_editor
        self.assertIsInstance(self.panel, expression_editor.ExpressionPanel)
        self.assertEqual(self.panel.code.text(), "$length / $longest")
        offered = [t for t, _ in self.panel.code.entries]
        self.assertIn("$length", offered)
        self.assertIn("smoothstep(", offered)
        # Typing commits after a pause (or on leaving the field): the graph has it.
        self.panel.code.setPlainText("$l = $length / $longest;\nclamp($l * 2, 0, 1)")
        wait(900)
        self.assertEqual(self.editor.model.param(self.node, "expression"), "$l = $length / $longest;\nclamp($l * 2, 0, 1)")
        # Its own variables: a number and a vector, offered as you type, stored as JSON.
        self.panel.variables.add("number")
        self.panel.variables.add("vector")
        stored = json.loads(self.editor.model.param(self.node, "variables"))
        self.assertEqual(stored, {"amount": 0.5, "point": [0.0, 0.0, 0.0]})
        offered = [t for t, _ in self.panel.code.entries]
        self.assertEqual(offered[:4], ["$amount", "$point.x", "$point.y", "$point.z"])
        self.panel.variables.set_value("amount", 0.8)
        self.assertEqual(json.loads(self.editor.model.param(self.node, "variables"))["amount"], 0.8)
        self.panel.variables.rename("amount", "gain")
        self.assertIn("gain", json.loads(self.editor.model.param(self.node, "variables")))
        # A mistake shows under the code as you type, and is not written to the node until you leave it.
        before = self.editor.model.param(self.node, "expression")
        self.panel.code.setPlainText("$gain +")
        self.panel.code.paused()
        self.assertEqual(self.editor.model.param(self.node, "expression"), before)
        self.assertIn("unexpected end", self.panel.error.text())
        self.panel.code.commit()
        wait(50)
        self.assertTrue(self.panel.error.text(), "no error shown")
        # The library: an example loads text, variables and range.
        from groomlab import expressions
        example = expressions.find_expression("Near a point")
        # The node's own code is replaced only once confirmed (no: nothing changes).
        asked = []
        self.panel.ask = lambda question: asked.append(question) or False
        self.assertFalse(self.panel.load(example))
        self.assertEqual(len(asked), 1)
        self.panel.ask = lambda question: asked.append(question) or True
        def stored():
            return [n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == self.node][0]["params"]
        self.editor.apply()
        before = stored()
        self.assertTrue(self.panel.load(example))
        wait(50)
        self.assertEqual(self.editor.model.param(self.node, "expression"), example["expression"])
        self.assertEqual(json.loads(self.editor.model.param(self.node, "variables")), example["variables"])
        # One undo step takes the whole entry back.
        self.assertEqual(stored().get("expression"), example["expression"])
        cmds.undo()
        self.assertEqual(stored(), before)
        cmds.redo()
        self.editor.reload()
        self.editor.items[self.node].setSelected(True)
        wait(50)
        self.panel = self.editor.params.expression  # the panel is rebuilt to show the new min and max
        # Saving: one form (name and what it does); an existing name is replaced only once confirmed.
        import tempfile
        before_dir = os.environ.get("GROOMLAB_USER_EXPRESSIONS")
        os.environ["GROOMLAB_USER_EXPRESSIONS"] = tempfile.mkdtemp()
        self.panel.ask = lambda question: False
        try:
            self.assertTrue(self.panel.save("batch3 probe", "a test"))
            self.assertIsNone(self.panel.save("batch3 probe", "again"))  # not replaced: asked, said no
        finally:
            if before_dir is None:
                os.environ.pop("GROOMLAB_USER_EXPRESSIONS", None)
            else:
                os.environ["GROOMLAB_USER_EXPRESSIONS"] = before_dir
        self.panel.fill_library()
        names = [a.text() for a in self.panel.library_menu.actions()]
        self.assertIn("Stripes around the scalp", names)
        self.assertIn("Save to Library...", names)


class LiveVariablesTest(unittest.TestCase):
    """Expression variables in the Graph Editor: dragged live and made keyable."""

    def setUp(self):
        from groomlab.ui import graph_editor
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=12)[0])
        self.editor = graph_editor.GraphEditor(self.groom)

    def tearDown(self):
        self.editor.deleteLater()

    def test_variables_drag_live_and_become_keyable(self):
        import json as _json
        node = self.editor.add_node("Expression")
        self.editor.scene.clearSelection()
        self.editor.items[node].setSelected(True)
        panel = self.editor.params.expression
        panel.variables.add("number")  # $amount, 0.5
        steps = len(cmds.undoInfo(query=True, printQueue=True) or [])
        panel.variables.drag_value("amount", 0.9)  # a drag: on the groom at once, outside undo
        wait(120)
        stored = _json.loads(next(n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == node)["params"]["variables"])
        self.assertEqual(stored["amount"], 0.9)
        self.assertEqual(len(cmds.undoInfo(query=True, printQueue=True) or []), steps)
        panel.variables.set_value("amount", 0.8)  # released: one step
        # Keyable: an attribute on the groom the slider now sets.
        panel.variables.set_keyable("amount", True)
        wait(50)
        control = _json.loads(self.editor.model.param(node, "variables"))["amount"]["control"]
        self.assertTrue(cmds.attributeQuery("ctl_" + control, node=self.groom, exists=True))
        self.assertAlmostEqual(cmds.getAttr(self.groom + ".ctl_" + control), 0.8, places=5)
        panel = self.editor.params.expression
        panel.variables.set_value("amount", 0.3)
        self.assertAlmostEqual(cmds.getAttr(self.groom + ".ctl_" + control), 0.3, places=5)
        panel.variables.set_keyable("amount", False)  # back to a plain number, at its value
        wait(50)
        self.assertEqual(_json.loads(self.editor.model.param(node, "variables"))["amount"], 0.3)


class CachedQueriesTest(unittest.TestCase):
    """groomLabGuides -listGroups and groomLabScalpMaps -list/-kindOf/-channelsOf answer from the
    shape's parses of guideData and scalpMaps (kept per change): the same answers as the stored
    text gives, after edits, undo and a text that does not parse."""

    @staticmethod
    def rows_from_json(groom):
        data = json.loads(cmds.getAttr(groom + ".guideData") or "{}")
        groups = data.get("groups", [])
        by_id = dict((g["id"], g) for g in groups)

        def depth(g):
            d, seen = 0, set()
            while g.get("parent") in by_id and g["id"] not in seen:
                seen.add(g["id"])
                g = by_id[g["parent"]]
                d += 1
            return d
        rows = []
        for g in groups:
            parent = by_id.get(g.get("parent"))
            count = sum(1 for guide in data.get("guides", []) if guide.get("group") == g["id"])
            rows.append("%s|%s|%d|%d" % (g["name"], parent["name"] if parent else "", depth(g), count))
        return rows

    def test_answers_follow_the_stored_text(self):
        cmds.file(new=True, force=True)
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0])
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)])])
        # (Each row ends in the color the group draws in: checked below, not in the stored text.)
        listed = lambda: ["|".join(r.split("|")[:4]) for r in cmds.groomLabGuides(groom, listGroups=True) or []]
        self.assertEqual(listed(), self.rows_from_json(groom))
        groomlab.add_guide_group(groom, "kids", "primary")
        self.assertEqual(listed(), self.rows_from_json(groom))
        self.assertIn("kids|primary|1|0", listed())
        colors = dict((g["name"], g["color"]) for g in groomlab.list_guide_groups(groom))
        self.assertTrue(all(len(c) == 7 and c.startswith("#") for c in colors.values()), colors)
        self.assertNotEqual(colors["kids"], colors["primary"])  # a child group its own color
        data = json.loads(cmds.getAttr(groom + ".guideData"))
        data["groups"][0]["name"] = "renamed"
        cmds.setAttr(groom + ".guideData", json.dumps(data), type="string")
        self.assertEqual(listed(), self.rows_from_json(groom))
        self.assertTrue(listed()[0].startswith("renamed|"))
        cmds.undo()
        self.assertEqual(listed(), self.rows_from_json(groom))
        self.assertTrue(listed()[0].startswith("primary|"))
        good = cmds.getAttr(groom + ".guideData")
        cmds.setAttr(groom + ".guideData", "{not json", type="string")
        with self.assertRaises(RuntimeError):
            cmds.groomLabGuides(groom, listGroups=True)
        cmds.setAttr(groom + ".guideData", good, type="string")
        self.assertEqual(listed(), self.rows_from_json(groom))

        # Maps: guide layers, vertex and texture maps, gray and color.
        groomlab.add_map(groom, "layer", 0.5)
        groomlab.add_map(groom, "verts", 0.2, kind="vertices")
        groomlab.add_map(groom, "tint", kind="vertices", color=(1.0, 0.0, 0.0, 1.0))
        groomlab.add_map(groom, "tex", 0.0, kind="texture", resolution=16)

        def expected():
            stored = json.loads(cmds.getAttr(groom + ".scalpMaps") or "{}")
            names = set()
            for guide in json.loads(cmds.getAttr(groom + ".guideData")).get("guides", []):
                names.update((guide.get("attributes") or {}).keys())
                names.update((guide.get("pointAttributes") or {}).keys())
            rows = ["%s|guides||1" % n for n in sorted(names) if n not in stored]
            rows += ["%s|%s|%s|%d" % (n, m.get("kind", "vertices"), m.get("texture", ""), m.get("channels", 1))
                     for n, m in sorted(stored.items())]
            return rows
        self.assertEqual(list(cmds.groomLabScalpMaps(groom, list=True)), expected())
        for name, kind, channels in (("layer", "guides", 1), ("verts", "vertices", 1), ("tint", "vertices", 4),
                                     ("tex", "texture", 1), ("none", "", 0)):
            self.assertEqual(cmds.groomLabScalpMaps(groom, kindOf=name) or "", kind)
            self.assertEqual(cmds.groomLabScalpMaps(groom, channelsOf=name), channels)
        groomlab.remove_map(groom, "verts")
        self.assertEqual(list(cmds.groomLabScalpMaps(groom, list=True)), expected())
        cmds.undo()
        self.assertEqual(list(cmds.groomLabScalpMaps(groom, list=True)), expected())
        self.assertEqual(cmds.groomLabScalpMaps(groom, kindOf="verts"), "vertices")
        cmds.setAttr(groom + ".scalpMaps", "{not json", type="string")
        with self.assertRaises(RuntimeError):
            cmds.groomLabScalpMaps(groom, list=True)
        cmds.undo()
        self.assertEqual(list(cmds.groomLabScalpMaps(groom, list=True)), expected())

    def test_evaluations_count(self):
        """groomLabGraphStats -evaluations moves when the groom evaluates, not when it is asked."""
        cmds.file(new=True, force=True)
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10)[0])
        first = cmds.groomLabGraphStats(groom, evaluations=True)
        self.assertEqual(cmds.groomLabGraphStats(groom, evaluations=True), first)
        cmds.setAttr(groom + ".previewDensity", 0.5)
        self.assertGreater(cmds.groomLabGraphStats(groom, evaluations=True), first)


class PaletteTickBudgetTest(unittest.TestCase):
    """What the Groom Tools' poll (every 400 ms while it shows) costs on a groom with guides and
    correctives, on the Sculpt tab (the correctives showing) and on the Tools tab (reported only)."""

    def test_idle_poll_cost(self):
        import time
        from gui_support import say
        from groomlab import shots
        from groomlab.ui import palette
        cmds.file(new=True, force=True)
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0])
        curves = [cmds.curve(degree=1, point=[(0.1 * k - 0.5, 1, 0), (0.1 * k - 0.5, 2, 0)]) for k in range(10)]
        groomlab.guides_from_curves(groom, curves)
        for _ in range(4):
            shots.add_layer(groom, all_frames=True)
        shots.set_target(groom, None)
        cmds.select(groom)
        tools = palette.ToolPalette()
        try:
            tools.show()
            tools.poll()
            costs = {}
            for tab in ("sculpt", "tools"):
                tools.show_tab(tab)
                wait(20)
                tools.poll()
                start = time.time()
                for _ in range(60):
                    tools.poll()
                costs[tab] = (time.time() - start) * 1000.0 / 60
            say("[ budget ] Groom Tools idle poll: %.2f ms a tick on the Sculpt tab, %.2f ms on the Tools tab "
                "(reported only)" % (costs["sculpt"], costs["tools"]))
            self.assertEqual(len(shots.layers(groom)), 4)
        finally:
            tools.close()
            tools.deleteLater()


class StarterCardTest(unittest.TestCase):
    """The Groom Tools' first-groom checklist ticks steps off as the groom gets there."""

    def test_steps_follow_the_groom(self):
        from groomlab.ui import palette
        cmds.file(new=True, force=True)
        cmds.optionVar(remove=palette.STARTER_VAR)
        groom = groomlab.create_groom(cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0])
        self.assertEqual(palette.starter_progress(groom), {"guides": False, "look": False})
        groomlab.guides_from_curves(groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)])])
        done = palette.starter_progress(groom)
        self.assertTrue(done["guides"])
        cmds.select(groom)
        tools = palette.ToolPalette()
        try:
            tools.show()
            tools.poll()
            card = tools.starter
            self.assertEqual(card.isVisibleTo(tools), not all(done.values()))
            if not done["look"]:  # the next step: marked, with how to do it
                self.assertIn("Graph Editor", card.tip.text())
                self.assertTrue(card.rows["look"][0].text().endswith("Give it a look"))
                self.assertTrue(card.rows["guides"][0].text().startswith(chr(0x2713)))
            card.dismiss()  # hidden for good
            self.assertFalse(card.isVisibleTo(tools))
            card.refresh(groom)
            self.assertFalse(card.isVisibleTo(tools))
        finally:
            cmds.optionVar(remove=palette.STARTER_VAR)
            tools.close()
            tools.deleteLater()


class TransferWindowTest(unittest.TestCase):
    """Utilities > Transfer Groom to Scalp...: the Wrap-style window (never modal). Two embedded Maya
    views, each with its own camera and isolated to its mesh; marker points placed through the
    window's script API (the clicks call the same functions after picking), numbered by pair with
    unmatched points drawn dim, deletes that take the partner along, undo and redo, symmetry,
    storage on the groom, the ray pick through a known camera, and the transfer."""

    def setUp(self):
        cmds.file(new=True, force=True)
        self.scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20, name="head")[0]
        self.groom = groomlab.create_groom(self.scalp)
        groomlab.guides_from_curves(self.groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)])])
        self.head = cmds.duplicate(self.scalp, name="newHead")[0]
        cmds.xform(self.head, rotation=(0, 90, 0), translation=(5, 0, 0), worldSpace=True)
        self.box = cmds.polyCube(name="box")[0]
        cmds.xform(self.box, translation=(-5, 0, 0), worldSpace=True)

    def vertex(self, mesh, i):
        return cmds.xform("%s.vtx[%d]" % (mesh, i), query=True, translation=True, worldSpace=True)

    def assertClose(self, a, b, delta=1e-3):
        self.assertLess(max(abs(x - y) for x, y in zip(a, b)), delta, "%s != %s" % (a, b))

    def assert_markers(self, window):
        """Each side's marker set holds the model's points, matched flags and hover."""
        for side in ("old", "new"):
            state = window.marker_state(side)
            points = window.points(side)
            self.assertEqual(len(state["points"]), len(points), side)
            for got, want in zip(state["points"], points):
                self.assertClose(got, want, 1e-4)
            self.assertEqual(state["matched"], [1 if window.editor.matched(side, i) else 0 for i in range(len(points))])
            self.assertEqual(state["hover"], window._hover_index(side))

    def colored_near(self, window, side, center, radius):
        """How many pixels of the side's view (its color buffer, after a refresh) are its marker
        color on the ring of `radius` around port pixel `center`, and within 2 pixels of it."""
        import tempfile
        from maya.api import OpenMaya as om
        from PySide2 import QtGui
        from groomlab.ui import transfer_window
        view = window.view(side)
        view.refresh(False, True)
        image = om.MImage()
        view.readColorBuffer(image, True)
        path = os.path.join(tempfile.gettempdir(), "groomlab_transfer_buffer_%s.png" % side)
        image.writeToFile(path, "png")  # kept: what the failures name
        buffer = QtGui.QImage(path)
        want = [int(c * 255) for c in transfer_window.COLORS[side]]

        def colored(x, y):
            # The buffer may be stored either way up: the middle of the port is the same both ways.
            c = buffer.pixelColor(int(round(x)), buffer.height() - 1 - int(round(y)))
            return all(abs(a - b) < 60 for a, b in zip((c.red(), c.green(), c.blue()), want))

        import math
        ring = sum(colored(center[0] + radius * math.cos(a * math.pi / 8), center[1] + radius * math.sin(a * math.pi / 8))
                   for a in range(16))
        middle = sum(colored(center[0] + dx, center[1] + dy) for dx in (-2, 0, 2) for dy in (-2, 0, 2))
        return ring, middle

    def test_marker_views_points_and_a_transfer(self):
        from groomlab.ui import menu, transfer_window
        from groomlab import transfer
        from PySide2 import QtCore
        long = lambda n: cmds.ls(n, long=True)[0]
        window = menu.transfer_dialog(groom=self.groom)  # no choices given: the window, not a modal dialog
        try:
            wait(50)
            self.assertIs(window, transfer_window.window())
            # Two views, each a modelEditor with its own hidden camera, isolated to its mesh.
            self.assertEqual(sorted(window.editors), ["new", "old"])
            for side in ("old", "new"):
                editor = window.editors[side]
                self.assertTrue(cmds.modelEditor(editor, exists=True))
                camera = cmds.modelEditor(editor, query=True, camera=True)
                self.assertIn(window.cameras[side].split("|")[-1], camera)
                self.assertTrue(cmds.modelEditor(editor, query=True, viewSelected=True))
                self.assertFalse(cmds.modelEditor(editor, query=True, pluginShapes=True))  # no strands drawn
                self.assertTrue(cmds.modelEditor(editor, query=True, polymeshes=True))
                self.assertTrue(window.cameras[side].startswith("|" + transfer_window.UI_GROUP + "|"))
            window.set_new_scalp(self.head)
            self.assertIn(long(self.scalp), window.isolated("old"))
            self.assertNotIn(long(self.head), window.isolated("old"))
            self.assertIn(long(self.head), window.isolated("new"))
            self.assertNotIn(long(self.scalp), window.isolated("new"))
            # The dropdown reloads the right view and keeps the left one.
            window.set_new_scalp(self.box)
            self.assertEqual(window.isolated("new"), [long(self.box)])
            self.assertIn(long(self.scalp), window.isolated("old"))
            self.assertIn("box", window.view_titles["new"].text())
            window.set_new_scalp(self.head)

            # Closest point: no marker editing, Transfer ready.
            window.set_method("closest")
            self.assertFalse(window.marker_editing())
            self.assertFalse(window.symmetry.isEnabled())
            self.assertTrue(window.transfer_button.isEnabled())
            self.assertFalse(cmds.objExists(transfer.MARKER_GROUP))
            window.set_method("markersAlign")
            self.assertFalse(window.transfer_button.isEnabled())
            self.assertIn("3 or more pairs", window.status.text())
            self.assertIn("3 or more pairs", window.transfer_button.toolTip())

            # Three on the left first, then two on the right: pairs by index, in any order.
            olds = [self.vertex(self.scalp, i) for i in (45, 170, 333)]
            news = [self.vertex(self.head, i) for i in (45, 170, 333)]
            for p in olds:
                window.add_point("old", p)
            self.assertEqual(window.pending(), [("old", 0), ("old", 1), ("old", 2)])
            window.add_point("right", news[0])
            window.add_point("new", news[1])
            self.assertEqual(len(window.pairs()), 2)
            self.assertEqual(window.pending(), [("old", 2)])
            self.assertIn("2 pairs, 1 unmatched", window.status.text())
            self.assertClose(window.pairs()[1]["new"]["point"], news[1])
            # One marker set per side (the plugin's locator: filled dots matched, rings pending,
            # numbered), each in its own view only, under the window's hidden group; no locators or
            # annotations.
            self.assertEqual(sorted(window.marker_sets), ["new", "old"])
            for side in ("old", "new"):
                node = window.marker_sets[side]
                self.assertEqual(cmds.nodeType(node), "groomLabMarkerSet")
                self.assertTrue(node.startswith("|" + transfer_window.UI_GROUP + "|"), node)
                self.assertClose(cmds.getAttr(node + ".color")[0], transfer_window.COLORS[side])
                self.assertIn(window.cameras[side].split("|")[-1], cmds.getAttr(node + ".camera"))
            self.assertEqual(window.marker_state("old")["matched"], [1, 1, 0])
            self.assertEqual(window.marker_state("new")["matched"], [1, 1])
            self.assert_markers(window)
            self.assertTrue(any("groomLabTransferOldMarkers" in o for o in window.isolated("old")))
            self.assertFalse(any("groomLabTransferOldMarkers" in o for o in window.isolated("new")))
            self.assertTrue(any("groomLabTransferNewMarkers" in o for o in window.isolated("new")))
            self.assertEqual([n for n in cmds.ls(type="locator") or [] if cmds.nodeType(n) == "locator"], [])
            self.assertEqual(cmds.ls(type="annotationShape"), [])
            self.assertFalse(cmds.objExists(transfer.MARKER_GROUP))
            # Only complete pairs are stored on the groom, snapped onto both meshes.
            stored = transfer.get_transfer_markers(self.groom)
            self.assertEqual(len(stored), 2)
            self.assertGreaterEqual(stored[0]["new"]["face"], 0)

            # Pair 2 deleted from the right: the left's 2 goes too and its 3 becomes 2.
            self.assertEqual(window.delete_point("new", 1), [1])
            self.assertClose(window.points("old")[1], olds[2])
            self.assertEqual(len(window.pairs()), 1)
            self.assertEqual(window.pending(), [("old", 1)])
            self.assertEqual(len(transfer.get_transfer_markers(self.groom)), 1)
            self.assert_markers(window)
            self.assertEqual(window.marker_state("old")["matched"], [1, 0])
            # An unmatched point deletes alone.
            window.delete_point("old", 1)
            self.assertEqual((len(window.points("old")), len(window.points("new"))), (1, 1))
            # Undo and redo: the window's own stack, the groom's markers following.
            self.assertTrue(window.undo())
            self.assertEqual(window.pending(), [("old", 1)])
            self.assertTrue(window.undo())
            self.assertEqual(len(window.pairs()), 2)
            self.assertEqual(len(transfer.get_transfer_markers(self.groom)), 2)
            self.assert_markers(window)
            self.assertTrue(window.redo())
            self.assert_markers(window)
            self.assertEqual(len(window.pairs()), 1)
            # Ctrl+Z in the window (Qt's key events, not Maya's undo).
            press(window.undo_button, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
            self.assertEqual(len(window.pairs()), 2)
            press(window.undo_button, QtCore.Qt.Key_Y, QtCore.Qt.ControlModifier)
            self.assertEqual(len(window.pairs()), 1)
            window.undo()
            window.add_point("new", news[2])
            self.assertEqual(window.pending(), [])
            self.assertTrue(window.transfer_button.isEnabled())
            self.assertIn("3 pairs", window.status.text())

            # Symmetry X: the mirror is the next point on that side; deleting one deletes both.
            window.symmetry.setChecked(True)
            near = [0.5, 0.6, 0.62]
            self.assertEqual(window.add_point("old", near), 3)
            a, b = window.points("old")[3], window.points("old")[4]
            self.assertClose([-b[0], b[1], b[2]], a, 0.05)
            self.assertEqual(window.pending(), [("old", 3), ("old", 4)])
            # A pending point hovered: its ring lit, no partner to light.
            window._set_hover(("old", 4))
            self.assertEqual((window.marker_state("old")["hover"], window.marker_state("new")["hover"]), (4, -1))
            window._set_hover(None)
            self.assertEqual(window.delete_point("old", 4), [3, 4])
            self.assertEqual(len(window.points("old")), 3)
            window.undo()
            self.assertEqual(len(window.points("old")), 5)
            window.undo()
            self.assertEqual(len(window.points("old")), 3)
            window.symmetry.setChecked(False)

            # Hover lights the point and its partner.
            window._set_hover(("old", 1))
            self.assertEqual((window.marker_state("old")["hover"], window.marker_state("new")["hover"]), (1, 1))
            self.assert_markers(window)
            window._set_hover(None)
            self.assertEqual((window.marker_state("old")["hover"], window.marker_state("new")["hover"]), (-1, -1))

            # The pick: a known camera on the left view, the ray through the port's middle.
            cam = window.cameras["old"]
            cmds.xform(cam, translation=(0, 0, 6), rotation=(0, 0, 0), worldSpace=True)
            cmds.refresh(force=True)
            view = window.view("old")
            hit = window.pick("old", view.portWidth() / 2.0, view.portHeight() / 2.0)
            self.assertIsNotNone(hit)
            self.assertClose(hit["point"], (0, 0, 1), 0.02)
            self.assertGreaterEqual(hit["face"], 0)
            self.assertTrue(-1e-4 <= hit["u"] <= 1.0001 and -1e-4 <= hit["v"] <= 1.0001, hit)
            origin, direction = transfer.view_ray(view, view.portWidth() / 2.0, view.portHeight() / 2.0)
            self.assertClose(direction, (0, 0, -1), 0.01)
            self.assertIsNone(window.pick("old", 1, 1))  # the corner misses the sphere
            # A point's projection is found near the cursor (Ctrl+click and drags grab it).
            window.add_point("old", (0, 0, 1))
            self.assertEqual(window.point_near("old", view.portWidth() / 2.0 + 3, view.portHeight() / 2.0), 3)
            self.assertIsNone(window.point_near("old", 5, 5))
            # Drawn by the plugin where the point projects (the port's middle): pending, a ring in
            # the side's color around the mesh showing through; matched, a filled dot.
            center = (view.portWidth() / 2.0, view.portHeight() / 2.0)
            radius = cmds.getAttr(window.marker_sets["old"] + ".size")
            ring, middle = self.colored_near(window, "old", center, radius)
            self.assertGreater(ring, 0, "the pending point's ring is drawn (%s, radius %s at %s)" % (
                os.path.join(__import__("tempfile").gettempdir(), "groomlab_transfer_buffer_old.png"), radius, center))
            self.assertEqual(middle, 0, "a ring is hollow")
            window.add_point("new", news[0])
            ring, middle = self.colored_near(window, "old", center, radius)
            self.assertGreater(middle, 0, "the matched point's dot is filled")
            window.undo()
            window.undo()
            window.frame("old")

            # The picture: a few pairs, one unmatched, symmetry on.
            window.symmetry.setChecked(True)
            window.add_point("old", [0.45, 0.35, 0.82])
            window.symmetry.setChecked(False)
            window.add_point("new", window.points("new")[0])
            window.add_point("new", self.vertex(self.head, 290))
            # One pending, on the far side of the head from the left view's camera: faint.
            window.add_point("old", [-0.25, 0.35, -0.9])
            window.symmetry.setChecked(True)
            self.assertIn("5 pairs, 1 unmatched", window.status.text())
            window._set_hover(("new", 1))
            self.assertEqual((window.marker_state("old")["hover"], window.marker_state("new")["hover"]), (1, 1))
            self.assert_markers(window)
            shot = os.environ.get("GROOMLAB_TRANSFER_SHOT")
            if shot:
                wait(200)
                self.assertTrue(window.grab_image(shot))
            window._set_hover(None)
            for _ in range(4):
                window.undo()
            window.symmetry.setChecked(False)
            self.assertEqual(len(window.pairs()), 3)

            result = window.transfer()
            self.assertIsNotNone(result, window.message.text())
            self.assertEqual(result["method"], "markersAlign")
            guide = groomlab.get_hierarchy(self.groom)["guides"][0]["id"]
            xyz = cmds.groomLabGuides(self.groom, guidePoints=str(guide))
            self.assertAlmostEqual(xyz[0], 5.0, delta=0.05)  # the top of the turned copy, (5, 1, 0)
            self.assertAlmostEqual(xyz[1], 1.0, delta=0.05)
            self.assertIn(long(self.head), window.isolated("old"))  # the groom's scalp now
            # One Maya undo step takes the whole transfer back (the window's redraws after it are
            # outside the queue); one redo does it again.
            self.assertEqual(cmds.undoInfo(query=True, undoName=True), "groomLab transfer")
            cmds.undo()
            self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True),
                             cmds.listRelatives(self.scalp, shapes=True))
            xyz = cmds.groomLabGuides(self.groom, guidePoints=str(guide))
            self.assertClose(xyz[:3], (0, 1, 0), 0.02)
            # The window follows the undo: the old scalp on the left again, the new one picked on
            # the right, the markers as stored.
            wait(20)
            self.assertEqual(window.meshes["old"], long(self.scalp))
            self.assertEqual(window.scalp(), long(self.head))
            self.assertIn(long(self.scalp), window.isolated("old"))
            self.assertEqual(len(window.pairs()), 3)
            cmds.redo()
            self.assertEqual(cmds.listConnections(self.groom + ".inMesh", shapes=True),
                             cmds.listRelatives(self.head, shapes=True))
            xyz = cmds.groomLabGuides(self.groom, guidePoints=str(guide))
            self.assertAlmostEqual(xyz[0], 5.0, delta=0.05)
            wait(20)
            self.assertEqual(window.meshes["old"], long(self.head))  # and the redo
            self.assertIn(long(self.head), window.isolated("old"))
        finally:
            editors = dict(window.editors)
            window.close()
        wait(20)
        self.assertIsNone(transfer_window.window())
        self.assertFalse(cmds.objExists(transfer_window.UI_GROUP))  # the cameras and marker sets go with the window
        self.assertEqual(cmds.ls(type="groomLabMarkerSet"), [])
        for editor in editors.values():
            self.assertFalse(cmds.modelEditor(editor, exists=True))
            self.assertFalse(cmds.objExists(editor + "ViewSelectedSet"))

    def test_a_new_scene_closes_the_window(self):
        from groomlab.ui import transfer_window
        window = transfer_window.show(self.groom, scalp=self.head)
        wait(50)
        editors = dict(window.editors)
        self.assertEqual(len(editors), 2)
        cmds.file(new=True, force=True)
        wait(50)
        self.assertIsNone(transfer_window.window())
        for editor in editors.values():
            self.assertFalse(cmds.modelEditor(editor, exists=True))
        self.assertFalse(cmds.objExists(transfer_window.UI_GROUP))


def widgets_theme_menu(parent):
    from groomlab.ui import widgets
    return widgets.theme_menu(parent)


class RenderProgressTest(unittest.TestCase):
    """A standalone render's progress window (V-Ray, RenderMan): a tester saw it flash as a small
    white box until the image came."""

    def test_one_steady_window_over_maya_until_done(self):
        from PySide2 import QtWidgets
        from groomlab.ui import menu, widgets

        class FakeJob(object):
            frames = [1, 2]
            cancelled = False
            folder = log = ""

            def __init__(self):
                self.step = 0

            def poll(self):  # (written, frames, done, percent)
                self.step += 1
                return min(self.step // 4, 2), 2, self.step >= 9, (self.step % 4) * 25.0

            def cancel(self):
                self.cancelled = True

            def images(self):
                return []

            def ok(self):
                return False

            def errors(self):
                return ["fake"]

        before = set(QtWidgets.QApplication.topLevelWidgets())
        job = FakeJob()
        menu._watch_render(job, {"label": "Fake", "window": "GroomLab Fake", "title": "Render in Fake"})
        _, progress, timer = menu._RENDER_JOBS[-1]
        try:
            self.assertEqual(progress.parentWidget().objectName(), widgets._maya_main_window().objectName())  # over Maya
            self.assertTrue(progress.isVisible())
            width = progress.width()
            seen = []
            while job.step < 9:
                timer.timeout.emit()
                wait(5)
                seen.append((progress.isVisible(), progress.width()))
            self.assertTrue(all(v for v, _ in seen[:-1]), seen)  # shown the whole time, never hidden and shown again
            self.assertEqual({w for _, w in seen}, {width})  # a new label never resizes it
            new = [w for w in set(QtWidgets.QApplication.topLevelWidgets()) - before if w is not progress and w.isVisible()]
            self.assertEqual(new, [])
            self.assertFalse(progress.isVisible())  # done: closed
            self.assertNotIn((job, progress, timer), menu._RENDER_JOBS)
        finally:
            timer.stop()
            progress.close()


class DockableWindowsTest(unittest.TestCase):
    """The menu's entry points: Maya's dockable wrappers, as a user opens them."""

    def setUp(self):
        cmds.file(new=True, force=True)
        self.groom = groomlab.create_groom(cmds.polySphere()[0])
        groomlab.set_graph(self.groom, CLASSIC_GRAPH)

    def test_add_node_menu_works_in_the_docked_window(self):
        import gc
        from PySide2 import QtWidgets
        from groomlab.ui import graph_editor
        window = graph_editor.show(self.groom)
        try:
            gc.collect()  # an unowned menu would be gone now
            button = [b for b in window.findChildren(QtWidgets.QToolButton) if b.text().startswith("Add Node")][0]
            menu = button.menu()
            self.assertIsNotNone(menu)
            fields = [a.menu() for a in menu.actions() if a.text().replace("&&", "&") == "Fields & Curves"][0]
            [a for a in fields.actions() if a.text() == "Field Noise" and a.data() == "FieldNoise"][0].trigger()  # the label
            self.assertIn("FieldNoise", [window.model.node_type(n) for n in window.model.order])
            # The canvas menu places the node where you clicked.
            placed = window.build_add_menu(at=(1234.0, 567.0))
            [a for a in [a.menu() for a in placed.actions() if a.text() == "Look"][0].actions()
             if a.text() == "Curl"][0].trigger()
            curl = [n for n in window.model.order if window.model.node_type(n) == "Curl"][0]
            self.assertEqual(window.model.position(curl), (1234.0, 567.0))
        finally:
            window.close()

    def test_the_docked_graph_editor_links_ports(self):
        # Maya's dockable window: method names like connect reach QObject there.
        from groomlab.ui import graph_editor
        window = graph_editor.show(self.groom)
        try:
            window.unlink("interpolate", "guides")
            self.assertIsNone(window.model.source_of("interpolate", "guides"))
            window.link("guides", "interpolate", "guides")
            self.assertEqual(window.model.source_of("interpolate", "guides"), "guides")
        finally:
            window.close()

    def test_themes_restyle_open_windows(self):
        from groomlab.ui import graph_editor, palette, theme
        editor = graph_editor.GraphEditor(self.groom)
        tools = palette.ToolPalette()
        try:
            theme.set_theme("studio")
            self.assertEqual(graph_editor.HEADER_STYLE, "tinted")
            self.assertEqual(editor.scene.backgroundBrush().color().name().upper(), "#212224")
            self.assertIn("border-radius: %dpx" % theme.px(4), tools.styleSheet())
            theme.set_theme("playful")
            self.assertEqual(graph_editor.HEADER_STYLE, "filled")
            self.assertIn("border-radius: %dpx" % theme.px(12), editor.styleSheet())
            self.assertIn("font-size: %dpx" % theme.px(12), editor.styleSheet())  # text scales with Maya's UI
            menu = widgets_theme_menu(tools)
            menu.aboutToShow.emit()
            self.assertIn("Studio", [a.text() for a in menu.actions()])
            self.assertIn("New Theme from Current...", [a.text() for a in menu.actions()])
        finally:
            tools.deleteLater()
            editor.deleteLater()
            cmds.optionVar(remove=theme.THEME_VAR)

    def test_gpu_preview_preference(self):
        """The Graph Editor's Preview Drags on the GPU: checked by default, its check sets the
        preference (the optionVar the plugin reads at load) and follows it."""
        import groomlab
        from groomlab.ui import graph_editor
        was = groomlab.gpu_preview()
        stored = cmds.optionVar(query=groomlab.GPU_PREVIEW_VAR) if cmds.optionVar(exists=groomlab.GPU_PREVIEW_VAR) else None
        editor = graph_editor.GraphEditor()
        try:
            editor.sync_gpu_action()
            self.assertEqual(editor.gpu_action.isChecked(), was)
            editor.gpu_action.trigger()  # unchecks
            self.assertEqual(groomlab.gpu_preview(), not was)
            self.assertEqual(cmds.optionVar(query=groomlab.GPU_PREVIEW_VAR), 0 if was else 1)
            groomlab.gpu_preview(was)
            editor.sync_gpu_action()
            self.assertEqual(editor.gpu_action.isChecked(), was)
        finally:
            groomlab.gpu_preview(was)
            if stored is None:
                cmds.optionVar(remove=groomlab.GPU_PREVIEW_VAR)
            else:
                cmds.optionVar(intValue=(groomlab.GPU_PREVIEW_VAR, stored))
            editor.deleteLater()

    def test_support_groomlab_opens_the_donations_page(self):
        # GroomLab is free: GroomLab > Support GroomLab and About's button open Ko-fi (no dialog).
        from groomlab import links
        from groomlab.ui import menu
        self.assertEqual(menu.DONATE_URL, "https://ko-fi.com/getgroomlab")
        opened = []
        was = links.OPEN_URL
        links.OPEN_URL = lambda url: opened.append(url) or True
        try:
            menu.install()  # Help > Support GroomLab
            help_menu = next(menu.MENU + "|" + i for i in cmds.menu(menu.MENU, query=True, itemArray=True)
                             if cmds.menuItem(i, query=True, label=True) == "Help")
            item = next(help_menu + "|" + i for i in cmds.menu(help_menu, query=True, itemArray=True)
                        if cmds.menuItem(help_menu + "|" + i, query=True, label=True) == "Support GroomLab")
            self.assertIn(menu.DONATE_URL, cmds.menuItem(item, query=True, annotation=True))
            self.assertEqual(menu.open_donate(), menu.DONATE_URL)
            window = menu.show_about()
            try:
                self.assertTrue(cmds.button("groomLabAboutSupport", exists=True))
                self.assertEqual(cmds.button("groomLabAboutSupport", query=True, label=True), "Support GroomLab")
            finally:
                cmds.deleteUI(window)
            self.assertEqual(opened, [menu.DONATE_URL])
        finally:
            links.OPEN_URL = was

    def test_no_learning_mode_badge_or_watermark(self):
        # No license, no Learning mode: the Groom Tools and the Graph Editor carry no badge.
        from groomlab.ui import graph_editor, palette
        from groomlab.ui.qt import QtWidgets
        for window in (palette.show(), graph_editor.show()):
            texts = [w.text() for w in window.findChildren(QtWidgets.QLabel)]
            self.assertFalse([t for t in texts if "Learning" in t or "non-commercial" in t or "License" in t], window)
            self.assertFalse(hasattr(window, "learning"))
        self.assertFalse(hasattr(cmds, "groomLabLicense"))

    def test_the_menu_is_short(self):
        from groomlab.ui import menu
        menu.install()
        try:
            labels = [cmds.menuItem(i, query=True, label=True) for i in cmds.menu(menu.MENU, query=True, itemArray=True)
                      if not cmds.menuItem(i, query=True, divider=True)]
            self.assertEqual(labels, ["Groom Tools", "Graph Editor", "Create Groom from Selection", "Add Sample Mesh",
                                      "Guides from Photos (experimental)...", "Groom Files", "Import", "Add", "Simulate",
                                      "Display", "Review", "Studio", "Render", "Export", "Utilities", "Theme",
                                      "Install GroomLab Hotkeys",
                                      "Help"])

            def entries_of(label, tooltips=True, parent=menu.MENU):
                sub = next(parent + "|" + i for i in cmds.menu(parent, query=True, itemArray=True)
                           if cmds.menuItem(parent + "|" + i if parent != menu.MENU else i, query=True,
                                            label=True) == label)
                self.assertTrue(cmds.menuItem(sub, query=True, subMenu=True))
                found = [sub + "|" + i for i in cmds.menu(sub, query=True, itemArray=True)
                         if not cmds.menuItem(sub + "|" + i, query=True, divider=True)]
                for entry in found if tooltips else []:
                    self.assertTrue(cmds.menuItem(entry, query=True, annotation=True), entry)  # a tooltip each
                return [cmds.menuItem(i, query=True, label=True) for i in found]
            # The graph's building blocks, one Simulate menu (nHair under Legacy), the renders, the
            # exports, help; a tooltip on every item.
            self.assertEqual(entries_of("Add"), ["Feathers...", "Braid on Curves...", "Cards", "Collider", "Wind",
                                                 "Jiggle"])
            self.assertEqual(entries_of("Simulate"), ["Add Simulate Node", "Simulate Range", "Bake Simulation",
                                                      "Clear Simulation", "Legacy"])
            simulate = next(menu.MENU + "|" + i for i in cmds.menu(menu.MENU, query=True, itemArray=True)
                            if cmds.menuItem(i, query=True, label=True) == "Simulate")
            self.assertEqual(entries_of("Legacy", parent=simulate),
                             ["Simulate Guides (nHair)...", "Bake Simulation to Cache...", "Remove Simulation",
                              "Export Guides for Simulation...", "Load Guide Cache..."])
            self.assertEqual(entries_of("Review"), ["Density Heatmap", "Strand Count HUD", "Turntable",
                                                    "Render Turntable..."])
            for item in (menu.HEATMAP_ITEM, menu.HUD_ITEM):  # check items, their state as the menu opens
                self.assertIn(cmds.menuItem(item, query=True, checkBox=True), (True, False))
            menu._sync_review_items()
            self.assertEqual(cmds.menuItem(menu.HUD_ITEM, query=True, checkBox=True), bool(groomlab.review.hud_visible()))
            self.assertEqual(entries_of("Studio"), ["Portrait", "Beauty", "Rim Drama", "High Key", "Turntable",
                                                    "Look Through Studio Camera", "Remove Studio"])
            self.assertEqual(entries_of("Render"), ["Render from Groom File...", "Render as Curves",
                                                    "Maya Hardware 2.0 (no renderer needed)...", "RenderMan (prman)...",
                                                    "V-Ray Standalone...", "V-Ray for Maya Hook (2024+, experimental)",
                                                    "Load as VRayScene (2024+, experimental)"])
            exports = entries_of("Export")
            self.assertIn("Unreal Cards...", exports)
            self.assertNotIn("Groom File...", exports)
            self.assertNotIn("Bake Scale Maps", exports)
            self.assertEqual(entries_of("Help"), ["Docs", "Keyboard Shortcuts", "Check for Updates...", "Discord",
                                                  "Support GroomLab", "About GroomLab"])
            self.assertTrue(menu.docs_folder().endswith("docs"))
            self.assertIn("B + drag", menu.shortcuts_text())
            self.assertIn("GroomLab", menu.about_text())
            self.assertIn(menu.DONATE_URL, menu.about_text())
            self.assertIn("Open source (Apache-2.0): " + menu.REPO_URL, menu.about_text())
            # Grooms to and from .groom files (the standalone app's round trip), a tooltip each.
            files = next(menu.MENU + "|" + i for i in cmds.menu(menu.MENU, query=True, itemArray=True)
                         if cmds.menuItem(i, query=True, label=True) == "Groom Files")
            entries = [files + "|" + i for i in cmds.menu(files, query=True, itemArray=True)
                       if not cmds.menuItem(files + "|" + i, query=True, divider=True)]
            self.assertEqual([cmds.menuItem(i, query=True, label=True) for i in entries],
                             ["Import Groom File...", "Reload from Groom File", "Save to Groom File",
                              "Save Groom File As..."])
            for entry in entries:
                self.assertTrue(cmds.menuItem(entry, query=True, annotation=True))
            menu._sims()  # Remove Simulation's scene callbacks
            self.assertTrue(menu._SIMS_SCENE_JOBS)
            menu.install()  # rebuilt (a new theme): they stay
            self.assertTrue(menu._SIMS_SCENE_JOBS)
        finally:
            menu.uninstall()
        self.assertEqual(menu._SIMS_SCENE_JOBS, [])  # gone with the plugin

    def test_utilities_submenu_and_bake_to_guides(self):
        from groomlab.ui import menu
        menu.install()
        try:
            items = cmds.menu(menu.MENU, query=True, itemArray=True)
            utilities = next(menu.MENU + "|" + i for i in items if cmds.menuItem(i, query=True, label=True) == "Utilities")
            self.assertTrue(cmds.menuItem(utilities, query=True, subMenu=True))
            entries = [utilities + "|" + i for i in cmds.menu(utilities, query=True, itemArray=True)
                       if not cmds.menuItem(i, query=True, divider=True)]
            self.assertEqual([cmds.menuItem(i, query=True, label=True) for i in entries],
                             ["Guides to Curves", "Curves to Guides...", "Strands to Curves...", "Strands to Mesh...",
                              "Bake Node to Guides...", "Bake Scale Maps", "Delete All Flow Arrows",
                              "Transfer Groom to Scalp...",
                              "Copy Graph to Other Grooms...", "Find Missing Files...", "Collect Files"])
            for entry in entries:
                self.assertTrue(cmds.menuItem(entry, query=True, annotation=True))  # a tooltip each
        finally:
            menu.uninstall()
        # Guides to Curves works on the selected groom's guides with no questions asked.
        curve = cmds.curve(degree=1, point=[(0.0, 1.0, 0.0), (0.0, 1.5, 0.2), (0.0, 2.0, 0.4)])
        groomlab.guides_from_curves(self.groom, [curve])
        cmds.select(self.groom)
        made = menu.guides_to_curves_action()
        self.assertEqual(made["count"], 1)
        self.assertEqual(cmds.ls(selection=True, long=True), [made["group"]])
        # The Graph Editor's Bake Node to Guides (a node's right-click menu), given the group.
        from groomlab.ui import graph_editor
        editor = graph_editor.GraphEditor(self.groom)
        try:
            ids = editor.bake_to_guides("scatter", group="baked", density=0.02)
            self.assertTrue(ids)
            self.assertEqual(sorted(groomlab.guide_ids(self.groom, "baked")), sorted(ids))
            self.assertIn("baked %d guides" % len(ids), editor.status.text())
            self.assertIsNone(editor.bake_to_guides("nope", group="baked"))  # reported, not raised
        finally:
            editor.deleteLater()

    def test_a_pinned_groom_holds_the_graph_editor(self):
        from groomlab.ui import graph_editor
        other = groomlab.create_groom(cmds.polySphere()[0], name="other")
        cmds.select(other)
        groomlab.pin_groom(self.groom)
        window = None
        try:
            window = graph_editor.show()  # the pinned groom, not the selected one
            self.assertEqual(window.groom, self.groom)
            window._selection_changed_in_maya()  # (the SelectionChanged job) pinned: it stays
            self.assertEqual(window.groom, self.groom)
            groomlab.pin_groom(None)
            window._selection_changed_in_maya()  # unpinned: it follows the selection again
            self.assertEqual(window.groom, other)
        finally:
            groomlab.pin_groom(None)
            if window is not None:
                window.close()

    def test_one_current_groom_for_the_groom_tools_and_the_graph_editor(self):
        # Testers edited the wrong groom: the Groom Tools and the Graph Editor picked their grooms
        # apart. One current groom: a pick in either list moves both, a groom selected becomes it
        # (and stays it while other things are selected), and a pinned Graph Editor stays on its own.
        from groomlab.ui import graph_editor, palette
        other = groomlab.create_groom(cmds.polySphere(name="otherHead")[0], name="other")
        groomlab.set_current_groom(None)
        self.addCleanup(groomlab.set_current_groom, None)
        cmds.select(self.groom)
        tools = palette.show()
        editor = graph_editor.show()
        try:
            tools.poll()
            self.assertEqual((groomlab.active_groom(), editor.groom, tools.grooms.currentData()),
                             (self.groom, self.groom, self.groom))
            self.assertFalse(editor.pinned)
            self.assertEqual(editor.pin_button.glyph, "pin")
            # Picked in the Graph Editor: the Groom Tools (and the brushes) move too.
            editor.pick_groom(other)
            wait(30)
            tools.poll()
            self.assertEqual((groomlab.active_groom(), editor.groom, tools.grooms.currentData()), (other, other, other))
            # Something else selected: the picked groom stays current.
            cmds.select("otherHead")
            editor._selection_changed_in_maya()
            self.assertEqual((groomlab.active_groom(), editor.groom), (other, other))
            # Picked in the Groom Tools: the Graph Editor follows.
            tools.groom_picked(tools.grooms.findData(self.groom))
            wait(30)
            self.assertEqual((groomlab.active_groom(), editor.groom), (self.groom, self.groom))
            # A groom selected: it is the current groom for both.
            cmds.select(other)
            editor._selection_changed_in_maya()
            tools.poll()
            self.assertEqual((groomlab.active_groom(), editor.groom, tools.grooms.currentData()), (other, other, other))
            cmds.select(clear=True)
            self.assertEqual(groomlab.active_groom(), other)  # (nothing selected: it stays current)
            # Pinned: the graph stays while the Groom Tools move on; its own list moves only the graph.
            editor.pin_button.click()
            self.assertTrue(editor.pinned)
            self.assertEqual(editor.pin_button.glyph, "pinned")
            tools.groom_picked(tools.grooms.findData(self.groom))
            wait(30)
            cmds.select(self.groom)
            editor._selection_changed_in_maya()
            self.assertEqual((groomlab.active_groom(), editor.groom), (self.groom, other))
            editor.pick_groom(self.groom)
            editor.pick_groom(other)
            self.assertEqual((groomlab.active_groom(), editor.groom), (self.groom, other))
            graph_editor.show()  # opened again (Groom Tools > Graph): a pinned editor stays on its groom
            self.assertEqual(editor.groom, other)
            # Unpinned: back to the current groom.
            editor.pin_button.click()
            self.assertFalse(editor.pinned)
            self.assertEqual(editor.groom, self.groom)
        finally:
            editor.set_pinned(False, follow=False)
            editor.close()
            tools.close()

    def test_graph_editor_and_groom_tools_open_docked(self):
        from groomlab.ui import graph_editor, palette
        window = graph_editor.show(self.groom)
        self.assertTrue(window.isVisible())
        self.assertEqual(window.groom, self.groom)
        window.close()
        self.assertIs(graph_editor.show(), window)  # reopened, not rebuilt
        window.close()
        tools = palette.show(tab="guides")
        self.assertEqual(tools.tabs.currentIndex(), palette.TABS.index("tools"))
        self.assertTrue(tools.guides_section.expanded())
        tools.close()
        self.assertIs(palette.show(), tools)
        tools.close()

    def test_docks_and_windows_reopen_where_they_were(self):
        # Testers' windows went back to their default places after an update (a restart): the docks
        # carry a uiScript, so Maya keeps them in its workspace and rebuilds them where they were,
        # and a dock Maya kept is filled again rather than replaced; standalone windows keep their
        # geometry in an optionVar.
        import maya.OpenMayaUI as omui
        from PySide2 import QtWidgets
        from shiboken2 import wrapInstance
        from groomlab.ui import graph_editor, palette, updates_window, widgets
        control = "groomLabGraphEditorWorkspaceControl"

        def top():
            return wrapInstance(int(omui.MQtUtil.findControl(control)), QtWidgets.QWidget).window()
        window = graph_editor.show(self.groom)
        try:
            cmds.workspaceControl(control, edit=True, floating=True)
            wait(30)
            top().setGeometry(240, 180, 820, 460)
            wait(30)
            placed = top().geometry()
            # Maya quitting (its panel goes, the dock stays as Maya saves it), then the editor opened again.
            graph_editor._close_at_exit()
            self.assertFalse(widgets.alive(window))
            window = graph_editor.show(self.groom)
            wait(30)
            self.assertTrue(cmds.workspaceControl(control, query=True, floating=True))
            self.assertEqual(top().geometry(), placed)
            # Maya rebuilding it as it restores its workspace (the uiScript, its dock the current parent).
            cmds.setParent(control)
            graph_editor.restore()
            window = graph_editor._window
            wait(30)
            self.assertTrue(window.isVisible())
            self.assertEqual(window.groom, self.groom)
            self.assertEqual(top().geometry(), placed)
            self.assertEqual(len(top().findChildren(QtWidgets.QWidget, "groomLabGraphEditor")), 1)
        finally:
            if widgets.alive(window):
                window.close()
        # The Groom Tools rebuilt the same way, in the dock they had.
        tools = palette.show()
        cmds.setParent("groomLabToolPaletteWorkspaceControl")
        palette.restore()
        self.assertFalse(widgets.alive(tools))
        tools = palette._window
        try:
            self.assertIs(palette.show(), tools)
            self.assertTrue(tools.isVisible())
        finally:
            tools.close()
        # A standalone window: where it was, after Maya quit with it open.
        updates = updates_window.show(check=False)
        updates.setGeometry(300, 260, 520, 360)
        wait(700)  # (its geometry written soon after a move)
        placed = updates.geometry()
        updates_window._close_at_exit()
        updates = updates_window.show(check=False)
        try:
            self.assertEqual(updates.geometry(), placed)
        finally:
            updates.close()

    def test_output_strip_and_alembic_curves_import(self):
        """Groom Tools' Output strip runs the GroomLab menu's items (one table); Import > Alembic
        Curves... makes the file's curves the groom's guides in one undo step."""
        import tempfile
        from PySide2 import QtWidgets
        from groomlab import sim
        from groomlab.ui import menu, palette
        cmds.select(self.groom)
        tools = palette.show()
        try:
            self.assertTrue(tools.output_section.expanded())
            self.assertTrue(tools.output_strip.isVisibleTo(tools))
            labels = [c.text() for c in tools.output_chips]
            self.assertEqual([t.split("  ")[0] for t in labels], ["Simulate Range", "Export", "Render", "Studio",
                                                                  "Transfer...", "Import"])

            def actions(kind):
                chip = tools.findChild(QtWidgets.QToolButton, "output_" + kind)
                return [a.text() for a in chip.menu().actions() if not a.isSeparator()]
            exports = actions("export")
            for label in ("Alembic...", "USD...", "Unreal Groom...", "Unity Strands (.abc)...", "glTF Cards..."):
                self.assertIn(label, exports)
            self.assertNotIn("Scales Mesh (FBX)...", exports)  # the menu's long list only
            self.assertIn("Alembic Curves...", actions("import"))
            self.assertIn("Render Turntable...", actions("render"))
            self.assertIn("Portrait", actions("studio"))
            self.assertIn("Remove Studio", actions("studio"))
            # The menu shows the same table, without the strip's shortcuts.
            menu_exports = [i[0] for i in menu.shown_items("export", menu.MENU_ONLY) if i]
            self.assertIn("Scales Mesh (FBX)...", menu_exports)
            self.assertNotIn("Save Groom File As...", menu_exports)
            self.assertIn("Alembic Curves...", [i[0] for i in menu.shown_items("import", menu.MENU_ONLY) if i])
            self.assertIsNot(menu.shown_items("render", menu.MENU_ONLY)[-1], None)  # no trailing divider
        finally:
            tools.close()
        # Import > Alembic Curves...: an ImportCurves node in place of the Guides node; undo restores it.
        curves = [cmds.curve(degree=1, point=[(0, 1, 0), (0.1, 1.5, 0), (0.2, 2.0, 0)]),
                  cmds.curve(degree=1, point=[(0, -1, 0), (0.1, -1.5, 0), (0.2, -2.0, 0)])]
        groomlab.guides_from_curves(self.groom, curves)
        path = os.path.join(tempfile.mkdtemp(), "curves.abc").replace("\\", "/")
        sim.export_guides(self.groom, path, "primary", start=1, end=1)
        before = groomlab.get_graph(self.groom)
        self.assertEqual(menu.import_alembic_curves_dialog(path=path, groom=self.groom), self.groom)
        guides = [n for n in groomlab.get_graph(self.groom)["nodes"] if n["name"] == "guides"][0]
        self.assertEqual(guides["type"], "ImportCurves")
        self.assertTrue(guides["params"]["path"].endswith("curves.abc"))
        cmds.undo()
        self.assertEqual(groomlab.get_graph(self.groom), before)

    def test_tool_palette_drives_the_brush(self):
        from groomlab.ui import palette, widgets
        from PySide2 import QtCore
        groomlab.guides_from_curves(self.groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)])])
        groomlab.add_map(self.groom, "fur")  # maps live on guides
        cmds.select(self.groom)
        window = palette.show()
        try:
            self.assertTrue(window.isVisible())
            rows = [window.grooms.itemData(i) for i in range(window.grooms.count())]
            self.assertIn(self.groom, rows)  # the groom choice
            self.assertFalse(window.empty.isVisibleTo(window))  # a groom to work on: no "start a groom" card
            self.assertTrue(window.work.isVisibleTo(window))  # and the tabs and tools
            for tool in ("place", "draw", "comb", "twist", "erase", "paint"):
                window.buttons[tool].click()
                self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True), tool,
                                 window.status.text() + " maps: %s" % groomlab.list_maps(self.groom))
                self.assertEqual(cmds.currentCtx(), groomlab.BRUSH_CONTEXT)
            self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, attribute=True), "fur")
            # The palette is the core's catalog: Comb (with Stretch) and no Grab, no separate Clump
            # tile, Shape (the tubes' brush) under Shape, every section in order.
            self.assertEqual([section for section, _ in palette.SECTIONS], ["Create", "Shape", "Trim", "Paint", "Guides"])
            self.assertEqual(len(palette.CATALOG), sum(len(tools) for _, tools in palette.SECTIONS))
            self.assertNotIn("grab", window.buttons)
            self.assertNotIn("clump", window.buttons)
            self.assertIn("shape", [t for t, _, _ in dict(palette.SECTIONS)["Shape"]])
            self.assertNotIn("tube", window.buttons)
            self.assertEqual(palette.NAMES["straighten"], "Straighten")
            self.assertEqual(window.mask_scalp.text(), "Front facing only")
            self.assertEqual(window.falloff_label.text(), "Falloff")
            window.buttons["paint"].click()
            self.assertFalse(window.collide.isVisibleTo(window))  # painting: no collide, as the catalog says
            window.buttons["comb"].click()  # Comb: keep lengths or stretch
            self.assertTrue(window.stretch_box.isVisibleTo(window))
            window.stretch.setChecked(True)
            self.assertTrue(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, stretch=True))
            window.stretch.setChecked(False)
            self.assertFalse(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, stretch=True))
            self.assertEqual(window.tabs.currentIndex(), palette.TABS.index("paint"))  # Paint shows its maps
            self.assertEqual(window.paint_tab.current(), "fur")
            window.buttons["freeze"].click()  # Freeze is a brush on the Tools tab
            self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, attribute=True), "freeze")
            window.poll()
            self.assertEqual(window.tool, "freeze")
            window.groom_picked(window.grooms.findData(self.groom))  # picked: the current groom
            self.assertEqual(groomlab.pinned_groom(), self.groom)
            transform = cmds.listRelatives(self.groom, parent=True)[0]
            window.visible.setChecked(False)  # the hair's eye hides the hair alone (displayHair), not the node
            self.assertFalse(cmds.getAttr(self.groom + ".displayHair"))
            self.assertTrue(cmds.getAttr(transform + ".visibility"))
            self.assertTrue(cmds.getAttr(self.groom + ".displayGuides"))
            window.visible.setChecked(True)
            self.assertTrue(cmds.getAttr(self.groom + ".displayHair"))
            # The guides' eye: every guide group of the groom (displayGuides), one undo step.
            self.assertTrue(window.guides_eye.isChecked())
            window.guides_eye.setChecked(False)
            self.assertFalse(cmds.getAttr(self.groom + ".displayGuides"))
            self.assertEqual(window.guides_eye.glyph, "guidesOff")
            cmds.undo()
            self.assertTrue(cmds.getAttr(self.groom + ".displayGuides"))
            window.fill_grooms(groomlab.active_groom())  # (the tick's: the eye follows)
            self.assertTrue(window.guides_eye.isChecked())
            # Shift+click (or the right-click menu): every groom in the scene.
            other = groomlab.create_groom(cmds.polySphere(name="otherHead")[0], name="otherGroom")
            window._every_groom = lambda: True
            try:
                window.guides_eye.setChecked(False)
                self.assertEqual([cmds.getAttr(g + ".displayGuides") for g in (self.groom, other)], [False, False])
                window.visible.setChecked(False)
                self.assertEqual([groomlab.hair_visible(g) for g in (self.groom, other)], [False, False])
                self.assertEqual([cmds.getAttr(g + ".displayGuides") for g in (self.groom, other)], [False, False])
            finally:
                del window._every_groom
            items = [a.text() for a in window.guides_eye.eye_menu.actions()]
            self.assertEqual(items, ["Show All Guides", "Hide All Guides"])
            self.assertEqual([a.text() for a in window.visible.eye_menu.actions()], ["Show All Hair", "Hide All Hair"])
            [a for a in window.visible.eye_menu.actions() if a.text() == "Show All Hair"][0].trigger()
            [a for a in window.guides_eye.eye_menu.actions() if a.text() == "Show All Guides"][0].trigger()
            self.assertEqual([groomlab.hair_visible(g) for g in (self.groom, other)], [True, True])
            self.assertEqual([groomlab.guides_visible(g) for g in (self.groom, other)], [True, True])
            self.assertIn("Shift+click", window.guides_eye.toolTip())
            cmds.delete(cmds.listRelatives(other, parent=True), "otherHead")
            cmds.select(self.groom)
            groomlab.set_current_groom(None)  # no current groom: the selected one leads
            self.assertIsNone(groomlab.pinned_groom())
            self.assertEqual(groomlab.active_groom(), self.groom)
            window.buttons["part"].click()  # parts are drawn from the palette too
            self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True), "part")
            self.assertTrue(window.part_box.isVisibleTo(window))
            # Only the sections the tool uses (palette.json's "tools"): Part draws no tubes and strokes no
            # strands; the guide groups (Tools tab) and Output always there.
            self.assertFalse(window.tubes_section.isVisibleTo(window))
            self.assertFalse(window.display_section.isVisibleTo(window))
            self.assertEqual(window.guides_section.isVisibleTo(window),
                             palette.TABS[window.tabs.currentIndex()] == "tools")  # (the Tools tab's)
            self.assertTrue(window.output_section.isVisibleTo(window))
            window.buttons["twist"].click()
            self.assertEqual(window.strength.lo, -1.0)  # signed tools get a signed strength
            self.assertTrue(window.display_section.isVisibleTo(window))  # While sculpting: with the brushes
            self.assertFalse(window.tubes_section.isVisibleTo(window))
            # The Shape brush: what it paints (Width, Flatness, Twist), and the selected guides' tubes
            # as a whole with the groom's Fill Tubes (each change one undo step).
            window.groom_picked(window.grooms.findData(self.groom))
            window.buttons["shape"].click()
            self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True), "shape")
            self.assertEqual(window.strength.lo, -1.0)  # Ctrl does the opposite
            self.assertTrue(window.tube_channel_box.isVisibleTo(window))
            self.assertTrue(window.tubes_section.expanded())  # its own section, opened by the Shape brush
            self.assertTrue(window.tubes_section.isVisibleTo(window))
            self.assertTrue(window.tube_panel.isVisibleTo(window))
            self.assertFalse(window.collide.isVisibleTo(window))  # it moves no guide point
            window.tube_channel_buttons["flatness"].click()
            self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, tubeChannel=True), "flatness")
            panel = window.tube_panel
            guide = groomlab.guide_ids(self.groom, "primary")[0]
            groomlab.clear_guide_selection(self.groom)
            panel.refresh(self.groom)
            # Nothing selected: nothing to smooth (the shape: Place's), unless the groom has one guide,
            # which the panel then edits.
            only_one = sum(g.get("count", 0) for g in groomlab.list_guide_groups(self.groom)) == 1
            self.assertEqual(panel.smooth.isEnabled(), only_one)
            groomlab.select_guides(self.groom, guides=[guide])
            panel.refresh(self.groom)
            self.assertTrue(panel.smooth.isEnabled())
            panel.shape.setCurrentIndex(panel.shape.findData("ribbon"))
            panel.shape.activated.emit(panel.shape.currentIndex())
            self.assertEqual(groomlab.get_tube(self.groom, guide)["shape"], "ribbon")
            cmds.undo()
            self.assertEqual(groomlab.get_tube(self.groom, guide)["shape"], "round")
            panel.refresh(self.groom)
            widest = max(groomlab.get_tube(self.groom, guide)["width"])
            panel.width.committed.emit(panel.width.value() * 2.0)
            self.assertAlmostEqual(max(groomlab.get_tube(self.groom, guide)["width"]), 2.0 * widest, places=4)
            panel.flatness.committed.emit(0.75)
            self.assertAlmostEqual(groomlab.get_tube(self.groom, guide)["aspect"][0], 0.25, places=4)
            panel.twist.committed.emit(90.0)
            self.assertAlmostEqual(groomlab.get_tube(self.groom, guide)["twist"][0], math.pi / 2, places=4)
            # Each guide keeps its own tube: the panel edits the selection only, and its shape picker
            # says "Mixed" while the selected guides' shapes differ.
            other = groomlab.guides_from_curves(self.groom, [cmds.curve(degree=1, point=[(1, 0, 0), (2, 0, 0)])],
                                                group="extra")[0]
            cmds.select(self.groom)  # (the curve made selected the curve: the palette follows the selection)
            self.assertEqual(groomlab.get_tube(self.groom, other)["twist"][0], 0.0)
            groomlab.set_tube(self.groom, guides=[guide], shape="blade")
            groomlab.select_guides(self.groom, guides=[guide, other])
            panel.refresh(self.groom)
            self.assertEqual(panel.shape.currentText(), "Mixed")
            groomlab.select_guides(self.groom, guides=[guide])
            panel.refresh(self.groom)
            self.assertEqual(panel.shape.currentData(), "blade")
            self.assertEqual(panel.shape.findText("Mixed"), -1)
            # Smooth: the channel the brush paints (Flatness) evened out along the selected guide.
            groomlab.set_tube(self.groom, guides=[guide], aspect=[1.0, 0.2, 1.0, 0.2, 1.0])
            panel.smooth_what.setCurrentIndex(panel.smooth_what.findData("current"))
            panel.smooth.click()
            smoothed = groomlab.get_tube(self.groom, guide)["aspect"]
            self.assertGreaterEqual(len(smoothed), 5)  # one section per guide point (or the list's own, if more)
            self.assertGreater(min(smoothed), 0.3)  # the dips filled in
            self.assertAlmostEqual(smoothed[0], 1.0, places=4)  # the root's kept
            self.assertAlmostEqual(groomlab.get_tube(self.groom, guide)["twist"][0], math.pi / 2, places=4)  # not smoothed
            cmds.undo()
            self.assertEqual(len(groomlab.get_tube(self.groom, guide)["aspect"]), 5)
            self.assertTrue(panel.fill.isEnabled())  # the default graph's Interpolate
            panel.fill.setChecked(True)
            self.assertTrue(groomlab.fill_tubes(self.groom)["fillTubes"])
            panel.blend.committed.emit(40.0)
            self.assertAlmostEqual(groomlab.fill_tubes(self.groom)["tubeBlend"], 0.4, places=5)
            cmds.undo()
            cmds.undo()
            self.assertFalse(groomlab.fill_tubes(self.groom)["fillTubes"])
            groomlab.clear_guide_selection(self.groom)
            groomlab.set_current_groom(None)  # the selected groom leads again
            window.radius.setValue(150)
            window.radius.committed.emit(150)
            self.assertEqual(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, radius=True), 150)
            cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, radius=90)  # middle-drag in the view
            window.poll()
            self.assertEqual(window.radius.value(), 90)
            # Level picked in the view (Shift+click) shows in the Guides choice.
            window.buttons["twist"].click()
            cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, level=-1, group="")
            window.poll()
            self.assertEqual(window.level.currentData(), -1)
            # Correctives: a stack with the base groom at the bottom; the picked row is what brushes edit.
            from PySide2 import QtCore
            from groomlab import shots
            box = window.correctives
            box.set_groom(self.groom)
            cmds.currentTime(7)
            self.assertEqual(box.tree.topLevelItemCount(), 1)  # just the base groom
            box.new_layer()
            self.assertEqual(shots.target(self.groom), "sculpt1")
            self.assertTrue(cmds.getAttr(self.groom + ".shotSculpt"))
            self.assertEqual(box.current(), "sculpt1")
            box.fix_frame()
            self.assertEqual(box.current(), "shot7")
            shot7 = next(l for l in shots.layers(self.groom) if l["name"] == "shot7")
            self.assertEqual([k[0] for k in shot7["keys"]], [1.0, 7.0, 13.0])  # blends over 6 frames each side
            self.assertEqual(box.tree.topLevelItem(0).text(0), "shot7")  # top of the stack first
            self.assertEqual(box.tree.topLevelItem(2).text(0), "Base groom")
            self.assertEqual(box.tree.topLevelItem(0).text(2), "◆ 3")
            box.solo()  # only shot7 on
            self.assertEqual([l["enabled"] for l in shots.layers(self.groom)], [False, True])
            box.solo()  # all back on
            box.tree.topLevelItem(1).setCheckState(0, QtCore.Qt.Unchecked)  # hide sculpt1
            self.assertFalse(shots.layers(self.groom)[0]["enabled"])
            box.set_weight(0.5)  # keyed: keyed here
            self.assertAlmostEqual(shots.weight_at(self.groom, "shot7", 7), 0.5, places=4)
            box.move(-1)
            self.assertEqual([l["name"] for l in shots.layers(self.groom)], ["shot7", "sculpt1"])
            box.tree.setCurrentItem(box.tree.topLevelItem(box.tree.topLevelItemCount() - 1))  # the base groom
            wait(50)
            self.assertIsNone(shots.target(self.groom))
            self.assertFalse(box.weight.isEnabled())
            shots.set_target(self.groom, "shot7")  # picked elsewhere: the list follows
            wait(50)
            self.assertEqual(box.current(), "shot7")
            box.delete()
            self.assertEqual(box.tree.topLevelItemCount(), 2)
            self.assertFalse(cmds.attributeQuery("shot7", node=self.groom, exists=True))
            # Flow: the arrows kept on the groom counted beside Clear All, which takes them all off (one step).
            groomlab.flow_stroke(self.groom, [(-0.3 + 0.1 * i, 0.9, 0.3) for i in range(7)], radius=0.2)
            window.buttons["flow"].click()
            self.assertTrue(window.flow_box.isVisibleTo(window))
            self.assertFalse(window.tubes_section.isVisibleTo(window))
            arrows = len(groomlab.flow_arrows(self.groom))
            self.assertEqual(window.flow_count.text(), palette.flow_count_text(arrows, 0))
            self.assertEqual(window.clear_flow_button.isEnabled(), arrows > 0)
            window.clear_flow_button.click()
            self.assertEqual(groomlab.flow_arrows(self.groom), [])
            self.assertEqual(window.flow_count.text(), "No flow arrows yet")
            self.assertFalse(window.clear_flow_button.isEnabled())
            window.buttons["select"].click()
            self.assertEqual(cmds.currentCtx(), groomlab.SELECT_CONTEXT)
            self.assertFalse(window.radius.isEnabled())
            self.assertTrue(window.tubes_section.isVisibleTo(window))  # the selected guides' tubes
            self.assertFalse(window.flow_box.isVisibleTo(window))
            self.assertTrue(widgets.icon("comb").availableSizes() or not widgets.icon("comb").isNull())
            # Guide groups (a Tools section): the groups, and edits on many guides.
            window.show_tab("guides")
            self.assertTrue(window.guides_section.isVisibleTo(window))
            tree = window.guides_tab.tree
            self.assertEqual(tree.topLevelItem(0).text(0), "primary")
            window.guides_tab.select_group()
            self.assertEqual(len(groomlab.selected_guides(self.groom)), 1)
            # Paint tab: maps follow the groom.
            window.show_tab("paint")
            self.assertTrue(window.settings.isVisibleTo(window))
            groomlab.add_map(self.groom, "density")
            self.app_events()
            from PySide2 import QtCore as _qc
            self.assertIn("density", [window.paint_tab.list.item(i).data(_qc.Qt.UserRole) for i in range(window.paint_tab.list.count())])
            window.paint_tab.value.setValue(0.3)
            window.paint_tab.fill(False)
            stored = groomlab.get_hierarchy(self.groom)["guides"]
            self.assertAlmostEqual(stored[0]["attributes"][window.paint_tab.current()], 0.3, places=5)
            # One Fill: the selected guides when there are some, else the whole map.
            groomlab.select_guides(self.groom, group="primary")
            window.paint_tab.value.setValue(0.7)
            window.paint_tab.fill()
            stored = groomlab.get_hierarchy(self.groom)["guides"]
            self.assertAlmostEqual(stored[0]["attributes"][window.paint_tab.current()], 0.7, places=5)
            self.assertIn("selected", window.status.text())
            # The Paint tile again: done painting.
            window.buttons["paint"].click()
            self.assertEqual(cmds.currentCtx(), groomlab.BRUSH_CONTEXT)
            window.buttons["paint"].click()
            self.assertNotEqual(cmds.currentCtx(), groomlab.BRUSH_CONTEXT)
            self.assertIsNone(window.tool)
            self.assertFalse(window.buttons["paint"].isChecked())
        finally:
            window.close()

    def test_tube_shape_editor_and_library(self):
        # The Tubes panel's shape editor: a built-in shown read-only, Edit as Custom copying it into
        # the library (the targets take it), points dragged (mirrored), added and removed, each a
        # commit every guide taking the shape follows; New, Duplicate, Rename in place, Delete on a
        # second click (no dialogs); the panel following a Shift+click solo; Place's tube.
        from groomlab.ui import palette
        guides = groomlab.guides_from_curves(self.groom, [cmds.curve(degree=1, point=[(0, 1, 0), (0, 2, 0)]),
                                                          cmds.curve(degree=1, point=[(0.3, 0.95, 0), (0.6, 1.9, 0)])])
        cmds.select(self.groom)
        window = palette.show()
        try:
            window.groom_picked(window.grooms.findData(self.groom))
            window.buttons["shape"].click()
            panel = window.tube_panel
            editor = panel.editor
            groomlab.select_guides(self.groom, guides=[guides[0]])
            panel.refresh(self.groom)
            self.assertIn("1 guide selected", panel.targets.text())
            self.assertEqual(panel.shape.currentData(), "round")
            self.assertFalse(editor.editable)  # a built-in: read-only
            self.assertTrue(panel.edit_custom.isVisible())
            panel.shape.setCurrentIndex(panel.shape.findData("ribbon"))
            panel.shape.activated.emit(panel.shape.currentIndex())
            self.assertEqual(groomlab.get_tube(self.groom, guides[0])["shape"], "ribbon")
            panel.edit_custom.click()  # a copy in the library, the selected guide taking it
            self.assertEqual(panel._choice, "ribbon 1")
            self.assertEqual(groomlab.get_tube(self.groom, guides[0])["shape"], "ribbon 1")
            self.assertTrue(editor.editable)
            self.assertFalse(panel.edit_custom.isVisible())
            self.assertEqual([s["name"] for s in groomlab.tube_shapes(self.groom) if not s["builtin"]], ["ribbon 1"])
            panel.refresh(self.groom)
            self.assertEqual(panel._choice, "ribbon 1")
            # A drag (mirrored: the point's mirror image moves too), committed on release.
            right = max(range(len(editor.points)), key=lambda i: editor.points[i][0] + 0.01 * editor.points[i][1])
            u, v = editor.points[right]
            editor.move_point(right, u + 0.3, v)
            mates = [i for i, p in enumerate(editor.points) if abs(p[0] + u + 0.3) < 1e-6 and abs(p[1] - v) < 1e-6]
            self.assertEqual(len(mates), 1, "its mirror image moved with it")
            editor.committed.emit(list(editor.points))
            saved = [s for s in groomlab.tube_shapes(self.groom) if s["name"] == "ribbon 1"][0]["points"]
            self.assertEqual(len(saved), len(editor.points))
            us = sorted(round(p[0], 4) for p in saved)
            self.assertEqual(us, sorted(round(-x, 4) for x in us), "symmetric about the v axis")
            # A point added on the outline (and its mirror image), then removed (with it).
            count = len(editor.points)
            span, at = editor.edge_at(editor.to_screen(*editor._curve[3])) or (None, None)
            self.assertIsNotNone(span)
            editor.add_point(span, at)
            added = list(editor.points)
            editor.committed.emit(list(editor.points))
            self.assertIn(len(editor.points), (count + 1, count + 2), "%s: %s" % (window.status.text(), added))
            self.assertEqual(len([s for s in groomlab.tube_shapes(self.groom) if s["name"] == "ribbon 1"][0]["points"]),
                             len(editor.points))
            self.assertTrue(editor.remove_point(0))
            editor.committed.emit(list(editor.points))
            # A crossing outline is refused: said, the shape as it was.
            before = [s for s in groomlab.tube_shapes(self.groom) if s["name"] == "ribbon 1"][0]["points"]
            editor.mirror = False
            editor.points = [(-1, -1), (1, 1), (1, -1), (-1, 1)]
            editor.committed.emit(list(editor.points))
            self.assertIn("crosses itself", window.status.text())
            self.assertEqual([s for s in groomlab.tube_shapes(self.groom) if s["name"] == "ribbon 1"][0]["points"], before)
            panel.mirror.setChecked(True)
            # A live preview while dragging: nothing stored, no undo step.
            data = cmds.getAttr(self.groom + ".guideData")
            panel.shape_dragged(editor.points)
            panel.send_preview()
            self.assertEqual(cmds.getAttr(self.groom + ".guideData"), data)
            groomlab.end_guide_preview(self.groom)
            # New, Duplicate, Rename in place, Delete on a second click.
            panel.new_library_shape()
            self.assertEqual(panel._choice, "shape 1")
            panel.duplicate_library_shape()
            self.assertEqual(panel._choice, "shape 1 1")
            panel.start_rename()
            self.assertTrue(panel.rename_field.isVisible())
            panel.rename_field.setText("leaf")
            panel.finish_rename()
            self.assertFalse(panel.rename_field.isVisible())
            self.assertIn("leaf", [s["name"] for s in groomlab.tube_shapes(self.groom)])
            panel.apply_shape.click()
            self.assertEqual(groomlab.get_tube(self.groom, guides[0])["shape"], "leaf")
            panel.delete_shape.click()  # armed: says what goes round
            self.assertIn("1 goes round", panel.delete_shape.text())
            self.assertIn("leaf", [s["name"] for s in groomlab.tube_shapes(self.groom)])
            panel.delete_shape.click()
            self.assertNotIn("leaf", [s["name"] for s in groomlab.tube_shapes(self.groom)])
            self.assertEqual(groomlab.get_tube(self.groom, guides[0])["shape"], "round")
            self.assertEqual(panel.delete_shape.text(), "Delete")
            # The panel follows a solo (no selection, the rest frozen): the soloed guide's tube.
            groomlab.set_tube(self.groom, guides=[guides[1]], shape="blade")
            groomlab.clear_guide_selection(self.groom)
            groomlab.set_guide_attribute(self.groom, "freeze", 1.0, guides=guides)
            groomlab.set_guide_attribute(self.groom, "freeze", 0.0, guides=[guides[1]])
            window.poll()
            self.assertIn("soloed", panel.targets.text())
            self.assertEqual(panel.shape.currentData(), "blade")
            # Place's tube: the panel's shape (and values) as the brush's placement tube.
            groomlab.set_guide_attribute(self.groom, "freeze", 0.0, guides=guides)
            groomlab.clear_guide_selection(self.groom)
            window.buttons["place"].click()
            self.assertTrue(panel.isVisibleTo(window))
            panel.refresh(self.groom)
            panel.shape.setCurrentIndex(panel.shape.findData("teardrop"))
            panel.shape.activated.emit(panel.shape.currentIndex())
            self.assertEqual(groomlab.placement_tube()["shape"], "teardrop")
            panel.flatness.setValue(0.5)  # (as a drag of the slider leaves it, then commits)
            panel.flatness.committed.emit(0.5)
            self.assertAlmostEqual(groomlab.placement_tube()["aspect"][0], 0.5, places=4)
        finally:
            window.close()

    def test_paint_tool_follows_the_active_groom(self):
        # Two grooms, each with a texture map "mask" (its own file): while painting, making the
        # other groom active sends the tool that groom's file; the first one's never lingers.
        from groomlab.ui import palette
        other = groomlab.create_groom(cmds.polySphere(radius=1)[0], name="other")
        for g in (self.groom, other):
            groomlab.add_map(g, "mask", 0.0, kind="texture", resolution=16)
        stored = {g: next(m["texture"] for m in groomlab.maps(g) if m["name"] == "mask") for g in (self.groom, other)}
        self.assertNotEqual(stored[self.groom], stored[other])
        cmds.select(self.groom)
        window = palette.show()
        try:
            window.poll()
            window.buttons["paint"].click()
            self.app_events()
            ctx = groomlab.BRUSH_CONTEXT
            self.assertEqual(window.paint_tab.current(), "mask")
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, paintTexture=True), stored[self.groom])
            cmds.select(other)
            window.poll()
            self.app_events()
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, paintTexture=True), stored[other])
            window.buttons["paint"].click()
        finally:
            window.close()

    def test_the_map_shown_while_painting_goes_away_after(self):
        # Painting shows the map on the strands and the scalp; turning Paint off (the tile again,
        # another tool, Maya's own tool) puts back what showed before. A view the artist turned
        # on themselves while painting stays.
        from groomlab.ui import palette
        groomlab.add_map(self.groom, "mask", 0.5, kind="vertices")
        cmds.select(self.groom)
        window = palette.show()
        try:
            window.poll()
            before = (cmds.getAttr(self.groom + ".strandColorMode"), cmds.getAttr(self.groom + ".scalpMapDisplay") or "")
            self.assertEqual(before[1], "")
            shown = lambda: (cmds.getAttr(self.groom + ".strandColorMode"),
                             cmds.getAttr(self.groom + ".scalpMapDisplay") or "")
            # The tile on, then off.
            window.buttons["paint"].click()
            self.app_events()
            self.assertEqual(shown(), (2, "mask"))
            window.buttons["paint"].click()
            self.app_events()
            self.assertEqual(shown(), before)
            # Another GroomLab tool.
            window.buttons["paint"].click()
            self.app_events()
            self.assertEqual(shown(), (2, "mask"))
            window.activate("comb")
            self.app_events()
            self.assertEqual(shown(), before)
            # Maya's own tool taking over (Q).
            window.buttons["paint"].click()
            self.app_events()
            cmds.setToolTo("selectSuperContext")
            window.poll()
            self.assertEqual(shown(), before)
            # Back to the Tools tab: done painting (no brush left painting a map out of sight).
            window.buttons["paint"].click()
            self.app_events()
            self.assertEqual(shown(), (2, "mask"))
            window.show_tab("tools")
            self.app_events()
            self.assertEqual(shown(), before)
            self.assertIsNone(window.tool)
            self.assertNotEqual(cmds.currentCtx(), groomlab.BRUSH_CONTEXT)
            # GroomLab's Select tool from elsewhere (a shelf button, a script).
            window.buttons["paint"].click()
            self.app_events()
            self.assertEqual(shown(), (2, "mask"))
            groomlab.select_tool()
            window.poll()
            self.assertEqual(shown(), before)
            cmds.setToolTo("selectSuperContext")
            window.poll()
            # A map already on the scalp before painting stays on after.
            groomlab.show_map_on_scalp(self.groom, "mask")
            window.buttons["paint"].click()
            self.app_events()
            window.buttons["paint"].click()
            self.app_events()
            self.assertEqual(cmds.getAttr(self.groom + ".scalpMapDisplay"), "mask")
            groomlab.show_map_on_scalp(self.groom, None)
            # The strands' coloring changed by hand while painting: kept as set.
            window.buttons["paint"].click()
            self.app_events()
            cmds.setAttr(self.groom + ".strandColorMode", 1)
            window.buttons["paint"].click()
            self.app_events()
            self.assertEqual(cmds.getAttr(self.groom + ".strandColorMode"), 1)
            self.assertEqual(cmds.getAttr(self.groom + ".scalpMapDisplay") or "", "")
        finally:
            window.close()

    def test_paint_tab_map_for_a_setting(self):
        # + New Map > For a setting: the dialog offers the common settings (Curl greyed out: the
        # default graph has no Curl node) and every mappable param under Other; Density makes a
        # white vertex map, links it to Scatter's density, picks it and turns the Paint brush on
        # it toward 1, all one undo step; asked again, the same map is picked.
        from groomlab.ui import palette, tool_tabs
        cmds.select(self.groom)
        window = palette.show()
        try:
            window.poll()
            window.show_tab("paint")
            tab = window.paint_tab
            settings, others = groomlab.setting_targets(self.groom)
            common = set((s["node"], s["param"]) for s in settings)
            other = next((o["node"], o["param"]) for o in others if (o["node"], o["param"]) not in common)
            dialog = tool_tabs.NewMapDialog(tab, targets=(settings, others))
            labels = [dialog.target.itemText(i) for i in range(dialog.target.count())]
            self.assertEqual(labels[0], "Nothing yet (a blank map)")
            self.assertEqual(labels[1:3], ["Setting: Density", "Setting: Width"])
            self.assertEqual(labels[-1], "Other setting...")
            self.assertFalse(dialog.target.model().item(labels.index("Setting: Curl")).isEnabled())
            self.assertIsNone(dialog.values()[4])
            self.assertTrue(dialog.set_target("scatter", "density"))
            self.assertFalse(dialog.name.isEnabled())  # named for the setting
            self.assertEqual(dialog.values()[1:], ("vertices", 1024, False, ("scatter", "density")))
            self.assertTrue(dialog.set_target(*other))  # not a common one: under Other
            self.assertFalse(dialog.other.isHidden())
            self.assertEqual(dialog.values()[4], other)
            dialog.deleteLater()
            graph0 = cmds.getAttr(self.groom + ".graph")
            name = tab.map_for_setting("scatter", "density", kind="vertices")
            self.assertEqual(name, "scatter_density", window.status.text())
            self.assertEqual(groomlab.map_kind(self.groom, name), "vertices")
            self.assertIn(("scatter", "density"), groomlab.map_users(self.groom, name))
            self.assertEqual(tab.current(), name)
            ctx = groomlab.BRUSH_CONTEXT
            self.assertEqual(cmds.currentCtx(), ctx)
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, mode=True), "paint")
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, attribute=True), name)
            self.assertAlmostEqual(cmds.groomLabBrushCtx(ctx, query=True, value=True), 1.0)
            self.assertIn("Ctrl", window.status.text())
            self.assertEqual(groomlab.errors(self.groom), [])
            cmds.undo()  # the map, its link and the brush on it: one step
            self.assertEqual(cmds.getAttr(self.groom + ".graph"), graph0)
            self.assertNotIn(name, groomlab.list_maps(self.groom))
            cmds.redo()
            self.assertIn(name, groomlab.list_maps(self.groom))
            maps = groomlab.list_maps(self.groom)
            self.assertEqual(tab.map_for_setting("scatter", "density", kind="texture"), name)
            self.assertEqual(groomlab.list_maps(self.groom), maps)
        finally:
            window.stop_tool()
            window.close()

    def test_paint_tab_color_maps(self):
        # A color map: the tab shows a swatch and channel toggles instead of the value; each
        # change reaches the Paint tool; Fill fills the color in the ticked channels. No dialog
        # opens from scripted paths (the swatch's dialog is stubbed here).
        from PySide2 import QtCore as _qc, QtGui, QtWidgets
        from groomlab.ui import palette, tool_tabs
        cmds.select(self.groom)
        window = palette.show()
        getColor = QtWidgets.QColorDialog.getColor
        try:
            window.poll()
            window.show_tab("paint")
            tab = window.paint_tab
            # The New Map dialog offers Gray / Color, color only on the scalp (not shown: no exec).
            dialog = tool_tabs.NewMapDialog(tab, name="tint", kind="guides")
            self.assertFalse(dialog.values_box.isEnabled())
            dialog.kind.setCurrentIndex(1)  # vertices
            dialog.values_box.setCurrentIndex(1)
            self.assertEqual(dialog.values(), ("tint", "vertices", 1024, True, None))
            dialog.deleteLater()
            self.assertEqual(tab.make_map("tint", "vertices", color=True), "tint")
            self.assertEqual(tab.current(), "tint")
            self.assertEqual(tab.current_channels(), 4)
            self.assertIn("color", tab.list.currentItem().text())
            self.assertFalse(tab.list.currentItem().icon().isNull())
            self.assertTrue(tab.color_row.isVisibleTo(window))
            self.assertFalse(tab.value_row.isVisibleTo(window))
            # Painting: the tool gets the color and channels, and each change after.
            window.buttons["paint"].click()
            self.app_events()
            ctx = groomlab.BRUSH_CONTEXT
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, attribute=True), "tint")
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, paintChannels=True), "rgba")
            tab.set_paint_color((0.2, 0.4, 0.6))
            self.assertEqual([round(float(v), 3) for v in cmds.groomLabBrushCtx(ctx, query=True, paintColor=True).split()],
                             [0.2, 0.4, 0.6, 1.0])
            tab.channel_buttons["g"].click()  # untick green
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, paintChannels=True), "rba")
            for letter in "rba":  # unticking every channel keeps the last one
                tab.channel_buttons[letter].click()
            self.assertEqual(tab.paint_channels, "a")
            self.assertTrue(tab.channel_buttons["a"].isChecked())
            tab.set_paint_channels("rb")
            self.assertEqual(cmds.groomLabBrushCtx(ctx, query=True, paintChannels=True), "rb")
            # The swatch's click opens the color dialog (stubbed: it answers red at once).
            QtWidgets.QColorDialog.getColor = staticmethod(lambda *a, **k: QtGui.QColor.fromRgbF(1.0, 0.0, 0.0, 1.0))
            tab.swatch.click()
            self.assertEqual([round(float(v), 3) for v in cmds.groomLabBrushCtx(ctx, query=True, paintColor=True).split()],
                             [1.0, 0.0, 0.0, 1.0])
            # Fill: the color in the ticked channels (r and b), green kept at 0.
            tab.fill()
            values = cmds.groomLabScalpMaps(self.groom, values="tint")
            self.assertEqual([round(v, 3) for v in values[:4]], [1.0, 0.0, 0.0, 1.0])
            tab.set_paint_color((0.5, 0.5, 0.25, 1.0))
            tab.fill()
            self.assertEqual([round(v, 3) for v in cmds.groomLabScalpMaps(self.groom, values="tint")[:4]], [0.5, 0.0, 0.25, 1.0])
            # The scalp shows the color map in color, or one channel.
            tab.view_channel.setCurrentIndex(tab.view_channel.findData("b"))
            self.assertEqual(cmds.getAttr(self.groom + ".scalpMapChannel"), "b")
            # A gray map: the value again, no swatch; Make Color from the list turns it color.
            groomlab.add_map(self.groom, "mask", 0.0, kind="vertices")
            self.app_events()
            tab.select("mask")
            self.assertEqual(tab.current_channels(), 1)
            self.assertTrue(tab.value_row.isVisibleTo(window))
            self.assertFalse(tab.color_row.isVisibleTo(window))
            tab.set_channels(4)
            self.assertEqual(groomlab.map_channels(self.groom, "mask"), 4)
            self.assertEqual(tab.current(), "mask")
            self.assertTrue(tab.color_row.isVisibleTo(window))
            window.buttons["paint"].click()
        finally:
            QtWidgets.QColorDialog.getColor = getColor
            window.close()
        # The Graph Editor offers the groom's maps, color ones first, for HairColor's map.
        from groomlab.ui import graph_editor
        groomlab.add_map(self.groom, "gray", 0.0, kind="vertices")
        editor = graph_editor.GraphEditor(self.groom)
        try:
            editor.model.add_node("HairColor", name="color")
            names, _placeholder = editor.params.suggestions("color", "map")
            self.assertEqual(names, ["mask", "tint", "gray"])
            # And for Interpolate's regionMap; the region channel it then writes is offered to Clump.
            editor.model.add_node("Interpolate", name="regioned")
            names, _placeholder = editor.params.suggestions("regioned", "regionMap")
            self.assertEqual(names, ["mask", "tint", "gray"])
            editor.model.set_param("regioned", "regionMap", "tint")
            editor.model.add_node("Clump", name="clumped")
            names, _placeholder = editor.params.suggestions("clumped", "regionChannel")
            self.assertNotIn("region", names)  # not connected: it does not reach the Clump
            editor.model.connect("regioned", "clumped", "strands")
            names, _placeholder = editor.params.suggestions("clumped", "regionChannel")
            self.assertIn("region", names)
            self.assertIn({"name": "region", "node": "regioned"}, editor.channel_sources("clumped"))
        finally:
            editor.deleteLater()

    def test_paint_tab_marks_missing_images_and_relinks_them(self):
        # A texture map whose image is missing shows a warning marker naming the file it expects;
        # the list's menu relinks it (driven with a path here: the file dialog opens only without
        # one), reloads it and shows it in Explorer (stubbed: no window opens).
        import tempfile
        from groomlab.ui import palette, tool_tabs
        folder = tempfile.mkdtemp(prefix="groomlab_relinktab_").replace("\\", "/")
        cmds.workspace(folder, openWorkspace=True)
        groomlab.add_map(self.groom, "grow", 1.0, kind="texture", resolution=16)
        groomlab.add_map(self.groom, "fur", kind="vertices")
        image = groomlab.map_texture(self.groom, "grow")
        cmds.select(self.groom)
        window = palette.show()
        reveal, shown = tool_tabs.reveal_file, []
        try:
            window.poll()
            window.show_tab("paint")
            tab = window.paint_tab
            self.assertTrue(tab.select("grow"))
            self.assertEqual([a[0] for a in tab.image_actions()], ["Relink Image...", "Reload Image", "Show in Explorer"])
            self.assertEqual(tab.current_missing(), "")
            self.assertFalse(tab.list.currentItem().text().startswith("\u26a0"))
            moved = folder + "/elsewhere/grow.png"
            os.makedirs(os.path.dirname(moved))
            os.rename(image, moved)
            tab.refresh()
            item = tab.list.currentItem()
            self.assertEqual(tab.current(), "grow")
            self.assertTrue(item.text().startswith("\u26a0"), item.text())
            self.assertIn(image, item.toolTip())
            self.assertIn("Relink Image", item.toolTip())
            self.assertEqual(tab.current_missing(), image)
            self.assertEqual(tab.relink_image(moved), "elsewhere/grow.png")
            self.app_events()
            self.assertEqual(tab.current(), "grow")
            self.assertFalse(tab.list.currentItem().text().startswith("\u26a0"))
            self.assertIn("grow reads elsewhere/grow.png", window.status.text())
            self.assertEqual(tab.reload_image(), [moved])
            tool_tabs.reveal_file = lambda path: shown.append(path) or path
            self.assertEqual(tab.show_in_explorer(), moved)
            self.assertEqual(shown, [moved])
            # A vertex map offers Relink only, and becomes a texture map reading the image.
            self.assertTrue(tab.select("fur"))
            self.assertEqual([a[0] for a in tab.image_actions()], ["Relink Image..."])
            self.assertEqual(tab.show_in_explorer(), "")
            tab.relink_image(moved, copy=True)
            self.assertEqual(groomlab.map_kind(self.groom, "fur"), "texture")
            self.assertEqual(tab.current_kind(), "texture")
        finally:
            tool_tabs.reveal_file = reveal
            window.close()

    def app_events(self):
        from PySide2 import QtWidgets
        for _ in range(5):
            QtWidgets.QApplication.processEvents()


class ReferenceTabTest(unittest.TestCase):
    """Groom Tools > Reference: photos as cameras, listed and driven from the tab (no dialogs: the
    file dialog opens only from the button's click; import_photo takes the path)."""

    def setUp(self):
        import tempfile
        from groomlab.ui import palette
        from PySide2 import QtGui
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.folder = tempfile.mkdtemp(prefix="groomlab_reftab_")
        cmds.workspace(self.folder, openWorkspace=True)
        self.scalp = cmds.polySphere(radius=10, subdivisionsX=30, subdivisionsY=15)[0]
        self.groom = groomlab.create_groom(self.scalp)
        cmds.select(self.groom)
        image = QtGui.QImage(64, 48, QtGui.QImage.Format_RGB32)
        image.fill(QtGui.QColor(90, 60, 40))
        self.photo = os.path.join(self.folder, "front.png").replace("\\", "/")
        self.assertTrue(image.save(self.photo))
        self.panel = next(p for p in cmds.getPanel(type="modelPanel")
                          if cmds.modelPanel(p, query=True, camera=True) == "persp")
        cmds.setFocus(self.panel)
        self.tools = palette.ToolPalette()
        self.tools.show()
        self.tools.show_tab("photos")
        self.tools.poll()
        self.tab = self.tools.reference_tab

    def tearDown(self):
        cmds.lookThru(self.panel, "persp")
        cmds.setToolTo("selectSuperContext")
        self.tools.close()
        self.tools.deleteLater()

    def chip(self, name):
        from PySide2 import QtWidgets
        return self.tab.findChild(QtWidgets.QToolButton, name)

    def test_the_tab_lists_references_and_drives_them(self):
        from groomlab import reference
        from groomlab.ui import palette
        self.assertEqual(palette.TABS, ["tools", "paint", "correctives", "photos"])
        self.tools.show_tab("sculpt")  # the old ids still work
        self.assertEqual(self.tools.tabs.currentIndex(), palette.TABS.index("correctives"))
        self.tools.show_tab("reference")
        self.assertEqual(self.tools.tabs.currentIndex(), palette.TABS.index("photos"))
        self.assertTrue(self.tab.isVisibleTo(self.tools))
        self.assertFalse(self.tab.look.isEnabled())  # nothing to look through yet
        cam = self.tab.import_photo(self.photo)
        self.assertEqual(reference.list_references(), [cam])
        self.assertEqual((self.tab.list.count(), self.tab.current()), (1, cam))
        self.assertIn("64 x 48", self.tab.list.item(0).text())
        self.assertFalse(self.tab.list.item(0).icon().isNull())  # the photo's thumbnail
        self.assertIn("Align Photo", self.tools.status.text())
        # The controls call through to the camera and its image plane.
        plane = reference.image_plane(cam)
        self.tab.opacity.committed.emit(0.3)
        self.assertAlmostEqual(cmds.getAttr(plane + ".alphaGain"), 0.3)
        self.tab.side_buttons["behind"].click()
        self.assertEqual(reference.reference_depth(cam), "behind")
        self.tab.side_buttons["front"].click()
        self.assertEqual(reference.reference_depth(cam), "front")
        self.tab.visible.setChecked(False)
        self.assertFalse(reference.reference_visible(cam))
        self.tab.visible.setChecked(True)
        # The focal length by hand: the camera takes it and Keep focal length turns on (a solve keeps it).
        shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
        self.assertFalse(reference.keep_focal(cam))
        self.tab.focal.committed.emit(85.0)
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength"), 85.0)
        self.assertTrue(reference.keep_focal(cam))
        # A focal length gone wrong: Reset Focal Length (the slider's right-click menu) brings the default
        # lens and the slider's starting range back.
        self.tab.focal.setValue(3000.0)  # (as a drag past the range would)
        self.tab.focal.committed.emit(3000.0)
        self.assertGreaterEqual(self.tab.focal.hi, 3000.0)
        self.tab.reset_focal()
        self.assertAlmostEqual(cmds.getAttr(shape + ".focalLength"), reference.DEFAULT_FOCAL)
        self.assertEqual((self.tab.focal.lo, self.tab.focal.hi), reference.FOCAL_RANGE)
        reference.set_keep_focal(cam, False)
        self.tab.lock.click()
        self.assertTrue(reference.is_locked(cam))
        self.tab.show_controls()
        self.assertFalse(self.tab.focal.isEnabled())  # a locked camera keeps its focal length
        self.assertIn("locked", self.tab.list.item(0).text())
        self.tab.lock.click()
        self.assertFalse(reference.is_locked(cam))
        # Look Through and back; the button follows the viewport.
        self.tab.look.click()
        self.assertEqual(reference.current_reference(self.panel), cam)
        self.assertTrue(self.tab.look.isChecked())
        self.assertEqual(self.tab.look.text(), "Back to Perspective")
        self.tab.look.click()
        self.assertEqual(cmds.modelPanel(self.panel, query=True, camera=True), "persp")
        self.assertFalse(self.tab.look.isChecked())
        # The list follows a reference looked through elsewhere.
        other = reference.import_reference(self.photo, name="side")
        self.tab.poll()
        self.assertEqual((self.tab.list.count(), self.tab.current()), (2, cam))
        reference.look_through(other, self.panel)
        self.tab.poll()
        self.assertEqual(self.tab.current(), other)
        self.assertTrue(self.tab.look.isChecked())
        # Looking through one, picking the other looks through it; Back to Perspective still goes back.
        self.tab.select(cam)
        self.assertEqual(reference.current_reference(self.panel), cam)
        self.tab.poll()
        self.assertEqual(self.tab.current(), cam)
        self.assertTrue(self.tab.look.isChecked())
        self.tab.look.click()
        self.assertEqual(cmds.modelPanel(self.panel, query=True, camera=True), "persp")
        self.tab.poll()
        self.assertFalse(self.tab.look.isChecked())
        self.tab.select(other)  # not looking through: picking only picks
        self.assertEqual(cmds.modelPanel(self.panel, query=True, camera=True), "persp")
        # Remove is one undo step (no question asked: undo brings it back).
        self.tab.select(other)
        self.tab.remove()
        self.assertEqual(reference.list_references(), [cam])
        self.assertEqual(self.tab.list.count(), 1)
        cmds.undo()
        self.tab.poll(full=True)  # (the tick that rereads the scene every few seconds)
        self.assertEqual(self.tab.list.count(), 2)

    def test_align_photo_opens_the_window_and_the_tab_shows_where_it_stands(self):
        """The pairs are made in the Photos window (Align Photo...); the tab says how far along they are."""
        from PySide2 import QtWidgets
        from groomlab import reference
        from groomlab.ui import reference_window
        from maya.api import OpenMaya as om
        cam = self.tab.import_photo(self.photo)
        shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
        self.tools.poll()
        self.assertIn("Not lined up yet", self.tab.align_about.text())
        # Pairs from a known camera (as the window's clicks would make them).
        cmds.xform(cam, worldSpace=True, translation=(12, 8, 40), rotation=(-10, 16, 0))
        cmds.setAttr(shape + ".focalLength", 50)
        inverse = om.MSelectionList().add(cam).getDagPath(0).inclusiveMatrixInverse()
        xyz = cmds.xform(self.scalp + ".vtx[*]", query=True, worldSpace=True, translation=True)
        points = [p for p in (tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)) if p[2] > 3.0][::9][:7]
        k = 50.0 / 25.4
        for i, p in enumerate(points[:3]):
            c = om.MPoint(*p) * inverse
            u = (k * c.x / -c.z / cmds.getAttr(shape + ".horizontalFilmAperture") + 0.5) * 64
            v = (0.5 - k * c.y / -c.z / cmds.getAttr(shape + ".verticalFilmAperture")) * 48
            reference.set_pair(cam, i, image=(u, v), mesh=p)
        self.tab.poll(full=True)  # pairs made elsewhere: the full tick shows them
        self.assertIn("3 of 6 pairs", self.tab.align_about.text())
        reference.set_keep_focal(cam, True)
        self.tab.poll(full=True)
        self.assertIn("3 of 4 pairs (focal length kept)", self.tab.align_about.text())
        reference.set_keep_focal(cam, False)
        for i, p in enumerate(points[3:], 3):
            c = om.MPoint(*p) * inverse
            u = (k * c.x / -c.z / cmds.getAttr(shape + ".horizontalFilmAperture") + 0.5) * 64
            v = (0.5 - k * c.y / -c.z / cmds.getAttr(shape + ".verticalFilmAperture")) * 48
            reference.set_pair(cam, i, image=(u, v), mesh=p)
        cmds.xform(cam, worldSpace=True, translation=(0, 0, 60), rotation=(0, 0, 0))
        cmds.setAttr(shape + ".focalLength", 35)
        reference.solve(cam)
        self.tab.poll(full=True)
        self.assertIn("Lined up by 7 pairs", self.tab.align_about.text())
        self.assertIn("50.0 mm", self.tab.align_about.text())
        # Align Photo...: the Photos window on the picked photo, its Align stage (landmarks only).
        self.tab.findChild(QtWidgets.QAbstractButton, "referenceAlign").click()
        window = reference_window.window()
        try:
            self.assertIsNotNone(window)
            self.assertEqual(reference._camera(window.current_photo()), reference._camera(cam))
            self.assertEqual((window.stage(), window.mode()), ("align", "landmarks"))
            self.assertFalse(window.guides_panel.isVisibleTo(window))
            self.assertFalse(window.generate_button.isVisibleTo(window))
            self.assertFalse(window.mode_buttons["flow"].isVisibleTo(window))
            self.assertEqual(window.pairs_tree.topLevelItemCount(), 7)
            self.assertAlmostEqual(window.focal.value(), cmds.getAttr(shape + ".focalLength"), places=3)
            # Guides from Photos opens the same window at its Guides stage.
            self.tab.findChild(QtWidgets.QToolButton, "referenceOpenWindow").click()
            self.assertIs(reference_window.window(), window)
            self.assertEqual(window.stage(), "guides")
            self.assertTrue(window.guides_panel.isVisibleTo(window))
            self.assertTrue(window.generate_button.isVisibleTo(window))
        finally:
            window.close()

    def test_the_tab_opens_the_reference_window_and_the_old_flow_tool_still_draws(self):
        """Guides from Reference lives in the Photos window's Guides stage: the tab keeps a way in and
        where things stand. The older Reference Flow tool (looking through the camera) still draws."""
        from PySide2 import QtWidgets
        from maya.api import OpenMaya as om
        import maya.api.OpenMayaUI as omui
        from groomlab import from_reference, reference
        from groomlab.ui import reference_window
        cam = self.tab.import_photo(self.photo)
        self.tools.poll()
        self.assertIn("0 landmark pairs, 0 flow arrows", self.tab.gen_hint.text())
        self.tab.findChild(QtWidgets.QToolButton, "referenceOpenWindow").click()
        window = reference_window.window()
        try:
            self.assertIsNotNone(window)
            self.assertEqual(window.groom, self.groom)
            self.assertEqual(reference._camera(window.current_photo()), reference._camera(cam))
            self.assertEqual(window.stage(), "guides")
        finally:
            window.close()
        wait(30)
        self.assertFalse(cmds.objExists(reference_window.UI_GROUP))
        # The Reference Flow tool: through the camera, a stroke from view coordinates (its hook).
        reference.look_through(cam, self.panel)
        from_reference.flow_tool(self.groom, cam)
        cmds.refresh(force=True)

        def to_view(u, v):
            shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
            depth = cmds.getAttr(reference.image_plane(cam) + ".depth")
            k = depth / (cmds.getAttr(shape + ".focalLength") / 25.4)
            local = om.MPoint((u / 64.0 - 0.5) * cmds.getAttr(shape + ".horizontalFilmAperture") * k,
                              (0.5 - v / 48.0) * cmds.getAttr(shape + ".verticalFilmAperture") * k, -depth)
            world = local * om.MSelectionList().add(cam).getDagPath(0).inclusiveMatrix()
            x, y, _ = omui.M3dView.active3dView().worldToView(world)
            return x, y

        stroke = " ".join("%d %d" % to_view(32.0, 8.0 + 3.0 * k) for k in range(11))
        cmds.groomLabReferenceFlowCtx(from_reference.FLOW_CONTEXT, edit=True, debugStroke=stroke)
        self.assertEqual(len(from_reference.reference_hints(self.groom, cam)), 1)
        self.tools.poll()
        self.assertIn("1 flow arrow", self.tab.gen_hint.text())


def widgets_alive(widget):
    from groomlab.ui import widgets
    return widgets.alive(widget)


class ReferenceWindowTest(unittest.TestCase):
    """GroomLab > Guides > Guides from Reference...: the Reference window (never modal). The photo
    view (a widget of its own: synthetic mouse and key events, no real cursor) and the mesh view (a
    Maya view: its event filter and pick), Wrap-style landmarks, the camera solving live and the
    mesh over the photo, flow arrows and mask touch-ups with the window's undo, storage on the
    camera and the groom, Generate, the measured look, following Maya's undo, closing."""

    def setUp(self):
        import tempfile
        from PySide2 import QtGui
        cmds.file(new=True, force=True)
        cmds.undoInfo(state=True, infinity=True)
        self.folder = tempfile.mkdtemp(prefix="groomlab_refwin_")
        cmds.workspace(self.folder, openWorkspace=True)
        self.scalp = cmds.polySphere(radius=10, subdivisionsX=30, subdivisionsY=15, name="head")[0]
        self.groom = groomlab.create_groom(self.scalp)
        cmds.select(self.groom)
        # A light backdrop, dark vertical strands where the ball shows.
        image = QtGui.QImage(160, 120, QtGui.QImage.Format_RGB32)
        image.fill(QtGui.QColor(230, 230, 235))
        for x in range(40, 120):
            for y in range(15, 115):
                image.setPixelColor(x, y, QtGui.QColor(70, 45, 30) if x % 3 == 0 else QtGui.QColor(115, 80, 55))
        self.photo = os.path.join(self.folder, "strands.png").replace("\\", "/")
        self.assertTrue(image.save(self.photo))
        self.window = None

    def tearDown(self):
        if self.window is not None and widgets_alive(self.window):
            self.window.close()
        wait(20)

    def mouse(self, widget, kind, pos, button=None, buttons=None, modifiers=None):
        from PySide2 import QtCore, QtGui, QtWidgets
        button = QtCore.Qt.LeftButton if button is None else button
        buttons = button if buttons is None else buttons
        modifiers = QtCore.Qt.NoModifier if modifiers is None else modifiers
        event = QtGui.QMouseEvent(kind, QtCore.QPointF(pos), button, buttons, modifiers)
        QtWidgets.QApplication.sendEvent(widget, event)

    def click_photo(self, u, v, modifiers=None):
        from PySide2 import QtCore
        view = self.window.photo_view
        at = view.to_widget(u, v)
        self.mouse(view, QtCore.QEvent.MouseButtonPress, at, modifiers=modifiers)
        self.mouse(view, QtCore.QEvent.MouseButtonRelease, at, buttons=QtCore.Qt.NoButton, modifiers=modifiers)

    def drag_photo(self, points, modifiers=None):
        from PySide2 import QtCore
        view = self.window.photo_view
        self.mouse(view, QtCore.QEvent.MouseButtonPress, view.to_widget(*points[0]), modifiers=modifiers)
        for p in points[1:]:
            self.mouse(view, QtCore.QEvent.MouseMove, view.to_widget(*p), button=QtCore.Qt.NoButton,
                       buttons=QtCore.Qt.LeftButton, modifiers=modifiers)
        self.mouse(view, QtCore.QEvent.MouseButtonRelease, view.to_widget(*points[-1]), buttons=QtCore.Qt.NoButton,
                   modifiers=modifiers)

    def key(self, widget, key, modifiers):
        from PySide2 import QtCore, QtGui, QtWidgets
        QtWidgets.QApplication.sendEvent(widget, QtGui.QKeyEvent(QtCore.QEvent.KeyPress, key, modifiers))

    def project(self, cam, p):
        """A world point through the reference camera: photo pixels (from the top-left, v down)."""
        from maya.api import OpenMaya as om
        from groomlab import reference
        c = om.MPoint(*p) * om.MMatrix(cmds.getAttr(cam + ".worldMatrix[0]")).inverse()
        f = reference.focal_px(cam)
        return 80.0 + f * c.x / -c.z, 60.0 - f * c.y / -c.z

    def test_landmarks_solve_live_and_the_photo_tools(self):
        from PySide2 import QtCore
        from maya.api import OpenMaya as om
        from groomlab import from_reference, reference
        from groomlab.ui import menu, reference_window
        self.window = window = menu.open_guides_from_reference(self.groom)
        wait(80)
        self.assertIs(window, reference_window.window())
        self.assertTrue(cmds.modelEditor(window.mesh_view.editor, exists=True))
        self.assertTrue(window.mesh_view.camera.startswith("|" + reference_window.UI_GROUP + "|"))
        self.assertIn(cmds.ls(self.scalp, long=True)[0], window.mesh_view.isolated())
        self.assertFalse(window.generate_button.isEnabled())  # no photo yet
        cam = window.import_photo(self.photo)
        self.assertEqual(window.tabs.count(), 1)
        self.assertIsNotNone(window.photo_view.pixmap)
        truth = {"t": cmds.xform(cam, query=True, worldSpace=True, translation=True),
                 "focal": cmds.getAttr(reference._shape(cam) + ".focalLength")}
        # Points on the ball's front and where the camera Import aimed sees them.
        points = [(0, 0, 10), (6, 4, 6.6), (-6, 4, 6.6), (5, -6, 6.1), (-5, -6, 6.1), (0, 8, 6), (3, 2, 9.3)]
        # On the mesh itself (a far 35 mm camera barely tells its distance from its focal length: the
        # points have to be where the photo shows them).
        from groomlab import transfer
        points = [tuple(transfer.closest_on_mesh(self.scalp, p)) for p in points]
        pixels = [self.project(cam, p) for p in points]
        # The camera moved off: the solve has to bring it back.
        cmds.xform(cam, worldSpace=True, translation=(truth["t"][0] + 4.0, truth["t"][1] - 3.0, truth["t"][2] + 6.0))
        # Pair 1: the photo first (a click in the photo view), the mesh through the mesh view's own
        # pick (a click on its widget, through the window's filter).
        self.click_photo(*pixels[0])
        self.assertEqual(len(window.landmarks("photo")), 1)
        self.assertLess(max(abs(a - b) for a, b in zip(window.landmarks("photo")[0], pixels[0])), 0.6)
        self.assertEqual(window.editor().pending_count, 1)
        cmds.refresh(force=True)
        view = window.view()
        x, y, inside = view.worldToView(om.MPoint(*points[0]))
        self.assertTrue(inside)
        widget = window.mesh_view.view_widget()
        scale = view.portWidth() / float(widget.width())
        at = QtCore.QPointF(x / scale, (view.portHeight() - y) / scale)
        self.mouse(widget, QtCore.QEvent.MouseButtonPress, at)
        self.mouse(widget, QtCore.QEvent.MouseButtonRelease, at, buttons=QtCore.Qt.NoButton)
        self.assertEqual(window.editor().pair_count, 1)
        self.assertLess(max(abs(a - b) for a, b in zip(window.landmarks("mesh")[0], points[0])), 0.6)
        self.assertEqual(len(window.marker_state()["points"]), 1)
        # The mesh side first for the next ones (either order); 3 pairs: no solve yet.
        for p, uv in zip(points[1:3], pixels[1:3]):
            window.add_landmark("mesh", p)
            self.click_photo(*uv)
        self.assertEqual(window.editor().pair_count, 3)
        self.assertFalse(window.solve_result()["ok"])
        self.assertIn("4 landmark pairs", window.solve_label.text())
        # The fourth: the camera solves (the focal length kept) and moves.
        window.add_landmark("mesh", points[3])
        self.click_photo(*pixels[3])
        solve = window.solve_result()
        self.assertTrue(solve["ok"], solve)
        self.assertFalse(solve["focal_solved"])
        self.assertIn("camera lined up", window.solve_label.text())
        self.assertGreater(len(window.overlay_lines()), 10)  # the outline over the photo
        moved = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        # (Back from 10 units off, within a few hundredths of its distance: turning and sliding sideways
        # look alike from 50 units away.)
        self.assertLess(max(abs(a - b) for a, b in zip(moved, truth["t"])), 1.5, (moved, truth))
        self.assertLess(solve["rms"], 1.0)
        # 6 pairs and more solve the focal length too. (On a photo this small, 160 pixels, a far 35 mm
        # camera's distance and focal length trade off within a pixel: only the fit is checked.)
        for p, uv in zip(points[4:], pixels[4:]):
            window.add_landmark("mesh", p)
            window.add_landmark("photo", uv)
        solve = window.solve_result()
        self.assertTrue(solve["focal_solved"])
        self.assertLess(solve["rms"], 1.0)
        window.keep_focal.setChecked(True)
        self.assertFalse(window.solve_result()["focal_solved"])
        self.assertTrue(reference.keep_focal(cam))  # kept on the camera (the tab and a script's solve read it)
        window.keep_focal.setChecked(False)
        self.assertFalse(reference.keep_focal(cam))
        # The focal length by hand: Keep focal length turns on and the camera lines up again keeping it.
        solved = cmds.getAttr(reference._shape(cam) + ".focalLength")
        window.focal.committed.emit(solved * 1.5)
        self.assertAlmostEqual(cmds.getAttr(reference._shape(cam) + ".focalLength"), solved * 1.5, places=3)
        self.assertTrue(window.keep_focal.isChecked())
        self.assertFalse(window.solve_result()["focal_solved"])
        self.assertTrue(window.solve_result()["ok"])
        self.assertTrue(window.reset_focal())  # the default lens, the slider's starting range
        self.assertAlmostEqual(cmds.getAttr(reference._shape(cam) + ".focalLength"), reference.DEFAULT_FOCAL, places=3)
        self.assertEqual((window.focal.lo, window.focal.hi), reference.FOCAL_RANGE)
        window.keep_focal.setChecked(False)  # 7 pairs: the focal length solved again
        self.assertTrue(window.solve_result()["focal_solved"])
        self.assertAlmostEqual(window.focal.value(), cmds.getAttr(reference._shape(cam) + ".focalLength"), places=3)
        # Lock: the landmarks no longer move the camera, nor does the focal length change.
        window.lock.click()
        self.assertTrue(reference.is_locked(cam))
        self.assertFalse(window.focal.isEnabled())
        before = cmds.xform(cam, query=True, worldSpace=True, translation=True)
        self.assertFalse(window.set_focal(20.0))
        window.add_landmark("photo", (5.0, 5.0))
        window.add_landmark("mesh", points[0])
        self.assertEqual(cmds.xform(cam, query=True, worldSpace=True, translation=True), before)
        self.assertIn("locked", window.message.text())
        window.undo()
        window.undo()
        window.lock.click()
        self.assertFalse(reference.is_locked(cam))
        self.assertTrue(window.focal.isEnabled())
        # The stages: Align shows the landmarks only; a Guides tool moves to the Guides stage.
        window.set_stage("align")
        self.assertFalse(window.guides_panel.isVisibleTo(window))
        self.assertFalse(window.mode_buttons["flow"].isVisibleTo(window))
        # Stored on the camera (pairs with their errors) as the old tab reads them.
        self.assertEqual(len(reference.complete_pairs(cam)), len(points))
        self.assertIsNotNone(reference.pairs(cam)[0].get("error"))
        self.assertEqual(window.pairs_tree.topLevelItemCount(), len(points))
        # A pair dragged off: it turns bad (red); Ctrl+Z puts it back, Ctrl+Y again.
        uv = window.landmarks("photo")[2]
        self.drag_photo([uv, (uv[0] + 4, uv[1]), (uv[0] + 8, uv[1] + 2)])
        self.assertTrue(window.solve_result()["bad"][2], window.solve_result())
        self.assertEqual(window.editor().undo_text, "move landmark")
        self.key(window.photo_view, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)
        self.assertLess(abs(window.landmarks("photo")[2][0] - uv[0]), 0.01)
        self.key(window.photo_view, QtCore.Qt.Key_Y, QtCore.Qt.ControlModifier)
        self.assertGreater(window.landmarks("photo")[2][0], uv[0] + 7)
        window.undo()
        # Ctrl+click deletes a pair (its partner too).
        self.click_photo(*window.landmarks("photo")[6], modifiers=QtCore.Qt.ControlModifier)
        self.assertEqual((len(window.landmarks("photo")), len(window.landmarks("mesh"))), (6, 6))
        window.undo()
        # Hover lights the partner on the mesh view's marker set.
        window.set_hover(("photo", 1))
        self.assertEqual(window.marker_state()["hover"], 1)
        window.set_hover(None)

        # Flow arrows: a drag root to tip, Ctrl+drag erases; each an undo step.
        window.set_mode("flow")
        self.assertEqual(window.stage(), "guides")
        self.assertTrue(window.guides_panel.isVisibleTo(window))
        self.drag_photo([(80, 30 + 7 * k) for k in range(11)])
        self.assertEqual(len(window.editor().strokes), 1)
        self.assertEqual(len(from_reference.reference_hints(self.groom, cam)), 1)  # stored on the groom
        self.assertLess(abs(from_reference.reference_hints(self.groom, cam)[0]["points"][0][1] - 30.0), 1.0)
        self.drag_photo([(60, 30 + 7 * k) for k in range(11)])
        self.drag_photo([(61, 60), (62, 62)], modifiers=QtCore.Qt.ControlModifier)
        self.assertEqual(len(window.editor().strokes), 1)
        self.assertEqual(window.editor().undo_text, "erase flow arrows")
        window.undo()
        self.assertEqual(len(window.editor().strokes), 2)
        window.undo()
        self.assertEqual(len(from_reference.reference_hints(self.groom, cam)), 1)

        # Mask touch-ups: backdrop over part of the strands, the preview's mask follows.
        window.set_overlay(mask=True, field=True)
        before = window.compute_preview()
        self.assertTrue(before and before["qlines"], before)
        window.set_mode("backdrop")
        self.drag_photo([(45, 20), (70, 20), (70, 50)])
        self.assertEqual(len(window.editor().mask_strokes), 1)
        self.assertFalse(window.editor().mask_strokes[0]["include"])
        self.assertEqual(len(from_reference.mask_strokes(self.groom, cam)), 1)
        after = window.compute_preview()
        self.assertLess(after["hair_share"], before["hair_share"])
        window.set_mode("hair")
        self.drag_photo([(130, 60), (140, 60)], modifiers=QtCore.Qt.ControlModifier)  # Ctrl: backdrop
        self.assertFalse(window.editor().mask_strokes[1]["include"])
        window.undo()
        window.grab_image(os.path.join(self.folder, "reference_window.png"))  # draws without trouble
        probe = os.environ.get("GROOMLAB_PROBE_DIR")
        if probe:
            window.grab_image(os.path.join(probe, "reference_window_test.png"))

        # Generate, the measured look, Maya's undo followed.
        window.gen_count.setValue(120)
        window.gen_symmetry.setCurrentIndex(window.gen_symmetry.findData("x"))
        window.gen_length.setValue(8.0)
        result = window.generate()
        self.assertTrue(result, window.message.text())
        groups = {g["name"]: g["count"] for g in groomlab.list_guide_groups(self.groom)}
        self.assertGreater(groups.get(from_reference.GROUP, 0), 10)
        self.assertIn("guides in fromReference", window.message.text())
        self.assertTrue(window.look_button.isEnabled())
        rows = window.preview_look()
        self.assertTrue(rows and window.look_panel.isVisible())
        self.assertIn("HairColor", window.look_types())
        self.assertTrue(window.apply_look())
        cmds.undo()  # the look
        cmds.undo()  # the guides
        wait(50)
        self.assertNotIn(from_reference.GROUP, [g["name"] for g in groomlab.list_guide_groups(self.groom)])
        self.assertFalse(window.look_button.isEnabled())
        self.assertEqual(len(window.landmarks("photo")), 7)  # the window's own edits are not Maya's
        # A second photo, removed, and Maya's undo brings it back with its landmarks.
        second = window.import_photo(self.photo)
        self.assertEqual(window.tabs.count(), 2)
        window.add_landmark("photo", (10, 10))
        window.select_photo(cam)
        self.assertEqual(len(window.landmarks("photo")), 7)
        window.select_photo(second)
        self.assertTrue(window.remove_photo())
        self.assertEqual(window.tabs.count(), 1)
        cmds.undo()
        wait(50)
        self.assertEqual(window.tabs.count(), 2)
        window.select_photo(second)
        self.assertEqual(window.landmarks("photo"), [[10.0, 10.0]])
        # Closing takes the view, its camera and markers along; Maya's undo queue is on.
        window.close()
        wait(30)
        self.assertIsNone(reference_window.window())
        self.assertFalse(cmds.objExists(reference_window.UI_GROUP))
        self.assertTrue(cmds.undoInfo(query=True, state=True))
