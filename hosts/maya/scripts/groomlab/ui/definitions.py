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

The files ship beside themes/ in the module (GroomLab/ui) with their reader, ui_definitions.py,
which Blender's add-on shares; a source tree reads engine/ui.
"""

import importlib.util
import os

HOST = "maya"
_HERE = os.path.dirname(os.path.abspath(__file__))
# The module's copy (installed beside themes/), else the source tree's.
SEARCH = (os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, "ui")),
          os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, os.pardir, os.pardir, os.pardir, "engine", "ui")))


def _reader():
    """engine/ui/ui_definitions.py, the reader every Python host shares, from the first folder of
    SEARCH holding it with the JSON it reads."""
    for d in SEARCH:
        path = os.path.join(d, "ui_definitions.py")
        if os.path.isfile(path) and os.path.isfile(os.path.join(d, "actions.json")):
            spec = importlib.util.spec_from_file_location("groomlab_ui_definitions", path)
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
