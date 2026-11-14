# SPDX-License-Identifier: GPL-3.0-or-later
"""GroomLab's themes in Blender, as Maya's (hosts/maya/scripts/groomlab/ui/theme.py) and the app's:
the look of the GroomLab window, the Graph Editor's canvas and its params, picked in GroomLab >
Theme and remembered across sessions.

The themes are the shipped ones (Charcoal, Paper, Playful, Studio: the extension's themes folder)
and the user's own, looked up in order in every folder on GROOMLAB_THEME_PATH, the user's folder
(the extension's user folder's themes/: New Theme from Current puts a copy of the active one there
to edit, Open Themes Folder shows it) and the shipped folder; an earlier one hides a later one of
the same name. A theme may set only some keys: the rest come from the theme it "extends" (default
"playful", which sets them all), merged key by key as Maya merges them."""

import copy
import json
import os

import bpy

from . import uikit

DEFAULT = "charcoal"  # the theme a user who never picked one gets (Maya's and the app's)
BASE = "playful"      # the theme every theme extends unless it says otherwise
PATH_ENV = "GROOMLAB_THEME_PATH"

_active = {"name": None}


def shipped_dir():
    return os.path.dirname(uikit.THEME_FILE)


def user_dir(create=False):
    """The user's themes folder (the extension's user folder's themes/), "" when Blender cannot say."""
    try:
        return bpy.utils.extension_path_user(__package__, path="themes", create=create)
    except (ValueError, RuntimeError, AttributeError):
        return ""


def search_dirs():
    dirs = [d for d in os.environ.get(PATH_ENV, "").split(os.pathsep) if d]
    user = user_dir()
    if user:
        dirs.append(user)
    dirs.append(shipped_dir())
    return dirs


def list_themes():
    names = set()
    for d in search_dirs():
        if os.path.isdir(d):
            names.update(f[:-5] for f in os.listdir(d) if f.endswith(".json"))
    return sorted(names)


def label(name):
    """A theme's name as menus show it ("my_theme" -> "My Theme")."""
    return name.replace("_", " ").title()


def theme_path(name):
    for d in search_dirs():
        path = os.path.join(d, name + ".json")
        if os.path.isfile(path):
            return path
    raise ValueError("no theme named %r (looked in %s)" % (name, ", ".join(search_dirs())))


def _merge(base, over):
    out = copy.deepcopy(base)
    for key, value in over.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _merge(out[key], value)
        else:
            out[key] = copy.deepcopy(value)
    return out


def load(name, _seen=None):
    """The theme's data, filled in from the theme it extends."""
    seen = _seen or set()
    if name in seen:
        raise ValueError("themes extend each other in a loop: %s" % ", ".join(sorted(seen)))
    seen.add(name)
    with open(theme_path(name), encoding="utf-8") as f:
        try:
            data = json.load(f)
        except ValueError as e:
            raise ValueError("theme %r is not valid JSON: %s" % (name, e))
    base = data.get("extends", BASE if name != BASE else None)
    return _merge(load(base, seen), data) if base else data


# ---- The active theme, remembered ------------------------------------------------------------------

def _settings_path():
    try:
        folder = bpy.utils.extension_path_user(__package__, path="", create=True)
    except (ValueError, RuntimeError, AttributeError):
        return ""
    return os.path.join(folder, "theme.json")


def active():
    """The active theme's name (the one picked last, else Charcoal)."""
    if _active["name"] is None:
        name = DEFAULT
        path = _settings_path()
        if path and os.path.isfile(path):
            try:
                with open(path, encoding="utf-8") as f:
                    name = str(json.load(f).get("theme") or DEFAULT)
            except (OSError, ValueError, AttributeError):
                name = DEFAULT
        _active["name"] = name
    return _active["name"]


def apply(name=None):
    """Draw GroomLab with the theme `name` (default: the active one); a theme that will not load
    leaves the default's. Returns the theme applied."""
    name = name or active()
    try:
        data = load(name)
    except (ValueError, OSError):
        name, data = DEFAULT, load(DEFAULT)
    uikit.set_theme_data(data)
    restyle()
    return name


def set_theme(name):
    """Make `name` the active theme (remembered across sessions) and restyle what GroomLab draws.
    Raises ValueError, before anything changes, when it does not load."""
    data = load(name)
    _active["name"] = name
    path = _settings_path()
    if path:
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"theme": name}, f)
        except OSError:
            pass
    uikit.set_theme_data(data)
    restyle()
    return name


