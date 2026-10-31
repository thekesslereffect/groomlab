# SPDX-License-Identifier: GPL-3.0-or-later
"""Groom Tools: Maya's GroomLab palette (hosts/maya/scripts/groomlab/ui/palette.py) as the 3D
view sidebar's GroomLab tab, built from the shared UI definitions (engine/ui/*.json: palette.json's
header, empty state, tabs, the tool cards' texts, the Guide groups section and first-groom steps;
lists.json's bars and right-click menus; choices.json's option lists), in the redesign's order
(research/ui-redesign/QT.md, "Groom Tools layout"): the groom picker with its eyes, the Graph
Editor and a More menu; how to start a groom, then the first-groom steps; the Tools, Paint,
Correctives and Photos tabs; the tool grid (tiles by the catalog's categories, with Maya's icons;
the Paint tile alone on Paint); the tool's card right under it (its name, a ? with the whole hint,
one line, then its blocks: Brush, the tool's own, Applies to, exactly the settings the catalog lists
for it); then the tab's list (Paint's maps, Correctives' layers, the photos; the Tools tab's Guide
groups as the subpanel under the card) with the picked item's own settings under it; GroomLab's
undo; While sculpting (folded) and the groom's Display (Blender's own).

An action shows when actions.py binds it (the rest is hidden). Drawing never writes Blender data:
the lists (groups, maps, parts, tubes, correctives) are refreshed by the tools after each edit, or
by a timer when the panel shows another groom than they were filled for."""

import json
import os

import bpy

from . import groom, icons, menu, photos, process, tools, updates
from . import definitions as defs
from .ops import active_groom
from .ui import _wrap

NO_TOOL = "Pick a tool, then drag over the groom in the viewport."
# Blender's keys (palette.json's gestures are Maya's: middle-drag, the hotkey set).
GESTURES = ("B + drag: radius · M + drag: strength · Ctrl: the opposite (Length shortens, Lift lays down, Shape shrinks; "
            "painting, toward 0) · Shift+drag: paint the selection (the rest frozen) · Shift+click: add the guides in the "
            "ring (on none: deselect and unfreeze all) · painting maps or Freeze, and Shape, Shift+drag: smooth")
FLOW_HINTS = {"": defs.text("flowArrows"), "*": defs.text("flowSinks")}
TABS = {"TOOLS": "tools", "PAINT": "paint", "CORRECTIVES": "correctives", "PHOTOS": "photos"}  # palette.json's ids
STARTER = tuple((s["id"], s["label"], s["tip"]) for s in defs.get("palette/starter/steps"))
_lists_for = [None]


def _chars(context):
    region = getattr(context, "region", None)
    width = region.width if region is not None and region.width > 50 else 300
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
    return "%d arrow%s \u00b7 %d sink%s" % (arrows, "" if arrows == 1 else "s", sinks, "" if sinks == 1 else "s")


def section_shown(section_id, context):
    """Whether a Groom Tools section shows (palette.json's "tools": the active tool one of them;
    "tabs": the current tab one of them; neither: always), as Maya's and the app's."""
    s = defs.section(section_id)
    t = context.scene.groomlab_tools
    if s.get("tools") and (t.tool or "") not in s["tools"]:
        return False
    return not s.get("tabs") or TABS.get(t.tab, "") in s["tabs"]


def tile(layout, tool, active):
    """A tool's tile: its icon and name (the name and hint in the tooltip: GROOMLAB_OT_tool.description)."""
    info = tools.tool_info(tool)
    op = layout.operator("groomlab.tool", text=info["label"] if info else tool, icon_value=icons.icon(tool),
                         depress=active == tool)
    op.tool = tool
    return op


TILE_SCALE = 1.4  # the tiles a little taller than a row


def tool_grid(layout, tool_ids, active, columns=0):
    """Labeled tiles of one size, as many to a row as the sidebar's width holds (Blender's
    automatic columns), as Maya's and the app's tool palettes."""
    grid = layout.grid_flow(row_major=True, columns=columns, even_columns=True, even_rows=True, align=True)
    grid.scale_y = TILE_SCALE
    for t in tool_ids:
        tile(grid, t, active)
    return grid


