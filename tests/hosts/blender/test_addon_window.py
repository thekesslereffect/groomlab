# SPDX-License-Identifier: Apache-2.0
"""The GroomLab window's parts that run without a screen (window.py, uikit.py), in a headless Blender:

- typed values: arithmetic, a leading * or / on the current value, junk refused;
- the PNG reader on the add-on's own icons (what the window draws them from, without bpy.data);
- the theme read from the extension (the app's charcoal colors, as linear for the 3D view's overlay),
  its derived tokens (a theme with base tones only gets the app's formulas) and components' sizes;
- Prop over a real property (range, clamping, int rounding, text);
- the window's operators and gizmo group registered, Toggle shows and hides it;
- the sidebar's Groom Tools panel drawn into drawn.DrawnLayout (what the window lays out): its
  empty state's actions recorded as the sidebar draws them, on the window's cards, no tab row (the
  window's rail: palette.json's pages, their icons shipped).

Drawing and input need a window: build/probe/blender_window drives them with simulated events."""

import importlib
import os
import sys
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402

pkg, m = bu.enable()
uikit = importlib.import_module(bu.MODULE + ".uikit")
window = importlib.import_module(bu.MODULE + ".window")
drawn = importlib.import_module(bu.MODULE + ".drawn")
editors = importlib.import_module(bu.MODULE + ".editors")
blender_theme = importlib.import_module(bu.MODULE + ".blender_theme")
graph_tree = importlib.import_module(bu.MODULE + ".graph_tree")
palette = importlib.import_module(bu.MODULE + ".palette")


class TypedValues(unittest.TestCase):
    def test_arithmetic(self):
        self.assertEqual(uikit.evaluate("0.5*0.5"), 0.25)
        self.assertEqual(uikit.evaluate(" (2 + 3) * 4 "), 20.0)
        self.assertEqual(uikit.evaluate("-1.5"), -1.5)
        self.assertEqual(uikit.evaluate("2**3"), 8.0)
        self.assertEqual(uikit.evaluate("1,5"), 1.5)  # a decimal comma

    def test_relative_to_the_current_value(self):
        self.assertEqual(uikit.evaluate("*2", 30), 60.0)
        self.assertEqual(uikit.evaluate("/4", 30), 7.5)

    def test_junk_is_refused(self):
        for text in ("", "abc", "1/0", "__import__('os')", "2 +", "[1]"):
            with self.assertRaises(ValueError, msg=text):
                uikit.evaluate(text)


class Assets(unittest.TestCase):
    def test_icons_read(self):
        names = sorted(f[:-4] for f in os.listdir(uikit.ICON_DIR) if f.endswith(".png"))
        self.assertIn("logo", names)
        for name in names:
            w, h, rows = uikit.read_png(os.path.join(uikit.ICON_DIR, name + ".png"))
            self.assertEqual((w, h, len(rows)), (64, 64, 64), name)
            self.assertTrue(all(len(r) == w * 4 for r in rows), name)
            self.assertTrue(any(r[x] for r in rows for x in range(3, w * 4, 4)), name + ": all transparent")

    def test_font_and_theme_ship(self):
        self.assertTrue(os.path.isfile(uikit.FONT_FILE), uikit.FONT_FILE)
        self.assertTrue(os.path.isfile(uikit.THEME_FILE), uikit.THEME_FILE)
        t = uikit.Theme()
        self.assertAlmostEqual(t.panel[0], uikit.linear(0x17 / 255.0), places=5)  # charcoal's #17181B
        self.assertAlmostEqual(t.accent[2], uikit.linear(0xFF / 255.0), places=5)  # #8B7DFF
        self.assertEqual(t.row, 28.0)

    def test_derived_tokens_and_components(self):
        t = uikit.Theme()
        self.assertAlmostEqual(t.card[0], uikit.linear(0x1F / 255.0), places=5)  # charcoal's own ui.card
        self.assertAlmostEqual(t.accentSoft[2], uikit.linear(0x50 / 255.0), places=5)  # #2C2850
        self.assertEqual(t.size("iconRail", "width", 0), 44.0)
        self.assertEqual(t.size("iconRail", "padding", 0, 1), 10.0)
        self.assertEqual(t.role("chip", "onText", "text"), t.accentInk)
        self.assertEqual(t.role("segTrack", "track", "text"), t.sunken)
        # Base tones only: the derived formulas (charcoal.json's "derived"), its components Charcoal's.
        import json
        path = os.path.join(bu.TMP, "base_tones.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"ui": {"background": "#000000", "panel": "#202020", "raised": "#404040", "hover": "#606060",
                              "text": "#FFFFFF", "textDim": "#808080", "accent": "#FF0000"}}, f)
        b = uikit.Theme(path)
        mix = lambda a, c, k: a + (c - a) * k  # noqa: E731
        self.assertAlmostEqual(b.card[0], uikit.linear(mix(0x20, 0x40, 0.45) / 255.0), places=5)
        self.assertAlmostEqual(b.accentSoft[0], uikit.linear(mix(mix(0x20, 0x40, 0.45), 0xFF, 0.22) / 255.0), places=5)
        self.assertAlmostEqual(b.sunken[0], uikit.linear(mix(0x00, 0x20, 0.3) / 255.0), places=5)
        self.assertEqual(b.size("card", "radius", 0), 10.0)


