# SPDX-License-Identifier: GPL-3.0-or-later
"""The add-on's operators: make a groom on a mesh, sculpt its guides, presets, graph edits, .groom
files and Alembic/USD exports."""

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import graph, graph_tree, groom, vray
from .helper_client import HelperError, sentence


def active_groom(context):
    """The groom the panel works on: the scene's choice, else the active object's groom."""
    chosen = context.scene.groomlab.groom
    if chosen is not None and groom.is_groom(chosen):
        return chosen
    return groom.scalp_of(context.active_object)


def _fail(op, e):
    op.report({"ERROR"}, sentence(e.message if isinstance(e, HelperError) else str(e)))
    return {"CANCELLED"}


class GroomOperator(bpy.types.Operator):
    @classmethod
    def poll(cls, context):
        return active_groom(context) is not None


class GROOMLAB_OT_add_groom(bpy.types.Operator):
    """Grow a GroomLab groom on the selected mesh: place and shape its guides with the Groom Tools, GroomLab makes the hair from them"""
    bl_idname = "groomlab.add_groom"
    bl_label = "Create Groom from Selection"
    bl_options = {"REGISTER", "UNDO"}

    # As Maya's and the app's Create Groom: the default graph fitted to the scalp, no dialog (a
    # look comes later from the Presets menu). Scripts may name a preset and starter guides.
    preset: StringProperty(name="Preset", default="", options={"HIDDEN", "SKIP_SAVE"},
                           description="A preset's look to start from (\"\": the default graph)")
    guides: IntProperty(name="Starter Guides", default=0, min=0, max=5000, options={"HIDDEN", "SKIP_SAVE"},
                        description="Guides spread over the scalp to start from (0: place them with Place and Draw, as in Maya)")
    guide_points: IntProperty(name="Points per Guide", default=8, min=2, max=64, options={"HIDDEN", "SKIP_SAVE"})

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return obj is not None and obj.type == "MESH" and obj.groomlab.role == "NONE"

    def execute(self, context):
        scalp = context.active_object
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            groom.create_groom(context, scalp, preset=self.preset, guide_count=self.guides, guide_points=self.guide_points)
            context.scene.groomlab.groom = scalp
            graph.refresh(scalp)
        except HelperError as e:
            return _fail(self, e)
        from . import tools
        try:
            tools.refresh_info(scalp, context.scene)
        except HelperError:
            pass
        self.report({"INFO"}, "New groom created: place its guides with Groom Tools > Place")
        return {"FINISHED"}


class GROOMLAB_OT_remove_groom(GroomOperator):
    """Remove the groom: its guides and hair objects are deleted, the mesh stays"""
    bl_idname = "groomlab.remove_groom"
    bl_label = "Remove Groom"
    bl_options = {"REGISTER", "UNDO"}

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        scalp = active_groom(context)
        bpy.ops.groomlab.tool_exit()
        groom.close(scalp)
        graph_tree.remove_tree(scalp)
        scalp.groomlab_tools.state = ""
        g = scalp.groomlab
        g.view_node = ""
        for obj in (g.guides, g.hair):
            if obj is not None:
                data = obj.data
                bpy.data.objects.remove(obj)
                if data is not None and data.users == 0:
                    bpy.data.hair_curves.remove(data)
        vray.remove_object(scalp)
        g.role = "NONE"
        g.uid = ""
        g.snapshot = ""
        g.graph_json = ""
        g.nodes.clear()
        g.params.clear()
        groom.forget_cache()
        if context.scene.groomlab.groom == scalp:
            context.scene.groomlab.groom = None
        return {"FINISHED"}


