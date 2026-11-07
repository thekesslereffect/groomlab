# SPDX-License-Identifier: GPL-3.0-or-later
"""The GroomLab menu in Blender's top bar and every menu the shared UI definitions describe
(engine/ui/*.json, docs/UI_DEFINITIONS.md): Maya's GroomLab menu (menus.json "main") in its order,
the Groom Tools' More menu, Output strip menus and empty state, and the lists' right-click menus
(guide groups and Edit Guides, maps, correctives, Sculpt nodes). An item shows when actions.py binds
its id (Blender runs it) and its conditions hold; the rest is hidden. Each submenu is a Menu class
made at registration from the files (GROOMLAB_MT_<menu>_<submenu>)."""

import bpy
from bpy.props import EnumProperty

from . import actions, definitions, icons, process, tool_ops
from .ui import _wrap

DOCS_URL = actions.DOCS_URL
UPDATES_URL = actions.UPDATES_URL
DONATE_URL = actions.DONATE_URL
DISCORD_URL = actions.DISCORD_URL
REPO_URL = "https://github.com/thekesslereffect/groomlab"  # the source: open source (Apache-2.0; this add-on GPL)
ABOUT = ("Hair, fur and feather grooming in Blender: the Groom Tools for the guides, the Graph Editor for the "
         "look, Cycles and EEVEE renders, and a .groom file Maya and the GroomLab app open as it is.")
# The Groom Tools' tabs Blender has (palette.json's tabs: all of them).
TABS = ("tools", "paint", "correctives", "photos")
# A bar entry's glyph (lists.json) as a Blender icon.
GLYPHS = {"rename": "GREASEPENCIL", "select": "RESTRICT_SELECT_OFF", "delete": "TRASH", "convert": "FILE_REFRESH",
          "up": "TRIA_UP", "down": "TRIA_DOWN", "solo": "SOLO_OFF"}
# Menus built from a container of the files: (class name, key of its submenus' classes, slash path,
# whether entries' conditions apply; an action bar shows every entry).
ROOTS = (
    ("GROOMLAB_MT_main", "main", "menus/main", True),
    ("GROOMLAB_MT_tools_more", "more", "palette/header/more", True),
    ("GROOMLAB_MT_guide_edits", "edits", "lists/groups/edits", True),
    ("GROOMLAB_MT_group_context", "groups", "lists/groups/context", True),
    ("GROOMLAB_MT_map_context", "maps", "lists/maps/context", True),
    ("GROOMLAB_MT_corrective_context", "correctives", "lists/correctives/context", True),
    ("GROOMLAB_MT_sculpt_node_context", "sculptnode", "lists/correctives/nodeContext", True),
    ("GROOMLAB_MT_photo_context", "photos", "lists/photos/context", True),
    ("GROOMLAB_MT_pair_context", "pairs", "lists/photos/pairsContext", True),
)
# Containers drawn inline in a panel whose submenus still need classes.
INLINE = (("output", "palette/output"), ("empty", "palette/empty"), ("mapsbar", "lists/maps/bar"))


def tab_items():
    """The Groom Tools' tabs as Blender enum items (palette.json's, those Blender has)."""
    return [(t["id"].upper(), t["label"], t.get("tip", "")) for t in definitions.tabs() if t["id"] in TABS]


# ---- What holds (the definitions' "needs" and "enabled" conditions) ---------------------------------

def state(context):
    """The conditions that hold now: groom, catalog, group, group.color, map, map.texture,
    map.color, map.scalp, photo."""
    from . import photos, tools
    from .ops import active_groom
    s = set()
    scene = getattr(context, "scene", None)
    if scene is None:
        return s
    if active_groom(context) is not None:
        s.add("groom")
    if tools.cached_catalog():
        s.add("catalog")
    t = scene.groomlab_tools
    g = next((g for g in t.groups if g.name == t.group), None)
    if g is not None:
        s.add("group")
        if g.has_color:
            s.add("group.color")
    m = tools.current_map(scene)
    if m is not None:
        s.add("map")
        if m.kind == "texture":
            s.add("map.texture")
        if m.channels > 1:
            s.add("map.color")
        if m.kind in ("vertices", "texture"):
            s.add("map.scalp")
    if photos.current(scene) is not None:
        s.add("photo")
    return s