class Props(unittest.TestCase):
    def test_prop_over_a_real_property(self):
        t = bpy.context.scene.groomlab_tools
        radius = uikit.Prop(t, "radius")
        self.assertEqual(radius.label, "Radius")
        self.assertEqual(radius.span(), (5.0, 400.0))  # its soft range: what a drag covers
        radius.set(120.6)
        self.assertEqual(t.radius, 121)
        radius.set(1e9)
        self.assertEqual(t.radius, 2000)  # clamped to the hard range
        self.assertEqual(radius.text(), "2000 px")  # a PIXEL int shows its unit
        self.assertEqual(radius.edit_text(), "2000")  # typing starts from the number alone
        strength = uikit.Prop(t, "strength")
        strength.set(0.25)
        self.assertEqual(strength.text(), "0.25")
        curve = uikit.Prop(t, "falloff_curve")
        self.assertIn("SMOOTH", [i[0] for i in curve.items()])


class Window(unittest.TestCase):
    def test_registered(self):
        self.assertTrue(hasattr(window.GROOMLAB_GGT_window, "bl_rna"))
        self.assertTrue(hasattr(window.GROOMLAB_GT_window, "bl_rna"))
        for op in ("window_toggle", "window_interact", "window_scroll"):
            self.assertTrue(hasattr(bpy.ops.groomlab, op), op)

    def test_toggle(self):
        window.state.shown = False
        self.assertEqual(bpy.ops.groomlab.window_toggle(), {"FINISHED"})
        self.assertTrue(window.state.shown)
        self.assertEqual(bpy.ops.groomlab.window_toggle(), {"FINISHED"})
        self.assertFalse(window.state.shown)


class Drawn(unittest.TestCase):
    def test_groom_tools_recorded(self):
        rec = drawn.DrawnLayout("column")
        drawn.draw_panel(palette.VIEW3D_PT_groomlab_tools, rec, bpy.context)
        items = []

        def walk(node):
            for child in node.children:
                if isinstance(child, drawn.DrawnLayout):
                    walk(child)
                else:
                    items.append(child)
        walk(rec)
        ops = [i.args["idname"] for i in items if i.kind == "operator"]
        self.assertIn("groomlab.add_groom", ops)  # the empty state's Create Groom (no groom in the file)
        props = [i.args["prop"] for i in items if i.kind == "prop"]
        self.assertIn("groom", props)  # the groom picker heads the panel
        self.assertEqual(drawn.icon_name("ADD"), "ui_plus")
        self.assertTrue(os.path.isfile(os.path.join(uikit.ICON_DIR, "ui_plus.png")))
        for name in set(drawn.ICONS.values()):
            self.assertTrue(os.path.isfile(os.path.join(uikit.ICON_DIR, name + ".png")), name)

    def test_window_rail_and_cards(self):
        rail = window._rail(bpy.context)
        self.assertEqual([p[0] for p in rail.pages], ["TOOLS", "PAINT", "CORRECTIVES", "PHOTOS"])
        for _ident, _label, _tip, icon in rail.pages:
            self.assertTrue(os.path.isfile(os.path.join(uikit.ICON_DIR, icon + ".png")), icon)
        rec = drawn.DrawnLayout("column")
        drawn.draw_panel(palette.VIEW3D_PT_groomlab_tools, rec, bpy.context)
        roles, props = [], []

        def walk(node):
            for child in node.children:
                if isinstance(child, drawn.DrawnLayout):
                    roles.append(child.groomlab_role)
                    walk(child)
                elif child.kind == "prop":
                    props.append(child.args["prop"])
        walk(rec)
        self.assertNotIn("tab", props)  # (the rail switches the pages)
        self.assertIn("primary", roles)  # the empty state's Create Groom in the accent