class GROOMLAB_OT_sculpt_guides(GroomOperator):
    """Sculpt the guides with Blender's Curves Sculpt brushes (Comb, Add, Delete, Grow/Shrink, Snake Hook, Puff, Smooth...)"""
    bl_idname = "groomlab.sculpt_guides"
    bl_label = "Sculpt Guides"

    @classmethod
    def poll(cls, context):
        s = active_groom(context)
        return s is not None and s.groomlab.guides is not None  # B2's guides as Blender curves

    def execute(self, context):
        guides = active_groom(context).groomlab.guides
        if guides is None:
            self.report({"ERROR"}, "This groom has no Blender curve guides: edit its guides with the Groom Tools")
            return {"CANCELLED"}
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        guides.hide_set(False)
        for o in context.selected_objects:
            o.select_set(False)
        guides.select_set(True)
        context.view_layer.objects.active = guides
        bpy.ops.object.mode_set(mode="SCULPT_CURVES")
        return {"FINISHED"}


class GROOMLAB_OT_update(GroomOperator):
    """Send the scalp and guides again and evaluate (also restarts the helper if it stopped)"""
    bl_idname = "groomlab.update"
    bl_label = "Update"

    def execute(self, context):
        scalp = active_groom(context)
        try:
            groom.sync(scalp, scene=context.scene, force_eval=True, force_inputs=True)
            graph.refresh(scalp)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_refresh_presets(bpy.types.Operator):
    """List the shipped and user presets again"""
    bl_idname = "groomlab.refresh_presets"
    bl_label = "Refresh Presets"

    def execute(self, context):
        try:
            graph.presets(refresh=True)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_apply_preset(GroomOperator):
    """Replace the groom's graph with the preset's, fitted to this scalp (the guides stay)"""
    bl_idname = "groomlab.apply_preset"
    bl_label = "Apply Preset"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        scalp = active_groom(context)
        try:
            r = graph.apply_preset(scalp, context.scene.groomlab.preset)
        except HelperError as e:
            return _fail(self, e)
        note = graph.preset_maps_note(r)
        if note:
            self.report(*note)
        if r.get("drawGroup"):
            graph.draw_into(context, scalp, r["drawGroup"])  # a hand-placed look: draw its guides
        return {"FINISHED"}


class GROOMLAB_OT_insert_preset(GroomOperator):
    """Add the preset's look after the selected node, as one compound node"""
    bl_idname = "groomlab.insert_preset"
    bl_label = "Insert Preset"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        scalp = active_groom(context)
        node = graph.active_node(scalp)
        try:
            graph.insert_preset(scalp, context.scene.groomlab.preset, after=node.name if node and not node.is_output else None)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_node_add(GroomOperator):
    """Add a node after the selected one on the strands' chain"""
    bl_idname = "groomlab.node_add"
    bl_label = "Add Node"
    bl_options = {"REGISTER", "UNDO"}

    type: StringProperty(name="Type")

    def invoke(self, context, event):
        try:
            graph.operator_specs()
        except HelperError as e:
            return _fail(self, e)
        return self.execute(context)

    def execute(self, context):
        scalp = active_groom(context)
        node = graph.active_node(scalp)
        kind = self.type or context.scene.groomlab.add_type
        try:
            made, error = graph.add_node(scalp, kind, after=node.name if node else None)
        except HelperError as e:
            return _fail(self, e)
        if error:
            self.report({"WARNING"}, sentence(error))
        return {"FINISHED"}


class GROOMLAB_OT_node_remove(GroomOperator):
    """Remove the selected node (the chain closes over it)"""
    bl_idname = "groomlab.node_remove"
    bl_label = "Remove Node"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        scalp = active_groom(context)
        node = graph.active_node(scalp)
        if node is None:
            return {"CANCELLED"}
        try:
            error = graph.remove_node(scalp, node.name)
        except HelperError as e:
            return _fail(self, e)
        if error:
            self.report({"WARNING"}, sentence(error))
        return {"FINISHED"}


