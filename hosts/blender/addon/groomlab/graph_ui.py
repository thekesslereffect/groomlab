# SPDX-License-Identifier: GPL-3.0-or-later
"""The Graph Editor's menus, labels and colors from GroomLab's shared UI definitions
(engine/ui/*.json, docs/UI_DEFINITIONS.md), as Maya's Graph Editor builds its own: the node,
backdrop, Edit, Add Node, Presets and param menus, the category, port and backdrop colors.

The files ship in the extension's ui/ folder (a source tree reads engine/ui); no engine is loaded.
`resolve` is Maya's definitions.resolve for the host "blender". Each action id is bound here to
the operator that runs it (`bind`, filled by graph_editor.py and graph_authoring.py); a menu is
drawn from a container with `draw`: Blender's own menus, submenus through one generic menu class,
each entry's tooltip its action's tip (`ActionTip`)."""


import bpy
from bpy.props import StringProperty

from . import definitions as _defs

HOST = "blender"

# Actions the Graph Editor's menus list that Blender has no counterpart for (left out of its menus;
# a test checks every other id has a binding): Maya's viewport GPU preview.
NOT_IN_BLENDER = {"graph.gpuPreview"}

# The files and their reading: the reader every Python host shares (engine/ui/ui_definitions.py,
# through definitions.py), here forgiving of a missing value as the menus need.
folder = _defs.folder
document = _defs.document
fill = _defs.fill


def get(path, default=None):
    """A value by its slash path ("graph/nodeMenu"); `default` when missing (or the files are)."""
    try:
        return _defs.get(path)
    except (KeyError, IOError, ValueError, TypeError):
        return default


def action(action_id):
    return (get("actions") or {}).get(action_id, {})


def label(action_id, fallback=""):
    return action(action_id).get("label", fallback)


def tip(action_id, fallback=""):
    return action(action_id).get("tip", fallback)


def choices(name):
    return (get("choices/" + name) or {}).get("items", [])


# ---- Colors (graph_ui.json: a theme overrides them in Maya; Blender has the defaults) ------------

def _rgb(text):
    text = text.lstrip("#")
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def category_colors(fallback=None):
    """{category: (r, g, b) 0-255}: the node bodies' tint, the accent bar, the Add menu's icons."""
    colors = get("graph/categoryColors")
    return {k: _rgb(v) for k, v in colors.items()} if colors else dict(fallback or {})


def port_colors(fallback=None):
    colors = get("graph/portColors")
    return {k: _rgb(v) for k, v in colors.items()} if colors else dict(fallback or {})


def backdrop_colors():
    """[(label, [r, g, b])]: the backdrops' Color menu (Maya's)."""
    return [(c["label"], list(c["rgb"])) for c in get("graph/backdropColors", [])]


def plumbing(fallback=None):
    items = get("graph/plumbing")
    return {tuple(p) for p in items} if items else set(fallback or ())


# ---- Resolving a container: the shared reader's (Maya's definitions.resolve) ----------------------

def resolve(entries, state=(), values=None, generators=None, host=HOST):
    """`entries` for `host`, as items ({"kind": "divider" | "action" | "submenu" | "generator", ...}):
    ui_definitions.resolve, every Python host's (host labels and tips included)."""
    return _defs.resolve(entries or [], state or (), values, generators, host)


def listed_ids(entries, host=HOST):
    """Every action id `entries` can show on `host` (conditions ignored): for the binding check."""
    return _defs._ui.listed_ids(entries or [], host)


# ---- Bindings: action id -> the operator that runs it ---------------------------------------------

class Binding(object):
    """`idname` run with the properties `props(ctx, item)` gives; `checked(ctx, item)`: whether a
    checkable entry shows checked."""

    def __init__(self, idname, props=None, checked=None):
        self.idname, self.props, self.checked = idname, props, checked


ACTIONS = {}


def bind(action_id, idname, props=None, checked=None):
    ACTIONS[action_id] = Binding(idname, props, checked)