class Editors(unittest.TestCase):
    def test_tube_outline_matches_the_core(self):
        # The square's corners: smooth, a closed curve through every point (the core's denseCurve).
        pts = [(1.0, -1.0), (1.0, 1.0), (-1.0, 1.0), (-1.0, -1.0)]
        curve = editors.dense_curve(pts, True)
        self.assertEqual(len(curve), 4 * editors.samples_per_span(4))
        for p in pts:
            self.assertIn(p, curve)  # each span starts at its control point
        straight = editors.dense_curve(pts, False)
        self.assertAlmostEqual(straight[1][0], 1.0)  # straight edges stay on the side
        self.assertEqual(editors.partners([(0.5, 0.0), (0.0, 1.0), (-0.5, 0.0), (0.0, -1.0)]), [2, 1, 0, 3])

    def test_menus_draw_as_entries(self):
        entries = drawn.menu_entries("GROOMLAB_MT_tools_more", bpy.context)
        self.assertTrue(entries)
        texts = [e.get("text") for e in entries]
        self.assertIn("GroomLab Window", texts)
        self.assertTrue(all(e["kind"] in ("op", "check", "sub", "label", "sep") for e in entries))
        self.assertIsNone(drawn.menu_entries("NO_SUCH_MENU", bpy.context))


class CharcoalTheme(unittest.TestCase):
    def test_preset_from_the_theme_file(self):
        import xml.dom.minidom
        doc = xml.dom.minidom.parseString(blender_theme.preset_xml())
        editor = doc.getElementsByTagName("ThemeNodeEditor")[0]
        self.assertEqual(len(doc.getElementsByTagName("ThemeStyle")), 1)  # (Blender's loader asks for it)
        g = blender_theme.graph_colors()
        self.assertEqual(editor.getAttribute("node_backdrop"), g["node"].lower() + "ff")
        self.assertEqual(editor.getAttribute("node_selected"), g["accent"].lower())
        space = doc.getElementsByTagName("ThemeSpaceGeneric")[0]
        self.assertEqual(space.getAttribute("back"), g["canvas"].lower())
        self.assertIsNone(graph_tree.charcoal_colors())  # not Blender's theme: the graph keeps its colors

    def test_applied_theme_tints_the_graph(self):
        prefs = bpy.context.preferences
        self.assertEqual(bpy.ops.groomlab.theme_apply(), {"FINISHED"})
        try:
            self.assertTrue(prefs.themes[0].filepath.endswith(blender_theme.PRESET_NAME + ".xml"))
            back = blender_theme.rgb255(blender_theme.graph_colors()["canvas"])
            self.assertAlmostEqual(prefs.themes[0].node_editor.space.back[0], back[0] / 255.0, places=2)
            categories, ports, node_bg = graph_tree.charcoal_colors()
            self.assertEqual(ports["Strands"], blender_theme.rgb255(blender_theme.graph_colors()["ports"]["Strands"]))
        finally:
            bpy.ops.preferences.reset_default_theme()
        self.assertIsNone(graph_tree.charcoal_colors())


if __name__ == "__main__":
    bu.run(TypedValues, Assets, Props, Window, Drawn, Editors, CharcoalTheme)