class GROOMLAB_OT_node_move(GroomOperator):
    """Move the selected node along the strands' chain"""
    bl_idname = "groomlab.node_move"
    bl_label = "Move Node"
    bl_options = {"REGISTER", "UNDO"}

    direction: IntProperty(default=-1)

    def execute(self, context):
        scalp = active_groom(context)
        node = graph.active_node(scalp)
        try:
            if node is None or not graph.move_node(scalp, node.name, self.direction):
                self.report({"INFO"}, "This node does not move that way: the source and the output stay put")
                return {"CANCELLED"}
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_toggle_group(GroomOperator):
    """Show or fold this group of params"""
    bl_idname = "groomlab.toggle_group"
    bl_label = "Toggle Group"

    group: StringProperty()

    def execute(self, context):
        g = active_groom(context).groomlab
        groups = [x for x in g.open_groups.split("|") if x]
        if self.group in groups:
            groups.remove(self.group)
        else:
            groups.append(self.group)
        g.open_groups = "|".join(groups)
        return {"FINISHED"}


class GROOMLAB_OT_save_groom(GroomOperator, ExportHelper):
    """Save the groom to a groom file (.groom; Y up, centimeters: Maya and the GroomLab app open it as it is)"""
    bl_idname = "groomlab.save_groom"
    bl_label = "Save Groom File"
    filename_ext = ".groom"
    filter_glob: StringProperty(default="*.groom", options={"HIDDEN"})
    up_axis: EnumProperty(name="Up Axis", items=[("Y", "Y (Maya)", "GroomLab's own space"), ("Z", "Z", "Z up (Blender, Houdini pipelines)")],
                          default="Y")

    def execute(self, context):
        scalp = active_groom(context)
        try:
            r = groom.save_groom(scalp, self.filepath, up_axis=self.up_axis)
        except HelperError as e:
            return _fail(self, e)
        scalp.groomlab.groom_file = r.get("path") or self.filepath  # Save to / Reload from Groom File
        self.report({"INFO"}, "Saved %s" % r["path"])
        return {"FINISHED"}


class GROOMLAB_OT_save_groom_linked(GroomOperator):
    """Write the groom to the groom file it is linked to (asked for when none), for the standalone app to open"""
    bl_idname = "groomlab.save_groom_linked"
    bl_label = "Save to Groom File"

    def invoke(self, context, event):
        if not active_groom(context).groomlab.groom_file:
            return bpy.ops.groomlab.save_groom("INVOKE_DEFAULT")
        return self.execute(context)

    def execute(self, context):
        scalp = active_groom(context)
        path = bpy.path.abspath(scalp.groomlab.groom_file)
        if not path:
            self.report({"ERROR"}, "This groom has no groom file yet: Save Groom File As... first")
            return {"CANCELLED"}
        try:
            r = groom.save_groom(scalp, path)
        except HelperError as e:
            return _fail(self, e)
        self.report({"INFO"}, "Saved %s" % r["path"])
        return {"FINISHED"}


class GROOMLAB_OT_reload_groom(GroomOperator):
    """Replace the groom's graph, guides, correctives and maps with its groom file's (the standalone app's edits),
    keeping the scalp"""
    bl_idname = "groomlab.reload_groom"
    bl_label = "Reload from Groom File"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        scalp = active_groom(context)
        return scalp is not None and bool(scalp.groomlab.groom_file)

    def execute(self, context):
        scalp = active_groom(context)
        path = bpy.path.abspath(scalp.groomlab.groom_file)
        try:
            scalp, info, _ = groom.load_groom(context, path, scalp)
            graph_tree.mark_replaced(scalp)
            graph.refresh(scalp)
        except HelperError as e:
            return _fail(self, e)
        for n in list(info.get("notes") or []) + list(info.get("inputErrors") or []):
            self.report({"WARNING"}, sentence(n))
        return {"FINISHED"}


class GROOMLAB_OT_add_node_before_output(GroomOperator):
    """A node of this type just before the groom's output"""
    bl_idname = "groomlab.add_node_before_output"
    bl_label = "Add Node"
    bl_options = {"REGISTER", "UNDO"}

    type: StringProperty(name="Type")

    @classmethod
    def description(cls, context, properties):
        from . import actions
        return actions.tip_for(cls.bl_idname, properties, "A node of this type just before the groom's output")

    def execute(self, context):
        scalp = active_groom(context)
        try:
            made, error = graph.add_node(scalp, self.type)
            if self.type == "Wind":
                graph.set_param(scalp, made, "strength", 0.3)  # Maya's add_wind
        except HelperError as e:
            return _fail(self, e)
        graph_tree.graph_changed(scalp)
        if error:
            self.report({"WARNING"}, sentence(error))
        self.report({"INFO"}, "Added %s: play the timeline" % made)
        return {"FINISHED"}


