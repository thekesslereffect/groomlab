# SPDX-License-Identifier: GPL-3.0-or-later
"""Groom Tools: Maya's GroomLab palette (hosts/maya/scripts/groomlab/ui/palette.py) as the 3D
view sidebar's GroomLab tab, built from the shared UI definitions (engine/ui/*.json: palette.json's
header, empty state, pages, the tool cards' texts and the sections; lists.json's header actions and
row menus; choices.json's option lists), in the "Simple" redesign's order
(research/ui-redesign/SIMPLE.md):

- the groom picker and its buttons: the hair's eye, the guides' eye with how they draw beside it
  (a popover: flow, tubes, width, points, colors, smoothing, the Tube brush's tubes), the Graph
  Editor, Find Command and More; then the pages, labeled: Tools, Paint, Photos;
- Tools: the brushes in their trays (no group names), then the brush's card, then the Guide
  groups and the Correctives (one line per row, nothing expands: a row's settings and its menu open
  from icons on it, in one order on every list);
- Paint: the Paint card (the page is painting: it starts Paint on the picked map), then the maps;
- Photos: photos.py's.

The card: one header row (the brush's icon and name, and what the next stroke edits), Radius and
Strength, the brush's essentials, one row of toggles (Mirror first), then More settings (folded).
The Tube brush (Maya's Simple card): the levels' row, what it paints, Radius and Strength, its
toggles and More settings, then the Profile (Root, Mid and Tip, always shown) and the selected
tubes (folded). The Roots brush: the levels' row and the root tools (Draw, Edit, Select root).

An action shows when actions.py binds it (the rest is hidden). Drawing never writes Blender data:
the lists (groups, maps, parts, tubes, correctives) are refreshed by the tools after each edit, or
by a timer when the panel shows another groom than they were filled for."""

import re
import json
import os

import bpy
from bpy.props import EnumProperty, IntProperty, StringProperty

from . import agent_link, groom, icons, menu, photos, process, tool_props, tools, updates
from . import definitions as defs
from .ops import active_groom
from .ui import _wrap

NO_TOOL = "Pick a tool, then drag over the groom in the viewport."
# Blender's keys (palette.json's gestures are Maya's: middle-drag, the hotkey set).
GESTURES = ("B + drag: radius · M + drag: strength · Ctrl: the opposite (Length shortens, Lift lays down, Shape shrinks; "
            "painting, toward 0) · Shift+drag: smooth (the guides, a painted map, the Tube brush's channel) · select "
            "guides with the Select tool")
TABS = {"TOOLS": "tools", "PAINT": "paint", "PHOTOS": "photos"}  # palette.json's pages (Correctives: a Tools section)
_lists_for = [None]


# Characters a wrapped line holds when the panel draws elsewhere than the sidebar (the GroomLab
# window, window.py, sets it while it draws the panel): None for the sidebar's own width.
CHARS = [None]


def _chars(context):
    if CHARS[0]:
        return CHARS[0]
    region = getattr(context, "region", None)
    # (the sidebar's own width; drawn elsewhere, a popup's: about a sidebar's)
    width = region.width if region is not None and region.width > 50 and region.type == "UI" else 300
    scale = (context.preferences.system.ui_scale if context.preferences else 1.0) or 1.0
    return max(20, int(width / (7.0 * scale)))


def _lists_current(context, scalp):
    """Fills the palette's lists (by a timer: drawing cannot) when they are another groom's."""
    rt = groom.runtime_if_any(scalp)
    if rt is None or rt.session is None or not process.running():
        return
    if _lists_for[0] != rt.uid and not bpy.app.timers.is_registered(_refresh_lists):
        bpy.app.timers.register(_refresh_lists, first_interval=0.0)


def _refresh_lists():
    context = bpy.context
    scalp = active_groom(context)
    if scalp is not None and tools.is_hierarchy(scalp):
        try:
            tools.session(scalp)
            tools.refresh_info(scalp)
            _lists_for[0] = groom.runtime(scalp).uid
        except Exception as e:  # noqa: BLE001
            print("GroomLab: %r" % e)
        groom._redraw()
    return None


def flow_count_text(arrows, sinks):
    """The Flow card's count line, as every host words it: "34 arrows · 2 sinks", "No flow arrows yet"."""
    if not arrows and not sinks:
        return "No flow arrows yet"
    return "%d arrow%s · %d sink%s" % (arrows, "" if arrows == 1 else "s", sinks, "" if sinks == 1 else "s")


def current_tab(t):
    """The page open: Tools, Paint or Photos (a .blend saved with the Correctives page: Tools)."""
    return t.tab if t.tab in TABS else "TOOLS"


def section_shown(section_id, context):
    """Whether a Groom Tools section shows (palette.json's "tools": the active tool one of them;
    "tabs": the current page one of them; neither: always), as Maya's and the app's."""
    s = defs.section(section_id)
    t = context.scene.groomlab_tools
    if s.get("tools") and (t.tool or "") not in s["tools"]:
        return False
    return not s.get("tabs") or TABS.get(current_tab(t), "") in s["tabs"]


def in_window(layout):
    """Whether the panel draws in the GroomLab window (window.py: drawn.DrawnLayout), not the sidebar."""
    return hasattr(layout, "groomlab_role")


SMOOTHING = ("Shape", "Trim")  # the groups whose tools smooth on Shift+drag (Create's, Paint's and Guides' don't say)


def short_hint(hint, category=""):
    """A tool's line, as the app's (groom_tools.cpp refreshCard): the hint's first clause without its
    asides in parentheses (cut at ". " or "; " past 24 characters), then, for the Shape and Trim
    tools, the Shift+drag smoothing and Ctrl+Shift+drag selecting they share."""
    short = re.sub(r"\s*\([^()]*\)", "", hint)  # (asides in parentheses: the tooltip's)
    for stop in (". ", "; ", " ("):
        at = short.find(stop)
        if at > 24:
            short = short[:at] + "."
    if not short.endswith("."):
        short += "."
    return short + (" Shift+drag smooths, Ctrl+Shift+drag selects." if category in SMOOTHING else "")


def look(layout, role):
    """The GroomLab window's look (the app's cards, titles, hints: drawn.DrawnLayout.groomlab_role):
    in the window a column of `layout` in that role; in the sidebar `layout` itself, so the sidebar
    draws as Blender's own."""
    if not in_window(layout):
        return layout
    sub = layout.column()
    sub.groomlab_role = role
    return sub


def fold(layout, fold_id, title, default_closed=True):
    """A folding section (Blender's layout panels; the window folds its own): its body, None while
    folded. A layout without panels (a test's stand-in) draws the body under the title."""
    if hasattr(layout, "panel"):
        head, body = layout.panel("groomlab_" + fold_id, default_closed=default_closed)
        head.label(text=title)
        return body
    layout.label(text=title)
    return layout.column()


def tile(layout, tool, active, named=True):
    """A tool's tile: its icon and name (the name and hint in the tooltip: GROOMLAB_OT_tool.description);
    not `named`, its icon alone."""
    info = tools.tool_info(tool)
    text = (info["label"] if info else tool) if named else ""
    if tool == "lengths" and in_window(layout):  # (a tile's word fits its width: the app's "Keys")
        text = "Keys"
    op = layout.operator("groomlab.tool", text=text, icon_value=icons.icon(tool), depress=active == tool)
    op.tool = tool
    return op


TILE_SCALE = 1.4  # the tiles a little taller than a row


def tool_grid(layout, tool_ids, active, columns=5, greyed=(), named=True):
    """Tiles of one size, five to a row (Maya's Simple trays), named or (`named` False) their icons
    alone; the tools in `greyed` drawn greyed out where they are (a corrective picked: the tiles
    never move)."""
    # (not aligned: a greyed tile's own cell keeps the size and place of the others)
    grid = layout.grid_flow(row_major=True, columns=columns, even_columns=True, even_rows=True, align=False)
    grid.scale_y = TILE_SCALE
    for t in tool_ids:
        if t in greyed:
            cell = grid.row(align=True)
            cell.enabled = False
            tile(cell, t, active, named)
        else:
            tile(grid, t, active, named)
    return grid