def label(sid, fallback=""):
    """A catalog setting's label (Maya's)."""
    return tools.setting_label(sid, fallback)


def heading(layout, text):
    row = layout.row()
    row.label(text=text.upper())
    return row


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


def list_with_menu(layout, list_type, data, prop, active_prop, menu_idname, rows=4):
    """A list with its right-click menu also behind a button beside it (Blender's specials menu)."""
    row = layout.row()
    row.template_list(list_type, "", data, prop, data, active_prop, rows=rows)
    if menu_idname:
        col = row.column(align=True)
        col.menu(menu_idname, text="", icon="DOWNARROW_HLT")
    return row


def block(layout, title, actions=None):
    """One of the tool card's blocks (Maya's widgets.Block): a hairline, its small title (and any
    action icons on its right), then its rows: (column, header row)."""
    layout.separator(type="LINE")
    col = layout.column()
    head = col.row(align=True)
    head.label(text=title)
    return col, head


def section_header(layout, title, count=None):
    """A list's header (Guide groups, Maps, Layers): its name and how many; the row for its icons."""
    layout.separator(type="LINE")
    row = layout.row(align=True)
    row.label(text=title.upper() + ("   %d" % count if count is not None else ""))
    return row


def bar_icons(layout, path, key, context, ids, glyphs=None):
    """Some entries of a list's bar (lists.json) as icon buttons, in `ids`' order: a list's header
    (New, Import, Fix Frame) or its picked item's actions (rename, convert, delete...)."""
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
                "corrective.fixFrame": "DECORATE_KEYFRAME", "photo.import": "IMPORT"}


def help_text(tool):
    """The card's ? tooltip: the tool's name and whole hint, then (the brushes) the gestures, one
    a line."""
    info = tools.tool_info(tool)
    if not info:
        return "Pick a tool"
    text = "%s: %s" % (info["label"], info["hint"])
    if "radius" in info.get("settings", []):
        text += "\n\n" + "\n".join(GESTURES.split(" · "))
    return text