class MenuContext(object):
    """What a menu's bindings read: the groom, its tree, the node (its graph name), the param, the
    backdrop (a frame's name)."""

    def __init__(self, scalp=None, tree=None, node=None, param=None, frame=None, **extra):
        self.scalp, self.tree, self.node, self.param, self.frame = scalp, tree, node, param, frame
        self.__dict__.update(extra)


class ActionTip(object):
    """An operator whose tooltip is its menu entry's: the `action` id (or the class's `action_id`)
    in actions.json, else its docstring."""

    action_id = ""

    @classmethod
    def description(cls, context, properties):
        action_id = getattr(properties, "action", "") or cls.action_id
        text = tip(action_id) if action_id else ""
        return text or (cls.__doc__ or "").strip()


def action_prop():
    return StringProperty(options={"HIDDEN", "SKIP_SAVE"})


# ---- Drawing ------------------------------------------------------------------------------------

_submenus = {}   # key -> the prepared entries a GROOMLAB_MT_graph_submenu draws


def prepare(items, ctx, key):
    """Resolved items as what Blender draws: ("divider",), ("label", text), ("op", text, idname,
    props, icon, enabled, action id), ("menu", text, key). Unbound actions are left out (Blender has
    none: NOT_IN_BLENDER)."""
    out = []
    for i, item in enumerate(items):
        kind = item["kind"]
        if kind == "divider":
            if out and out[-1][0] != "divider":
                out.append(("divider",))
        elif kind == "submenu":
            sub = prepare(item["items"], ctx, "%s/%s" % (key, item.get("id") or i))
            if any(e[0] != "divider" for e in sub):
                subkey = "%s/%s" % (key, item.get("id") or i)
                _submenus[subkey] = (item["label"], sub)
                out.append(("menu", item["label"], subkey))
        elif kind == "action":
            b = ACTIONS.get(item["id"])
            if b is None:
                continue
            props = dict(b.props(ctx, item) if b.props else {})
            if b.idname.startswith("groomlab."):
                props.setdefault("action", item["id"])
            icon = "NONE"
            if item["checkable"]:
                on = bool(b.checked(ctx, item)) if b.checked else False
                icon = "CHECKBOX_HLT" if on else "CHECKBOX_DEHLT"
            out.append(("op", item["label"], b.idname, props, icon, item["enabled"], item["id"]))
    while out and out[-1][0] == "divider":
        out.pop()
    return out


def draw_prepared(layout, entries):
    for e in entries:
        if e[0] == "divider":
            layout.separator()
        elif e[0] == "label":
            layout.label(text=e[1])
        elif e[0] == "menu":
            layout.context_string_set("groomlab_menu", e[2])
            layout.menu(GROOMLAB_MT_graph_submenu.bl_idname, text=e[1])
        elif e[0] == "op":
            _, text, idname, props, icon, enabled, _id = e
            target = layout
            if not enabled:
                target = layout.column()
                target.enabled = False
            op = target.operator(idname, text=text, icon=icon)
            for k, v in props.items():
                try:
                    setattr(op, k, v)
                except (AttributeError, TypeError):
                    pass


def draw(layout, entries, ctx, key, state=(), values=None, generators=None):
    """A container (a list of entries, or a slash path into the files) drawn into `layout`."""
    if isinstance(entries, str):
        key = key or entries
        entries = get(entries, [])
    if isinstance(entries, dict):
        entries = entries.get("items", [])
    prepared = prepare(resolve(entries, state, values, generators), ctx, key)
    draw_prepared(layout, prepared)
    return prepared


class GROOMLAB_MT_graph_submenu(bpy.types.Menu):
    """A submenu of a menu made from the UI definitions"""
    bl_idname = "GROOMLAB_MT_graph_submenu"
    bl_label = "GroomLab"

    def draw(self, context):
        key = getattr(context, "groomlab_menu", "") or ""
        _label, entries = _submenus.get(key, ("", []))
        draw_prepared(self.layout, entries)


classes = (GROOMLAB_MT_graph_submenu,)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
    _submenus.clear()