CHAR_PX = 6.5   # px a character of a tile's name takes in Blender's interface font (at 100%)
TILE_EXTRA = 36  # px a named tile adds to its name: the icon and the button's padding (at 100%)
TILE_NAMES = ("Invert Frozen", "Unfreeze All")  # (the Guides tray's two buttons, named as tiles)


def tiles_named(layout, context):
    """Whether the tiles show their names: in the GroomLab window (each its icon over its name), or
    a sidebar wide enough for every tile's whole name five a row; else the icons alone, as Blender's
    toolbar (each name in its tooltip): a cut name ("Sm...") reads worse than none. Blender's buttons
    put a name beside the icon, not under it; a popup's width is not known here, so it shows icons."""
    if in_window(layout):
        return True
    region = getattr(context, "region", None)
    if region is None:  # (drawn outside the interface: a test's stand-in)
        return True
    if region.type != "UI":
        return False
    scale = (context.preferences.system.ui_scale if context.preferences else 1.0) or 1.0
    names = [x["label"] for _c, tlist in tools.sections() for x in tlist] + list(TILE_NAMES)
    need = max(len(n) for n in names) * CHAR_PX + TILE_EXTRA
    return region.width - 30 * scale >= 5 * need * scale


PILL_EXTRA = 32  # px a toggle button adds to its label in Blender's interface (at 100%)
PILL_GAP = 8     # px between the buttons of a row that is not aligned (at 100%)


def pills(layout, context, switches, width=None):
    """Switches as the Simple redesign's chips (research/ui-redesign parts.tsx Chip; Maya's
    pill_row): `switches` are (data, property, text or None). In the GroomLab window pills (drawn.py's
    "pills" role: each as wide as its name, left to right, wrapping); in Blender's sidebar and its
    popovers toggle buttons as wide as their labels, left to right, as many to a row as fit `width`
    (px; default: the sidebar's), the rest on the next row. Blender draws its own buttons: rounded and
    colored by its theme, no pill and no outline."""
    def put(row, data, prop, text):
        if text is None:
            row.prop(data, prop, toggle=True)
        else:
            row.prop(data, prop, text=text, toggle=True)

    if in_window(layout):
        flow = look(layout, "pills")
        for data, prop, text in switches:
            put(flow, data, prop, text)
        return flow
    prefs = getattr(context, "preferences", None)
    scale = (prefs.system.ui_scale if prefs else 1.0) or 1.0
    if width is None:  # the sidebar's width; drawn elsewhere (a popup), Blender's popover's 10 units
        region = getattr(context, "region", None)
        width = region.width - 40 * scale if region is not None and region.type == "UI" else 10 * 20 * scale
    col = layout.column(align=False)
    row, used = None, 0.0
    for data, prop, text in switches:
        name = text if text is not None else data.bl_rna.properties[prop].name
        need = (len(name) * CHAR_PX + PILL_EXTRA) * scale
        if row is None or (width and used + PILL_GAP * scale + need > width):
            row = col.row(align=False)
            row.alignment = "LEFT"  # (each button as wide as its label)
            used = need
        else:
            used += PILL_GAP * scale + need
        put(row, data, prop, text)
    return col


def label(sid, fallback=""):
    """A catalog setting's label (Maya's)."""
    return tools.setting_label(sid, fallback)


def draw_bar(layout, path, key, context):
    """A list's action bar (lists.json): its labelled entries on the left, its glyph entries as
    icons on the right; every entry shown (Blender greys out what does not apply)."""
    items = menu.resolve(defs.get(path), context, conditions=False)
    row = layout.row(align=True)
    left = row.row(align=True)
    right = row.row(align=True)
    right.alignment = "RIGHT"
    for item in items:
        if item["kind"] == "divider":
            continue
        glyph = item.get("glyph")
        where = right if glyph else left
        if item["kind"] == "submenu":
            where.menu(menu.submenu_idname(key, item["id"]), text="" if glyph else item["label"],
                       icon=menu.GLYPHS.get(glyph, "NONE"))
        elif glyph:
            menu.draw_action(where, item, context, text="", icon=menu.GLYPHS.get(glyph, "NONE"))
        else:
            menu.draw_action(where, item, context, icon="ADD" if item["label"].startswith("+") else None,
                             text=item["label"].lstrip("+ ").strip() if item["label"].startswith("+") else None)
    return row


def list_with_menu(layout, list_type, data, prop, active_prop, menu_idname, rows=4, icon="DOWNARROW_HLT"):
    """A list with a menu behind a button beside it (Blender's specials menu); the window leaves
    an empty list out (its card's empty state says what to do)."""
    row = layout.row()
    if in_window(layout) and not len(getattr(data, prop)):
        return row
    row.template_list(list_type, "", data, prop, data, active_prop, rows=rows)
    if menu_idname:
        col = row.column(align=True)
        col.menu(menu_idname, text="", icon=icon)
    return row


def plain_list(layout, list_type, data, prop, active_prop, rows=1):
    """A list whose rows carry their own icons and menu (ROW_ORDER); the window leaves an empty one out."""
    if in_window(layout) and not len(getattr(data, prop)):
        return None
    layout.template_list(list_type, "", data, prop, data, active_prop, rows=rows)
    return layout


def block(layout, title, actions=None, titled=True):
    """A titled block of the card (Maya's widgets.Block): a hairline, its small title, then its rows:
    (column, header row)."""
    layout.separator(type="LINE")
    col = layout.column()
    head = col.row(align=True)
    if titled or not in_window(layout):
        look(head, "tray").label(text=title)
    return col, head


def section_header(layout, title, count=None):
    """A list's header (Guide groups, Correctives, Maps, Photos): its name and how many; the row for
    its icons. In the window (the section on its card) the name as the card's title, the count beside it."""
    if in_window(layout):
        row = layout.row(align=True)
        names = row.row(align=True)
        names.alignment = "LEFT"
        look(names, "title").label(text=title)
        if count is not None:
            look(names, "count").label(text="%d" % count)
        return row
    layout.separator(type="LINE")
    row = layout.row(align=True)
    row.label(text=title.upper() + ("   %d" % count if count is not None else ""))
    return row


def empty_state(layout, title, text, icon, chars):
    """The window's empty state (components.emptyState): a disc with the icon, a title, a line; the
    sidebar draws nothing."""
    if not in_window(layout):
        return
    col = look(layout, "empty")
    col.label(text=title, icon=icon)
    _wrap(col, text, width=chars)


def bar_icons(layout, path, key, context, ids, glyphs=None):
    """Some entries of a list's bar (lists.json) as icon buttons, in `ids`' order: a list's header
    (New, Import, Fix Frame, Add Camera)."""
    glyphs = dict(HEADER_ICONS, **(glyphs or {}))
    items = dict((item["id"], item) for item in menu.resolve(defs.get(path), context, conditions=False)
                 if item["kind"] != "divider")
    for aid in ids:
        item = items.get(aid)
        if item is None:
            continue
        icon = glyphs.get(aid) or menu.GLYPHS.get(item.get("glyph"), "NONE")
        if item["kind"] == "submenu":
            layout.menu(menu.submenu_idname(key, item["id"]), text="", icon=icon)
        else:
            menu.draw_action(layout, item, context, text="", icon=icon)


# The lists' labelled actions as icons in their header (Maya's tool_tabs.HEADER_GLYPHS).
HEADER_ICONS = {"group.new": "ADD", "map.new": "ADD", "map.import": "IMPORT", "corrective.new": "ADD",
                "corrective.fixFrame": "DECORATE_KEYFRAME", "photo.import": "IMPORT", "photo.addView": "CAMERA_DATA"}


def help_text(tool):
    """A tool tile's tooltip: the tool's name and whole hint, then (the brushes) the gestures, one
    a line."""
    info = tools.tool_info(tool)
    if not info:
        return "Pick a tool"
    text = "%s: %s" % (info["label"], info["hint"])
    if "radius" in info.get("settings", []):
        text += "\n\n" + "\n".join(GESTURES.split(" · "))
    return text


