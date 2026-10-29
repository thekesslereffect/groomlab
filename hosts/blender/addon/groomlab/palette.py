# SPDX-License-Identifier: GPL-3.0-or-later
"""Groom Tools: Maya's GroomLab palette (hosts/maya/scripts/groomlab/ui/palette.py) as the 3D
view sidebar's GroomLab tab, built from the shared UI definitions (engine/ui/*.json: palette.json's
header, empty state, tabs, sections, Output strip and first-groom steps; lists.json's bars and
right-click menus; choices.json's option lists). The groom picker with its eye, the Graph Editor
and a More menu; how to start a groom, then the first-groom steps; the Group the brushes edit and
Isolate; the Tools, Paint and Correctives tabs (tiles by the catalog's categories, with Maya's
icons); the active tool's card with exactly the settings the catalog lists for it, by Maya's labels
and hints; GroomLab's undo; and, folded, the sections in palette.json's order (Tubes, While
sculpting, Guide groups & edits, Output), each only with the tools and tabs it lists (Tubes with
Place, Draw, Shape and Select...), then the groom's Display (Blender's own).

An action shows when actions.py binds it (the rest is hidden). Drawing never writes Blender data:
the lists (groups, maps, parts, tubes, correctives) are refreshed by the tools after each edit, or
by a timer when the panel shows another groom than they were filled for."""

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
            "ring (on none: deselect and unfreeze all) · painting maps or Freeze, Shift+drag: smooth")
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
    info = tools.tool_info(tool)
    op = layout.operator("groomlab.tool", text=info["label"] if info else tool, icon_value=icons.icon(tool),
                         depress=active == tool)
    op.tool = tool
    return op


