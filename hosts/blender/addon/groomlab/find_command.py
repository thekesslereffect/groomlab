# SPDX-License-Identifier: GPL-3.0-or-later
"""Find Command (GroomLab > Find Command..., the Groom Tools' header): one search over every GroomLab
command (the GroomLab menu's, engine/ui/menus.json, as Blender binds them), every brush and every
graph preset, as Maya's find_command.py. Blender's own search popup is the box (an operator with a
search enum: type to filter, Enter runs), so it reads and behaves as Blender's F3 does."""

import bpy
from bpy.props import EnumProperty

from . import actions, definitions, graph, menu, process, tools
from .helper_client import HelperError
from .ops import active_groom

_entries = []  # [(identifier, name, description, run(context))]: what the search lists
_items = []    # the enum's items (Blender keeps no copy of a dynamic enum's strings)


def _run_action(item):
    """An action of the definitions as its binding runs it: its operator (invoked, so a file
    browser or a dialog opens), or its setting toggled."""
    def run(context):
        idname, props = actions.binding(item["id"])
        if idname.startswith("prop:"):
            data, path = context.scene, idname[len("prop:"):].split(".")
            if path[0] == "groom":
                data, path = active_groom(context), path[1:]
                if data is None:
                    raise RuntimeError("select a groom first")
            for part in path[:-1]:
                data = getattr(data, part)
            setattr(data, path[-1], not getattr(data, path[-1]))
            return
        props = dict(props)
        value_prop = props.pop("$value", "value")
        if item.get("value") is not None:
            props[value_prop] = item["value"]
        module, name = idname.split(".", 1)
        getattr(getattr(bpy.ops, module), name)("INVOKE_DEFAULT", **props)
    return run


def _run_brush(tool):
    def run(context):
        bpy.ops.groomlab.tool(tool=tool)
    return run


def _run_preset(name):
    def run(context):
        scalp = active_groom(context)
        if scalp is None:
            raise RuntimeError("select a groom first")
        graph.apply_preset(scalp, name)
    return run


def entries(context):
    """[(identifier, name, description, run)]: the brushes, the GroomLab menu's commands (by their
    submenu), the graph presets (when the helper has listed them)."""
    found = []
    for t in (tools.cached_catalog() or {}).get("tools") or []:
        found.append(("brush:" + t["id"], "%s  ·  Brush" % t["label"], t.get("hint", ""), _run_brush(t["id"])))

    def walk(items, where):
        for item in items:
            if item["kind"] == "submenu":
                walk(item["items"], item["label"])
            elif item["kind"] == "action" and item["id"] != "commands.find":
                key = "action:%s:%s" % (item["id"], item.get("value", ""))
                if any(e[0] == key for e in found):
                    continue
                found.append((key, "%s  ·  %s" % (item["label"].replace("...", ""), where or "GroomLab"),
                              definitions.plain(item.get("tip", "")), _run_action(item)))
    walk(menu.resolve(definitions.get("menus/main"), context, conditions=False), "")
    presets = graph.cached_presets()
    if not presets and process.running():
        try:
            presets = graph.presets()
        except (HelperError, RuntimeError):
            presets = []
    for name, title, description, _category in presets or []:
        found.append(("preset:" + name, "Preset: %s  ·  Graph" % title,
                      description or "Apply the graph preset to the current groom", _run_preset(name)))
    return found


def _command_items(self, context):
    return _items


class GROOMLAB_OT_find_command(bpy.types.Operator):
    """Search every GroomLab command, brush and graph preset by name, and run it"""
    bl_idname = "groomlab.find_command"
    bl_label = "Find Command"
    bl_property = "command"

    command: EnumProperty(name="Command", items=_command_items)

    @classmethod
    def description(cls, context, properties):
        return definitions.tip("commands.find") or cls.__doc__

    def invoke(self, context, event):
        _entries[:] = entries(context)
        _items[:] = [(key, name, tip) for key, name, tip, _run in _entries] or [("NONE", "Nothing to find", "")]
        context.window_manager.invoke_search_popup(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        if not _entries:
            _entries[:] = entries(context)
        entry = next((e for e in _entries if e[0] == self.command), None)
        if entry is None:
            return {"CANCELLED"}
        try:
            entry[3](context)
        except (HelperError, RuntimeError, TypeError) as e:
            self.report({"ERROR"}, "GroomLab: %s" % getattr(e, "message", e))
            return {"CANCELLED"}
        return {"FINISHED"}


classes = (GROOMLAB_OT_find_command,)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
    _entries.clear()
    _items.clear()
