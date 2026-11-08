# SPDX-License-Identifier: Apache-2.0
"""GroomLab's UI structure, shared by every host: engine/ui/*.json (docs/UI_DEFINITIONS.md).

The files say what the menus, the Groom Tools' tabs and sections, the lists' action bars and
right-click menus, the option lists, the marking menu and the Graph Editor's menus hold: action
ids with their labels, tooltips, order, dividers, shortcut ids, enable conditions and host masks.
This host binds each id to a callable (menu.ACTIONS, the windows' ACTIONS tables); the layout and
the widgets stay in code.

    from groomlab.ui import definitions as d
    d.action("export.alembic")["label"]          # "Alembic..."
    d.get("menus/export")                        # the GroomLab > Export entries
    d.choices("flowSink")                        # [{"value": "", "label": "Arrows", "tip": ...}, ...]
    d.resolve(d.get("lists/maps/context"), state={"map"})   # entries for this host, conditions met

The files ship beside themes/ in the module (GroomLab/ui); a source tree reads engine/ui.
"""

import json
import os
import re

HOST = "maya"
FILES = ("actions", "menus", "palette", "lists", "choices", "marking_menu", "graph_ui", "properties")
_HERE = os.path.dirname(os.path.abspath(__file__))
# The module's copy (installed beside themes/), else the source tree's.
SEARCH = (os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, "ui")),
          os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, os.pardir, os.pardir, "engine", "ui")))

_doc = None


def folder():
    """The folder the definitions are read from."""
    for d in SEARCH:
        if os.path.isfile(os.path.join(d, "actions.json")):
            return d
    raise IOError("GroomLab's UI definitions are missing (looked in %s)" % ", ".join(SEARCH))


def document():
    """Every file merged into one dict: {"actions": ..., "menus": ..., "palette": ..., ...}."""
    global _doc
    if _doc is None:
        doc = {}
        base = folder()
        for name in FILES:
            with open(os.path.join(base, name + ".json"), "rb") as f:
                data = json.loads(f.read().decode("utf-8"))
            data.pop("$schema", None)
            data.pop("version", None)
            doc.update(data)
        _doc = doc
    return _doc


def get(path):
    """A value by its slash path from the merged root: "menus/export", "palette/texts/gestures"."""
    node = document()
    for part in path.split("/"):
        node = node[part]
    return node


def action(action_id):
    return get("actions")[action_id]


def text(name):
    """A text of the Groom Tools (palette.json's texts)."""
    return get("palette/texts")[name]


def choices(name):
    """An option list: [{"value", "label", "tip", ...}] in its order."""
    return get("choices")[name]["items"]


def choice_default(name):
    return get("choices")[name].get("default")


def choice(name, value):
    """One option of a list by its value (None when there is none)."""
    return next((c for c in choices(name) if c["value"] == value), None)


_FIELD = re.compile(r"\{(\w+)\}")


def fill(text, values):
    """`text` with {name} replaced by values[name] (names without a value stay as they are)."""
    if not values or not text or "{" not in text:
        return text
    return _FIELD.sub(lambda m: str(values[m.group(1)]) if m.group(1) in values else m.group(0), text)


def _met(conditions, state):
    """Whether every condition holds: "x" in state, "!x" not in state."""
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


def tabs(host=HOST):
    """The Groom Tools' tabs this host has (palette.json's tabs; a tab's "hosts" says which have it:
    the app's Outliner is its own)."""
    return [t for t in get("palette/tabs") if _for_host(t, host)]


def host_text(key, host, *sources):
    """An entry's label or tip ("label", "tip") on `host`: from the first source (the entry at its
    place, then its action or container) with a per-host override ("labels": {"app": ...}) or the
    text itself; "" when none has it."""
    for source in sources:
        override = source.get(key + "s", {})
        if host in override:
            return override[host]
        if key in source:
            return source[key]
    return ""


def _container(path):
    """A referenced container: a list of entries, or {"label", "tip", "items"}."""
    node = get(path)
    return node if isinstance(node, dict) else {"items": node}


def items_of(entry):
    """A submenu entry's own entries: its "items", else its "ref"'s."""
    return entry["items"] if "items" in entry else _container(entry["ref"])["items"]


