# SPDX-License-Identifier: GPL-3.0-or-later
"""The Graph Editor's menus, labels and colors from GroomLab's shared UI definitions
(engine/ui/*.json, docs/UI_DEFINITIONS.md), as Maya's Graph Editor builds its own: the node,
backdrop, Edit, Add Node, Presets and param menus, the category, port and backdrop colors.

The files ship in the extension's ui/ folder (a source tree reads engine/ui); no engine is loaded.
`resolve` is Maya's definitions.resolve for the host "blender". Each action id is bound here to
the operator that runs it (`bind`, filled by graph_editor.py and graph_authoring.py); a menu is
drawn from a container with `draw`: Blender's own menus, submenus through one generic menu class,
each entry's tooltip its action's tip (`ActionTip`)."""

import json
import os
import re

import bpy
from bpy.props import StringProperty

HOST = "blender"
FILES = ("actions", "menus", "palette", "lists", "choices", "marking_menu", "graph_ui", "properties")
_HERE = os.path.dirname(os.path.abspath(__file__))
# The extension's copy (GroomLabBlender/ui), else the source tree's.
SEARCH = (os.path.join(_HERE, "ui"),
          os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, os.pardir, "engine", "ui")))

# Actions the Graph Editor's menus list that Blender has no counterpart for (left out of its menus;
# a test checks every other id has a binding): Maya's viewport GPU preview.
NOT_IN_BLENDER = {"graph.gpuPreview"}

_doc = [None]


def folder():
    for d in SEARCH:
        if os.path.isfile(os.path.join(d, "actions.json")):
            return d
    raise IOError("GroomLab's UI definitions are missing (looked in %s)" % ", ".join(SEARCH))


def document():
    """Every file merged into one dict, read once."""
    if _doc[0] is None:
        doc = {}
        base = folder()
        for name in FILES:
            path = os.path.join(base, name + ".json")
            if not os.path.isfile(path):
                continue
            with open(path, "rb") as f:
                data = json.loads(f.read().decode("utf-8"))
            data.pop("$schema", None)
            data.pop("version", None)
            doc.update(data)
        _doc[0] = doc
    return _doc[0]


def get(path, default=None):
    """A value by its slash path ("graph/nodeMenu"); `default` when missing (or the files are)."""
    try:
        node = document()
        for part in path.split("/"):
            node = node[part]
        return node
    except (KeyError, IOError, ValueError):
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


# ---- Resolving a container (Maya's definitions.resolve) ------------------------------------------

_FIELD = re.compile(r"\{(\w+)\}")


def fill(text, values):
    if not values or not text or "{" not in text:
        return text
    return _FIELD.sub(lambda m: str(values[m.group(1)]) if m.group(1) in values else m.group(0), text)


def _met(conditions, state):
    for c in conditions or ():
        if c.startswith("!"):
            if c[1:] in state:
                return False
        elif c not in state:
            return False
    return True


def _for_host(entry, host):
    hosts = entry.get("hosts")
    return hosts is None or host in hosts


def _container(path):
    node = get(path, [])
    return node if isinstance(node, dict) else {"items": node}


def _choice_entries(entry):
    return [{"id": entry["action"], "value": c["value"], "label": c.get("label", c["value"]), "tip": c.get("tip", "")}
            for c in choices(entry["choices"])]


def _backdrop_entries(entry):
    return [{"id": entry["action"], "value": rgb, "label": text} for text, rgb in backdrop_colors()]


BUILTIN_GENERATORS = {"choices": _choice_entries, "backdropColors": _backdrop_entries}


def resolve(entries, state=(), values=None, generators=None, host=HOST):
    """`entries` for `host`, as items: {"kind": "divider"}, {"kind": "action", "id", "label", "tip",
    "checkable", "value", "enabled", "entry"}, {"kind": "submenu", "id", "label", "tip", "items"},
    {"kind": "generator", "name", "entry"} (one `generators` has no function for). `state`: the
    conditions that hold ("needs" left out otherwise, "enabled" the enabled flag). As Maya's."""
    state = set(state or ())
    generators = dict(BUILTIN_GENERATORS, **(generators or {}))
    out = []

    def extend(items):
        out.extend(i for i in items if i["kind"] != "divider" or (out and out[-1]["kind"] != "divider"))

    for entry in entries or []:
        if entry == "-":
            if out and out[-1]["kind"] != "divider":
                out.append({"kind": "divider"})
            continue
        if not isinstance(entry, dict):
            entry = {"id": entry}
        is_action = "id" in entry and not set(entry) & {"submenu", "ref", "generator", "include"}
        base = action(entry["id"]) if is_action else {}
        if not _for_host(entry, host) or not _for_host(base, host):
            continue
        if not _met(entry["needs"] if "needs" in entry else base.get("needs"), state):
            continue
        if "include" in entry:
            extend(resolve(get(entry["include"], []), state, values, generators, host))
            continue
        if "generator" in entry:
            fn = generators.get(entry["generator"])
            if fn is None:
                out.append({"kind": "generator", "name": entry["generator"], "entry": entry})
            else:
                extend(resolve(fn(entry) or [], state, values, generators, host))
            continue
        if "submenu" in entry or "ref" in entry:
            container = _container(entry["ref"]) if "ref" in entry else {}
            items = entry.get("items", container.get("items", []))
            resolved = resolve(items, state, values, generators, host)
            if not resolved:
                continue
            out.append({"kind": "submenu", "id": entry.get("id", entry.get("ref")),
                        "label": fill(entry.get("submenu") or container.get("label", ""), values),
                        "tip": fill(entry.get("tip", container.get("tip", "")), values),
                        "items": resolved, "entry": entry})
            continue
        merged = dict(base)
        merged.update(entry)
        enabled = _met(merged.get("enabled"), state)
        text = merged.get("tip", "") if enabled else merged.get("tipDisabled", merged.get("tip", ""))
        out.append({"kind": "action", "id": merged["id"], "label": fill(merged.get("label", ""), values),
                    "tip": fill(text, values), "checkable": bool(merged.get("checkable")),
                    "value": merged.get("value"), "enabled": enabled, "entry": merged})
    while out and out[-1]["kind"] == "divider":
        out.pop()
    return out


def listed_ids(entries, host=HOST):
    """Every action id `entries` can show on `host` (conditions ignored; generated entries by their
    "action"): for the binding check."""
    ids = []
    if isinstance(entries, dict):
        entries = entries.get("items", [])
    for entry in entries or []:
        if entry == "-":
            continue
        if not isinstance(entry, dict):
            entry = {"id": entry}
        if not _for_host(entry, host):
            continue
        if "include" in entry:
            ids += listed_ids(get(entry["include"], []), host)
        elif "generator" in entry:
            if entry.get("action"):
                ids.append(entry["action"])
        elif "submenu" in entry or "ref" in entry:
            items = entry.get("items", _container(entry["ref"]).get("items", []) if "ref" in entry else [])
            ids += listed_ids(items, host)
        elif _for_host(action(entry["id"]), host):
            ids.append(entry["id"])
    return ids


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