def resolve(entries, context, conditions=True):
    """`entries` for Blender: bound actions only, conditions met (`conditions`, else every entry
    as an action bar shows them), the host's generators left out."""
    return definitions.resolve(entries, state(context) if conditions else None, keep=actions.bound,
                               drop_generators=True)


def _key(text):
    return "".join(c for c in str(text).lower() if c.isalnum())


def submenu_idname(key, item_id):
    return "GROOMLAB_MT_%s_%s" % (key, _key(item_id))


def draw_action(layout, item, context, text=None, icon=None, emboss=True):
    """One bound action as Blender draws it: its operator (or a setting's checkbox), the
    definitions' label, the item's value on the binding's property."""
    idname, props = actions.binding(item["id"])
    label = item["label"] if text is None else text
    where = layout
    if not item.get("enabled", True):
        where = layout.row()
        where.enabled = False
    if idname.startswith("prop:"):
        data = context.scene
        path = idname[len("prop:"):].split(".")
        if path[0] == "groom":  # a setting of the active groom (its scalp object's), not the scene's
            from .ops import active_groom
            data, path = active_groom(context), path[1:]
            if data is None:
                return None
        for part in path[:-1]:
            data = getattr(data, part)
        where.prop(data, path[-1], text=label)
        return None
    kwargs = {"text": label, "emboss": emboss}
    stem = item.get("icon")
    if icon is not None:
        kwargs["icon"] = icon
    elif stem:
        kwargs["icon_value"] = icons.icon(stem)
    op = where.operator(idname, **kwargs)
    props = dict(props)
    value_prop = props.pop("$value", "value")
    for k, v in props.items():
        setattr(op, k, v)
    if item.get("value") is not None:
        setattr(op, value_prop, item["value"])
    return op


def draw_items(layout, items, key, context):
    """Resolved items into a Blender menu (or a panel's column): actions, dividers, submenus."""
    for item in items:
        kind = item["kind"]
        if kind == "divider":
            layout.separator()
        elif kind == "submenu":
            layout.menu(submenu_idname(key, item["id"]), text=item["label"])
        elif kind == "action":
            draw_action(layout, item, context)


def draw_container(layout, path, key, context, conditions=True):
    node = definitions.get(path)
    entries = node["items"] if isinstance(node, dict) else node
    draw_items(layout, resolve(entries, context, conditions), key, context)


def draw_root(layout, idname, context):
    """A root menu's entries drawn into `layout` (a right-click menu appends them)."""
    for name, key, path, conditions in ROOTS:
        if name == idname:
            draw_container(layout, path, key, context, conditions)
            return


# ---- Menu classes from the files -------------------------------------------------------------------

class DefinitionMenu(object):
    """A menu whose entries are a container of the definitions (`_entries`), its submenus' classes
    named from `_key`."""
    _entries = ()
    _key = ""
    _conditions = True

    def draw(self, context):
        draw_items(self.layout, resolve(self._entries, context, self._conditions), self._key, context)
        extra = EXTRAS.get(self.bl_idname)
        if extra is not None:
            extra(self.layout, context)


def _guide_edit_extras(layout, context):
    """Blender's own guide edits after Maya's (the brushes' selection and freeze work without a
    marking menu here): lock, freeze, delete, select."""
    layout.separator()
    for key in ("LOCK", "UNLOCK", "FREEZE", "UNFREEZE", "UNFREEZE_ALL", "DELETE", "SELECT_ALL"):
        label = next(l for k, l, _t in tool_ops.GUIDE_EXTRAS if k == key)
        layout.operator("groomlab.guides_edit", text=label).action = key


def _more_extras(layout, context):
    layout.separator()
    layout.operator("groomlab.tool_exit")
    layout.operator("groomlab.shortcuts", text="Keyboard Shortcuts...")
    layout.operator("groomlab.window_toggle")