def header_buttons(row, context):
    """The current groom's eyes (the strands; the guides, with how they draw beside it: Maya's split
    button; Shift+click or right-click an eye: every groom's), the Graph Editor, Find Command and
    More (palette.json's header)."""
    scalp = active_groom(context)
    window = in_window(row)  # (the window's glyphs: Maya's; the sidebar Blender's own icons)
    if scalp is not None:
        d = scalp.groomlab_tools
        op = row.operator("groomlab.eye", text="", icon="HIDE_OFF" if d.show_strands else "HIDE_ON")
        op.what, op.mode = "STRANDS", "TOGGLE"
        guides = row.row(align=True)
        if window:
            op = guides.operator("groomlab.eye", text="", icon_value=icons.icon("ui_guides"), depress=bool(d.show_guides))
        else:
            op = guides.operator("groomlab.eye", text="", icon="OUTLINER_DATA_CURVES", depress=bool(d.show_guides))
        op.what, op.mode = "GUIDES", "TOGGLE"
        guides.popover(GROOMLAB_PT_guide_display.__name__, text="", icon="DISCLOSURE_TRI_DOWN")
    agent_link.draw_badge(row)  # (while an AI agent is connected: the badge and its Stop)
    row.operator("groomlab.graph_open", text="", icon_value=icons.icon("ui_network" if window else "graph"))
    row.operator("groomlab.find_command", text="", icon="VIEWZOOM")
    row.menu("GROOMLAB_MT_tools_more", text="", icon="DOWNARROW_HLT")


def editing_text(context, tool):
    """What the next stroke edits, for the card's header: (text, icon). Paint: the map it paints;
    the others: the picked group, on the base groom or a corrective (or a Sculpt node)."""
    scene = context.scene
    t = scene.groomlab_tools
    if tool == "paint":
        m = tools.current_map(scene)
        if m is None:
            return "no map yet: + under Maps", "NONE"
        return "on %s%s" % (m.name, "" if t.map_read_by else "  ·  unused"), "NONE"  # (its info: what reads it)
    group = t.group if t.group and t.group != tools.ALL_GROUPS else "All groups"
    # A level of a tube hierarchy kept in view while another's guides are picked: said here, so the
    # brush is seen to comb these guides under those tubes.
    tubes = ""
    if t.tube_view_group and t.tube_view_group != t.group:
        levels = tool_props.tube_hierarchy(t, t.tube_view_group)
        tubes = "  \u00b7  tubes: %s" % ("Level %d" % (levels.index(t.tube_view_group) + 1)
                                         if t.tube_view_group in levels else t.tube_view_group)
    kind, name = tools.picked_corrective(scene)
    if kind in ("layer", "node"):
        return "%s on %s%s" % (group, name, tubes), "SHAPEKEY_DATA"
    return "%s on Base groom%s" % (group, tubes), "NONE"