def tool_grid(layout, tool_ids, active, columns=4):
    grid = layout.grid_flow(row_major=True, columns=columns, even_columns=True, even_rows=True, align=True)
    grid.scale_y = 1.4
    for t in tool_ids:
        tile(grid, t, active)
    return grid


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
        row = layout.row(align=True)
        row.label(text="Group")
        row.prop(t, "group", text="")
        row.prop(t, "isolate", toggle=True)
        if t.status:
            _wrap(layout, t.status, icon="INFO", width=_chars(context))
        layout.row().prop(t, "tab", expand=True)
        if t.tab == "TOOLS":
            for category, tlist in tools.sections():
                heading(layout, category)
                ids = [x["id"] for x in tlist]
                grid = tool_grid(layout, ids, t.tool)
                if "freeze" in ids:  # beside Freeze: every guide brushable again
                    grid.operator("groomlab.guides_edit", text="Unfreeze All",
                                  icon_value=icons.icon("unfreeze")).action = "UNFREEZE_ALL"
        elif t.tab == "PAINT":
            self.draw_paint(layout, context, t)
        elif t.tab == "PHOTOS":
            photos.draw_tab(layout, context, _chars(context))
        else:
            self.draw_correctives(layout, context, t, scalp)
        if t.tab in ("TOOLS", "PAINT") or (t.tab == "CORRECTIVES" and t.tool):
            self.draw_card(layout, context, t, scalp)
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

    def draw_paint(self, layout, context, t):
        tool_grid(layout, ["paint"], t.tool)
        draw_bar(layout, "lists/maps/bar", "mapsbar", context)
        list_with_menu(layout, "GROOMLAB_UL_maps", t, "maps", "map_index", "GROOMLAB_MT_map_context")
        m = tools.current_map(context.scene)
        col = layout.column(align=True)
        col.prop(t, "paint_mode", expand=True)
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
        row = layout.row(align=True)
        row.label(text="Show on")
        row.prop(t, "show_on_guides", toggle=True)
        row.prop(t, "show_on_scalp", toggle=True)
        if m is not None and m.channels > 1:
            row.prop(t, "scalp_channel", text="")
        if m is not None:
            _wrap(layout, "Read by: %s" % (t.map_read_by or "nothing yet (a Map node or a map param reads it)"),
                  width=_chars(context))
        _wrap(layout, "A map is a 0-1 value painted on the scalp that any param can read (the Graph Editor's Map node).",
              width=_chars(context))

    def draw_correctives(self, layout, context, t, scalp):
        """The Correctives tab (Maya's CorrectivesBox): the stack, top first, the Sculpt nodes, the
        base groom; the picked row is what brushes edit; a corrective's weight keyed with Blender's
        keyframes."""
        lists = defs.get("lists/correctives")
        _wrap(layout, defs.plain(lists["about"]), width=_chars(context))
        draw_bar(layout, "lists/correctives/bar", "correctivesbar", context)
        list_with_menu(layout, "GROOMLAB_UL_correctives", t, "correctives", "corrective_index",
                       menu.correctives_menu(context), rows=5)
        kind, name = tools.picked_corrective(context.scene)
        w = scalp.groomlab_tools.corrective_weights.get(name) if kind == "layer" else None
        row = layout.row(align=True)
        if w is not None:
            row.prop(w, "weight", slider=True)
            row.operator("groomlab.corrective_key", text="", icon="DECORATE_KEYFRAME")
        else:
            row.enabled = False
            row.label(text="Weight: pick a corrective")

    def draw_alpha(self, card, t, label):
        """Paint's alpha on a scalp map: the library's grid (the thumbnail opens it), Add and the
        folder beside it; while one is picked, its rotation, spacing and invert."""
        row = card.row(align=False)
        row.label(text=label("alpha", "Alpha"))
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
            angle.prop(t, "alpha_angle")
            col.prop(t, "alpha_spacing")
            col.prop(t, "alpha_invert")

    def draw_card(self, layout, context, t, scalp):
        tool = t.tool
        info = tools.tool_info(tool) if tool else None
        heading(layout, info["label"] if info else "Tool")
        if info is None:
            _wrap(layout, NO_TOOL, width=_chars(context))
            return
        _wrap(layout, info["hint"], width=_chars(context))
        uses = set(info["settings"])
        card = layout.box()
        label = tools.setting_label
        if "radius" in uses:
            col = card.column(align=True)
            col.prop(t, "radius", text=label("radius", "Radius"))
            col.prop(t, "strength", text="Strength (±)" if info.get("signed") else label("strength", "Strength"),
                     slider=True)
            col.prop(t, "falloff", text=label("falloff", "Falloff"), slider=True)
            if "falloffProfile" in uses:
                col.prop(t, "falloff_curve", text=label("falloffProfile", "Falloff curve"))
            if "strengthProfile" in uses:
                col.prop(t, "strength_profile", text=label("strengthProfile", "Root to tip"))
            if "smoothStrength" in uses:
                col.prop(t, "smooth_strength", text=label("smoothStrength", "Smooth"))
        m = tools.current_map(context.scene) if "alpha" in uses else None
        if m is not None and m.kind in ("vertices", "texture"):  # a guide map takes no alpha
            self.draw_alpha(card, t, label)
        if "tubeChannel" in uses:
            row = card.row(align=True)
            row.label(text=label("tubeChannel", "Paints"))
            row.prop(t, "tube_channel", expand=True)
        checks = [(k, p) for k, p in (("symmetry", "symmetry"), ("selectedOnly", "selected_only"),
                                      ("pressure", "pressure"), ("collide", "collide"),
                                      ("frontFacingOnly", "front_facing")) if k in uses]
        if checks:
            grid = card.grid_flow(row_major=True, columns=2, even_columns=True, align=False)
            for k, p in checks:
                grid.prop(t, p, text=label(k, k))
        if "symmetry" in uses and t.symmetry:
            card.row(align=True).prop(t, "symmetry_mode", expand=True)
        if "newGuides" in uses:
            box = card.column()
            box.label(text=label("newGuides", "New guides").upper())
            box.prop(t, "match_neighbors")
            box.prop(t, "length")
            box.prop(t, "segments")
            if "spacing" in uses:
                box.prop(t, "spacing")
            if "soloNewGuides" in uses:
                box.prop(t, "solo_new_guides", text=label("soloNewGuides", "Solo new guides"))
        if "parts" in uses:
            box = card.column()
            box.label(text=label("parts", "Parts").upper())
            box.prop(t, "part_name")
            box.template_list("GROOMLAB_UL_parts", "", t, "parts", t, "part_index", rows=3)
            row = box.row(align=True)
            for item in menu.resolve(defs.get("lists/parts/bar"), context, conditions=False):
                if item["kind"] == "action":
                    menu.draw_action(row, item, context)
        if "flow" in uses:
            box = card.column()
            box.label(text=label("flow", "Flow").upper())
            box.row(align=True).prop(t, "flow_sink_item")
            # Where the arrows live (on the groom) and how many, and Clear All (GroomLab > Utilities >
            # Delete All Flow Arrows).
            row = box.row(align=True)
            row.label(text=flow_count_text(t.flow_arrows, t.flow_sinks))
            sub = row.row(align=True)
            sub.enabled = bool(t.flow_arrows or t.flow_sinks)
            sub.operator("groomlab.flow_clear", text="Clear All")
            _wrap(box, FLOW_HINTS["" if t.flow_sink_item == "ARROWS" else "*"], width=_chars(context))
        if "stretch" in uses:
            card.prop(t, "stretch", text=label("stretch", "Stretch"))
        if "erases" in uses:
            row = card.row(align=True)
            row.label(text=label("erases", "Erases"))
            for p in ("erase_guides", "erase_parts", "erase_flow"):
                row.prop(t, p, toggle=True)
            _wrap(card, defs.text("eraseNote"), width=_chars(context))
        if "targets" in uses:
            row = card.row(align=True)
            row.label(text=label("targets", "Acts on"))
            for p in ("target_guides", "target_parts", "target_flow"):
                row.prop(t, p, toggle=True)
        if "map" in uses:
            m = tools.current_map(context.scene)
            card.label(text="%s: %s" % (label("map", "Map"), m.name if m else "make one in the Paint tab"),
                       icon="BRUSH_DATA")
        if "selection" in uses:
            box = card.column()
            box.label(text=label("selection", "Selection").upper())
            box.prop(t, "select_shape", expand=True)
            box.prop(t, "soft_radius")
            row = box.row(align=True)
            row.label(text="%d selected" % t.selected_count)
            row.operator("groomlab.guides_edit", text="All").action = "SELECT_ALL"
            row.operator("groomlab.guides_edit", text="None").action = "CLEAR"
            _wrap(box, "Drag: lasso (or box) · click: a guide · Shift: toggle · Ctrl: deselect · the rest freeze, so "
                       "brushes work on the selection (nothing picked: all unfrozen)", width=_chars(context))
        if "freezeMask" in uses:
            box = card.column()
            box.label(text=label("freezeMask", "Freeze mask").upper())
            row = box.row(align=True)
            row.label(text="%d frozen" % t.frozen_count)
            row.operator("groomlab.guides_edit", text="Unfreeze All").action = "UNFREEZE_ALL"
        if "tube" in uses:
            card.label(text="Tube shape, width, flatness, twist: the Tubes section below", icon="INFO")
        if "radius" in uses:
            _wrap(card, GESTURES, width=_chars(context))
        row = layout.row(align=True)
        row.operator("groomlab.tool_exit", text="Leave Tool (Esc)", icon="CANCEL")


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