EXTRAS = {"GROOMLAB_MT_guide_edits": _guide_edit_extras, "GROOMLAB_MT_tools_more": _more_extras}


def _menu_class(idname, key, entries, label, tip="", conditions=True):
    return type(idname, (DefinitionMenu, bpy.types.Menu), {
        "bl_idname": idname, "bl_label": label, "bl_description": definitions.plain(tip),
        "_entries": entries, "_key": key, "_conditions": conditions})


def _submenus(entries, key, conditions, out):
    """Classes for every submenu in `entries` (and theirs), for Blender (hosts filtered)."""
    for entry in entries:
        if not isinstance(entry, dict) or not definitions._for_host(entry, definitions.HOST):
            continue
        if "include" in entry:
            _submenus(definitions.get(entry["include"]), key, conditions, out)
        elif "submenu" in entry or "ref" in entry:
            item_id = entry.get("id", entry.get("ref"))
            container = definitions._container(entry["ref"]) if "ref" in entry else {}
            idname = submenu_idname(key, item_id)
            items = definitions.items_of(entry)
            if idname not in out:
                out[idname] = _menu_class(idname, "%s_%s" % (key, _key(item_id)), items,
                                          entry.get("submenu") or container.get("label", ""),
                                          entry.get("tip", container.get("tip", "")), conditions)
            _submenus(items, "%s_%s" % (key, _key(item_id)), conditions, out)


def definition_menus():
    """Every Menu class the definitions make: the roots, then their submenus'."""
    out = {}
    for idname, key, path, conditions in ROOTS:
        node = definitions.get(path)
        entries = node["items"] if isinstance(node, dict) else node
        label = node.get("label", "") if isinstance(node, dict) else ""
        tip = node.get("tip", "") if isinstance(node, dict) else ""
        if idname == "GROOMLAB_MT_main":
            label, tip = "GroomLab", "GroomLab: grooming tools, the node graph, grooms and exports"
        elif idname == "GROOMLAB_MT_tools_more":
            label, tip = "More", definitions.get("palette/header/more").get("tip", "")
        out[idname] = _menu_class(idname, key, entries, label or idname.split("_MT_")[1].replace("_", " ").title(), tip,
                                  conditions)
        _submenus(entries, key, conditions, out)
    for key, path in INLINE:
        node = definitions.get(path)
        _submenus(node["items"] if isinstance(node, dict) else node, key, True, out)
    return list(out.values())


# ---- Operators -------------------------------------------------------------------------------------

def version():
    h = process.hello() if process.running() else {}
    if h.get("version"):
        return h["version"]
    try:
        import addon_utils
        for mod in addon_utils.modules():
            if mod.__name__ == __package__:
                info = addon_utils.module_bl_info(mod)
                return ".".join(str(v) for v in info.get("version", ()))
    except Exception:
        pass
    return ""


class GROOMLAB_OT_groom_tools(bpy.types.Operator):
    """Brushes, guide groups, painted maps and sculpt layers: the 3D view sidebar's GroomLab tab"""
    bl_idname = "groomlab.groom_tools"
    bl_label = "Groom Tools"

    tab: EnumProperty(items=tab_items(), default="TOOLS", options={"SKIP_SAVE"})

    def execute(self, context):
        from . import props
        with props.quiet():
            context.scene.groomlab_tools.tab = self.tab
        wm = context.window_manager
        shown = 0
        for window in wm.windows:
            for area in window.screen.areas:
                if area.type == "VIEW_3D":
                    area.spaces.active.show_region_ui = True
                    area.tag_redraw()
                    shown += 1
        if not shown and not bpy.app.background:
            self.report({"INFO"}, "Open a 3D Viewport: Groom Tools are in its sidebar (N), GroomLab tab")
        return {"FINISHED"}


class _TextPopup(bpy.types.Operator):
    lines = ()
    width = 460

    def invoke(self, context, event):
        return context.window_manager.invoke_popup(self, width=self.width)

    def execute(self, context):
        return {"FINISHED"}