class GROOMLAB_OT_review_heatmap(GroomOperator):
    """Color strands by root density: blue sparse, green typical, red bunched"""
    bl_idname = "groomlab.review_heatmap"
    bl_label = "Density Heatmap"

    def execute(self, context):
        from . import display
        d = active_groom(context).groomlab_tools
        d.strand_color = "solid" if d.strand_color == "density" else "density"
        if d.strand_color == "density":
            display.show_attribute_colors(context)
        return {"FINISHED"}


class GROOMLAB_OT_load_groom(bpy.types.Operator, ImportHelper):
    """Open a groom file (.groom, from Maya, the GroomLab app or Blender) on the selected mesh, or with the file's own scalp"""
    bl_idname = "groomlab.load_groom"
    bl_label = "Open Groom File"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".groom"
    filter_glob: StringProperty(default="*.groom", options={"HIDDEN"})
    use_file_scalp: BoolProperty(name="Use the File's Scalp", default=False,
                                 description="Make a mesh of the scalp stored in the file instead of growing on the selected mesh")

    def invoke(self, context, event):
        obj = groom.scalp_of(context.active_object) or context.active_object
        self.use_file_scalp = obj is None or obj.type != "MESH"
        return ImportHelper.invoke(self, context, event)

    def execute(self, context):
        target = None
        if not self.use_file_scalp:
            target = groom.scalp_of(context.active_object) or context.active_object
            if target is None or target.type != "MESH":
                self.report({"ERROR"}, "No mesh selected: select the mesh to grow the groom on, or turn on Use the File's Scalp")
                return {"CANCELLED"}
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            scalp, info, _ = groom.load_groom(context, self.filepath, target)
            scalp.groomlab.groom_file = self.filepath  # Save to / Reload from Groom File
            context.scene.groomlab.groom = scalp
            graph_tree.mark_replaced(scalp)  # the file's layout in the Graph Editor
            graph.refresh(scalp)
        except HelperError as e:
            return _fail(self, e)
        notes = list(info.get("notes") or []) + list(info.get("inputErrors") or [])
        try:  # the file's reference photos (the app's Reference window, or saved here) as cameras
            from . import photos
            made = photos.restore_from_groom(scalp, context.scene)
            if made:
                notes.append("%d reference photo%s as cameras (Groom Tools > Photos)" % (len(made), "" if len(made) == 1 else "s"))
        except (HelperError, RuntimeError, ValueError) as e:
            notes.append("the file's reference photos did not come in: %s" % e)
        for n in notes:
            self.report({"WARNING"}, sentence(n))
        return {"FINISHED"}


class _Export(GroomOperator, ExportHelper):
    frames: EnumProperty(name="Frames", items=[("CURRENT", "Current Frame", ""), ("SCENE", "Scene Range", "")], default="CURRENT")
    density: FloatProperty(name="Density", default=1.0, min=0.0, max=1.0, subtype="FACTOR",
                           description="Share of the strands written (1: all, as rendered)")
    fmt = ""

    def execute(self, context):
        scene = context.scene
        first = last = scene.frame_current
        if self.frames == "SCENE":
            first, last = scene.frame_start, scene.frame_end
        try:
            r = groom.export(active_groom(context), scene, self.fmt, self.filepath, first, last, density=self.density)
        except HelperError as e:
            return _fail(self, e)
        self.report({"INFO"}, "Exported %s" % r.get("path", self.filepath))
        return {"FINISHED"}


class GROOMLAB_OT_export_alembic(_Export):
    """Export the groom's strands as Alembic curves (Y up, centimeters; Blender's importer converts)"""
    bl_idname = "groomlab.export_alembic"
    bl_label = "Export Alembic"
    filename_ext = ".abc"
    filter_glob: StringProperty(default="*.abc", options={"HIDDEN"})
    fmt = "alembic"


