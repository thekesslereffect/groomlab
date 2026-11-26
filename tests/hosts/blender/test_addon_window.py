# SPDX-License-Identifier: Apache-2.0
"""The GroomLab window's parts that run without a screen (window.py, uikit.py), in a headless Blender:

- typed values: arithmetic, a leading * or / on the current value, junk refused;
- the PNG reader on the add-on's own icons (what the window draws them from, without bpy.data);
- the theme read from the extension (the app's charcoal colors, as linear for the 3D view's overlay),
  its derived tokens (a theme with base tones only gets the app's formulas) and components' sizes;
- Prop over a real property (range, clamping, int rounding, text);
- the window's operators and gizmo group registered, Toggle shows and hides it;
- the sidebar's Groom Tools panel drawn into drawn.DrawnLayout (what the window lays out): its
  empty state's actions recorded as the sidebar draws them, on the window's cards; the window's rail
  holds the groom picker and the header's buttons (the pages are labeled, in the content).

Drawing and input need a window: build/probe/blender_window drives them with simulated events."""

import importlib
import os
import sys
import types
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


class TypedLengths(unittest.TestCase):
    """A length field (uikit.Prop's): shown as the scene shows lengths, a bare number typed read in
    the unit it shows (a tester typed 0.15 in a field showing 0.15 mm and got 0.15 m)."""

    def units(self, system="METRIC", length_unit="ADAPTIVE", scale=1.0):
        return types.SimpleNamespace(system=system, length_unit=length_unit, scale_length=scale)

    def test_a_bare_number_is_in_the_shown_unit(self):
        units = self.units()
        shown = uikit.length_text(0.00015, units)
        self.assertEqual(shown.replace(" ", ""), "0.15mm")
        self.assertAlmostEqual(uikit.parse_length("0.15", shown, units), 0.00015, places=9)
        self.assertAlmostEqual(uikit.parse_length("0.3", shown, units), 0.0003, places=9)
        self.assertAlmostEqual(uikit.parse_length("2cm", shown, units), 0.02, places=7)  # its own unit
        self.assertAlmostEqual(uikit.parse_length("0.15*2", shown, units), 0.0003, places=9)

    def test_the_scenes_length_unit(self):
        units = self.units(length_unit="CENTIMETERS")
        shown = uikit.length_text(0.15, units)
        self.assertEqual(shown, "15 cm")
        self.assertAlmostEqual(uikit.parse_length("20", shown, units), 0.2, places=7)
        self.assertEqual(uikit.length_text(0.00015, units), "0.015 cm")

    def test_the_unit_scale(self):
        units = self.units(length_unit="CENTIMETERS", scale=0.01)  # a Blender unit is a centimeter
        shown = uikit.length_text(15.0, units)
        self.assertEqual(shown, "15 cm")
        self.assertAlmostEqual(uikit.parse_length("2", shown, units), 2.0, places=6)

    def test_no_unit_system(self):
        units = self.units(system="NONE")
        self.assertEqual(uikit.length_text(0.15, units), "0.15")
        self.assertAlmostEqual(uikit.parse_length("0.3", "0.15", units), 0.3)


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
        t = uikit.Theme(uikit.THEME_FILE)  # (the components' file: Charcoal's)
        self.assertAlmostEqual(t.panel[0], uikit.linear(0x1C / 255.0), places=5)  # charcoal's #1C1C1C
        self.assertAlmostEqual(t.accent[2], uikit.linear(0xEF / 255.0), places=5)  # #5B8DEF
        self.assertEqual(t.row, 26.0)  # (its ui.rowHeight)

    def test_derived_tokens_and_components(self):
        t = uikit.Theme(uikit.THEME_FILE)
        self.assertAlmostEqual(t.card[0], uikit.linear(0x20 / 255.0), places=5)  # charcoal's own ui.card #202020
        self.assertAlmostEqual(t.accentSoft[2], uikit.linear(0x5E / 255.0), places=5)  # #35435E
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


