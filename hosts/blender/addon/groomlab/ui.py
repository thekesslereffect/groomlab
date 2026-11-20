# SPDX-License-Identifier: GPL-3.0-or-later
"""The GroomLab panel of the 3D view's sidebar, under Groom Tools: the groom, its status, files and
exports. Presets and the graph's nodes live in the Graph Editor (its Presets menu and sidebar), as in
Maya, not in the 3D view."""


import bpy

from . import groom, process, shading, units
from .ops import active_groom


def _words(name):
    """A param's label as every host writes it (graph_tree.pretty: the core's paramLabel rule)."""
    from .graph_tree import pretty
    return pretty(name)


def _wrap(layout, text, icon="NONE", width=48):
    words, line, first = text.split(), "", True
    for w in words:
        if line and len(line) + len(w) + 1 > width:
            layout.label(text=line, icon=icon if first else "BLANK1")
            line, first = w, False
        else:
            line = (line + " " + w).strip()
    if line:
        layout.label(text=line, icon=icon if first else "BLANK1")


class VIEW3D_PT_groomlab(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "GroomLab"
    bl_label = "GroomLab"
    bl_order = 1  # under Groom Tools (palette.py)

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        layout.prop(scene.groomlab, "groom")
        scalp = active_groom(context)
        if scalp is None:
            # The empty state the Groom Tools panel shows too (palette.py): one name, one button.
            layout.label(text="Select the scalp mesh, then:")
            layout.operator("groomlab.add_groom", text="Create Groom from Selection", icon="ADD")
            layout.operator("groomlab.load_groom", text="Open Groom File...", icon="FILE_FOLDER")
            return
        obj = context.active_object
        if obj is not None and obj.type == "MESH" and obj.groomlab.role == "NONE":
            layout.operator("groomlab.add_groom", text="Create Groom from Selection", icon="ADD")
        g = scalp.groomlab
        rt = groom.runtime_if_any(scalp)
        row = layout.row(align=True)
        if g.guides is not None:  # B2's guides as Blender curves (GroomLab's tools edit the others: Groom Tools)
            row.operator("groomlab.sculpt_guides", icon="SCULPTMODE_HLT")
        row.operator("groomlab.update", text="" if g.guides is not None else "Update", icon="FILE_REFRESH")
        row = layout.row(align=True)
        row.prop(g, "preview_density", slider=True)
        row.prop(g, "preview_point_budget", text="Budget (k)")
        layout.prop(g, "follow_scalp")
        if rt is not None and rt.last:
            mode = "render" if rt.last.get("mode") == "render" else "preview"
            layout.label(text="%s strands, %s points (%s, %.0f ms)" % (
                "{:,}".format(rt.last.get("strandCount") or 0), "{:,}".format(rt.last.get("pointCount") or 0), mode,
                rt.last.get("milliseconds") or 0.0), icon="INFO")
        elif not process.running():
            layout.label(text="The helper starts on the first change", icon="INFO")
        if rt is not None:
            box = None
            for text in [rt.error, rt.scalp_note] + rt.errors + rt.messages:
                if text:
                    box = box or layout.box()
                    _wrap(box, text, icon="ERROR")
        for text in shading.render_notes(context.scene, scalp):
            _wrap(layout.box(), text, icon="INFO")
        unsupported = sorted({n.unsupported for n in g.nodes if n.unsupported})
        for text in unsupported:
            _wrap(layout.box(), text, icon="INFO")
        col = layout.column(align=True)
        row = col.row(align=True)
        row.operator("groomlab.save_groom", text="Save Groom File...", icon="FILE_TICK")
        row.operator("groomlab.load_groom", text="Open Groom File...", icon="FILE_FOLDER")
        row = col.row(align=True)
        row.operator("groomlab.export_alembic", text="Alembic", icon="EXPORT")
        row.operator("groomlab.export_usd", text="USD", icon="EXPORT")
        col.operator("groomlab.bake_card_maps", text="Card Maps...", icon="TEXTURE")
        layout.operator("groomlab.remove_groom", icon="TRASH")


def draw_param(layout, p, scene):
    label = _words(p.name) + units.suffix(p.unit, scene)
    field = {"bool": "value_bool", "int": "value_int", "float": "value_float", "length": "value_length",
             "factor": "value_factor", "color": "value_color", "vector": "value_vector", "enum": "value_enum",
             "string": "value_string"}.get(p.widget)
    if field is None:
        row = layout.row()
        row.label(text="%s: %s" % (_words(p.name), p.note))
        return
    if p.widget == "vector":
        layout.label(text=label)
        layout.row(align=True).prop(p, field, text="")
        return
    layout.prop(p, field, text=label)


classes = (VIEW3D_PT_groomlab,)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
