# SPDX-License-Identifier: Apache-2.0
"""How a Python host reads GroomLab's UI structure (engine/ui/*.json, docs/UI_DEFINITIONS.md): one
reader for Maya (hosts/maya/scripts/groomlab/ui/definitions.py) and Blender
(hosts/blender/addon/groomlab/definitions.py), which load this file from the folder that holds the
JSON (it ships beside them: the Maya module's ui/, the Blender extension's ui/) and add their own
helpers. The app reads the same files in C++ (hosts/app/src/definitions.cpp).

Plain Python 3.7 (Maya 2022's) and no imports past the standard library: the Blender add-on draws
before the engine is loaded.
"""

import json
import os
import re

FILES = ("actions", "menus", "palette", "lists", "choices", "marking_menu", "graph_ui", "properties")

_doc = None


def folder():
    """The folder the definitions are read from: this file's."""
    return os.path.dirname(os.path.abspath(__file__))


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


def label(action_id):
    return action(action_id).get("label", action_id)


def tip(action_id):
    return action(action_id).get("tip", "")


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


def choice_label(name, value, fallback=None):
    c = choice(name, value)
    return c["label"] if c else (fallback if fallback is not None else str(value))


def section(section_id):
    """A Groom Tools section (palette.json's sections) by its id."""
    return next(s for s in get("palette/sections") if s["id"] == section_id)


_FIELD = re.compile(r"\{(\w+)\}")


def fill(text, values):
    """`text` with {name} replaced by values[name] (names without a value stay as they are)."""
    if not values or not text or "{" not in text:
        return text
    return _FIELD.sub(lambda m: str(values[m.group(1)]) if m.group(1) in values else m.group(0), text)


def met(conditions, state):
    """Whether every condition holds: "x" in state, "!x" not in state."""
    for c in conditions or ():
        if c.startswith("!"):
            if c[1:] in state:
                return False
        elif c not in state:
            return False
    return True


def for_host(entry, host):
    """Whether an entry (or an action) is on `host`: its "hosts" names it, or it has none."""
    hosts = entry.get("hosts")
    return hosts is None or host in hosts


def tabs(host):
    """The Groom Tools' tabs `host` has (palette.json's tabs; a tab's "hosts" says which have it:
    the app's Outliner is its own)."""
    return [t for t in get("palette/tabs") if for_host(t, host)]


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


def container(path):
    """A referenced container: a list of entries, or {"label", "tip", "items"}."""
    node = get(path)
    return node if isinstance(node, dict) else {"items": node}


def items_of(entry):
    """A submenu entry's own entries: its "items", else its "ref"'s."""
    return entry["items"] if "items" in entry else container(entry["ref"])["items"]