class VIEW3D_PT_groomlab_tools_tubes(GroomToolsSubpanel, bpy.types.Panel):
    bl_label, bl_options = _section("tubes")
    section_id = "tubes"

    def draw(self, context):
        layout = self.layout
        t = context.scene.groomlab_tools
        if t.tube_targets:
            how = "selected" if t.tube_source == "selection" else "soloed"
            text = "%d guide%s %s: changes here go to %s, and to the guides Place and Draw make." % (
                t.tube_targets, "" if t.tube_targets == 1 else "s", how, "it" if t.tube_targets == 1 else "them")
        else:
            text = ("No guides selected or soloed: the shape and values here go to the guides Place and Draw make. "
                    "Select guides (Select tool, or Shift with a brush) to set theirs.")
        _wrap(layout, text, width=_chars(context))
        layout.prop(t, "tube_shape")
        library = t.tube_choice in [x.name for x in t.tube_shapes]
        if not library:
            row = layout.row()
            row.operator("groomlab.tube_shape_custom", text="Edit as Custom")
        row = layout.row()
        row.template_list("GROOMLAB_UL_tube_shapes", "", t, "tube_shapes", t, "tube_shape_index", rows=2)
        col = row.column(align=True)
        col.operator("groomlab.tube_shape_new", text="", icon="ADD")
        col.operator("groomlab.tube_shape_new", text="", icon="DUPLICATE").duplicate = True
        col.operator("groomlab.tube_shape_rename", text="", icon="GREASEPENCIL")
        col.operator("groomlab.tube_shape_remove", text="", icon="TRASH")
        sub = layout.row()
        sub.enabled = t.tube_targets > 0
        sub.operator("groomlab.tube_apply", text="Apply to Selected")
        col = layout.column(align=True)
        col.prop(t, "tube_width")
        col.prop(t, "tube_flatness", slider=True)
        col.prop(t, "tube_twist")
        row = layout.row(align=True)
        row.enabled = t.tube_targets > 0
        row.operator("groomlab.tube_smooth", text="Smooth")
        row.prop(t, "tube_smooth_what", text="")
        col = layout.column()
        col.enabled = t.tube_has_fill
        col.prop(t, "tube_fill")
        sub = col.row()
        sub.enabled = t.tube_fill
        sub.prop(t, "tube_blend", slider=True)