class GROOMLAB_OT_shortcuts(_TextPopup):
    """The brush and Graph Editor keys: change the pie menu's key in Preferences > Add-ons > GroomLab"""
    bl_idname = "groomlab.shortcuts"
    bl_label = "GroomLab: Keyboard Shortcuts"

    def draw(self, context):
        layout = self.layout
        layout.label(text="GroomLab: Keyboard Shortcuts")
        col = layout.column(align=True)
        for keys, what in tool_ops.shortcuts():
            row = col.row()
            split = row.split(factor=0.38)
            split.label(text=tool_ops.pie_key() if keys == "Y" else keys)
            split.label(text=what)
        layout.separator()
        layout.label(text="The pie menu's key: Preferences > Add-ons > GroomLab", icon="PREFERENCES")


class GROOMLAB_OT_about(_TextPopup):
    """The version and where the add-on lives"""
    bl_idname = "groomlab.about"
    bl_label = "About GroomLab"

    def draw(self, context):
        layout = self.layout
        row = layout.row()
        row.label(text="GroomLab %s" % (version() or ""), icon_value=icons.icon("logo"))
        _wrap(layout, ABOUT, width=64)
        _wrap(layout, "Open source (Apache-2.0); this add-on is GPL-3.0-or-later. Free for any use, commercial "
                      "included.", width=64)
        layout.operator("wm.url_open", text="Source on GitHub", icon="URL").url = REPO_URL
        layout.operator("wm.url_open", text="Discord", icon="URL").url = DISCORD_URL
        layout.operator("wm.url_open", text="Support GroomLab", icon="FUND").url = DONATE_URL


def draw_topbar(self, context):
    self.layout.menu("GROOMLAB_MT_main")


# The lists' right-click menus (Blender's list item context menu): the definitions' for our lists.
LIST_MENUS = {"GROOMLAB_UL_groups": "GROOMLAB_MT_group_context", "GROOMLAB_UL_maps": "GROOMLAB_MT_map_context",
              "GROOMLAB_UL_photos": "GROOMLAB_MT_photo_context", "GROOMLAB_UL_photo_pairs": "GROOMLAB_MT_pair_context"}


def draw_list_context(self, context):
    ui_list = getattr(context, "ui_list", None)
    name = getattr(ui_list, "bl_idname", "") if ui_list is not None else ""
    if name == "GROOMLAB_UL_correctives":
        name = correctives_menu(context)
    else:
        name = LIST_MENUS.get(name)
    if name:
        self.layout.separator()
        draw_root(self.layout, name, context)


def draw_eye_context(self, context):
    """Right-click on the Groom Tools' eyes: Show / Hide for every groom (palette.json's header items)."""
    op = getattr(context, "button_operator", None)
    if op is None or getattr(op.bl_rna, "identifier", "") != "GROOMLAB_OT_eye" or op.every:
        return
    items = resolve(definitions.get("palette/header/%s/items" % ("guides" if op.what == "GUIDES" else "visible")), context)
    self.layout.separator()
    draw_items(self.layout, items, "eye", context)


def correctives_menu(context):
    """The Correctives list's right-click menu for the picked row: a corrective's, a Sculpt node's,
    none for the base groom."""
    t = context.scene.groomlab_tools
    if 0 <= t.corrective_index < len(t.correctives):
        kind = t.correctives[t.corrective_index].kind
        return {"layer": "GROOMLAB_MT_corrective_context", "node": "GROOMLAB_MT_sculpt_node_context"}.get(kind)
    return None


_classes = []


def register():
    _classes[:] = [GROOMLAB_OT_groom_tools, GROOMLAB_OT_shortcuts, GROOMLAB_OT_about] + definition_menus()
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_editor_menus.append(draw_topbar)
    bpy.types.UI_MT_list_item_context_menu.append(draw_list_context)
    bpy.types.UI_MT_button_context_menu.append(draw_eye_context)


def unregister():
    bpy.types.UI_MT_button_context_menu.remove(draw_eye_context)
    bpy.types.UI_MT_list_item_context_menu.remove(draw_list_context)
    bpy.types.TOPBAR_MT_editor_menus.remove(draw_topbar)
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
    _classes[:] = []