def short_text(tool):
    """The card's one line under the tool's name (palette.json's palette.tools)."""
    entry = (defs.get("palette/tools") or {}).get(tool)
    return entry["short"] if isinstance(entry, dict) else ""


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
        # a groom makes it current), its eyes (the strands, the guides; Shift+click or right-click:
        # every groom's), the Graph Editor, More (palette.json's header).
        row = layout.row(align=True)
        row.prop(scene.groomlab, "groom", text="")
        if scalp is not None:
            d = scalp.groomlab_tools
            op = row.operator("groomlab.eye", text="", icon="HIDE_OFF" if d.show_strands else "HIDE_ON")
            op.what, op.mode = "STRANDS", "TOGGLE"
            op = row.operator("groomlab.eye", text="", icon="OUTLINER_DATA_CURVES", depress=bool(d.show_guides))
            op.what, op.mode = "GUIDES", "TOGGLE"
        row.operator("groomlab.graph_open", text="", icon_value=icons.icon("graph"))
        row.menu("GROOMLAB_MT_tools_more", text="", icon="DOWNARROW_HLT")
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
        if t.guide_count == 0 and not t.starter_dismissed:
            self.draw_starter(layout, context)
        if t.status:
            _wrap(layout, t.status, icon="INFO", width=_chars(context))
        # The tabs, then the tool grid (the Tools and Correctives tabs; the Paint tile alone on
        # Paint), the tool's card right under it, then the list the tab works on.
        layout.row().prop(t, "tab", expand=True)
        if t.tab in ("TOOLS", "CORRECTIVES"):
            for category, tlist in tools.sections():
                ids = [x["id"] for x in tlist]
                if category == "Paint" or ids == ["paint"]:
                    continue  # the painting brush lives on the Paint tab, beside the maps it paints
                layout.label(text=category)
                grid = tool_grid(layout, ids, t.tool)
                if "freeze" in ids:  # beside Freeze, a tile: every guide brushable again (its tooltip says so)
                    sub = grid.row(align=True)
                    sub.scale_y = TILE_SCALE
                    sub.operator("groomlab.guides_edit", text="Unfreeze All",
                                 icon_value=icons.icon("unfreeze")).action = "UNFREEZE_ALL"
        elif t.tab == "PAINT":
            tool_grid(layout, ["paint"], t.tool)
        if t.tab == "PHOTOS":
            photos.draw_tab(layout, context, _chars(context))
        else:
            self.draw_card(layout, context, t, scalp, "paint" if t.tab == "PAINT" else
                           (t.tool if t.tool != "paint" else ""))
            if t.tab == "PAINT":
                self.draw_maps(layout, context, t)
            elif t.tab == "CORRECTIVES":
                self.draw_correctives(layout, context, t, scalp)
        row = layout.row(align=True)
        undo, redo = tools.undo_label(scalp), tools.undo_label(scalp, redo=True)
        sub = row.row(align=True)
        sub.enabled = bool(undo)
        sub.operator("groomlab.tool_undo", text="Undo %s" % undo if undo else "Undo", icon="LOOP_BACK").redo = False
        sub = row.row(align=True)
        sub.enabled = bool(redo)
        sub.operator("groomlab.tool_undo", text="Redo %s" % redo if redo else "Redo", icon="LOOP_FORWARDS").redo = True

    def draw_empty(self, layout, context):
        """palette.json's empty state: Create Groom, Add Sample Mesh, Import (what Blender runs)."""
        empty = defs.get("palette/empty")
        box = layout.box()
        box.label(text=empty["title"])
        _wrap(box, "Select the scalp mesh, then:", width=_chars(context))
        for item in menu.resolve(empty["items"], context):
            if item["kind"] == "action":
                menu.draw_action(box, item, context, icon="ADD" if item["id"] == "groom.create" else None)
            elif item["kind"] == "submenu":
                box.menu(menu.submenu_idname("empty", item["id"]), text=item["label"], icon="IMPORT")

    def draw_starter(self, layout, context):
        box = layout.box()
        row = box.row()
        row.label(text=defs.get("palette/starter/title"))
        row.prop(context.scene.groomlab_tools, "starter_dismissed", text="", icon="X", emboss=False)
        first = True
        for key, text, tip in STARTER:
            op = box.operator("groomlab.tool" if key == "guides" else "groomlab.graph_open",
                              text=("▶  " if first else "○  ") + text, emboss=False)
            if key == "guides":
                op.tool = "place"
            if first:
                _wrap(box, tip, width=_chars(context))
            first = False

    def draw_maps(self, layout, context, t):
        """The Paint tab's list (under the Paint card): the maps, then the picked map's box: its
        actions, where it shows, what reads it."""
        head = section_header(layout, "Maps", len(t.maps))
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        bar_icons(sub, "lists/maps/bar", "mapsbar", context, ("map.import", "map.new"))
        list_with_menu(layout, "GROOMLAB_UL_maps", t, "maps", "map_index", "GROOMLAB_MT_map_context", rows=3)
        m = tools.current_map(context.scene)
        box = layout.box()
        row = box.row(align=True)
        row.alignment = "RIGHT"
        bar_icons(row, "lists/maps/bar", "mapsbar", context, ("map.rename", "map.convert", "map.delete"))
        box.label(text="Show on")
        row = box.row(align=True)
        row.prop(t, "show_on_guides", toggle=True)
        row.prop(t, "show_on_scalp", toggle=True)
        if m is not None and m.channels > 1:
            row.prop(t, "scalp_channel", text="")
        if m is not None:
            _wrap(box, "Read by %s" % (t.map_read_by or "nothing yet (a Map node or a map param reads it)"),
                  width=_chars(context))

    def draw_correctives(self, layout, context, t, scalp):
        """The Correctives tab's list (Maya's CorrectivesBox): the stack, top first, the Sculpt
        nodes, the base groom; the picked row is what brushes edit; its box: solo, up, down,
        delete, the weight keyed with Blender's keyframes."""
        head = section_header(layout, "Layers", sum(1 for r in t.correctives if r.kind == "layer"))
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        bar_icons(sub, "lists/correctives/bar", "correctivesbar", context, ("corrective.fixFrame", "corrective.new"))
        list_with_menu(layout, "GROOMLAB_UL_correctives", t, "correctives", "corrective_index",
                       menu.correctives_menu(context), rows=4)
        kind, name = tools.picked_corrective(context.scene)
        w = scalp.groomlab_tools.corrective_weights.get(name) if kind == "layer" else None
        box = layout.box()
        row = box.row(align=True)
        row.alignment = "RIGHT"
        bar_icons(row, "lists/correctives/bar", "correctivesbar", context,
                  ("corrective.solo", "corrective.up", "corrective.down", "corrective.delete"))
        row = box.row(align=True)
        if w is not None:
            row.prop(w, "weight", slider=True)
            row.operator("groomlab.corrective_key", text="", icon="DECORATE_KEYFRAME")
        else:
            row.enabled = False
            row.label(text="Weight: pick a corrective")

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
            col.prop(t, "alpha_invert", toggle=True)

    def draw_tube_fields(self, layout, t):
        col = layout.column(align=True)
        col.prop(t, "tube_width", slider=True)
        col.prop(t, "tube_flatness", slider=True)
        col.prop(t, "tube_twist", slider=True)

    def draw_card(self, layout, context, t, scalp, tool):
        """The tool's card: its name, a ? (the whole hint and the gestures in its tooltip) and one
        line, then its blocks in order: Brush, the tool's own, Applies to."""
        info = tools.tool_info(tool) if tool else None
        layout.separator(type="LINE")  # a plain section: a hairline, no box
        if info is None:
            _wrap(layout, NO_TOOL, width=_chars(context))
            return
        row = layout.row(align=True)
        row.label(text=info["label"], icon_value=icons.icon(tool))
        row.operator("groomlab.tool_help", text="", icon="QUESTION", emboss=False).tool = tool
        _wrap(layout, short_text(tool) or info["hint"], width=_chars(context))
        uses = set(info["settings"])
        card = layout.column()
        if "radius" in uses:  # Brush: how the stroke feels
            col, _ = block(card, defs.text("brushBlock"))
            sliders = col.column(align=True)
            sliders.prop(t, "radius", text=label("radius", "Radius"), slider=True)
            sliders.prop(t, "strength", text="Strength (±)" if info.get("signed") else label("strength", "Strength"),
                         slider=True)
            sliders.prop(t, "falloff", text=label("falloff", "Falloff"), slider=True)
            if "smoothStrength" in uses:
                sliders.prop(t, "smooth_strength", text=label("smoothStrength", "Smooth"), slider=True)
            # Its options as toggles, in one order whatever the tool (Selected only and Front only
            # are not in the panel: the brushes keep their defaults, off and on).
            toggles = [(k, p, text) for k, p, text in (("pressure", "pressure", "Pressure"), ("collide", "collide", "Off scalp"))
                       if k in uses]
            if toggles:
                row = col.row(align=True)
                for _k, p, text in toggles:
                    row.prop(t, p, text=text, toggle=True)
            if "falloffProfile" in uses:
                col.prop(t, "falloff_curve", text=label("falloffProfile", "Falloff curve"))
            if "strengthProfile" in uses:
                col.prop(t, "strength_profile", text=label("strengthProfile", "Root to tip"))
        self.draw_tool_blocks(card, context, t, info, uses)
        if "targets" in uses or "symmetry" in uses:  # Applies to: where the stroke lands
            col, _ = block(card, defs.text("appliesBlock"))
            if "targets" in uses:
                col.label(text=label("targets", "Acts on"))
                row = col.row(align=True)
                for p in ("target_guides", "target_parts", "target_flow"):
                    row.prop(t, p, toggle=True)
            if "symmetry" in uses:
                row = col.row(align=True)
                row.prop(t, "symmetry", text="Mirror", toggle=True)
                sub = row.row(align=True)
                sub.enabled = t.symmetry
                sub.prop(t, "symmetry_mode", text="")
        row = layout.row(align=True)
        row.operator("groomlab.tool_exit", text="Leave Tool (Esc)", icon="CANCEL")

    def draw_tool_blocks(self, card, context, t, info, uses):
        """The tool's own blocks, named for what they set."""
        if "map" in uses:  # Paint: the mode, the value (or color) and Fill, the stamp
            col, _ = block(card, info["label"])
            m = tools.current_map(context.scene)
            col.label(text="Mode")
            col.row(align=True).prop(t, "paint_mode", expand=True)
            if m is None or m.channels == 1:
                row = col.row(align=True)
                row.prop(t, "value", slider=True)
                fill = row.row(align=True)
                fill.enabled = m is not None
                fill.operator("groomlab.map_fill", text="Fill")
            if m is not None and m.channels > 1:
                row = col.row(align=True)
                row.prop(t, "paint_color", text="Color")
                for ch in ("channel_r", "channel_g", "channel_b", "channel_a"):
                    row.prop(t, ch, toggle=True)
                row.operator("groomlab.map_fill", text="Fill")
            if "alpha" in uses and m is not None and m.kind in ("vertices", "texture"):  # a guide map takes no stamp
                self.draw_alpha(col, t)
        if "newGuides" in uses:  # Place, Draw: the new guides and the tube they get
            col, _ = block(card, label("newGuides", "New guides"))
            sliders = col.column(align=True)
            sliders.prop(t, "length", slider=True)
            sliders.prop(t, "segments", slider=True)
            if "spacing" in uses:
                sliders.prop(t, "spacing", slider=True)
            row = col.row(align=True)
            row.prop(t, "match_neighbors", text="Match nearby", toggle=True)
            if "soloNewGuides" in uses:
                row.prop(t, "solo_new_guides", text="Solo new", toggle=True)
            if "tube" in uses:
                col, _ = block(card, label("tube", "Tube"))
                col.prop(t, "tube_shape", text="")
                col.label(text=defs.text("profileHint"))
                self.draw_tube_fields(col, t)
        if "parts" in uses:
            col, head = block(card, label("parts", "Parts"))
            items = [i for i in menu.resolve(defs.get("lists/parts/bar"), context, conditions=False) if i["kind"] == "action"]
            sub = head.row(align=True)
            sub.alignment = "RIGHT"
            for item in items:
                if item["id"] == "parts.fromCurves":
                    menu.draw_action(sub, item, context, text="", icon="CURVE_BEZCURVE")
            col.prop(t, "part_name")
            col.template_list("GROOMLAB_UL_parts", "", t, "parts", t, "part_index", rows=3)
            row = col.row(align=True)
            for item in items:
                if item["id"] != "parts.fromCurves":
                    menu.draw_action(row, item, context)
        if "flow" in uses:
            col, head = block(card, label("flow", "Flow"))
            sub = head.row(align=True)
            sub.alignment = "RIGHT"
            sub.enabled = bool(t.flow_arrows or t.flow_sinks)
            # Clear All (GroomLab > Utilities > Delete All Flow Arrows too).
            sub.operator("groomlab.flow_clear", text="Clear All", emboss=False)
            col.prop(t, "flow_sink_item")
            col.label(text=flow_count_text(t.flow_arrows, t.flow_sinks))
        if "lengthKeys" in uses:
            self.draw_lengths(card, context, t)
        if "stretch" in uses:  # Comb: keep lengths, or stretch
            col, _ = block(card, info["label"])
            col.prop(t, "stretch", text="Stretch while combing", toggle=True)
        if "tubeChannel" in uses:  # Shape: what it paints
            col, _ = block(card, label("tubeChannel", "Paints"))
            col.row(align=True).prop(t, "tube_channel", expand=True)
        if "selection" in uses:
            col, _ = block(card, label("selection", "Selection"))
            col.row(align=True).prop(t, "select_shape", expand=True)
            col.prop(t, "soft_radius", slider=True)
            row = col.row(align=True)
            row.label(text="%d selected" % t.selected_count)
            row.operator("groomlab.guides_edit", text="All").action = "SELECT_ALL"
            row.operator("groomlab.guides_edit", text="None").action = "CLEAR"
        if "tubeChannel" in uses or "selection" in uses:  # Shape, Select: the profile and the selected guides
            self.draw_profile(card, context, t)
        if "erases" in uses:
            col, _ = block(card, label("erases", "Erases"))
            row = col.row(align=True)
            for p in ("erase_guides", "erase_parts", "erase_flow"):
                row.prop(t, p, toggle=True)
            _wrap(col, defs.text("eraseNote"), width=_chars(context))

    def draw_profile(self, card, context, t):
        """Profile (the shape picked, Apply, the library with its New / Duplicate / Rename / Delete,
        Edit as Custom) and Selected guides (their Width, Flatness, Twist and Smooth)."""
        col, head = block(card, "Profile")
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        sub.operator("groomlab.tube_shape_new", text="", icon="ADD", emboss=False)
        sub.operator("groomlab.tube_shape_new", text="", icon="DUPLICATE", emboss=False).duplicate = True
        sub.operator("groomlab.tube_shape_rename", text="", icon="GREASEPENCIL", emboss=False)
        sub.operator("groomlab.tube_shape_remove", text="", icon="TRASH", emboss=False)
        row = col.row(align=True)
        row.prop(t, "tube_shape", text="")
        apply = row.row(align=True)
        apply.enabled = t.tube_targets > 0
        apply.operator("groomlab.tube_apply", text="Apply")
        col.template_list("GROOMLAB_UL_tube_shapes", "", t, "tube_shapes", t, "tube_shape_index", rows=2)
        if t.tube_choice not in [x.name for x in t.tube_shapes]:
            col.operator("groomlab.tube_shape_custom", text="Edit as Custom")
        col, head = block(card, "Selected guides")
        sub = head.row(align=True)
        sub.alignment = "RIGHT"
        sub.enabled = t.tube_targets > 0
        sub.prop(t, "tube_smooth_what", text="")
        sub.operator("groomlab.tube_smooth", text="Smooth")
        how = "selected" if t.tube_source == "selection" else "soloed"
        col.label(text="%d guide%s %s" % (t.tube_targets, "" if t.tube_targets == 1 else "s", how) if t.tube_targets
                  else "No guides selected: the values go to new guides")
        self.draw_tube_fields(col, t)


