# SPDX-License-Identifier: GPL-3.0-or-later
"""GroomLab's icons (the Maya palette's, icons/<name>.png rendered by tools/make_blender_icons.py)
as Blender previews: icon(name) is an icon_value for layout.operator / layout.label."""

import os

import bpy

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons")
_previews = [None]


def icon(name):
    """The icon_value of icons/<name>.png (0: none, Blender draws no icon)."""
    pc = _previews[0]
    if pc is None:
        return 0
    if name not in pc:
        path = os.path.join(DIR, name + ".png")
        if not os.path.isfile(path):
            return 0
        pc.load(name, path, "IMAGE")
    return pc[name].icon_id


def name_of(icon_value):
    """The icons/<name>.png an icon_value came from ("" for none of ours)."""
    pc = _previews[0]
    if pc is None or not icon_value:
        return ""
    for name, preview in pc.items():
        if preview.icon_id == icon_value:
            return name
    return ""


def names():
    return sorted(os.path.splitext(f)[0] for f in os.listdir(DIR) if f.endswith(".png")) if os.path.isdir(DIR) else []


def register():
    import bpy.utils.previews
    _previews[0] = bpy.utils.previews.new()
    for name in names():  # loaded now: panels only look them up while drawing
        _previews[0].load(name, os.path.join(DIR, name + ".png"), "IMAGE")


def unregister():
    if _previews[0] is not None:
        bpy.utils.previews.remove(_previews[0])
        _previews[0] = None
