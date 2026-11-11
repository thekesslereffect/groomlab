# SPDX-License-Identifier: GPL-3.0-or-later
"""GroomLab's UI structure, shared by every host: engine/ui/*.json (docs/UI_DEFINITIONS.md), read
the way Maya's hosts/maya/scripts/groomlab/ui/definitions.py reads it, for host "blender".

The files say what the menus, the Groom Tools' tabs and sections, the lists' action bars and
right-click menus, the option lists, the pie (Maya's marking menu) and the Graph Editor's menus
hold: action ids with their labels, tooltips, order, dividers, enable conditions and host masks.
actions.py binds each id to an operator; menu.py and palette.py build Blender's menus and panels
from both. Plain JSON, read with no engine loaded: the add-on draws before the helper starts.

    from . import definitions as d
    d.action("export.alembic")["label"]          # "Alembic..."
    d.get("menus/export")                        # the GroomLab > Export entries
    d.choices("flowSink")                        # [{"value": "", "label": "Arrows", "tip": ...}, ...]
    d.resolve(d.get("lists/maps/context"), state={"map"})   # entries for Blender, conditions met

The extension ships the files in GroomLabBlender/ui (hosts/blender/addon/CMakeLists.txt) with their
reader, ui_definitions.py (Maya's too: Apache-2.0); a source tree reads engine/ui."""

import importlib.util
import os
import re

HOST = "blender"
_HERE = os.path.dirname(os.path.abspath(__file__))
# The extension's copy (beside the add-on's modules), else the source tree's.
SEARCH = (os.path.join(_HERE, "ui"),
          os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, os.pardir, "engine", "ui")))


def _reader():
    """engine/ui/ui_definitions.py, the reader every Python host shares, from the first folder of
    SEARCH holding it with the JSON it reads."""
    for d in SEARCH:
        path = os.path.join(d, "ui_definitions.py")
        if os.path.isfile(path) and os.path.isfile(os.path.join(d, "actions.json")):
            spec = importlib.util.spec_from_file_location(__name__ + "_ui", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module
    raise IOError("GroomLab's UI definitions are missing (looked in %s)" % ", ".join(SEARCH))


_ui = _reader()
FILES = _ui.FILES
folder = _ui.folder
document = _ui.document
get = _ui.get
action = _ui.action
label = _ui.label
tip = _ui.tip
text = _ui.text
choices = _ui.choices
choice_default = _ui.choice_default
choice = _ui.choice
choice_label = _ui.choice_label
section = _ui.section
fill = _ui.fill
host_text = _ui.host_text
items_of = _ui.items_of
containers = _ui.containers
BUILTIN_GENERATORS = _ui.BUILTIN_GENERATORS
_met = _ui.met
_for_host = _ui.for_host
_container = _ui.container


def tabs(host=HOST):
    """The Groom Tools' tabs this host has (palette.json's tabs; a tab's "hosts" says which have it:
    the app's Outliner is its own)."""
    return _ui.tabs(host)


def resolve(entries, state=(), values=None, generators=None, host=HOST, keep=None, drop_generators=False):
    """`entries` for this host as items to build (ui_definitions.resolve: kinds divider, action,
    submenu, generator; `state` the conditions that hold, None ignores "needs"; `values` fill
    {name}; `generators` name -> function; `keep(action id)` whether an action shows;
    `drop_generators` leaves out a generator no function fills)."""
    return _ui.resolve(entries, state, values, generators, host, keep, drop_generators)


def listed_ids(entries, host=HOST, generators=()):
    """Every action id `entries` can show on this host whatever the state (ui_definitions.listed_ids):
    for the binding checks."""
    return _ui.listed_ids(entries, host, generators)


_TAG = re.compile(r"<[^>]+>")


def plain(s):
    """A text without its markup (<b>...</b>): Blender's labels are plain."""
    return _TAG.sub("", s or "")


def enum_items(name, empty="NONE", field="label"):
    """An option list as a Blender EnumProperty's items [(identifier, label, tip)]: the values as
    identifiers (str), an empty value as `empty` (Blender takes no empty identifier)."""
    return [(str(c["value"]) if c["value"] != "" else empty, c.get(field, c["label"]), c.get("tip", ""))
            for c in choices(name)]


def enum_default(name, empty="NONE"):
    value = choice_default(name)
    return str(value) if value != "" else empty


# ---- The Graph Editor's colors and plumbing (graph_ui.json) -------------------------------------

def _hex(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def category_colors():
    """{category: (r, g, b) 0-255}: the Graph Editor's node category colors."""
    return {k: _hex(v) for k, v in get("graph/categoryColors").items()}


def port_colors():
    return {k: _hex(v) for k, v in get("graph/portColors").items()}


def plumbing():
    """{(node type, param)}: the params that only wire the graph (hidden while at their default)."""
    return set(tuple(p) for p in get("graph/plumbing"))


def evaluation_labels():
    """{value: label} of the node evaluation choices besides Always."""
    return {c["value"]: c["label"] for c in choices("evaluation") if c["value"] != choice_default("evaluation")}