class GROOMLAB_OT_export_usd(_Export):
    """Export the groom's strands as USD basis curves (upAxis Y, metersPerUnit 0.01)"""
    bl_idname = "groomlab.export_usd"
    bl_label = "Export USD"
    filename_ext = ".usda"
    filter_glob: StringProperty(default="*.usd;*.usda", options={"HIDDEN"})
    fmt = "usd"


class GROOMLAB_OT_export_vrscene(_Export):
    """Export the groom as a V-Ray scene: its strands and V-Ray hair shader (centimeters, Y up), for
    V-Ray Standalone or V-Ray for Maya's VRayScene node"""
    bl_idname = "groomlab.export_vrscene"
    bl_label = "Export V-Ray Scene"
    filename_ext = ".vrscene"
    filter_glob: StringProperty(default="*.vrscene", options={"HIDDEN"})
    fmt = "vrscene"


_card_items = []  # kept: Blender reads an enum's item strings after the callback returns


def _card_preset_items(self, context):
    global _card_items
    _card_items = [(p["name"], p["name"], p.get("description", "")) for p in groom.card_map_presets()] or \
        [("Unreal Hair Cards", "Unreal Hair Cards", "")]
    return _card_items


class GROOMLAB_OT_bake_card_maps(GroomOperator):
    """Bake the groom's cards (its Cards nodes) and write their maps as a preset packs them: Unreal's
    hair card layout, a packed Unreal set, Unity HDRP hair, every map on its own, or a preset of yours
    (Documents/GroomLab/card_presets, shared with Maya and the app)"""
    bl_idname = "groomlab.bake_card_maps"
    bl_label = "Export Card Maps"
    bl_options = {"REGISTER"}

    preset: EnumProperty(name="Preset", items=_card_preset_items,
                         description="What goes in each file: an engine's layout, every map on its own, or your own")
    resolution: IntProperty(name="Resolution", default=2048, min=16, max=8192,
                            description="Texels square (the atlas is square), up to 8192")
    file_format: EnumProperty(name="Format", items=[("PRESET", "Preset's", "The format the preset suggests"),
                                                    ("tga", "TGA", ""), ("png", "PNG", ""), ("tiff", "TIFF", ""),
                                                    ("exr", "EXR", "")], default="PRESET")
    bit_depth: EnumProperty(name="Bit Depth", items=[("0", "Default", "The preset's, else the format's"),
                                                     ("8", "8-bit", ""), ("16", "16-bit", "PNG, TIFF; half floats in EXR"),
                                                     ("32", "32-bit float", "EXR, TIFF")], default="0")
    padding: IntProperty(name="Padding", default=-1, min=-1, max=256,
                         description="Texels the colors spread past the strands so mip maps don't bleed (-1: auto)")
    supersample: EnumProperty(name="Supersampling", items=[("1", "1x (off)", ""), ("2", "2x", ""), ("4", "4x", "")],
                              default="1", description="Anti-aliasing: baked bigger, then brought down")
    directory: StringProperty(name="Folder", subtype="DIR_PATH", default="//card_maps/")
    name: StringProperty(name="Name", default="", description="The files' {name} (default: the scalp's name)")

    def invoke(self, context, event):
        groom.card_map_presets(refresh=True)
        return context.window_manager.invoke_props_dialog(self, width=360)

    def execute(self, context):
        scalp = active_groom(context)
        directory = bpy.path.abspath(self.directory or "//card_maps/")
        try:
            r = groom.bake_card_maps(scalp, context.scene, self.preset, directory, self.name or scalp.name,
                                     resolution=self.resolution,
                                     fmt="" if self.file_format == "PRESET" else self.file_format,
                                     bit_depth=int(self.bit_depth), padding=self.padding,
                                     supersample=int(self.supersample))
        except HelperError as e:
            return _fail(self, e)
        self.report({"INFO"}, "Wrote %d card maps into %s" % (len(r.get("paths", [])), directory))
        return {"FINISHED"}


