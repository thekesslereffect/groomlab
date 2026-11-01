# SPDX-License-Identifier: Apache-2.0
"""The GroomLab window's parts that run without a screen (window.py, uikit.py), in a headless Blender:

- typed values: arithmetic, a leading * or / on the current value, junk refused;
- the PNG reader on the add-on's own icons (what the window draws them from, without bpy.data);
- the theme read from the extension (the app's charcoal colors, as linear for the 3D view's overlay);
- Prop over a real property (range, clamping, int rounding, text);
- the window's operators and gizmo group registered, Toggle shows and hides it;
- the sidebar's Groom Tools panel drawn into drawn.DrawnLayout (what the window lays out): its
  empty state's actions recorded as the sidebar draws them.

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
        self.assertAlmostEqual(t.panel[0], uikit.linear(0x1C / 255.0), places=5)  # charcoal's #1C1C1C
        self.assertAlmostEqual(t.accent[2], uikit.linear(0xEF / 255.0), places=5)  # #5B8DEF
        self.assertEqual(t.row, 26.0)


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


if __name__ == "__main__":
    bu.run(TypedValues, Assets, Props, Window, Drawn)