def resolve(entries, state, values, generators, host, keep=None, drop_generators=False):
    """`entries` (a container's list) for `host`, as items to build:

        {"kind": "divider"}
        {"kind": "action", "id", "label", "tip", "shortcut", "shortcutPart", "checkable", "icon", "glyph",
         "value", "enabled", "entry"}
        {"kind": "submenu", "id", "label", "tip", "items": [...], "entry"}
        {"kind": "generator", "name", "entry"}   (one `generators` has no function for: the host fills it)

    `state`: the conditions that hold (an entry's "needs", else its action's, must all hold, else
    it is left out; its "enabled" ones set the item's enabled flag); None ignores "needs" (an
    action bar, always shown). `values` fill {name} in labels and tips. A generator with a function
    in `generators` is replaced by the entries that function returns for it. `keep(action id)`:
    whether an action shows (Blender: it has a binding; the rest is hidden). `drop_generators`:
    a generator no function fills is left out (the host has nothing for it). Dividers are kept
    only between items; a submenu with nothing in it is left out."""
    every = state is None
    state = set(state or ())
    generators = dict(BUILTIN_GENERATORS, **(generators or {}))
    out = []

    def extend(items):
        out.extend(i for i in items if i["kind"] != "divider" or (out and out[-1]["kind"] != "divider"))

    def again(items, sub_state):
        return resolve(items, sub_state, values, generators, host, keep, drop_generators)

    for entry in entries:
        if entry == "-":
            if out and out[-1]["kind"] != "divider":
                out.append({"kind": "divider"})
            continue
        if not isinstance(entry, dict):
            entry = {"id": entry}
        is_action = "id" in entry and not set(entry) & {"submenu", "ref", "generator", "include"}
        base = get("actions").get(entry["id"], {}) if is_action else {}
        if not for_host(entry, host) or not for_host(base, host):
            continue
        if not every and not met(entry["needs"] if "needs" in entry else base.get("needs"), state):
            continue
        sub_state = None if every else state
        if "include" in entry:
            extend(again(get(entry["include"]), sub_state))
            continue
        if "generator" in entry:
            fn = generators.get(entry["generator"])
            if fn is None:
                if not drop_generators:
                    out.append({"kind": "generator", "name": entry["generator"], "entry": entry})
            else:
                extend(again(fn(entry) or [], sub_state))
            continue
        if "submenu" in entry or "ref" in entry:
            held = container(entry["ref"]) if "ref" in entry else {}
            resolved = again(entry.get("items", held.get("items", [])), sub_state)
            if not any(i["kind"] != "divider" for i in resolved):
                continue
            title = entry.get("labels", {}).get(host) or entry.get("submenu") or host_text("label", host, held)
            out.append({"kind": "submenu", "id": entry.get("id", entry.get("ref")),
                        "label": fill(title, values),
                        "tip": fill(host_text("tip", host, entry, held), values),
                        "glyph": entry.get("glyph"), "items": resolved, "entry": entry})
            continue
        if keep is not None and not keep(entry["id"]):
            continue
        merged = dict(base)
        merged.update(entry)
        enabled = met(merged.get("enabled"), state)
        hint = host_text("tip", host, entry, base)
        if not enabled:
            hint = merged.get("tipDisabled", hint)
        out.append({"kind": "action", "id": merged["id"], "label": fill(host_text("label", host, entry, base), values),
                    "tip": fill(hint, values), "shortcut": merged.get("shortcut"),
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


def listed_ids(entries, host, generators=()):
    """Every action id `entries` can show on `host` whatever the state (conditions ignored),
    referenced and included lists followed; generated entries' ids from `generators` (name -> the
    action id its entries use), else their "action". For the binding checks."""
    ids = []
    if isinstance(entries, dict):  # a container with a label
        entries = entries["items"]
    for entry in entries:
        if entry == "-":
            continue
        if not isinstance(entry, dict):
            entry = {"id": entry}
        if not for_host(entry, host):
            continue
        if "include" in entry:
            ids += listed_ids(get(entry["include"]), host, generators)
        elif "generator" in entry:
            made = dict(generators).get(entry["generator"]) or entry.get("action")
            if made:
                ids.append(made)
        elif "submenu" in entry or "ref" in entry:
            items = entry.get("items", container(entry["ref"]).get("items", []) if "ref" in entry else [])
            ids += listed_ids(items, host, generators)
        elif "id" in entry and for_host(get("actions").get(entry["id"], {}), host):
            ids.append(entry["id"])
    return ids


def containers():
    """Every container of action entries in the files, by slash path: what a host's binding check
    walks (the menus, the Groom Tools' header, empty state and Output strip, the lists' bars and
    menus, the marking menu's tail, the Graph Editor's menus)."""
    doc = document()
    out = {}

    def walk(path, node):
        if isinstance(node, list):
            if any(isinstance(e, str) or (isinstance(e, dict) and ({"id", "submenu", "ref", "include", "generator"} & set(e)))
                   for e in node):
                out[path] = node
            return
        if isinstance(node, dict):
            if "items" in node and isinstance(node["items"], list) and path != "markingMenu/overflow":
                walk(path, node["items"])
                return
            for k, v in node.items():
                walk(path + "/" + k, v)

    for top in ("menus", "palette", "lists", "graph"):
        walk(top, doc[top])
    out.pop("palette/tabs", None)
    out.pop("palette/sections", None)
    out.pop("palette/starter/steps", None)
    out.pop("palette/starter/lookTypes", None)
    out["markingMenu/tail"] = [e for e in doc["markingMenu"]["tail"] if isinstance(e, str)]
    return out