_EYE_PROPS = {"STRANDS": "show_strands", "GUIDES": "show_guides"}


def set_shown(what, on, scalps=None):
    """Show or hide the strands or the guides (every guide group) of `scalps` (default: every groom
    in the file). Returns the grooms changed."""
    prop = _EYE_PROPS[what]
    changed = []
    for scalp in (groom.scalps() if scalps is None else scalps):
        d = scalp.groomlab_tools
        if getattr(d, prop) != bool(on):
            setattr(d, prop, bool(on))  # (its update redraws and hides the hair)
            changed.append(scalp)
    return changed


class GROOMLAB_OT_eye(bpy.types.Operator):
    """Show or hide the groom's strands or its guides. Shift+click (or right-click): every groom in the file"""
    bl_idname = "groomlab.eye"
    bl_label = "Show or Hide"
    bl_options = {"REGISTER", "UNDO"}

    what: EnumProperty(name="What", items=[("STRANDS", "Strands", "The hair"), ("GUIDES", "Guides", "Every guide group")])
    mode: EnumProperty(name="Mode", items=[("TOGGLE", "Toggle", "Shown: hide; hidden: show"),
                                           ("SHOW", "Show", "Show"), ("HIDE", "Hide", "Hide")])
    every: BoolProperty(name="Every Groom", default=False, description="Every groom in the file, not only the current one")

    @classmethod
    def description(cls, context, properties):
        if properties.every and properties.mode != "TOGGLE":
            return "%s the %s of every groom in the file" % (properties.mode.capitalize(),
                                                             "guides" if properties.what == "GUIDES" else "strands")
        return ("Show or hide the groom's guides, every guide group" if properties.what == "GUIDES" else
                "Show or hide the groom's strands") + ". Shift+click (or right-click): every groom in the file"

    def invoke(self, context, event):
        if event.shift:
            self.every = True
        return self.execute(context)

    def execute(self, context):
        current = active_groom(context)
        if self.mode == "TOGGLE":
            if current is None:
                return {"CANCELLED"}
            on = not getattr(current.groomlab_tools, _EYE_PROPS[self.what])
        else:
            on = self.mode == "SHOW"
        set_shown(self.what, on, None if self.every else [current] if current is not None else [])
        return {"FINISHED"}


classes = (GROOMLAB_OT_eye, GROOMLAB_OT_add_groom, GROOMLAB_OT_remove_groom, GROOMLAB_OT_sculpt_guides, GROOMLAB_OT_update,
           GROOMLAB_OT_refresh_presets, GROOMLAB_OT_apply_preset, GROOMLAB_OT_insert_preset, GROOMLAB_OT_node_add,
           GROOMLAB_OT_node_remove, GROOMLAB_OT_node_move, GROOMLAB_OT_toggle_group, GROOMLAB_OT_save_groom,
           GROOMLAB_OT_load_groom, GROOMLAB_OT_save_groom_linked, GROOMLAB_OT_reload_groom,
           GROOMLAB_OT_add_node_before_output, GROOMLAB_OT_review_heatmap, GROOMLAB_OT_export_alembic,
           GROOMLAB_OT_export_usd, GROOMLAB_OT_export_vrscene, GROOMLAB_OT_bake_card_maps)


GROOM_FILE_TEXT = "GroomLab Groom File (.groom)"  # File > Import and File > Export, named as Blender names formats


def menu_add(self, context):
    self.layout.operator(GROOMLAB_OT_add_groom.bl_idname, icon="CURVES")


def menu_import(self, context):
    self.layout.operator(GROOMLAB_OT_load_groom.bl_idname, text=GROOM_FILE_TEXT)


def menu_export(self, context):
    self.layout.operator(GROOMLAB_OT_save_groom.bl_idname, text=GROOM_FILE_TEXT)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.VIEW3D_MT_object.append(menu_add)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    bpy.types.VIEW3D_MT_object.remove(menu_add)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
