# SPDX-License-Identifier: GPL-3.0-or-later
"""Brush alphas (Maya's Groom Tools' Alpha picker): the library the helper lists (brush_alphas:
GROOMLAB_ALPHA_PATH, the user's Documents/GroomLab/alphas that Maya and the app share, the
shipped ones) as an icon grid for template_icon_view. The tool's alpha is a file path
(GroomLabTools.alpha); alpha_choice shows it in the grid and sets it."""

import os
import time

import bpy

from . import process

_previews = [None]
_library = {"at": 0.0, "alphas": [], "userDir": ""}
_items = [[]]  # the enum's items, kept alive while Blender holds them


def library(refresh=False):
    """[{"name", "path", "shipped"}] (read again at most every two seconds: the grid asks as it draws)."""
    now = time.time()
    if refresh or now - _library["at"] > 2.0:
        try:
            r = process.call("brush_alphas")
            _library["alphas"], _library["userDir"] = r.get("alphas", []), r.get("userDir", "")
        except Exception:  # a library that cannot be read never stops the panel
            _library["alphas"] = []
        _library["at"] = now
    return _library["alphas"]


def user_dir():
    library()
    return _library["userDir"]


def icon(path):
    """An alpha's thumbnail as an icon_value (loaded again when its file changes)."""
    pc = _previews[0]
    if pc is None or not path:
        return 0
    try:
        key = "%s|%s" % (path, os.path.getmtime(path))
    except OSError:
        return 0
    if key not in pc:
        pc.load(key, path, "IMAGE")
    return pc[key].icon_id


def enum_items(self, context):
    items = [("NONE", "None", "No alpha: the brush's falloff alone", "BLANK1", 0)]
    for i, a in enumerate(library(), 1):
        items.append((a["path"], a["name"], a["path"], icon(a["path"]), i))
    _items[0] = items
    return items


def choice_get(self):
    items = _items[0] or enum_items(self, None)
    return next((i for i, item in enumerate(items) if item[0] == self.alpha), 0)


def choice_set(self, value):
    items = _items[0] or enum_items(self, None)
    path = items[value][0] if 0 <= value < len(items) else "NONE"
    self.alpha = "" if path == "NONE" else path


def register():
    import bpy.utils.previews
    _previews[0] = bpy.utils.previews.new()


def unregister():
    if _previews[0] is not None:
        bpy.utils.previews.remove(_previews[0])
        _previews[0] = None
    _items[0] = []