def create_theme(name, base=None):
    """A new theme file in the user's folder: a copy of `base` (default: the active theme) to edit.
    Returns its path."""
    if not name or any(c in name for c in '\\/:*?"<>|'):
        raise ValueError("invalid theme name %r" % name)
    folder = user_dir(create=True)
    if not folder:
        raise ValueError("Blender has no user folder for the extension")
    data = load(base or active())
    data["name"] = label(name)
    data.pop("extends", None)
    path = os.path.join(folder, name + ".json")
    if os.path.exists(path):
        raise ValueError("a theme named %r exists: %s" % (name, path))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return path


def restyle():
    """Everything GroomLab draws in its theme again: the caches dropped, every area redrawn."""
    uikit.forget_theme()
    from . import blender_theme, graph_canvas, window
    graph_canvas._theme_cache.clear()
    graph_canvas._dock_cache.clear()
    window._cache.clear()
    blender_theme._cache["stamp"] = None
    wm = getattr(bpy.context, "window_manager", None)
    for win in (wm.windows if wm is not None else []):
        for area in win.screen.areas:
            area.tag_redraw()


def menu_entries(entry):
    """The Theme menu's themes (menus.json's "themes" generator): one each, the active one checked."""
    from . import definitions
    tip = (definitions.action("theme.set") or {}).get("tip", "")
    current = active()
    return [{"id": entry["action"], "value": name, "label": label(name), "checked": name == current,
             "tip": tip.replace("{label}", label(name))} for name in list_themes()]


# ---- Operators (GroomLab > Theme) ---------------------------------------------------------------------

class GROOMLAB_OT_theme_set(bpy.types.Operator):
    """Restyle GroomLab with this theme (remembered across sessions)"""
    bl_idname = "groomlab.theme_set"
    bl_label = "Theme"
    bl_options = {"REGISTER"}

    name: bpy.props.StringProperty(name="Theme")

    @classmethod
    def description(cls, context, properties):
        return "Restyle GroomLab with the %s theme (remembered across sessions)" % label(properties.name or DEFAULT)

    def execute(self, context):
        try:
            set_theme(self.name)
        except (ValueError, OSError) as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        self.report({"INFO"}, "GroomLab theme: %s" % label(self.name))
        return {"FINISHED"}


class GROOMLAB_OT_theme_new(bpy.types.Operator):
    """A copy of the active theme in your themes folder, to edit"""
    bl_idname = "groomlab.theme_new"
    bl_label = "New Theme from Current"
    bl_options = {"REGISTER"}

    name: bpy.props.StringProperty(name="Name", default="my_theme")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        try:
            path = create_theme(self.name.strip())
        except (ValueError, OSError) as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        set_theme(self.name.strip())
        self.report({"INFO"}, "New theme %s: edit %s, then Reload Theme" % (label(self.name.strip()), path))
        return {"FINISHED"}


class GROOMLAB_OT_theme_reload(bpy.types.Operator):
    """Read the active theme's file again, after editing it"""
    bl_idname = "groomlab.theme_reload"
    bl_label = "Reload Theme"
    bl_options = {"REGISTER"}

    def execute(self, context):
        name = apply()
        self.report({"INFO"}, "GroomLab theme: %s" % label(name))
        return {"FINISHED"}


class GROOMLAB_OT_theme_open_folder(bpy.types.Operator):
    """Your themes folder (GROOMLAB_THEME_PATH adds others)"""
    bl_idname = "groomlab.theme_open_folder"
    bl_label = "Open Themes Folder"
    bl_options = {"REGISTER"}

    def execute(self, context):
        folder = user_dir(create=True)
        if not folder:
            self.report({"ERROR"}, "Blender has no user folder for the extension")
            return {"CANCELLED"}
        bpy.ops.wm.path_open(filepath=folder)
        return {"FINISHED"}


CLASSES = (GROOMLAB_OT_theme_set, GROOMLAB_OT_theme_new, GROOMLAB_OT_theme_reload, GROOMLAB_OT_theme_open_folder)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    # The Graph Editor's menus run what graph_ui binds: the Theme submenu's actions there too.
    from . import graph_ui
    graph_ui.bind("theme.set", GROOMLAB_OT_theme_set.bl_idname, lambda ctx, item: {"name": item.get("value") or ""},
                  lambda ctx, item: item["value"] == active())
    graph_ui.bind("theme.new", GROOMLAB_OT_theme_new.bl_idname)
    graph_ui.bind("theme.reload", GROOMLAB_OT_theme_reload.bl_idname)
    graph_ui.bind("theme.openFolder", GROOMLAB_OT_theme_open_folder.bl_idname)
    apply()


def unregister():
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
    _active["name"] = None