class GroomToolsSubpanel(object):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "GroomLab"
    bl_parent_id = "VIEW3D_PT_groomlab_tools"

    section_id = None  # palette.json's: shown with the tools and tabs it lists (section_shown)

    @classmethod
    def poll(cls, context):
        s = active_groom(context)
        return (s is not None and tools.is_hierarchy(s)
                and (cls.section_id is None or section_shown(cls.section_id, context)))


def _section(section_id):
    """A section's label and fold from palette.json."""
    s = defs.section(section_id)
    return s["label"], (set() if s.get("expanded") else {"DEFAULT_CLOSED"})


class GROOMLAB_UL_groups(bpy.types.UIList):
    """A guide group's row: its color, its name (indented under its parent), the guides, an eye."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        # The color it draws in (its own, else its automatic one): Color... in the right-click
        # menu changes it (one undo step).
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
        op = sub.operator("groomlab.group_edit", text="", icon="HIDE_ON" if item.hidden else "HIDE_OFF", emboss=False)
        op.action = "HIDE"
        op.group = item.name


class GROOMLAB_UL_maps(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        kind = defs.choice_label("mapKind", item.kind)
        row.label(text=item.name, icon="TEXTURE" if item.kind == "texture" else "VERTEXSEL" if item.kind == "vertices"
                  else "CURVES")
        sub = row.row()
        sub.alignment = "RIGHT"
        sub.label(text=kind + (" (color)" if item.channels > 1 else ""))


class GROOMLAB_UL_parts(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        layout.label(text=item.name, icon="IPO_LINEAR")


class GROOMLAB_UL_tube_shapes(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        row.label(text=item.name, icon="MESH_CIRCLE")
        sub = row.row()
        sub.alignment = "RIGHT"
        sub.label(text="%d guide%s" % (item.users, "" if item.users == 1 else "s"))


class GROOMLAB_UL_correctives(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        if item.kind == "layer":
            row.label(text=item.name, icon="SHAPEKEY_DATA" if item.enabled else "HIDE_ON")
            sub = row.row()
            sub.alignment = "RIGHT"
            sub.label(text="%d%%" % round(100 * item.weight_now))
            sub.label(text="◆ keyed" if item.keyed else "all frames")
        elif item.kind == "node":
            row.label(text="Node: " + item.name, icon="NODE")
            if item.guides:
                sub = row.row()
                sub.alignment = "RIGHT"
                sub.label(text="%d edited" % item.guides)
        else:
            row.label(text=item.name, icon="OUTLINER_OB_CURVES")


class VIEW3D_PT_groomlab_tools_guides(GroomToolsSubpanel, bpy.types.Panel):
    """Guide groups, right under the Tools tab's card: the groups (a click picks the one brushes
    edit and new guides go into), Isolate and + in the header; the picked group's box: rename,
    select, delete, its Segments and Edit Guides."""
    bl_label, bl_options = _section("groups")
    section_id = "groups"

    def draw_header_preset(self, context):
        t = context.scene.groomlab_tools
        row = self.layout.row(align=True)
        row.label(text="%d" % len(t.groups))
        row.prop(t, "isolate", text="", icon="RESTRICT_VIEW_OFF" if t.isolate else "RESTRICT_VIEW_ON", emboss=False)
        bar_icons(row, "lists/groups/bar", "groupsbar", context, ("group.new",))

    def draw(self, context):
        layout = self.layout
        t = context.scene.groomlab_tools
        lists = defs.get("lists/groups")
        list_with_menu(layout, "GROOMLAB_UL_groups", t, "groups", "group_index", "GROOMLAB_MT_group_context", rows=3)
        box = layout.box()
        row = box.row(align=True)
        row.alignment = "RIGHT"
        bar_icons(row, "lists/groups/bar", "groupsbar", context, ("group.rename", "group.select", "group.delete"))
        box.prop(t, "resample_segments", text="Segments (%s)" % t.segments_scope if t.segments_scope else "Segments",
                 slider=True)
        row = box.row(align=True)
        row.menu("GROOMLAB_MT_guide_edits", text=lists["edits"]["label"])
        row.prop(t, "children_count", text="x")
        box.label(text="%d guides, %d selected, %d frozen" % (t.guide_count, t.selected_count, t.frozen_count))


# The strokes that draw the strands while live (While sculpting): every brush that strokes, Place
# and Draw (palette.json's old section; Blender has no other place for these two settings).
SCULPTING_TOOLS = ("place", "draw", "flow", "comb", "slide", "smooth", "straighten", "lift", "twist", "curl", "attract",
                   "direction", "noise", "shape", "cut", "length", "erase")


class VIEW3D_PT_groomlab_tools_sculpting(GroomToolsSubpanel, bpy.types.Panel):
    """How the strands draw while a stroke is live (Maya and the app: the groom's own settings;
    Blender keeps them here, folded)."""
    bl_label = "While sculpting"
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return super(VIEW3D_PT_groomlab_tools_sculpting, cls).poll(context) and (context.scene.groomlab_tools.tool or "") in SCULPTING_TOOLS

    def draw(self, context):
        t = context.scene.groomlab_tools
        self.layout.prop(t, "sculpt_display")
        self.layout.prop(t, "full_density")


class VIEW3D_PT_groomlab_tools_display(GroomToolsSubpanel, bpy.types.Panel):
    bl_label = "Display"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        d = active_groom(context).groomlab_tools
        row = layout.row(align=True)
        for p in ("show_guides", "show_tubes", "show_flow", "show_strands"):
            row.prop(d, p, toggle=True)
        layout.prop(d, "guide_points")
        layout.prop(d, "guide_color_mode")
        if d.guide_color_mode == "layer":
            layout.prop(d, "guide_color_layer")
        layout.prop(d, "strand_color")
        if d.strand_color in ("colorSet", "attribute"):
            layout.prop(d, "strand_color_source")
            row = layout.row(align=True)
            row.prop(d, "strand_color_min")
            row.prop(d, "strand_color_max")
        if d.strand_color != "solid":
            layout.operator("groomlab.show_strand_colors", icon="SHADING_SOLID")


# Under the main panel (Blender draws subpanels in their registration order): the guide groups
# (palette.json's one section, the Tools tab's list), While sculpting, then Blender's Display.
classes = (VIEW3D_PT_groomlab_tools, GROOMLAB_UL_groups, GROOMLAB_UL_maps, GROOMLAB_UL_parts, GROOMLAB_UL_tube_shapes,
           GROOMLAB_UL_correctives, VIEW3D_PT_groomlab_tools_guides, VIEW3D_PT_groomlab_tools_sculpting,
           VIEW3D_PT_groomlab_tools_display)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    if bpy.app.timers.is_registered(_refresh_lists):
        bpy.app.timers.unregister(_refresh_lists)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