class VIEW3D_PT_groomlab_tools_sculpting(GroomToolsSubpanel, bpy.types.Panel):
    bl_label, bl_options = _section("sculptDisplay")
    section_id = "sculptDisplay"

    def draw(self, context):
        t = context.scene.groomlab_tools
        self.layout.prop(t, "sculpt_display")
        self.layout.prop(t, "full_density")


class GROOMLAB_UL_groups(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        row.label(text="    " * item.depth + item.name)
        sub = row.row(align=True)
        sub.alignment = "RIGHT"
        # The color it draws in (its own, else its automatic one): Color... in the right-click
        # menu changes it (one undo step).
        swatch = sub.row(align=True)
        swatch.enabled = False
        swatch.scale_x = 0.4
        swatch.prop(item, "color", text="")
        sub.label(text="%d" % item.guides)
        sub.label(text="L%d" % item.level)
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
    bl_label, bl_options = _section("groups")
    section_id = "groups"

    def draw(self, context):
        layout = self.layout
        t = context.scene.groomlab_tools
        lists = defs.get("lists/groups")
        _wrap(layout, defs.plain(lists["about"]), width=_chars(context))
        draw_bar(layout, "lists/groups/bar", "groupsbar", context)
        list_with_menu(layout, "GROOMLAB_UL_groups", t, "groups", "group_index", "GROOMLAB_MT_group_context")
        row = layout.row(align=True)
        row.menu("GROOMLAB_MT_guide_edits", text=lists["edits"]["label"])
        row.prop(t, "children_count", text="x")
        layout.prop(t, "resample_segments", text="Segments (%s)" % t.segments_scope if t.segments_scope else "Segments")
        layout.label(text="%d guides, %d selected, %d frozen" % (t.guide_count, t.selected_count, t.frozen_count))


class VIEW3D_PT_groomlab_tools_output(GroomToolsSubpanel, bpy.types.Panel):
    bl_label, bl_options = _section("output")
    section_id = "output"

    @classmethod
    def poll(cls, context):
        return active_groom(context) is not None and section_shown(cls.section_id, context)

    def draw(self, context):
        """palette.json's Output strip: what Blender runs of it (actions, and the Export / Import menus)."""
        layout = self.layout
        flow = layout.grid_flow(row_major=True, columns=2, even_columns=True, align=True)
        for item in menu.resolve(defs.get("palette/output"), context):
            if item["kind"] == "action":
                menu.draw_action(flow, item, context)
            elif item["kind"] == "submenu":
                flow.menu(menu.submenu_idname("output", item["id"]), text=item["label"],
                          icon={"export": "EXPORT", "import": "IMPORT"}.get(item["id"], "NONE"))


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


# The sections in palette.json's order (Blender draws subpanels in their registration order), then
# Blender's Display.
SECTIONS = {"tubes": VIEW3D_PT_groomlab_tools_tubes, "sculptDisplay": VIEW3D_PT_groomlab_tools_sculpting,
            "groups": VIEW3D_PT_groomlab_tools_guides, "output": VIEW3D_PT_groomlab_tools_output}
classes = ((VIEW3D_PT_groomlab_tools, GROOMLAB_UL_groups, GROOMLAB_UL_maps, GROOMLAB_UL_parts, GROOMLAB_UL_tube_shapes,
            GROOMLAB_UL_correctives)
           + tuple(SECTIONS[s["id"]] for s in defs.get("palette/sections") if s["id"] in SECTIONS)
           + (VIEW3D_PT_groomlab_tools_display,))


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    if bpy.app.timers.is_registered(_refresh_lists):
        bpy.app.timers.unregister(_refresh_lists)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