def resolve(entries, state=(), values=None, generators=None, host=HOST):
    """`entries` (a container's list) for `host`, as items to build:

        {"kind": "divider"}
        {"kind": "action", "id", "label", "tip", "shortcut", "shortcutPart", "checkable", "icon", "glyph",
         "value", "enabled", "entry"}
        {"kind": "submenu", "id", "label", "tip", "items": [...], "entry"}
        {"kind": "generator", "name", "entry"}   (one `generators` has no function for: the host fills it)

    `state`: the conditions that hold (an entry's "needs", else its action's, must all hold, else
    it is left out; its "enabled" ones set the item's enabled flag); None ignores "needs" (an
    action bar, always shown). `values` fill {name} in labels and tips. A generator with a function
    in `generators` is replaced by the entries that function returns for it. Dividers are kept only
    between items; a submenu with nothing in it is left out."""
    every = state is None
    state = set(state or ())
    generators = dict(BUILTIN_GENERATORS, **(generators or {}))
    out = []

    def extend(items):
        out.extend(i for i in items if i["kind"] != "divider" or (out and out[-1]["kind"] != "divider"))

    for entry in entries:
        if entry == "-":
            if out and out[-1]["kind"] != "divider":
                out.append({"kind": "divider"})
            continue
        if not isinstance(entry, dict):
            entry = {"id": entry}
        is_action = "id" in entry and not set(entry) & {"submenu", "ref", "generator", "include"}
        base = get("actions").get(entry["id"], {}) if is_action else {}
        if not _for_host(entry, host) or not _for_host(base, host):
            continue
        if not every and not _met(entry["needs"] if "needs" in entry else base.get("needs"), state):
            continue
        sub_state = None if every else state
        if "include" in entry:
            extend(resolve(get(entry["include"]), sub_state, values, generators, host))
            continue
        if "generator" in entry:
            fn = generators.get(entry["generator"])
            if fn is None:
                out.append({"kind": "generator", "name": entry["generator"], "entry": entry})
            else:
                extend(resolve(fn(entry) or [], sub_state, values, generators, host))
            continue
        if "submenu" in entry or "ref" in entry:
            container = _container(entry["ref"]) if "ref" in entry else {}
            items = entry.get("items", container.get("items", []))
            resolved = resolve(items, sub_state, values, generators, host)
            if not resolved:
                continue
            label = entry.get("labels", {}).get(host) or entry.get("submenu") or host_text("label", host, container)
            out.append({"kind": "submenu", "id": entry.get("id", entry.get("ref")),
                        "label": fill(label, values),
                        "tip": fill(host_text("tip", host, entry, container), values),
                        "glyph": entry.get("glyph"), "items": resolved, "entry": entry})
            continue
        merged = dict(base)
        merged.update(entry)
        enabled = _met(merged.get("enabled"), state)
        tip = host_text("tip", host, entry, base)
        if not enabled:
            tip = merged.get("tipDisabled", tip)
        out.append({"kind": "action", "id": merged["id"], "label": fill(host_text("label", host, entry, base), values),
                    "tip": fill(tip, values), "shortcut": merged.get("shortcut"),
                    "shortcutPart": merged.get("shortcutPart"), "checkable": bool(merged.get("checkable")),
                    "icon": merged.get("icon"), "glyph": merged.get("glyph"), "value": merged.get("value"),
                    "enabled": enabled, "entry": merged})
    while out and out[-1]["kind"] == "divider":
        out.pop()
    return out


def _choice_entries(entry):
    """The "choices" generator: an entry per option of choices.json's list, its action called with
    the option's value."""
    field = entry.get("labelField", "label")
    return [{"id": entry["action"], "value": c["value"], "label": c.get(field, c["label"]), "tip": c.get("tip", "")}
            for c in choices(entry["choices"])]


def _backdrop_entries(entry):
    return [{"id": entry["action"], "value": c["rgb"], "label": c["label"]} for c in get("graph/backdropColors")]


# Generators every host expands the same way, from the files themselves.
BUILTIN_GENERATORS = {"choices": _choice_entries, "backdropColors": _backdrop_entries}


def binder(table, owner=None):
    """A function giving the callable an item runs, from a host's table {action id: binding}: a
    callable, or (with `owner`) a method name or a function taking the owner. An item with a
    "value" (a generated choice) calls it with the value. KeyError for an id without a binding."""
    def bind(item):
        target = table[item["id"]]
        if isinstance(target, str):
            target = getattr(owner, target)
        elif owner is not None and not isinstance(target, _BOUND):
            target = _partial(target, owner)
        if item.get("value") is not None:
            return _partial(target, item["value"])
        return target
    return bind


class _BOUND(object):
    """A binding already bound (bound(fn)): called as it is, without the owner."""

    def __init__(self, fn):
        self.fn = fn

    def __call__(self, *args, **kwargs):
        return self.fn(*args, **kwargs)


def bound(fn):
    """A table entry run as it is, the owner not passed (a module function in an owner's table)."""
    return _BOUND(fn)


def _partial(fn, first):
    def call(*args, **kwargs):
        return fn(first, *args, **kwargs)
    return call


def listed_ids(entries, host=HOST, generators=()):
    """Every action id `entries` can show on `host` whatever the state (conditions ignored),
    referenced and included lists followed; generated entries' ids from `generators` (name -> the
    action id its entries use). For the binding checks."""
    ids = []
    if isinstance(entries, dict):  # a container with a label
        entries = entries["items"]
    for entry in entries:
        if entry == "-":
            continue
        if not isinstance(entry, dict):
            entry = {"id": entry}
        if not _for_host(entry, host):
            continue
        if "include" in entry:
            ids += listed_ids(get(entry["include"]), host, generators)
        elif "generator" in entry:
            made = dict(generators).get(entry["generator"]) or entry.get("action")
            if made:
                ids.append(made)
        elif "submenu" in entry or "ref" in entry:
            items = entry.get("items", _container(entry["ref"]).get("items", []) if "ref" in entry else [])
            ids += listed_ids(items, host, generators)
        elif _for_host(get("actions").get(entry["id"], {}), host):
            ids.append(entry["id"])
    return ids
