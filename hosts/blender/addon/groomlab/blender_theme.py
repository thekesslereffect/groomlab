# SPDX-License-Identifier: GPL-3.0-or-later
"""GroomLab Charcoal for Blender's node editor: a theme preset made from the app's theme file
(hosts/maya/themes/charcoal.json, its "graph" and "ui" colors) so the Graph Editor sits on the app's
canvas: its background, dot grid, node bodies and outlines, selection, wires and their curving.

Only the node editor's colors are in the preset (Blender draws an add-on's node headers in its Input
node color, so that one is Charcoal's neutral header gray, in every node editor); Blender resets the rest of its theme to the default
when a preset is picked (Preferences > Themes, where it is listed as GroomLab Charcoal and where
any other theme switches back). It is applied only when asked: GROOMLAB_OT_theme_apply (the
add-on's preferences and the Graph Editor's GroomLab panel)."""

import json
import os

import bpy

from . import uikit

PRESET_NAME = "GroomLab_Charcoal"  # Blender lists it as "GroomLab Charcoal"
NOODLE_CURVING = 7  # the app's wires curve: Blender's 0 (straight) to 10


_cache = {"stamp": None, "data": {}}


def _theme_json():
    """The theme file's data (read again only when the file changes: nodes ask for it as they tint)."""
    try:
        stamp = os.path.getmtime(uikit.THEME_FILE)
    except OSError:
        return {}
    if _cache["stamp"] != stamp:
        try:
            with open(uikit.THEME_FILE, encoding="utf-8") as f:
                _cache["data"] = json.load(f)
        except (OSError, ValueError):
            _cache["data"] = {}
        _cache["stamp"] = stamp
    return _cache["data"]


def graph_colors():
    """The theme file's graph section with names resolved: {key: "#RRGGBB"} (categories and ports
    as dicts of the same)."""
    data = _theme_json()
    palette = data.get("palette", {})
    graph = dict(data.get("graph", {}))

    def resolve(v):
        return palette.get(v, v) if isinstance(v, str) else v

    for key in list(graph):
        value = graph[key]
        graph[key] = {k: resolve(v) for k, v in value.items()} if isinstance(value, dict) else resolve(value)
    ui = data.get("ui", {})
    graph["accent"] = resolve(ui.get("accent", "#5B8DEF"))
    graph["uiTextDim"] = resolve(ui.get("textMuted", "#737373"))
    return graph


def rgb255(hex_color):
    h = hex_color.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def preset_xml():
    """The preset's XML: Blender's ThemeNodeEditor and its space's colors (sRGB, as presets hold)."""
    g = graph_colors()
    canvas, node, border = g.get("canvas", "#141414"), g.get("node", "#1C1C1C"), g.get("nodeBorder", "#2A2A2A")
    text, selected = g.get("text", "#EDEDED"), g.get("selected", "#EDEDED")
    low = lambda c: c.lower()  # noqa: E731
    return """<bpy>
  <Theme>
    <node_editor>
      <ThemeNodeEditor
        grid="{grid}"
        node_outline="{border}ff"
        node_selected="{accent}"
        node_active="{selected}"
        wire="{canvas}ff"
        wire_inner="{wire}"
        wire_select="{selected}cc"
        node_backdrop="{node}ff"
        frame_node="{node}cc"
        input_node="{header}"
        noodle_curving="{curving}"
        grid_levels="3"
        dash_alpha="0.5"
        >
        <space>
          <ThemeSpaceGeneric
            back="{canvas}"
            title="{text}"
            text="{text}"
            text_hi="#ffffff"
            header="{canvas}b3"
            header_text="{text}"
            header_text_hi="#ffffff"
            >
          </ThemeSpaceGeneric>
        </space>
      </ThemeNodeEditor>
    </node_editor>
  </Theme>
  <ThemeStyle>
  </ThemeStyle>
</bpy>
""".format(grid=low(g.get("grid", "#2A2A2A")), border=low(border), accent=low(g["accent"]), selected=low(selected),
           canvas=low(canvas), wire=low(g["uiTextDim"]), node=low(node), curving=NOODLE_CURVING, text=low(text),
           header=low(g.get("header", "#2E2E2E")))


def preset_path():
    folder = bpy.utils.user_resource("SCRIPTS", path=os.path.join("presets", "interface_theme"), create=True)
    return os.path.join(folder, PRESET_NAME + ".xml")


def write_preset():
    path = preset_path()
    with open(path, "w", encoding="utf-8") as f:
        f.write(preset_xml())
    return path


class GROOMLAB_OT_theme_apply(bpy.types.Operator):
    """Use GroomLab Charcoal in the node editor: the app's graph canvas, nodes, selection and wires
    (Preferences > Themes lists it; any other theme there switches back)"""
    bl_idname = "groomlab.theme_apply"
    bl_label = "Use GroomLab Charcoal Theme"
    bl_options = {"REGISTER"}

    def execute(self, context):
        try:
            path = write_preset()
        except OSError as e:
            self.report({"ERROR"}, "Couldn't write the theme preset: %s" % e)
            return {"CANCELLED"}
        bpy.ops.script.execute_preset(filepath=path, menu_idname="USERPREF_MT_interface_theme_presets")
        from . import graph_tree
        graph_tree.recolor_all()  # the nodes in Charcoal's category colors
        self.report({"INFO"}, "GroomLab Charcoal is Blender's theme now (Preferences > Themes switches back)")
        return {"FINISHED"}


classes = (GROOMLAB_OT_theme_apply,)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