class VIEW3D_PT_groomlab_tools(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "GroomLab"
    bl_label = "Groom Tools"
    bl_order = 0

    def draw_header(self, context):
        self.layout.label(text="", icon_value=icons.icon("palette"))

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        t = scene.groomlab_tools
        scalp = active_groom(context)
        # What the tools work on: the current groom (the Graph Editor's too unless pinned; selecting
        # a groom makes it current) and its buttons. In the GroomLab window they sit on its rail, in
        # view however far the window scrolls (issue 73).
        if not in_window(layout):
            row = layout.row(align=True)
            row.prop(scene.groomlab, "groom", text="")
            header_buttons(row, context)
        if updates.available:  # the startup check found a newer GroomLab
            row = layout.box().row(align=True)
            row.label(text="GroomLab %s is available" % updates.available[0], icon="INFO")
            row.operator("groomlab.check_updates", text="Update...")
        if scalp is None:
            self.draw_empty(layout, context)
            return
        if not tools.is_hierarchy(scalp):
            box = layout.box()
            _wrap(box, "This groom's guides are Blender curves: sculpt them with Blender's brushes. A groom made with "
                       "Create Groom uses GroomLab's own tools.", icon="INFO", width=_chars(context))
            box.operator("groomlab.sculpt_guides", icon="SCULPTMODE_HLT")
            return
        if not tools.cached_catalog():
            if process.running():
                try:
                    tools.catalog()
                except Exception:
                    pass
            if not tools.cached_catalog():
                layout.operator("groomlab.update", text="Start GroomLab", icon="FILE_REFRESH")
                return
        _lists_current(context, scalp)
        if t.status:
            _wrap(layout, t.status, icon="INFO", width=_chars(context))
        # The pages, labeled (Tools, Paint, Photos), then the page.
        layout.row(align=True).prop(t, "tab", expand=True)
        tab = current_tab(t)
        if tab == "TOOLS":  # the brushes, the brush's card, then what they work on
            self.draw_trays(layout, context, t)
            self.draw_card(layout, context, t, scalp, t.tool if t.tool != "paint" else "")
            if section_shown("groups", context):
                self.draw_groups(layout, context, t)
            if section_shown("correctives", context):
                self.draw_correctives(layout, context, t, scalp)
        elif tab == "PAINT":  # the page is painting: the Paint card, then the maps it paints
            self.draw_card(layout, context, t, scalp, "paint")
            self.draw_maps(layout, context, t)
        else:
            photos.draw_tab(layout, context, _chars(context))
        row = layout.row(align=True)
        undo, redo = tools.undo_label(scalp), tools.undo_label(scalp, redo=True)
        sub = row.row(align=True)
        sub.enabled = bool(undo)
        sub.operator("groomlab.tool_undo", text="Undo %s" % undo if undo else "Undo", icon="LOOP_BACK").redo = False
        sub = row.row(align=True)
        sub.enabled = bool(redo)
        sub.operator("groomlab.tool_undo", text="Redo %s" % redo if redo else "Redo", icon="LOOP_FORWARDS").redo = True

    def draw_empty(self, layout, context):
        """No groom yet: one card (palette.json's empty state). What to select and what is selected
        now (Maya's way: select, then create), Create (waits for a mesh), the sample meshes, Import."""
        empty = defs.get("palette/empty")
        box = layout.box()
        look(box, "title").label(text=empty["title"])
        _wrap(look(box, "about"), "Select the scalp mesh (and any guide curves), then create.", width=_chars(context))
        obj = context.active_object
        mesh = obj if obj is not None and obj.type == "MESH" and obj.groomlab.role == "NONE" else None
        curves = [o for o in getattr(context, "selected_objects", []) or [] if o.type in ("CURVES", "CURVE")] if mesh else []
        look(box, "status").label(
            text=("Selected: %s (the scalp)%s" % (mesh.name, " and %d curves (the guides)" % len(curves) if curves else ""))
            if mesh else "Nothing selected: select the scalp mesh", icon="MESH_DATA" if mesh else "NONE")
        for item in menu.resolve(empty["items"], context):
            if item["kind"] == "action":
                first = item["id"] == "groom.create"
                where = look(box, "primary") if first else box
                if first:
                    where = where.row()
                    where.enabled = mesh is not None
                menu.draw_action(where, item, context, icon="ADD" if first else None)
            elif item["kind"] == "submenu" and item["id"] == "samples":  # No mesh yet? the samples as chips
                look(box, "about").label(text="No mesh yet?")
                row = box.row(align=True)
                for sample in item["items"]:
                    if sample["kind"] == "action":
                        menu.draw_action(row, sample, context, text=sample["label"].split(" (")[0])
            elif item["kind"] == "submenu":
                box.menu(menu.submenu_idname("empty", item["id"]), text=item["label"], icon="IMPORT")

    # -- Tools: the trays -----------------------------------------------------------------------------

    def draw_trays(self, layout, context, t):
        """The brushes in their groups (the catalog's categories), a tray each without its name (the
        tiles speak for it); Invert Frozen and Unfreeze All beside Freeze. On a corrective or a
        Sculpt node the tiles that can't edit one grey out where they are."""
        kind, _name = tools.picked_corrective(context.scene)
        on_layer = kind in ("layer", "node")
        named = tiles_named(layout, context)
        for category, tlist in tools.sections():
            ids = [x["id"] for x in tlist]
            if category == "Paint" or ids == ["paint"] or not ids:
                continue  # the painting brush is the Paint page's
            tray = look(layout, "card")
            greyed = [x for x in ids if on_layer and not tools.edits_layers(x)]
            grid = tool_grid(tray, ids, t.tool, greyed=greyed, named=named)
            if "freeze" in ids:  # beside Freeze, two tiles of the grid's own size (their tooltips say what they do)
                cell = grid
                if on_layer:
                    cell = grid.row(align=True)
                    cell.enabled = False
                cell.operator("groomlab.guides_edit", text=("Invert" if in_window(layout) else "Invert Frozen") if named else "",
                              icon_value=icons.icon("invert")).action = "INVERT_FROZEN"
                if on_layer:
                    cell = grid.row(align=True)
                    cell.enabled = False
                cell.operator("groomlab.guides_edit", text=("Unfreeze" if in_window(layout) else "Unfreeze All") if named
                              else "", icon_value=icons.icon("unfreeze")).action = "UNFREEZE_ALL"

    # -- The card ---------------------------------------------------------------------------------------

    def draw_card(self, layout, context, t, scalp, tool):
        """The brush's card: its header, Radius and Strength, its essentials, the toggles row, More
        settings (folded). The Tube and Roots brushes: their own (draw_tube_card, draw_roots_card)."""
        info = tools.tool_info(tool) if tool else None
        if not in_window(layout):
            layout.separator(type="LINE")  # a plain section: a hairline, no box
        if info is None:
            _wrap(look(look(layout, "card"), "about"), NO_TOOL, width=_chars(context))
            return
        uses = set(info["settings"])
        card = look(layout, "card") if in_window(layout) else layout.column()
        self.draw_card_head(card, context, tool, info)
        if tool == "shape":
            self.draw_tube_card(card, context, t, info, uses)
        elif tool == "roots":
            self.draw_roots_card(card, context, t)
        else:
            paints = "map" in uses
            if paints:  # Paint: what a stroke paints first (the mode, the value or color, Fill)
                self.draw_paint_block(card, context, t)
            if "radius" in uses:
                self.draw_sliders(card, t, info)
            self.draw_tool_blocks(card, context, t, info, uses)
            self.draw_toggles(card, context, t, uses)
            self.draw_more(card, context, t, uses)
        row = card.row(align=True)
        row.operator("groomlab.tool_exit", text="Leave Tool (Esc)", icon="CANCEL")

    def draw_card_head(self, card, context, tool, info):
        """One row: the brush's icon and name (its whole hint on hover), and on the right what the
        next stroke edits ("primary on Base groom"; Paint: "on density")."""
        head = card.row(align=True)
        title = look(head.row(align=True), "tool")
        title.alignment = "LEFT"
        if in_window(card):
            title.groomlab_tip = info.get("hint", "")  # (the whole hint on the name's hover, as the app)
        title.label(text=info["label"], icon_value=icons.icon(tool))
        right = head.row(align=True)
        right.alignment = "RIGHT"
        text, icon = editing_text(context, tool)
        t = context.scene.groomlab_tools
        group = next((g for g in t.groups if g.name == t.group), None) if tool != "paint" else None
        if group is not None and in_window(card):  # the group's dot, in its color (Maya's)
            look(right, "dot").prop(group, "color", text="")
        look(right, "count").label(text=text, icon=icon)

    def draw_sliders(self, card, t, info):
        col = card.column(align=True)
        col.prop(t, "radius", text=label("radius", "Radius"), slider=True)
        col.prop(t, "strength", text="Strength (±)" if info.get("signed") else label("strength", "Strength"), slider=True)

    def draw_toggles(self, card, context, t, uses):
        """The stroke's switches, one row in one order whatever the tool: Mirror first, Pressure, Keep
        off scalp, then the brush's own (Comb's Stretch, Smooth's Straighten; Paint: Whole faces)."""
        toggles = [(p, text) for k, p, text in (("symmetry", "symmetry", "Mirror"),
                                                ("pressure", "pressure", "Pressure"),
                                                ("collide", "collide", "Keep off scalp"),
                                                ("stretch", "stretch", "Stretch"),
                                                ("straighten", "straighten", "Straighten"))
                   if k in uses]
        m = tools.current_map(context.scene) if "map" in uses else None
        if "map" in uses and (m is None or m.kind in ("vertices", "texture")):  # (a guide layer paints guides)
            toggles.append(("paint_whole_faces", "Whole faces"))
        if not toggles:
            return
        pills(card, context, [(t, p, text) for p, text in toggles])

    def draw_more(self, card, context, t, uses):
        """More settings (folded): Falloff, Smooth, Mirror across, Applies to, the curves, the stamp,
        Spacing, Solo new and the new guides' tube."""
        if not uses & {"radius", "symmetry", "targets", "newGuides", "map"}:
            return
        body = fold(card, "more", "More settings")
        if body is None:
            return
        col = body.column(align=True)
        if "radius" in uses:
            col.prop(t, "falloff", text=label("falloff", "Falloff"), slider=True)
        if "smoothStrength" in uses:
            col.prop(t, "smooth_strength", text=label("smoothStrength", "Smooth") + " (Shift)", slider=True)
        if "spacing" in uses:
            col.prop(t, "spacing", slider=True)
        if "symmetry" in uses:
            row = body.row(align=True)
            row.label(text="Mirror across")
            sub = row.row(align=True)
            sub.enabled = t.symmetry
            sub.prop(t, "symmetry_mode", text="")
        if "targets" in uses:
            row = body.row(align=True)
            row.label(text="Applies to")
            for p in ("target_guides", "target_parts", "target_flow"):
                row.prop(t, p, toggle=True)
        if "falloffProfile" in uses:
            body.prop(t, "falloff_curve", text=label("falloffProfile", "Falloff curve"))
        if "strengthProfile" in uses:
            body.prop(t, "strength_profile", text=label("strengthProfile", "Root to tip"))
        m = tools.current_map(context.scene) if "alpha" in uses else None
        if m is not None and m.kind in ("vertices", "texture"):  # the stamp (a guide map takes none)
            self.draw_alpha(body, t)
        if "soloNewGuides" in uses:
            body.prop(t, "solo_new_guides", text="Solo new", toggle=True)
        if "tube" in uses and "newGuides" in uses:  # Place, Draw: the tube the new guides get
            body.label(text=label("tube", "Tube"))
            body.prop(t, "tube_shape", text="")
            _wrap(body, defs.text("profileHint"), width=_chars(context))
            self.draw_tube_fields(body, t)

    def draw_paint_block(self, card, context, t):
        """Paint's own: the four modes, then the value (Maximum for Clamp Max) and Fill, or a color
        map's swatch, R G B A and Fill."""
        m = tools.current_map(context.scene)
        card.row(align=True).prop(t, "paint_mode", expand=True)
        if m is None or m.channels == 1:
            row = card.row(align=True)
            row.prop(t, "value", text="Maximum" if t.paint_mode == "clampMaximum" else "Value", slider=True)
            fill = row.row(align=True)
            fill.enabled = m is not None
            fill.operator("groomlab.map_fill", text="Fill")
        if m is not None and m.channels > 1:
            row = card.row(align=True)
            row.prop(t, "paint_color", text="")
            for ch in ("channel_r", "channel_g", "channel_b", "channel_a"):
                row.prop(t, ch, toggle=True)
            row.operator("groomlab.map_fill", text="Fill")

    def draw_lengths(self, card, context, t):
        """The Length Keys tool's Keys block: the selected key's scale and the length it gives its
        guide, Scale or Trim, Delete Key, Cancel and Apply (Clear in the header)."""
        try:
            state = json.loads(t.length_state or "{}")
        except ValueError:
            state = {}
        scales = state.get("scales") or []
        key = state.get("activeKey", -1)
        has_key = 0 <= key < len(scales)
        edited = bool(state.get("edited"))
        box, head = block(card, label("lengthKeys", "Keys"))
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        sub.enabled = bool(scales)
        sub.operator("groomlab.length_edit", text="Clear", emboss=False).edit = "clear"
        col = box.column(align=True)
        col.enabled = has_key
        col.prop(t, "length_scale", slider=True)
        col.prop(t, "length_key")
        box.label(text="Guides")
        box.row(align=True).prop(t, "length_change", expand=True)
        row = box.row(align=True)
        sub = row.row(align=True)
        sub.enabled = has_key
        sub.operator("groomlab.length_edit", text="Delete Key", emboss=False).edit = "deleteKey"
        sub = row.row(align=True)
        sub.enabled = edited
        sub.operator("groomlab.length_edit", text="Cancel").edit = "cancel"
        sub.operator("groomlab.length_edit", text="Apply", icon="CHECKMARK").edit = "apply"
        _wrap(box, "Click on the scalp to place a key." if not scales else
              "%d keys. Previewing: Apply sets the guides (Enter)." % len(scales) if edited else
              "%d keys. Right-drag a key to scale the guides round it." % len(scales),
              width=_chars(context))

    def draw_alpha(self, card, t):
        """Paint's stamp on a scalp map: the library's grid (the thumbnail opens it), Add and the
        folder beside it; while one is picked, its rotation, angle, spacing and invert."""
        row = card.row(align=False)
        row.label(text="Stamp")
        row.template_icon_view(t, "alpha_choice", show_labels=True, scale=2.5, scale_popup=4.0)
        side = row.column(align=True)
        side.label(text=os.path.splitext(os.path.basename(t.alpha))[0] if t.alpha else "None")
        buttons = side.row(align=True)
        buttons.operator("groomlab.alpha_add", text="", icon="ADD")
        buttons.operator("groomlab.alpha_folder", text="", icon="FILE_FOLDER")
        if t.alpha:
            buttons.operator("groomlab.alpha_clear", text="", icon="X")
            col = card.column(align=True)
            col.row(align=True).prop(t, "alpha_rotation", expand=True)
            angle = col.row(align=True)
            angle.enabled = t.alpha_rotation != "RANDOM"
            angle.prop(t, "alpha_angle", slider=True)
            col.prop(t, "alpha_spacing", slider=True)
            col.prop(t, "alpha_invert")

    def draw_tube_fields(self, layout, t):
        col = layout.column(align=True)
        col.prop(t, "tube_width", slider=True)
        col.prop(t, "tube_flatness", slider=True)
        col.prop(t, "tube_twist", slider=True)

    def draw_tool_blocks(self, card, context, t, info, uses):
        """The brush's essentials (the rest is in More settings): Place and Draw their Length and
        Segments; Clone its source; Part its next name and parts; Flow what a click lays; Select its
        soft radius; Erase what it removes; Length Keys its keys."""
        if "newGuides" in uses:
            col = card.column(align=True)
            col.prop(t, "length", slider=True)
            col.prop(t, "segments", slider=True)
            card.prop(t, "match_neighbors", text="Match nearby")
        if "clone" in uses:  # Pick Source, Reset, what the source holds, Replace
            row = card.row(align=True)
            row.operator("groomlab.clone_pick_source", text="Pick Source", depress=t.clone_pick_source)
            sub = row.row(align=True)
            sub.enabled = t.clone_source_size > 0 or t.clone_pick_source
            sub.operator("groomlab.clone_reset", text="Reset")
            n = t.clone_source_size
            card.label(text=("Source: %d guide%s" % (n, "" if n == 1 else "s")) if n else
                       "No source yet: Ctrl+click over guides")
            card.prop(t, "clone_replace", text="Replace")
        if "parts" in uses:
            items = [i for i in menu.resolve(defs.get("lists/parts/bar"), context, conditions=False) if i["kind"] == "action"]
            row = card.row(align=True)
            row.prop(t, "part_name", text="Next part")
            for item in items:
                if item["id"] == "parts.fromCurves":
                    menu.draw_action(row, item, context, text="", icon="CURVE_BEZCURVE")
            card.template_list("GROOMLAB_UL_parts", "", t, "parts", t, "part_index", rows=3)
            row = card.row(align=True)
            for item in items:
                if item["id"] != "parts.fromCurves":
                    menu.draw_action(row, item, context)
            row = card.row(align=True)  # Mirror to Other Side (issue 105)
            for item in menu.resolve(defs.get("lists/parts/mirror"), context, conditions=False):
                if item["kind"] == "action":
                    menu.draw_action(row, item, context)
        if "flow" in uses:
            card.prop(t, "flow_sink_item", text="Click lays")
            row = card.row(align=True)
            row.label(text=flow_count_text(t.flow_arrows, t.flow_sinks))
            sub = row.row(align=True)
            sub.alignment = "RIGHT"
            sub.enabled = bool(t.flow_arrows or t.flow_sinks)
            # Clear All (GroomLab > Utilities > Delete All Flow Arrows too).
            sub.operator("groomlab.flow_clear", text="Clear All", emboss=False)
            row = card.row(align=True)  # Mirror to Other Side (issue 105)
            for item in menu.resolve(defs.get("lists/flow/mirror"), context, conditions=False):
                if item["kind"] == "action":
                    menu.draw_action(row, item, context)
        if "lengthKeys" in uses:
            self.draw_lengths(card, context, t)
        if "selection" in uses:
            card.row(align=True).prop(t, "select_shape", expand=True)
            card.prop(t, "soft_radius", slider=True)
            row = card.row(align=True)
            row.label(text="%d selected" % t.selected_count)
            row.operator("groomlab.guides_edit", text="All").action = "SELECT_ALL"
            row.operator("groomlab.guides_edit", text="None").action = "CLEAR"
        if "erases" in uses:
            row = card.row(align=True)
            row.label(text="Removes")
            for p in ("erase_guides", "erase_parts", "erase_flow"):
                row.prop(t, p, toggle=True)

    # -- The Tube and Roots brushes ---------------------------------------------------------------------

    def draw_tube_card(self, card, context, t, info, uses):
        """The Tube brush (Maya's Simple card): the levels' row, what it paints (Width, Flatness,
        Twist), Radius and Strength, its toggles, More settings, then the Profile (always shown) and
        the selected tubes (folded). The tubes' display switches are the header's guides popover's."""
        self.draw_levels(card, context, t)
        card.row(align=True).prop(t, "tube_channel", expand=True)
        self.draw_sliders(card, t, info)
        self.draw_toggles(card, context, t, uses & {"symmetry", "pressure"})
        body = fold(card, "more", "More settings")
        if body is not None:
            col = body.column(align=True)
            col.prop(t, "falloff", text=label("falloff", "Falloff"), slider=True)
            if "smoothStrength" in uses:
                col.prop(t, "smooth_strength", text=label("smoothStrength", "Smooth") + " (Shift)", slider=True)
            if "symmetry" in uses:
                row = body.row(align=True)
                row.label(text="Mirror across")
                sub = row.row(align=True)
                sub.enabled = t.symmetry
                sub.prop(t, "symmetry_mode", text="")
            if "falloffProfile" in uses:
                body.prop(t, "falloff_curve", text=label("falloffProfile", "Falloff curve"))
            if "strengthProfile" in uses:
                body.prop(t, "strength_profile", text=label("strengthProfile", "Root to tip"))
        self.draw_profile(card, context, t)
        self.draw_selected_tubes(card, context, t)

    def draw_roots_card(self, card, context, t):
        """The Roots brush (tubes drawn root first; Maya's Roots card): the levels' row, then Draw
        root, Edit roots (Finish edit while editing), Select root (a mode's button again: Draw root);
        Undo CV, Delete, Duplicate; Subdivide, the density brush, Multiplier and Apply; Snap to
        neighbours and Draw guide; the new tubes' Point density (or Length and Segments) and Tip
        width."""
        self.draw_levels(card, context, t)
        mode = t.root_mode
        row = card.row(align=True)
        row.operator("groomlab.root_tube", text="Draw root", depress=mode in ("place", "draw")).action = "DRAW"
        row.operator("groomlab.root_tube", text="Finish edit" if mode == "edit" else "Edit roots",
                     depress=mode == "edit").action = "EDIT"
        row.operator("groomlab.root_tube", text="Select root", depress=mode in ("select", "duplicate")).action = "SELECT"
        row = card.row(align=True)
        sub = row.row(align=True)
        sub.enabled = mode in ("place", "draw", "edit")
        sub.operator("groomlab.root_tube", text="Undo CV").action = "UNDO_CV"
        sub = row.row(align=True)
        sub.enabled = t.root_selected > 0
        sub.operator("groomlab.root_tube", text="Delete").action = "DELETE"
        sub.operator("groomlab.root_tube", text="Duplicate", depress=mode == "duplicate").action = "DUPLICATE"
        has_map = bool(t.root_density_map)  # (the one map, or an older numbered one)
        row = card.row(align=True)
        sub = row.row(align=True)
        sub.enabled = t.root_selected > 0
        sub.operator("groomlab.root_tube", text="Subdivide").action = "SUBDIVIDE"
        sub = row.row(align=True)
        sub.enabled = has_map or t.root_painting
        sub.operator("groomlab.root_tube", text="", icon="BRUSH_DATA", depress=t.root_painting).action = "PAINT_DENSITY"
        sub = row.row(align=True)
        sub.scale_x = 1.3  # (the slider the row's widest, as Maya's; Subdivide whole)
        sub.prop(t, "root_multiplier", text="Multiplier", slider=True)
        sub = row.row(align=True)
        sub.enabled = has_map
        sub.operator("groomlab.root_tube", text="Apply").action = "APPLY_DENSITY"
        pills(card, context, [(t, "root_snap", None), (t, "root_draw_path", "Draw guide")])
        col = card.column(align=True)
        if t.root_draw_path:
            col.prop(t, "root_density", slider=True)
        else:
            col.prop(t, "root_length", slider=True)
            col.prop(t, "root_segments", slider=True)
        col.prop(t, "root_taper", slider=True)

    def draw_levels(self, card, context, t):
        """The tube hierarchy as one row: Level, a button a level (1, the base), then + and - (split,
        undo a split) and a menu (Lock view, Mixed view, colors per level)."""
        levels = [x for x in t.tube_levels.split(",") if x]
        count = max(len(levels), 1)
        row = card.row(align=True)
        row.label(text="Level")
        buttons = row.row(align=True)
        buttons.enabled = bool(t.tube_base)
        for k in range(count):
            op = buttons.operator("groomlab.tube_level", text=str(k + 1), depress=k + 1 == t.tube_level)
            op.action = "SHOW"
            op.level = k + 1
        edits = row.row(align=True)
        edits.alignment = "RIGHT"
        add = edits.row(align=True)
        add.enabled = bool(t.tube_base) and len(levels) < 9
        add.operator("groomlab.tube_level", text="", icon="ADD", emboss=False).action = "ADD"
        remove = edits.row(align=True)
        remove.enabled = len(levels) > 1
        if in_window(card):  # (the window has no minus glyph: the sign itself)
            remove.operator("groomlab.tube_level", text="−", emboss=False).action = "REMOVE"
        else:
            remove.operator("groomlab.tube_level", text="", icon="REMOVE", emboss=False).action = "REMOVE"
        edits.menu(GROOMLAB_MT_tube_levels.__name__, text="", icon="THREE_DOTS")

    PROFILE_PICKERS = {"ROOT": "tube_shape", "MID": "tube_mid_shape", "TIP": "tube_tip_shape"}

    def draw_profile(self, card, context, t):
        """The Profile (Maya's Simple, always shown): Root, Mid and Tip (in the GroomLab window as
        tiles with their outlines); the profile picked: its shape (the editor beside it in the
        window), Apply to the selected tubes, Hold, and a library shape's Mirror and Smooth; what the
        targets keep; a hint and the library's menu (Rename, New Shape, Duplicate, Delete Shape)."""
        window = in_window(card)
        card.separator(type="LINE")
        tiles = getattr(card, "groomlab_profile_tiles", None)
        if tiles is not None:
            tiles(context, t)
        else:
            card.row(align=True).prop(t, "tube_profile", expand=True)
        profile = t.tube_profile
        name, kind = tools.profile_choice(t)
        body = card.row(align=False)
        editor = getattr(body, "groomlab_profile_editor", None)
        if editor is not None:  # the GroomLab window draws the profile's cross-section (Maya's editor)
            editor(context, t)
        side = body.column(align=False)
        side.prop(t, self.PROFILE_PICKERS[profile], text="")
        apply = side.row(align=True)
        apply.enabled = t.tube_targets > 0
        apply.operator("groomlab.tube_apply", text="Apply").profile = profile
        hold = side.row(align=True)
        hold.enabled = t.tube_targets > 0
        hold.prop(t, "tube_%s_hold" % profile.lower(), text="Hold", slider=True)
        if kind == "library":  # (the outline's switches: a library shape's, a built-in is read-only)
            pills(side, context, [(t, "tube_shape_mirror", None), (t, "tube_shape_smooth", None)])
        notes = []
        n = t.tube_root_targets
        if t.tube_targets and n:
            notes.append("%d root tube%s keep%s %s drawn root: a Root profile goes to the other guides (Roots reshapes "
                         "a root)." % (n, "" if n == 1 else "s", "s" if n == 1 else "", "its" if n == 1 else "their"))
        if t.tube_targets and t.tube_split_targets:
            notes.append("Mid and tip don't carry into split levels.")
        if not t.tube_targets:
            notes.append("Profiles in this groom: %s. Pick one to edit it: every guide taking it follows." %
                         t.tube_in_use if t.tube_in_use else "No guides take a profile yet.")
        if notes:
            _wrap(look(card, "hint"), " ".join(notes), width=_chars(context))
        foot = card.row(align=True)
        if window:
            hint = ("Drag a point to edit; ⋯ for the library" if kind == "library"
                    else "⋯: duplicate it to edit a copy, or a new shape")
        else:  # (the outline is edited in the GroomLab window)
            hint = "⋯: the shape library"
        look(foot, "hint").label(text=hint)
        right = foot.row(align=True)
        right.alignment = "RIGHT"
        right.menu(GROOMLAB_MT_tube_library.__name__, text="", icon="THREE_DOTS")

    def draw_selected_tubes(self, card, context, t):
        """The tubes the edits go to (the selected guides, else the soloed ones), folded: "Selected
        tubes · N" with Smooth (and what it smooths) on its header; Width, Flatness and Twist inside."""
        n = t.tube_targets
        title = "Selected tubes · %d" % n if n else "Selected tubes"
        if hasattr(card, "panel"):
            head, body = card.panel("groomlab_selected_tubes", default_closed=True)
            head.label(text=title)
        else:  # (a layout without panels: a test's stand-in)
            head = card.row(align=True)
            head.label(text=title)
            body = card.column()
        smooth = head.row(align=True)
        smooth.alignment = "RIGHT"
        smooth.enabled = n > 0
        smooth.prop(t, "tube_smooth_what", text="")
        smooth.operator("groomlab.tube_smooth", text="Smooth")
        if body is None:
            return
        how = "selected" if t.tube_source == "selection" else "soloed"
        body.label(text="%d guide%s %s" % (n, "" if n == 1 else "s", how) if n else
                   "No guides selected: the values go to new guides")
        self.draw_tube_fields(body, t)

    # -- The lists: one line per row --------------------------------------------------------------------

    def draw_groups(self, layout, context, t):
        """Guide groups (a click picks the group brushes edit and new guides go into): Isolate and +
        in the header; each row its segments, eye and menu."""
        layout = look(layout, "card")
        head = section_header(layout, defs.section("groups")["label"], len(t.groups))
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        sub.prop(t, "isolate", text="", icon="SOLO_ON" if t.isolate else "SOLO_OFF", emboss=False)
        bar_icons(sub, "lists/groups/bar", "groupsbar", context, ("group.new",))
        if not t.groups:
            empty_state(layout, "No guide groups yet", "Place or Draw starts the first group.", "CURVES",
                        _chars(context))
        plain_list(layout, "GROOMLAB_UL_groups", t, "groups", "group_index")

    def draw_correctives(self, layout, context, t, scalp):
        """Correctives (Maya's CorrectivesBox, a section under the guide groups): the stack, top
        first, the Sculpt nodes, the base groom. A click makes the brushes edit that row (again: the
        base groom); each corrective its weight, Solo, eye and menu. Display while sculpting folded."""
        layout = look(layout, "card")
        head = section_header(layout, defs.section("correctives")["label"],
                              sum(1 for r in t.correctives if r.kind == "layer"))
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        bar_icons(sub, "lists/correctives/bar", "correctivesbar", context, ("corrective.fixFrame", "corrective.new"))
        plain_list(layout, "GROOMLAB_UL_correctives", t, "correctives", "corrective_index")
        body = fold(layout, "sculpt_display", "Display while sculpting")  # (what a corrective greys: the tiles say it)
        if body is not None:
            body.prop(t, "sculpt_display", text="Strands")
            body.prop(t, "full_density")

    def draw_maps(self, layout, context, t):
        """The maps (under the Paint card: what it paints): + and Import in the header; each row its
        RGBA tag, info and menu."""
        layout = look(layout, "card")
        head = section_header(layout, "Maps", len(t.maps))
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        bar_icons(sub, "lists/maps/bar", "mapsbar", context, ("map.new", "map.import"))
        if not t.maps:
            empty_state(layout, "No maps yet", "+ New Map: a setting such as Density to paint, or a blank one.",
                        "TEXTURE", _chars(context))
        plain_list(layout, "GROOMLAB_UL_maps", t, "maps", "map_index")


# ---- A row's settings and menu -------------------------------------------------------------------------

ROW_LISTS = [("GROUPS", "Guide groups", ""), ("MAPS", "Maps", ""), ("CORRECTIVES", "Correctives", ""),
             ("PHOTOS", "Photos", "")]


def row_button(layout, rows, index, text="", icon="THREE_DOTS", panel="", menu_name=""):
    """A row's icon that picks the row, then opens its settings (a popover panel) or its menu."""
    op = layout.operator("groomlab.row_popup", text=text, icon=icon, emboss=False)
    op.rows, op.index, op.panel, op.menu = rows, index, panel, menu_name
    return op


class GROOMLAB_OT_row_popup(bpy.types.Operator):
    """A list row's settings or its menu: the row picked first (what they act on)"""
    bl_idname = "groomlab.row_popup"
    bl_label = "Row"
    bl_options = {"INTERNAL"}

    rows: EnumProperty(items=ROW_LISTS)
    index: IntProperty(default=-1)
    panel: StringProperty()
    menu: StringProperty()

    TIPS = {"GROOMLAB_PT_group_segments": "Segments (click to change; By length)",
            "GROOMLAB_PT_corrective_weight": "Its weight on this frame (click to change, key it)",
            "GROOMLAB_PT_map_info": "What reads it, where it shows",
            "GROOMLAB_PT_photo_settings": "Opacity and focal length",
            "GROOMLAB_MT_group_context": "Rename, select, children, mirror, move, color, delete",
            "GROOMLAB_MT_corrective_context": "Rename, move, keys, delete",
            "GROOMLAB_MT_sculpt_node_context": "Clear Edits",
            "GROOMLAB_MT_map_context": "Rename, convert, the image, delete",
            "GROOMLAB_MT_photo_context": "Rename, Align Photo, Select Camera, Remove"}

    @classmethod
    def description(cls, context, properties):
        return cls.TIPS.get(properties.panel or properties.menu, "")

    def pick(self, context):
        scene = context.scene
        t = scene.groomlab_tools
        i = self.index
        if self.rows == "GROUPS" and 0 <= i < len(t.groups) and t.group_index != i:
            t.group_index = i
        elif self.rows == "MAPS" and 0 <= i < len(t.maps) and t.map_index != i:
            t.map_index = i
        elif self.rows == "CORRECTIVES" and 0 <= i < len(t.correctives) and t.corrective_index != i:
            t.corrective_index = i
        elif self.rows == "PHOTOS" and 0 <= i < len(bpy.data.objects) and photos.is_photo(bpy.data.objects[i]):
            scene.groomlab_photos.photo = bpy.data.objects[i]  # (picked, not looked through: a click on the row does that)

    def invoke(self, context, event):
        self.pick(context)
        if self.panel:
            bpy.ops.wm.call_panel(name=self.panel, keep_open=True)
        elif self.menu:
            bpy.ops.wm.call_menu(name=self.menu)
        return {"FINISHED"}

    def execute(self, context):
        self.pick(context)
        return {"FINISHED"}


class GROOMLAB_OT_map_show_reader(bpy.types.Operator):
    """Open the Graph Editor on the node that reads this map"""
    bl_idname = "groomlab.map_show_reader"
    bl_label = "Show in Graph"

    @classmethod
    def poll(cls, context):
        return bool(context.scene.groomlab_tools.map_read_by) and active_groom(context) is not None

    def execute(self, context):
        from . import graph_tree
        scalp = active_groom(context)
        node = context.scene.groomlab_tools.map_read_by.split(",")[0].strip().split(".")[0]
        result = bpy.ops.groomlab.graph_open()
        tree = graph_tree.tree_of(scalp)
        if tree is not None:
            for n in tree.nodes:
                n.select = getattr(n, "gl_name", None) == node
                if n.select:
                    tree.nodes.active = n
        return result


class _Popover(object):
    """A row's or the header's settings: a popover (Blender's own popup, in the sidebar and the
    GroomLab window alike)."""
    bl_space_type = "VIEW_3D"
    bl_region_type = "HEADER"
    bl_ui_units_x = 11


class GROOMLAB_PT_guide_display(_Popover, bpy.types.Panel):
    """How the guides draw (beside the guides' eye; Maya's Attribute Editor had them): flow and
    tubes, the guides' width, points, colors and smoothing; then the Tube brush's tubes."""
    bl_label = "Guide Display"

    def draw(self, context):
        layout = self.layout
        scalp = active_groom(context)
        if scalp is None:
            layout.label(text="No groom")
            return
        d = scalp.groomlab_tools
        t = context.scene.groomlab_tools
        prefs = getattr(context, "preferences", None)
        width = self.bl_ui_units_x * 20 * ((prefs.system.ui_scale if prefs else 1.0) or 1.0)  # (its content: a bit wider)
        # One row of what shows (Maya's guides display): Guides (the header's eye), Parts, Flow,
        # Tubes (the groom's Tubes switch: with every tool; the Tube and Roots tools show them either
        # way); then how the guides draw; then the tubes' look. Which level's tubes stay in view: a
        # level row's tube in Guide groups (the Roots card's Lock view).
        pills(layout, context, [(d, "show_guides", None), (d, "show_parts", None), (d, "show_flow", None),
                                (d, "show_tubes", "Tubes")], width)
        layout.separator(type="LINE")
        layout.label(text="Guides")
        layout.prop(d, "guide_width", slider=True)
        layout.prop(d, "guide_points", expand=True)
        layout.prop(d, "guide_color_mode", text="Colors", expand=True)
        if d.guide_color_mode == "layer":
            layout.prop(d, "guide_color_layer")
        pills(layout, context, [(d, "guides_smoothed", None)], width)
        layout.separator(type="LINE")
        layout.label(text="Tubes")
        pills(layout, context, [(t, "tube_shaded", None), (t, "tube_wireframe", None), (t, "tube_colors", None),
                                (t, "tube_level_colors", "Level colors")], width)


class GROOMLAB_PT_group_segments(_Popover, bpy.types.Panel):
    """A guide group's Segments (the selected guides', else the group's): By length, and what it
    applies to."""
    bl_label = "Segments"

    def draw(self, context):
        layout = self.layout
        t = context.scene.groomlab_tools
        layout.prop(t, "resample_segments", text="Segments", slider=True)
        layout.prop(t, "segments_by_length", text=defs.text("segmentsByLength"), toggle=True)
        layout.label(text="Applies to %s" % t.segments_scope if t.segments_scope else "No guides yet")


class GROOMLAB_PT_corrective_weight(_Popover, bpy.types.Panel):
    """A corrective's weight on this frame and its key (Blender's keyframes)."""
    bl_label = "Weight"

    def draw(self, context):
        layout = self.layout
        scalp = active_groom(context)
        kind, name = tools.picked_corrective(context.scene)
        w = scalp.groomlab_tools.corrective_weights.get(name) if scalp is not None and kind == "layer" else None
        if w is None:
            layout.label(text="Pick a corrective")
            return
        row = layout.row(align=True)
        row.prop(w, "weight", slider=True)
        row.operator("groomlab.corrective_key", text="", icon="DECORATE_KEYFRAME")


class GROOMLAB_PT_map_info(_Popover, bpy.types.Panel):
    """A map's info: its kind, what reads it (Show in Graph), where it shows, a color map's channel."""
    bl_label = "Map"
    bl_ui_units_x = 13

    def draw(self, context):
        layout = self.layout
        t = context.scene.groomlab_tools
        m = tools.current_map(context.scene)
        if m is None:
            layout.label(text="No map picked")
            return
        row = layout.row(align=True)
        row.label(text=m.name)
        sub = row.row(align=True)
        sub.alignment = "RIGHT"
        sub.label(text=defs.choice_label("mapKind", m.kind) + (", color" if m.channels > 1 else ""))
        _wrap(layout, "Read by %s" % (t.map_read_by or "nothing yet (a Map node or a map param reads it)"),
              width=40)
        layout.operator("groomlab.map_show_reader", icon="NODETREE")
        layout.label(text="Show on")
        row = layout.row(align=True)
        row.prop(t, "show_on_guides", toggle=True)
        row.prop(t, "show_on_strands", toggle=True)
        row.prop(t, "show_on_scalp", toggle=True)
        if m.channels > 1:
            layout.prop(t, "scalp_channel", text="Scalp shows")


class GROOMLAB_MT_tube_levels(bpy.types.Menu):
    """How the tube levels show"""
    bl_label = "Level View"

    def draw(self, context):
        t = context.scene.groomlab_tools
        self.layout.prop(t, "tube_lock_view")
        self.layout.prop(t, "tube_mixed_view")
        self.layout.prop(t, "tube_level_colors", text="Colors per level")


class GROOMLAB_MT_tube_library(bpy.types.Menu):
    """The tube shape library: rename, new, duplicate (how a built-in is edited), delete"""
    bl_label = "Shape Library"

    def draw(self, context):
        layout = self.layout
        layout.operator("groomlab.tube_shape_rename", text="Rename...")
        layout.operator("groomlab.tube_shape_new", text="New Shape")
        layout.operator("groomlab.tube_shape_new", text="Duplicate").duplicate = True
        layout.separator()
        row = layout.row()
        row.alert = True
        row.operator("groomlab.tube_shape_remove", text="Delete Shape")


class GroomToolsSubpanel(object):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "GroomLab"
    bl_parent_id = "VIEW3D_PT_groomlab_tools"

    @classmethod
    def poll(cls, context):
        s = active_groom(context)
        return s is not None and tools.is_hierarchy(s)


class GROOMLAB_UL_groups(bpy.types.UIList):
    """A guide group's row: its color, its name (indented under its parent), the guides, then
    (ROW_ORDER) its segments, its eye and its menu."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        # The color it draws in (its own, else its automatic one): Color... in its menu changes it.
        swatch = row.row(align=True)
        swatch.enabled = False
        swatch.scale_x = 0.3
        swatch.prop(item, "color", text="")
        name = row.row(align=True)
        name.active = not item.hidden
        name.label(text="    " * item.depth + item.name)
        sub = row.row(align=True)
        sub.alignment = "RIGHT"
        sub.label(text="%d" % item.guides)
        row_button(sub, "GROUPS", index, text="%d" % item.segments if item.segments else "", icon="CURVE_PATH",
                   panel=GROOMLAB_PT_group_segments.__name__)
        if any(g.tube_level for g in data.groups):
            # A tube hierarchy level's tube: its tubes kept in view whichever level's guides are
            # picked (pressed while kept); again: the tubes follow their guides. Apart from the
            # segments' count; a row of no level keeps its place, so the eyes stay in a column.
            look(sub, "gap").separator(factor=0.5)
            if item.tube_level:
                kept = data.tube_view_group == item.name
                sub.operator("groomlab.tube_view", text="", icon="MESH_CYLINDER", emboss=False,
                             depress=kept).group = "" if kept else item.name
            else:
                look(sub, "blank").label(text="", icon="BLANK1")
        op = sub.operator("groomlab.group_edit", text="", icon="HIDE_ON" if item.hidden else "HIDE_OFF", emboss=False)
        op.action = "HIDE"
        op.group = item.name
        row_button(sub, "GROUPS", index, menu_name="GROOMLAB_MT_group_context")


class GROOMLAB_UL_maps(bpy.types.UIList):
    """A map's row: its name, RGBA on a color map, then its info and its menu."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        row.label(text=item.name)
        sub = row.row(align=True)
        sub.alignment = "RIGHT"
        if item.channels > 1:
            sub.label(text="RGBA")
        row_button(sub, "MAPS", index, icon="INFO", panel=GROOMLAB_PT_map_info.__name__)
        row_button(sub, "MAPS", index, menu_name="GROOMLAB_MT_map_context")


class GROOMLAB_UL_parts(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        layout.label(text=item.name, icon="IPO_LINEAR")


class GROOMLAB_UL_correctives(bpy.types.UIList):
    """A row of the stack: a corrective (its name, its keys, then its weight, Solo, eye and menu), a
    Sculpt node (its edits and menu) or the base groom."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        if item.kind == "layer":
            name = row.row(align=True)
            name.active = item.enabled
            name.label(text=item.name, icon="SHAPEKEY_DATA")
            sub = row.row(align=True)
            sub.alignment = "RIGHT"
            if item.keyed:
                sub.label(text="◆ %d" % item.keys)
            row_button(sub, "CORRECTIVES", index, text="%.1f" % item.weight_now, icon="MOD_VERTEX_WEIGHT",
                       panel=GROOMLAB_PT_corrective_weight.__name__)
            others = [r for r in data.correctives if r.kind == "layer" and r.name != item.name]
            solo = item.enabled and others and not any(r.enabled for r in others)
            sub.operator("groomlab.corrective_solo", text="", icon="SOLO_ON" if solo else "SOLO_OFF",
                         emboss=False).name = item.name
            sub.operator("groomlab.corrective_enable", text="", icon="HIDE_OFF" if item.enabled else "HIDE_ON",
                         emboss=False).name = item.name
            row_button(sub, "CORRECTIVES", index, menu_name="GROOMLAB_MT_corrective_context")
        elif item.kind == "node":
            row.label(text="Node: " + item.name, icon="NODE")
            sub = row.row(align=True)
            sub.alignment = "RIGHT"
            if item.guides:
                sub.label(text="%d edited" % item.guides)
            row_button(sub, "CORRECTIVES", index, menu_name="GROOMLAB_MT_sculpt_node_context")
        else:
            row.label(text=item.name, icon="OUTLINER_OB_CURVES")
            sub = row.row(align=True)
            sub.alignment = "RIGHT"
            sub.label(text="base")


class VIEW3D_PT_groomlab_tools_display(GroomToolsSubpanel, bpy.types.Panel):
    """The strands' colors in the viewport (properties.json's Display: the guides' display is the
    header's guides popover)."""
    bl_label = "Display"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        d = active_groom(context).groomlab_tools
        layout.prop(d, "strand_color")
        if d.strand_color in ("colorSet", "attribute", "clump"):
            layout.prop(d, "strand_color_source")
        if d.strand_color == "attribute":
            row = layout.row(align=True)
            row.prop(d, "strand_color_min")
            row.prop(d, "strand_color_max")
            layout.operator("groomlab.auto_strand_color_range")
        if d.strand_color != "solid":
            layout.operator("groomlab.show_strand_colors", icon="SHADING_SOLID")


# Under the main panel: Display (the strands' colors), folded.
classes = (VIEW3D_PT_groomlab_tools, GROOMLAB_OT_row_popup, GROOMLAB_OT_map_show_reader, GROOMLAB_PT_guide_display,
           GROOMLAB_PT_group_segments, GROOMLAB_PT_corrective_weight, GROOMLAB_PT_map_info, GROOMLAB_MT_tube_levels,
           GROOMLAB_MT_tube_library, GROOMLAB_UL_groups, GROOMLAB_UL_maps, GROOMLAB_UL_parts,
           GROOMLAB_UL_correctives, VIEW3D_PT_groomlab_tools_display)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    if bpy.app.timers.is_registered(_refresh_lists):
        bpy.app.timers.unregister(_refresh_lists)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