class Themes(unittest.TestCase):
    """GroomLab > Theme in Blender (themes.py), as Maya's and the app's: the shipped themes listed,
    one picked restyles the window and the Graph Editor's canvas and is remembered, a theme sets
    only some keys over the one it extends, and the menu lists them with the active one checked."""

    def tearDown(self):
        themes = importlib.import_module(bu.MODULE + ".themes")
        themes.set_theme(themes.DEFAULT)

    def test_pick_a_theme(self):
        import json
        themes = importlib.import_module(bu.MODULE + ".themes")
        canvas = importlib.import_module(bu.MODULE + ".graph_canvas")
        menu = importlib.import_module(bu.MODULE + ".menu")
        self.assertTrue({"charcoal", "paper", "playful", "studio"} <= set(themes.list_themes()))
        # A user who never picked a theme gets Playful (the window, its tiles, the Theme menu).
        self.assertEqual(themes.DEFAULT, "playful")
        self.assertEqual(themes.active(), "playful")
        playful_panel = uikit._srgb(themes.load("playful")["ui"]["panel"], {})
        self.assertEqual(uikit._theme().panel, uikit._linear_rgba(playful_panel))
        self.assertTrue(uikit._theme().tool_on_category)
        self.assertEqual(uikit._theme().size("iconRail", "width", 0), 44.0)  # (its components: Charcoal's file's)
        dark = uikit._theme().panel
        canvas_dark = canvas.theme()["canvas"]
        themes.set_theme("paper")
        self.assertEqual(themes.active(), "paper")
        self.assertNotEqual(uikit._theme().panel, dark)
        self.assertGreater(sum(uikit._theme().panel[:3]), sum(dark[:3]))  # Paper is light
        self.assertNotEqual(canvas.theme()["canvas"], canvas_dark)
        with open(themes._settings_path(), encoding="utf-8") as f:
            self.assertEqual(json.load(f), {"theme": "paper"})  # remembered across sessions
        themes._active["name"] = None
        self.assertEqual(themes.active(), "paper")
        with self.assertRaises(ValueError):
            themes.set_theme("no_such_theme")
        self.assertEqual(themes.active(), "paper")  # (a theme that won't load changes nothing)
        # The Theme menu: every theme, the active one checked; the menu's other actions bound.
        items = menu.resolve([{"ref": "menus/theme"}], bpy.context)
        sub = next(i for i in items if i["kind"] == "submenu")
        listed = [(i["value"], bool(i["entry"].get("checked"))) for i in sub["items"] if i["kind"] == "action"
                  and i["id"] == "theme.set"]
        self.assertIn(("paper", True), listed)
        self.assertIn(("charcoal", False), listed)
        self.assertIn(("playful", False), listed)
        self.assertTrue({"theme.new", "theme.reload", "theme.openFolder"} <= {i["id"] for i in sub["items"]
                                                                               if i["kind"] == "action"})
        # The Groom Tools' More menu (palette.json's header "more": its themeMenu generator) and the
        # Graph Editor's Edit menu hold the same Theme submenu, as Maya's and the app's do.
        defs = importlib.import_module(bu.MODULE + ".definitions")
        more = menu.resolve(defs.get("palette/header/more")["items"], bpy.context)
        theme_sub = [i for i in more if i["kind"] == "submenu"]
        self.assertEqual([i["label"] for i in theme_sub], ["Theme"])
        self.assertIn("paper", [i.get("value") for i in theme_sub[0]["items"]])
        self.assertTrue(hasattr(bpy.types, menu.submenu_idname("more", "menus/theme")))  # (its class, to draw it)
        gu = importlib.import_module(bu.MODULE + ".graph_ui")
        ge = importlib.import_module(bu.MODULE + ".graph_editor")
        edit = gu.prepare(gu.resolve(gu.get("graph/editMenu"), {"groom"}, generators={
            "units": lambda e: [], "themeMenu": menu.theme_menu, "themes": themes.menu_entries}), gu.MenuContext(), "edit")
        sub = [e for e in edit if e[0] == "menu" and e[1] == "Theme"]
        self.assertTrue(sub)
        ops = gu._submenus[sub[0][2]][1]
        self.assertIn(("groomlab.theme_set", "paper"), [(e[2], e[3].get("name")) for e in ops if e[0] == "op"
                                                         and e[3].get("name") == "paper"])
        self.assertTrue(ge)
        # Tool tiles in their group's color (the theme's "icons", as the app's toolIconColor): Playful's
        # four groups apart, the guide tools paint's; Charcoal's one gray; Playful's picked tile washed.
        playful = uikit.Theme(data=themes.load("playful"))
        groups = {name: playful.tool_color(name) for name in ("place", "comb", "cut", "paint", "select", "freeze")}
        self.assertEqual(len({groups[n] for n in ("place", "comb", "cut", "paint")}), 4)
        self.assertEqual(groups["select"], groups["paint"])
        freeze = uikit._srgb(themes.load("playful")["icons"]["freeze"], {})  # (Freeze's own: icons.freeze)
        self.assertEqual(groups["freeze"], uikit._linear_rgba(freeze))
        emerald = uikit._srgb(themes.load("playful")["palette"]["emerald"], {})
        self.assertEqual(groups["place"], uikit._linear_rgba(emerald))
        self.assertTrue(playful.tool_on_category)
        charcoal = uikit.Theme(data=themes.load("charcoal"))
        self.assertEqual(len({charcoal.tool_color(n) for n in ("place", "comb", "cut", "paint")}), 1)
        self.assertFalse(charcoal.tool_on_category)
        # A tile draws its tool in two tones: its group's color and the theme's ink, picked or not in
        # Playful (its wash says it is picked); picked in Charcoal, the accent's ink; greyed, muted.
        tile = uikit.Tile("tile:comb", (0, 0, 46, 50), "Comb", None, "comb", False)
        self.assertEqual(tile.tones(playful), (playful.tool_color("comb"), playful.icon_ink))
        self.assertNotEqual(uikit.Tile("t", (0, 0, 46, 50), "Cut", None, "cut", False).tones(playful)[0],
                            tile.tones(playful)[0])  # (Trim's red, Shape's blue)
        tile.active = True
        self.assertEqual(tile.tones(playful), (playful.tool_color("comb"), playful.icon_ink))
        self.assertEqual(tile.tones(charcoal), (charcoal.accentInk, charcoal.accentInk))
        tile.disabled = True
        self.assertEqual(tile.tones(playful), (playful.textMuted, playful.textMuted))
        self.assertEqual(uikit.Tile.ICON, 28)  # (Maya's tiles' size)
        # A theme of the user's own: only its accent, the rest from the theme it extends.
        path = themes.create_theme("probe_accent", base="charcoal")
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"name": "Probe", "extends": "charcoal", "ui": {"accent": "#FF0000"}}, f)
        themes.set_theme("probe_accent")
        t = uikit._theme()
        self.assertAlmostEqual(t.accent[0], 1.0, places=5)
        self.assertAlmostEqual(t.panel[0], uikit.linear(0x1C / 255.0), places=5)  # Charcoal's panel kept (#1C1C1C)
        self.assertTrue(data.get("ui"))
        os.remove(path)


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
    def test_toggles_as_pills(self):
        """The brush's toggles (palette.pills): in the window pills, as the Simple mock's chips (each
        as wide as its name, a row less 2 px tall, 6 px apart, wrapping); in the sidebar toggle
        buttons in left-aligned rows, as many as fit the width."""
        t = bpy.context.scene.groomlab_tools
        switches = [(t, "symmetry", "Mirror"), (t, "pressure", "Pressure"), (t, "collide", "Keep off scalp"),
                    (t, "stretch", "Stretch")]
        rec = drawn.DrawnLayout("column")
        palette.pills(rec, bpy.context, switches)
        self.assertEqual(rec.children[-1].groomlab_role, "pills")
        ui = types.SimpleNamespace(s=1.0, row=26.0, widgets=[], w=200.0,
                                   win=types.SimpleNamespace(theme=uikit.Theme()))
        h = drawn.Placer(ui, bpy.context, window.ACTIONS).place(rec, 0.0, 0.0, 200.0, drawn._State())
        pills = [w for w in ui.widgets if isinstance(w, uikit.Pill)]
        self.assertEqual([p.text for p in pills], ["Mirror", "Pressure", "Keep off scalp", "Stretch"])
        self.assertLess(pills[0].rect[2], pills[2].rect[2])  # each as wide as its name
        self.assertTrue(all(p.rect[3] == 24.0 for p in pills))
        tops = sorted({p.rect[1] for p in pills}, reverse=True)
        self.assertGreater(len(tops), 1)  # wrapped onto a second row
        first = [p for p in pills if p.rect[1] == tops[0]]
        self.assertEqual(first[0].rect[0], 0.0)
        for a, b in zip(first, first[1:]):
            self.assertAlmostEqual(b.rect[0] - (a.rect[0] + a.rect[2]), uikit.Pill.GAP)
        self.assertTrue(all(p.rect[0] + p.rect[2] <= 200.0 + 1e-3 for p in pills))
        self.assertAlmostEqual(h, len(tops) * 24.0 + (len(tops) - 1) * uikit.Pill.GAP)

        class Rows(object):  # (Blender's layout, its rows recorded)
            def __init__(self):
                self.rows = []

            def column(self, align=False):
                return self

            def row(self, align=False):
                r = types.SimpleNamespace(alignment="EXPAND", props=[])
                r.prop = lambda data, prop, **kw: r.props.append(prop)
                self.rows.append(r)
                return r
        side = Rows()
        palette.pills(side, bpy.context, switches, width=220)
        self.assertEqual([r.props for r in side.rows], [["symmetry", "pressure"], ["collide", "stretch"]])
        self.assertTrue(all(r.alignment == "LEFT" for r in side.rows))

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
        self.assertIn("groomlab.add_sample", ops)  # its sample meshes, as chips
        labels = [i.args["text"] for i in items if i.kind == "label"]
        self.assertTrue([x for x in labels if x.startswith(("Selected: ", "Nothing selected"))], labels)  # what is selected now
        props = [i.args["prop"] for i in items if i.kind == "prop"]
        self.assertNotIn("groom", props)  # the groom picker: on the rail
        rail = []

        def walk_rail(node):
            for child in node.children:
                if isinstance(child, drawn.DrawnLayout):
                    walk_rail(child)
                else:
                    rail.append(child)
        walk_rail(window.rail_content(bpy.context))
        self.assertEqual([i.args["prop"] for i in rail if i.kind == "prop"], ["groom"])
        self.assertIn("groomlab.find_command", [i.args["idname"] for i in rail if i.kind == "operator"])
        self.assertEqual(drawn.icon_name("ADD"), "ui_plus")
        self.assertTrue(os.path.isfile(os.path.join(uikit.ICON_DIR, "ui_plus.png")))
        for name in set(drawn.ICONS.values()):
            self.assertTrue(os.path.isfile(os.path.join(uikit.ICON_DIR, name + ".png")), name)

    def test_window_rail_and_cards(self):
        rail = window._rail(bpy.context)
        self.assertEqual(rail.pages, [])  # (the pages: labeled, at the top of the content, as the sidebar's)
        t = bpy.context.scene.groomlab_tools
        self.assertEqual([i[0] for i in uikit.Prop(t, "tab").items()], ["TOOLS", "PAINT", "PHOTOS"])
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
        self.assertIn("primary", roles)  # the empty state's Create Groom in the accent
        # The header's buttons on the rail's right end (issue 73: they don't scroll), not in the column.
        self.assertIs(rail.extras, window._rail_extras)

        def named(node, kind, key):
            return [c for c in node.children if isinstance(c, drawn.DrawnLayout) and named(c, kind, key)] + [
                c for c in node.children if not isinstance(c, drawn.DrawnLayout) and c.kind == kind and
                c.args.get(key) in ("GROOMLAB_MT_tools_more", "groomlab.graph_open")]
        self.assertFalse(named(rec, "menu", "menu"))
        row = drawn.DrawnLayout("row")
        palette.header_buttons(row, bpy.context)
        self.assertTrue(named(row, "menu", "menu") and named(row, "operator", "idname"))
        # A list row's menu in the window: drawn as the window's menus (the row picked first).
        placer = drawn.Placer(types.SimpleNamespace(s=1.0, row=28.0, widgets=[]), bpy.context, window.ACTIONS)
        op = drawn.Item("operator", idname="groomlab.row_popup", text="", icon="ui_more", props=drawn.OperatorProperties(),
                        emboss=False, depress=False)
        op.args["props"].rows, op.args["props"].index, op.args["props"].menu = "GROUPS", 0, "GROOMLAB_MT_group_context"
        button = placer.operator(op, (0, 0, 24, 24), drawn._State())
        self.assertIsInstance(button, drawn.MenuButton)
        self.assertEqual(button.menu_name, "GROOMLAB_MT_group_context")


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
    bu.run(TypedValues, TypedLengths, Assets, Props, Themes, Window, Drawn, Editors, CharcoalTheme)
