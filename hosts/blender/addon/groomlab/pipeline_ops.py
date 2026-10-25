# SPDX-License-Identifier: GPL-3.0-or-later
"""The GroomLab menu's and the Groom Tools' Output strip's way out and other inputs (Maya's menu.py
output items, palette.py's Output strip, transfer_window.py): the engine exports (Unreal and Unity
grooms and cards, glTF cards, the scales as FBX or a USD instancer), Strands to Mesh, Bake Scale Maps,
Simulate (Add Simulate Node, Simulate Range with progress in the status bar, Bake, Clear), Add
(Feathers, Braid on Curves, Cards, Collider, the sample meshes), Transfer Groom to Scalp, Review (the
strand count HUD, a turntable), Find Missing Files and Copy Graph to Other Grooms. The work is
pipeline.py's (over the helper); actions.py binds these to the shared UI definitions' ids."""

import os
import tempfile

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import actions, groom, pipeline, tools
from . import definitions as defs
from .helper_client import HelperError, sentence
from .ops import GroomOperator, _fail, active_groom


class _Tipped(object):
    """The operator's tooltip: its action's in actions.json (the definitions' words, as Maya's)."""

    @classmethod
    def description(cls, context, properties):
        return actions.tip_for(cls.bl_idname, properties, cls.__doc__ or "")


def _say(op, text, level="INFO"):
    op.report({level}, "GroomLab: " + text)


def _export_folder():
    """Where exports start: beside the .blend, else the temp folder (an untitled file)."""
    return os.path.dirname(bpy.data.filepath) if bpy.data.filepath else tempfile.gettempdir()


def _select_only(context, obj):
    for o in context.view_layer.objects:
        if o is not None and o.select_get():
            o.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj


def _own_objects(scalp):
    g = scalp.groomlab
    return {o for o in (scalp, g.hair, g.guides) if o is not None}


def _selected(context, kinds, scalp=None):
    """Selected objects of `kinds` (object types) that are not the groom's own (scalp, hair, guides)."""
    own = _own_objects(scalp) if scalp is not None else set()
    return [o for o in context.selected_objects if o.type in kinds and o not in own and o.groomlab.role == "NONE"]


# ---- Exports -----------------------------------------------------------------------------------------

class _EngineExport(_Tipped, GroomOperator, ExportHelper):
    """An export to one file (Maya's fileDialog2 with the groom's name and a suffix as the start)."""
    bl_options = {"REGISTER"}
    fmt = ""
    suffix = ""
    done = "wrote %s"

    def invoke(self, context, event):
        if not self.filepath:
            self.filepath = os.path.join(_export_folder(), active_groom(context).name + self.suffix)
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def export_options(self, scalp):
        return {}

    def execute(self, context):
        scalp = active_groom(context)
        path = bpy.path.abspath(self.filepath)
        try:
            r = pipeline.export(scalp, self.fmt, path, **self.export_options(scalp))
        except HelperError as e:
            return _fail(self, e)
        for w in r.get("warnings") or []:
            _say(self, sentence(w), "WARNING")
        _say(self, self.done % ((r.get("fbxWritten") or r.get("files") or [r.get("path") or path])[0]))
        return {"FINISHED"}


class GROOMLAB_OT_export_unreal_groom(_EngineExport):
    """Alembic in Unreal's groom format: strands plus primary guides, at the current frame"""
    bl_idname = "groomlab.export_unreal_groom"
    bl_label = "Export for Unreal"
    filename_ext = ".abc"
    filter_glob: StringProperty(default="*.abc", options={"HIDDEN"})
    fmt, suffix, done = "unrealGroom", "_unreal.abc", "wrote %s for Unreal's groom importer"


class GROOMLAB_OT_export_unity_strands(_EngineExport):
    """Alembic curves for Unity's hair package: widths and root UVs per point, at the current frame"""
    bl_idname = "groomlab.export_unity_strands"
    bl_label = "Export Unity Strands"
    filename_ext = ".abc"
    filter_glob: StringProperty(default="*.abc", options={"HIDDEN"})
    fmt, suffix, done = "unityStrands", "_unity.abc", "wrote %s for Unity's hair package"


class GROOMLAB_OT_export_gltf_cards(_EngineExport):
    """The game cards as glTF 2.0 (.glb with the maps inside, or .gltf beside its files)"""
    bl_idname = "groomlab.export_gltf_cards"
    bl_label = "Export glTF Cards"
    filename_ext = ".glb"
    filter_glob: StringProperty(default="*.glb;*.gltf", options={"HIDDEN"})
    check_extension = None  # .glb or .gltf, as typed
    resolution: IntProperty(name="Resolution", default=0, min=0, max=8192,
                            description="The maps' texels square (0: the cards' own atlas size)")
    fmt, suffix = "gltfCards", "_cards.glb"

    def export_options(self, scalp):
        r = {"name": scalp.name, "binary": not self.filepath.lower().endswith(".gltf")}
        if self.resolution:
            r["resolution"] = self.resolution
        return r


class GROOMLAB_OT_export_scales_fbx(_EngineExport):
    """The scales at the current frame as an FBX mesh (Unreal's static or skeletal mesh import)"""
    bl_idname = "groomlab.export_scales_fbx"
    bl_label = "Export Scales Mesh"
    filename_ext = ".fbx"
    filter_glob: StringProperty(default="*.fbx", options={"HIDDEN"})
    fmt, suffix = "scalesFbx", "_scales.fbx"


class GROOMLAB_OT_export_scales_usd(_EngineExport):
    """The strands and the scales as a USD PointInstancer of their plates, at the current frame"""
    bl_idname = "groomlab.export_scales_usd"
    bl_label = "Export Scales Instancer"
    filename_ext = ".usda"
    filter_glob: StringProperty(default="*.usda;*.usd", options={"HIDDEN"})
    fmt, suffix = "scalesUsd", "_scales.usda"


class _FolderExport(_Tipped, GroomOperator):
    """An export into a folder (Maya's folder dialog, from the exports folder)."""
    bl_options = {"REGISTER"}
    directory: StringProperty(name="Folder", subtype="DIR_PATH")
    filter_folder: BoolProperty(default=True, options={"HIDDEN"})
    resolution: IntProperty(name="Resolution", default=0, min=0, max=8192,
                            description="The maps' texels square (0: the cards' own atlas size)")
    fmt = ""

    def invoke(self, context, event):
        if not self.directory:
            self.directory = _export_folder()
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        scalp = active_groom(context)
        folder = bpy.path.abspath(self.directory)
        if not folder:
            self.report({"ERROR"}, "Pick a folder to write into")
            return {"CANCELLED"}
        options = {"resolution": self.resolution} if self.resolution and self.fmt != "unityFeathers" else {}
        try:
            r = pipeline.export(scalp, self.fmt, directory=folder, name=scalp.name, **options)
        except HelperError as e:
            return _fail(self, e)
        for w in r.get("warnings") or []:
            _say(self, sentence(w), "WARNING")
        _say(self, self.done(r, folder))
        return {"FINISHED"}

    def done(self, r, folder):
        return "wrote %s" % (r.get("fbx") or folder)


class GROOMLAB_OT_export_unreal_cards(_FolderExport):
    """The game cards as FBX with TGA maps in Unreal's hair card layout and a manifest, into a folder"""
    bl_idname = "groomlab.export_unreal_cards"
    bl_label = "Export Unreal Cards"
    fmt = "unrealCards"

    def done(self, r, folder):
        return "wrote %s for Unreal's hair cards" % (r.get("fbx") or folder)


class GROOMLAB_OT_export_unity_cards(_FolderExport):
    """The game cards as FBX (meters, Y up) with PNG maps for HDRP/URP and a manifest, into a folder"""
    bl_idname = "groomlab.export_unity_cards"
    bl_label = "Export Unity Cards"
    fmt = "unityCards"


class GROOMLAB_OT_export_unity_feathers(_FolderExport):
    """Feathers for Unity: the strands .abc, the rachis FBX and a manifest of the Hair Asset settings"""
    bl_idname = "groomlab.export_unity_feathers"
    bl_label = "Export Unity Feathers"
    fmt = "unityFeathers"

    def done(self, r, folder):
        return "wrote %s (%d strand groups)" % (r.get("abc") or folder, len(r.get("groups") or []))


# ---- Utilities: Strands to Mesh, Bake Scale Maps --------------------------------------------------------

class GROOMLAB_OT_strands_to_mesh(_Tipped, GroomOperator):
    """The strands as one polygon mesh: tubes, or ribbons lying along the scalp, with UVs root to tip"""
    bl_idname = "groomlab.strands_to_mesh"
    bl_label = "Strands to Mesh"
    bl_options = {"REGISTER", "UNDO"}

    style: EnumProperty(name="Style", items=[("tube", "Tube", "Tubes round each strand"),
                                             ("ribbon", "Ribbon", "Flat ribbons lying along the scalp")],
                        default="tube")
    sides: IntProperty(name="Tube Sides", default=4, min=3, max=32)
    width_scale: FloatProperty(name="Width Scale", default=1.0, min=0.001, soft_max=10.0,
                               description="Times the strands' widths")
    max_strands: IntProperty(name="At Most", default=100000, min=1,
                             description="Beyond this many strands, thinned by strand ID")
    strand_uvs: BoolProperty(name="Per-strand UVs", default=False,
                             description="A second UV map, strandUVs: each strand in its own cell of a grid")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scalp = active_groom(context)
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            obj, r = pipeline.strands_to_mesh(scalp, style=self.style, sides=self.sides, width_scale=self.width_scale,
                                              max_strands=self.max_strands, strand_uvs=self.strand_uvs)
        except HelperError as e:
            return _fail(self, e)
        _select_only(context, obj)
        _say(self, "%s: %d strands as %ss, %d faces%s" % (obj.name, r.get("strands", 0), self.style, len(obj.data.polygons),
                                                        " (thinned to the cap)" if r.get("capped") else ""))
        return {"FINISHED"}


class GROOMLAB_OT_bake_scale_maps(_Tipped, GroomOperator):
    """The scales baked into the skin's UVs as normal, height and color maps (PNG, UDIM tiles too)"""
    bl_idname = "groomlab.bake_scale_maps"
    bl_label = "Bake Scale Maps"

    directory: StringProperty(name="Folder", subtype="DIR_PATH", default="",
                              description="Where the maps go (empty: beside the .blend in groomlab/bakes)")
    resolution: IntProperty(name="Resolution", default=2048, min=16, max=16384)

    def execute(self, context):
        scalp = active_groom(context)
        folder = bpy.path.abspath(self.directory) if self.directory else os.path.join(_export_folder(), "groomlab", "bakes")
        try:
            r = pipeline.bake_scale_maps(scalp, folder, name=scalp.name, resolution=self.resolution)
        except HelperError as e:
            return _fail(self, e)
        _say(self, "baked the scale maps to %s (height %.4g)" % ((r.get("pattern") or {}).get("ScaleNormal", folder),
                                                                 r.get("maxHeight", 0.0)))
        return {"FINISHED"}


# ---- Simulate ----------------------------------------------------------------------------------------

def playback_range(scene):
    """The frames Simulate Range steps over: the preview range when it is on, else the scene's."""
    if scene.use_preview_range:
        return scene.frame_preview_start, scene.frame_preview_end
    return scene.frame_start, scene.frame_end


class GROOMLAB_OT_simulate_add(_Tipped, GroomOperator):
    """A Simulate node between the guides and Interpolate: play the timeline to see it"""
    bl_idname = "groomlab.simulate_add"
    bl_label = "Add Simulate Node"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        scalp = active_groom(context)
        try:
            node = pipeline.add_simulate(scalp)
        except HelperError as e:
            return _fail(self, e)
        _say(self, "added %s to %s: play the timeline, or Simulate Range" % (node, scalp.name))
        return {"FINISHED"}


class GROOMLAB_OT_simulate_range(_Tipped, GroomOperator):
    """Step the Simulate nodes over the playback range into their cache (Esc stops); with Bake, write
    their frames as Alembic guide caches after"""
    bl_idname = "groomlab.simulate_range"
    bl_label = "Simulate Range"

    bake: BoolProperty(name="Bake", default=False, options={"SKIP_SAVE"},
                       description="Write the Simulate nodes' frames to the .blend's groomlab/caches folder after")
    BUDGET_MS = 100.0   # a request's share of each modal step: Blender stays responsive

    def _begin(self, context, budget_ms):
        self._scalp = active_groom(context)
        scene = context.scene
        status = pipeline.simulation_status(self._scalp)
        if not status.get("nodes"):
            raise HelperError("state", "the groom has no Simulate node: Simulate > Add Simulate Node first")
        first, self._last = playback_range(scene)
        pipeline.send_scalp_frames(self._scalp, first, self._last, scene)
        self._first = first
        return pipeline.simulate_range(self._scalp, self._last, budget_ms=budget_ms, feed=False, scene=scene)

    def _end(self, context, r, cancelled=False):
        scalp = self._scalp
        if self.bake and not cancelled:
            w = pipeline.simulate_bake(scalp, last=self._last)
            written = w.get("written") or {}
            for node, path in written.items():
                print("GroomLab: baked %s of %s to %s" % (node, scalp.name, path))
            _say(self, "simulated to frame %d and baked %d Simulate node(s) of %s" % (self._last, len(written), scalp.name))
        elif cancelled:
            _say(self, "simulation stopped at frame %s%s" % (r.get("frame", "?"), "; nothing baked" if self.bake else ""),
                 "WARNING")
        else:
            _say(self, "simulated %s through frame %d" % (scalp.name, self._last))
        for e in r.get("errors") or []:
            _say(self, sentence(e), "WARNING")
        groom.mark(scalp, "eval")

    def execute(self, context):
        """Scripts and tests: to the end in one request (budget 0), then the bake."""
        try:
            r = self._begin(context, 0.0)
            self._end(context, r)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}

    def invoke(self, context, event):
        try:
            r = self._begin(context, self.BUDGET_MS)
        except HelperError as e:
            return _fail(self, e)
        if not r.get("running"):
            try:
                self._end(context, r)
            except HelperError as e:
                return _fail(self, e)
            return {"FINISHED"}
        wm = context.window_manager
        self._timer = wm.event_timer_add(0.02, window=context.window)
        wm.progress_begin(0, 100)
        wm.modal_handler_add(self)
        self._show(context, r)
        return {"RUNNING_MODAL"}

    def _show(self, context, r):
        progress = float(r.get("progress", 0.0) or 0.0)
        context.window_manager.progress_update(int(progress * 100))
        if context.workspace is not None:
            context.workspace.status_text_set("GroomLab: simulating %s, frame %s of %d (%d%%). Esc stops"
                                              % (self._scalp.name, r.get("frame", "?"), self._last, int(progress * 100)))

    def _stop(self, context):
        wm = context.window_manager
        wm.event_timer_remove(self._timer)
        wm.progress_end()
        if context.workspace is not None:
            context.workspace.status_text_set(None)

    def modal(self, context, event):
        try:
            if event.type == "ESC":
                r = pipeline.simulate_cancel(self._scalp)
                self._stop(context)
                self._end(context, r, cancelled=True)
                return {"CANCELLED"}
            if event.type != "TIMER":
                return {"RUNNING_MODAL"}
            r = pipeline.simulate_poll(self._scalp, self.BUDGET_MS)
            if r.get("running"):
                self._show(context, r)
                return {"RUNNING_MODAL"}
            self._stop(context)
            self._end(context, r)
        except HelperError as e:
            self._stop(context)
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_simulate_clear(_Tipped, GroomOperator):
    """Drop the Simulate nodes' frames and bakes: the next frame shown simulates again"""
    bl_idname = "groomlab.simulate_clear"
    bl_label = "Clear Simulation"

    def execute(self, context):
        try:
            r = pipeline.simulate_clear(active_groom(context))
        except HelperError as e:
            return _fail(self, e)
        _say(self, "cleared the simulation%s" % (" and %d bake(s)" % len(r["clearedBakes"])
                                                 if r.get("clearedBakes") else ""))
        return {"FINISHED"}


# ---- Add ---------------------------------------------------------------------------------------------

_CURVE_KINDS = {"CURVE", "CURVES"}


class GROOMLAB_OT_add_feathers(_Tipped, GroomOperator):
    """Feathers on the groom: scattered feathers shaped by the guides (a design, or an assortment), or
    one hand-placed feather per guide of a guide group (selected curves join it)"""
    bl_idname = "groomlab.add_feathers"
    bl_label = "Add Feathers"
    bl_options = {"REGISTER", "UNDO"}

    design: StringProperty(name="Design", default="contour",
                           description="One design, or several separated by commas (an assortment: contour, down); "
                                       "contour, down, flight, afterfeather, barred, or a feathers_<name> preset of yours")
    weights: StringProperty(name="Weights", default="",
                            description="The assortment's shares, one per design (3, 1); empty: equal")
    # engine/ui/choices.json's featherSource: the words every host shows.
    source: EnumProperty(name=defs.get("choices")["featherSource"]["label"], items=defs.enum_items("featherSource"),
                         default=defs.enum_default("featherSource"))
    group: StringProperty(name="Guide Group", default="feathers",
                          description="One feather per guide: the guide group whose guides are the feathers, left out "
                                      "of the hair (pick one, or type a new name: flight_left, flight_right, tail)")

    def invoke(self, context, event):
        scalp = active_groom(context)
        try:
            groups = pipeline.feather_guide_groups(scalp)
        except HelperError:
            groups = []
        if groups:
            self.group = groups[0]
        self.source = "guides" if (groups or _selected(context, _CURVE_KINDS, scalp)) else defs.enum_default("featherSource")
        return context.window_manager.invoke_props_dialog(self, width=420)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "design")
        layout.prop(self, "weights")
        layout.prop(self, "source", expand=True)
        row = layout.row()
        row.enabled = self.source == "guides"
        row.prop_search(self, "group", context.scene.groomlab_tools, "groups", results_are_suggestions=True)
        n = len(_selected(context, _CURVE_KINDS, active_groom(context)))
        layout.label(text="%d curve object(s) selected: each curve becomes a feather in the guide group" % n if n else
                     "One feather per guide: each guide of the group is a feather (none yet: draw them with Draw)",
                     icon="INFO")

    def execute(self, context):
        scalp = active_groom(context)
        designs = [d.strip() for d in self.design.split(",") if d.strip()] or ["contour"]
        try:
            weights = [float(w) for w in self.weights.replace(",", " ").split()] or None
        except ValueError:
            self.report({"ERROR"}, "Weights are numbers, one per design (3, 1)")
            return {"CANCELLED"}
        group = self.group.strip() or "feathers"
        curves = _selected(context, _CURVE_KINDS, scalp) if self.source == "guides" else None
        try:
            pipeline.add_feathers(scalp, designs if len(designs) > 1 else designs[0], source=self.source,
                                  weights=weights, curves=curves, group=group)
        except HelperError as e:
            return _fail(self, e)
        tools.refresh_info(scalp, context.scene)
        if self.source != "guides":
            _say(self, "added scattered %s feathers to %s" % (", ".join(designs), scalp.name))
            return {"FINISHED"}
        count = next((g.guides for g in context.scene.groomlab_tools.groups if g.name == group), 0)
        _say(self, "added %s feathers to %s: one per guide of %s (%d now)" % (", ".join(designs), scalp.name, group, count))
        if not count and not curves:
            from . import graph
            graph.draw_into(context, scalp, group)  # none yet: every guide drawn is a feather
        return {"FINISHED"}


class GROOMLAB_OT_add_braid(_Tipped, GroomOperator):
    """The selected curves (drawn from the scalp) become braid paths: a Braid node braids the strands along them"""
    bl_idname = "groomlab.add_braid"
    bl_label = "Braid on Curves"
    bl_options = {"REGISTER", "UNDO"}

    style: EnumProperty(name="Style", items=[
        ("three", "Three", "A classic plait"), ("fishtail", "Fishtail", "A fishtail braid"),
        ("rope", "Rope", "Two strands twisted"), ("cornrow", "Cornrow", "Fed along the scalp")], default="three")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        self.layout.prop(self, "style")
        n = len(_selected(context, _CURVE_KINDS, active_groom(context)))
        self.layout.label(text="%d curve object(s) selected become braid paths" % n if n else
                          "No curves selected: the braids group's guides are the paths (draw them with Draw)", icon="INFO")

    def execute(self, context):
        scalp = active_groom(context)
        try:
            node = pipeline.add_braid(scalp, curves=_selected(context, _CURVE_KINDS, scalp) or None, style=self.style)
        except HelperError as e:
            return _fail(self, e)
        tools.refresh_info(scalp, context.scene)
        _say(self, "added %s (%s braid) to %s" % (node, self.style, scalp.name))
        return {"FINISHED"}


class GROOMLAB_OT_add_cards(_Tipped, GroomOperator):
    """A Cards node before the output at a level of detail"""
    bl_idname = "groomlab.add_cards"
    bl_label = "Add Cards"
    bl_options = {"REGISTER", "UNDO"}

    lod: EnumProperty(name="Level of Detail", items=[("hero", "Hero", ""), ("mid", "Mid", ""), ("low", "Low", "")],
                      default="mid")

    def execute(self, context):
        scalp = active_groom(context)
        try:
            node = pipeline.add_cards(scalp, self.lod)
        except HelperError as e:
            return _fail(self, e)
        _say(self, "added %s (%s cards) to %s" % (node, self.lod, scalp.name))
        return {"FINISHED"}


class GROOMLAB_OT_add_collider(_Tipped, GroomOperator):
    """The selected mesh (besides the groom) keeps the strands off it: a ColliderMesh and a Collide node before the output"""
    bl_idname = "groomlab.add_collider"
    bl_label = "Add Collider"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        scalp = active_groom(context)
        meshes = _selected(context, {"MESH"}, scalp)
        if not meshes:
            self.report({"WARNING"}, "Select a mesh (besides the groom) to collide with")
            return {"CANCELLED"}
        names = []
        try:
            for obj in meshes:
                pipeline.add_collider(scalp, obj)
                names.append(pipeline.collider_name(obj))
        except HelperError as e:
            return _fail(self, e)
        _say(self, "added collider %s to %s" % (", ".join(names), scalp.name))
        return {"FINISHED"}


def _frame_selected(context):
    """The 3D views framed on the selection: a sample comes in at its real size (a head 35 cm, a
    third of Blender's 1 m grid), so it fills the view rather than sitting small in it."""
    if bpy.app.background or context.window is None:
        return
    for area in context.window.screen.areas:
        if area.type != "VIEW_3D":
            continue
        region = next((r for r in area.regions if r.type == "WINDOW"), None)
        if region is None:
            continue
        try:
            with context.temp_override(window=context.window, area=area, region=region):
                bpy.ops.view3d.view_selected(use_all_regions=False)
        except RuntimeError:
            pass


class GROOMLAB_OT_add_sample(_Tipped, bpy.types.Operator):
    """A sample mesh at the origin and selected: then Create Groom from Selection"""
    bl_idname = "groomlab.add_sample"
    bl_label = "Add Sample Mesh"
    bl_options = {"REGISTER", "UNDO"}

    sample: EnumProperty(name="Sample", items=[("nika", "Nika (head)", ""), ("manny", "Manny (head)", ""),
                                               ("wolf", "Wolf", ""), ("buddy", "Buddy (rig)", "")], default="nika")

    def execute(self, context):
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            part = pipeline.add_sample(context, self.sample)
        except HelperError as e:
            return _fail(self, e)
        _frame_selected(context)
        _say(self, "added %s (selected): Create Groom from Selection grows hair on it" % part.name)
        return {"FINISHED"}


# ---- Transfer Groom to Scalp -----------------------------------------------------------------------------

METHOD_LABELS = {  # Maya's transfer.METHOD_LABELS
    "closest": ("Closest point", "Where the new scalp is nearest (the same place and size)"),
    "bounds": ("Fit bounding boxes", "The old scalp's bounds moved and scaled onto the new one's first"),
    "markersAlign": ("Markers: align (move, turn, scale)",
                     "3+ marker pairs (the groom's stored ones) give the best move, turn and scale"),
    "markersWarp": ("Markers: warp (follows proportions)",
                    "4+ marker pairs (the groom's stored ones) spread over the head bend space onto the new one"),
    "uv": ("Matching UVs", "Both meshes laid out alike in UV; each root at its UV"),
}


def _target_candidates(context, scalp):
    return [o for o in _selected(context, {"MESH"}, scalp)]


class GROOMLAB_OT_transfer_groom(_Tipped, GroomOperator):
    """Move the groom onto another mesh (a new sculpt, a retopologized head, another character): guides,
    parts and flow re-rooted"""
    bl_idname = "groomlab.transfer_groom"
    bl_label = "Transfer Groom to Scalp"
    bl_options = {"REGISTER", "UNDO"}

    target: StringProperty(name="New Scalp", description="The mesh the groom moves onto")
    method: EnumProperty(name="Method", items=[(m, METHOD_LABELS[m][0], METHOD_LABELS[m][1])
                                               for m in pipeline.TRANSFER_METHODS], default="closest")
    keep_lengths: BoolProperty(name="Keep guide lengths", default=False,
                               description="Guides turn with the new head but keep their lengths (off: they scale with it)")
    fit_size: BoolProperty(name="Fit groom to the new scalp's size", default=False,
                           description="The new scalp taken as the old one resized: the groom's scale grows with its "
                                       "area (lengths, widths and clumps follow, the strand count stays). Off: the groom "
                                       "keeps the size shown")
    maps: BoolProperty(name="Project painted maps", default=True,
                       description="Vertex and texture maps carried onto the new scalp by the same method (a texture "
                                   "map gets a new image; the old file is left as it is)")
    own: BoolProperty(name="The new scalp owns the groom", default=True,
                      description="Its hair and guides objects move to the new mesh, which is the groom from then on "
                                  "(off: the groom stays on this object, grown on the new mesh's shape)")

    _markers = 0

    def invoke(self, context, event):
        scalp = active_groom(context)
        found = _target_candidates(context, scalp)
        if found and not self.target:
            self.target = found[0].name
        try:
            type(self)._markers = len(pipeline.transfer_markers(scalp))
        except HelperError:
            type(self)._markers = 0
        return context.window_manager.invoke_props_dialog(self, width=420)

    def draw(self, context):
        layout = self.layout
        layout.label(text="Groom: %s" % active_groom(context).name)
        layout.prop_search(self, "target", context.scene, "objects")
        layout.prop(self, "method")
        col = layout.column(align=True)
        col.prop(self, "keep_lengths")
        col.prop(self, "fit_size")
        col.prop(self, "maps")
        col.prop(self, "own")
        if self.method.startswith("markers"):
            layout.label(text="%d stored marker pair(s) (picked in Maya or the GroomLab app)" % self._markers,
                         icon="INFO")

    def execute(self, context):
        scalp = active_groom(context)
        target = bpy.data.objects.get(self.target)
        if target is None or target.type != "MESH" or target in _own_objects(scalp):
            self.report({"ERROR"}, "Pick the mesh to move the groom onto (another mesh than its scalp)")
            return {"CANCELLED"}
        if self.own and target.groomlab.role != "NONE":
            self.report({"ERROR"}, "%s has a groom already: pick another mesh" % target.name)
            return {"CANCELLED"}
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            r = pipeline.transfer(scalp, target, method=self.method, maps=self.maps, keep_lengths=self.keep_lengths,
                                  fit_size=self.fit_size, own=self.own)
        except HelperError as e:
            return _fail(self, e)
        if self.own:
            _select_only(context, target)
        for w in r.get("warnings") or []:
            _say(self, sentence(w), "WARNING")
        if r.get("missing"):
            _say(self, "not carried onto the new scalp: %s" % ", ".join(r["missing"]), "WARNING")
        method = METHOD_LABELS.get(r.get("method"), (r.get("method"),))[0]
        _say(self, "the groom is on %s (%s guides re-rooted, %s)" % (target.name, r.get("guides", 0), method))
        return {"FINISHED"}


# ---- Review ------------------------------------------------------------------------------------------

class GROOMLAB_OT_turntable(_Tipped, GroomOperator):
    """A camera orbiting the groom over 120 frames, with a key, fill and rim light (the viewport looks through it)"""
    bl_idname = "groomlab.turntable"
    bl_label = "Turntable"
    bl_options = {"REGISTER", "UNDO"}

    frames: IntProperty(name="Frames", default=120, min=2, max=10000)

    def execute(self, context):
        scalp = active_groom(context)
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        rig = pipeline.turntable(context, scalp, frames=self.frames)
        screen = getattr(context, "screen", None)
        for area in (screen.areas if screen is not None else []):
            if area.type == "VIEW_3D":
                area.spaces.active.region_3d.view_perspective = "CAMERA"
                break
        _say(self, "the viewport looks through %s now (Numpad 0 to leave it); play the timeline" % rig["camera"].name)
        return {"FINISHED"}


_hud = [None]


def hud_text(context):
    """The HUD line for the active groom (Maya's review.hud_text): strands shown, at render, guides, ms."""
    scalp = active_groom(context)
    if scalp is None:
        return "GroomLab: no groom"
    rt = groom.runtime_if_any(scalp)
    last = rt.last if rt is not None else {}
    strands = int(last.get("strandCount") or 0)
    density = max(float(scalp.groomlab.preview_density), 1e-6)
    t = context.scene.groomlab_tools
    guides = t.guide_count if tools.is_hierarchy(scalp) else \
        (len(scalp.groomlab.guides.data.curves) if scalp.groomlab.guides is not None else 0)
    return "%s: %s strands (%s at render) | %s guides | %.0f ms" % (
        scalp.name, format(strands, ","), format(int(round(strands / density)), ","), format(guides, ","),
        float(last.get("milliseconds") or 0.0))


def draw_hud():
    context = bpy.context
    scene = getattr(context, "scene", None)
    if scene is None or not scene.groomlab_tools.show_hud:
        return
    import blf
    region = context.region
    scale = context.preferences.system.ui_scale
    blf.size(0, 12 * scale)
    blf.color(0, 1.0, 1.0, 1.0, 0.9)
    blf.enable(0, blf.SHADOW)
    blf.shadow(0, 3, 0.0, 0.0, 0.0, 0.8)
    blf.position(0, 20 * scale, region.height - 80 * scale, 0)
    blf.draw(0, hud_text(context))
    blf.disable(0, blf.SHADOW)


# ---- Utilities: Find Missing Files, Copy Graph to Other Grooms ------------------------------------------

def _grooms_for(context):
    """The selected grooms, else every groom of the scene."""
    picked = []
    for o in context.selected_objects:
        s = groom.scalp_of(o)
        if s is not None and s not in picked:
            picked.append(s)
    return picked or [s for s in groom.scalps() if s.name in context.scene.objects]


class GROOMLAB_OT_find_missing_files(_Tipped, bpy.types.Operator):
    """Relink the selected grooms' (else every groom's) missing texture maps, images, Alembic files and
    groom files to files of the same name in a folder you pick"""
    bl_idname = "groomlab.find_missing_files"
    bl_label = "Find Missing Files"
    bl_options = {"REGISTER"}

    directory: StringProperty(name="Folder", subtype="DIR_PATH",
                              description="The folder (and its subfolders) to look for the missing files in")
    filter_folder: BoolProperty(default=True, options={"HIDDEN"})

    def invoke(self, context, event):
        grooms = _grooms_for(context)
        if not grooms:
            self.report({"WARNING"}, "No grooms in the scene")
            return {"CANCELLED"}
        try:
            missing = sum(pipeline.count({"x": m}) for m in (pipeline.missing_files(s) for s in grooms))
        except HelperError as e:
            return _fail(self, e)
        if not missing:
            _say(self, "no missing files in %d groom(s)" % len(grooms))
            return {"FINISHED"}
        if not self.directory:
            self.directory = _export_folder()
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        grooms = _grooms_for(context)
        folder = bpy.path.abspath(self.directory)
        try:
            report = pipeline.find_missing_files(grooms, folder)
        except HelperError as e:
            return _fail(self, e)
        for name, kinds in sorted(report["relinked"].items()):
            for kind, entries in sorted(kinds.items()):
                for key, path in sorted(entries.items()):
                    print("GroomLab: %s: %s %s -> %s" % (name, kind[:-1], key, path))
        for name, kinds in sorted(report["ambiguous"].items()):
            for kind, entries in sorted(kinds.items()):
                for key, candidates in sorted(entries.items()):
                    _say(self, "%s: %s %s: several files fit, none relinked: %s" % (name, kind[:-1], key,
                                                                                  ", ".join(candidates)), "WARNING")
        for name, kinds in sorted(report["still_missing"].items()):
            for kind, entries in sorted(kinds.items()):
                for key, path in sorted(entries.items()):
                    _say(self, "%s: %s %s: no %s in %s" % (name, kind[:-1], key, os.path.basename(path), folder),
                         "WARNING")
        _say(self, "relinked %d missing file(s); %d still missing, %d with several matches"
             % (pipeline.count(report["relinked"]), pipeline.count(report["still_missing"]),
                pipeline.count(report["ambiguous"])))
        return {"FINISHED"}


class GROOMLAB_OT_copy_graph_to_grooms(_Tipped, GroomOperator):
    """The active groom's graph (its look) onto the other selected grooms; their guides kept"""
    bl_idname = "groomlab.copy_graph_to_grooms"
    bl_label = "Copy Graph to Other Grooms"
    bl_options = {"REGISTER", "UNDO"}

    @staticmethod
    def targets(context):
        source = active_groom(context)
        return [s for s in _grooms_for(context) if s != source and any(o.select_get() for o in _own_objects(s))]

    def invoke(self, context, event):
        if not self.targets(context):
            self.report({"WARNING"}, "Select the grooms to copy %s's graph to (pick %s in the Groom list first)"
                        % ((active_groom(context).name,) * 2))
            return {"CANCELLED"}
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        source = active_groom(context)
        targets = self.targets(context)
        try:
            report = pipeline.copy_graph(source, targets)
        except HelperError as e:
            return _fail(self, e)
        missing = sorted({"%s (%s)" % (m, why) for r in report.values() for m, why in r["missing"].items()})
        _say(self, "copied %s's graph to %d groom(s)" % (source.name, len(report)))
        if missing:
            _say(self, "maps the graph reads that some grooms lack: " + "; ".join(missing), "WARNING")
        return {"FINISHED"}


# ---- Registration --------------------------------------------------------------------------------------

@bpy.app.handlers.persistent
def _after_undo(*_args):
    try:
        pipeline.undo_moves()
    except Exception as e:  # noqa: BLE001 - an undo handler must not take Blender down
        print("GroomLab: undoing a transfer: %s" % e)


class GROOMLAB_OT_import_alembic_curves(_Tipped, bpy.types.Operator, ImportHelper):
    """An Alembic file's curves as the groom's guides (an ImportCurves node rooted on the scalp); with a mesh
    selected and no groom, a groom is made on it first"""
    bl_idname = "groomlab.import_alembic_curves"
    bl_label = "Import Alembic Curves as Guides"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".abc"
    filter_glob: StringProperty(default="*.abc", options={"HIDDEN"})
    object_name: StringProperty(name="Object", description="Only the curves under this object (empty: every curve)")
    samples: IntProperty(name="Points per Guide", default=24, min=0, max=1024,
                         description="Points each guide is resampled to (0: as read)")

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return active_groom(context) is not None or (obj is not None and obj.type == "MESH")

    def execute(self, context):
        scalp = active_groom(context)
        try:
            if scalp is None:
                mesh = context.active_object
                if context.mode != "OBJECT":
                    bpy.ops.object.mode_set(mode="OBJECT")
                groom.create_groom(context, mesh)
                scalp = mesh
                context.scene.groomlab.groom = scalp
            r = pipeline.import_alembic_curves(scalp, bpy.path.abspath(self.filepath), self.object_name, self.samples)
        except HelperError as e:
            return _fail(self, e)
        _say(self, "the guides now come from %s (%d curves; Utilities > Bake Node to Guides... makes them brushable)"
             % (os.path.basename(self.filepath), r["curves"]))
        return {"FINISHED"}


_OUTPUT = "__output__"


def _bake_node_items(self, context):
    scalp = active_groom(context)
    items = [(_OUTPUT, "(output)", "The groom's output: its strands as they are now")]
    if scalp is not None:
        try:
            items += [(n, n, "") for n in pipeline.bakeable_nodes(scalp)]
        except HelperError:
            pass
    _bake_node_items.keep = items  # Blender keeps no reference to the strings of a dynamic enum
    return items


class GROOMLAB_OT_bake_to_guides(_Tipped, GroomOperator):
    """A graph node's strands or guides (ToGuides, a Scatter...) become guides you can brush"""
    bl_idname = "groomlab.bake_to_guides"
    bl_label = "Bake Node to Guides"
    bl_options = {"REGISTER"}

    node: StringProperty(name="Node", description="The node to bake (empty: the groom's output)")
    pick: EnumProperty(name="Node", items=_bake_node_items, options={"SKIP_SAVE"},
                       description="The groom's output, or any node's strands or guides, as they are now")
    group: StringProperty(name="Into Group", default="baked", description="A root group: made when new, added to otherwise")
    density: FloatProperty(name="Density", default=1.0, min=0.001, max=1.0, subtype="FACTOR",
                           description="Of the node's strands (1: every one)")

    def invoke(self, context, event):
        if self.node:
            try:
                self.pick = self.node
            except TypeError:
                pass
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "pick")
        layout.prop(self, "group")
        layout.prop(self, "density")
        layout.label(text="Each strand becomes a primary guide you can brush, mirror and give children.")

    def execute(self, context):
        scalp = active_groom(context)
        node = self.node
        if self.properties.is_property_set("pick"):
            node = "" if self.pick == _OUTPUT else self.pick
        try:
            r = pipeline.bake_to_guides(scalp, node, self.group or "baked", self.density)
        except HelperError as e:
            return _fail(self, e)
        _say(self, "baked %d guides from %s into %s" % (r["guides"], r["node"], r["group"]))
        return {"FINISHED"}


classes = (GROOMLAB_OT_export_unreal_groom, GROOMLAB_OT_export_unity_strands, GROOMLAB_OT_export_gltf_cards,
           GROOMLAB_OT_export_scales_fbx, GROOMLAB_OT_export_scales_usd, GROOMLAB_OT_export_unreal_cards,
           GROOMLAB_OT_export_unity_cards, GROOMLAB_OT_export_unity_feathers, GROOMLAB_OT_strands_to_mesh,
           GROOMLAB_OT_bake_scale_maps, GROOMLAB_OT_simulate_add, GROOMLAB_OT_simulate_range,
           GROOMLAB_OT_simulate_clear, GROOMLAB_OT_add_feathers, GROOMLAB_OT_add_braid, GROOMLAB_OT_add_cards,
           GROOMLAB_OT_add_collider, GROOMLAB_OT_add_sample, GROOMLAB_OT_transfer_groom, GROOMLAB_OT_turntable,
           GROOMLAB_OT_find_missing_files, GROOMLAB_OT_copy_graph_to_grooms, GROOMLAB_OT_import_alembic_curves,
           GROOMLAB_OT_bake_to_guides)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    if not bpy.app.background:
        _hud[0] = bpy.types.SpaceView3D.draw_handler_add(draw_hud, (), "WINDOW", "POST_PIXEL")
    for lst in (bpy.app.handlers.undo_post, bpy.app.handlers.redo_post):
        if _after_undo not in lst:
            lst.append(_after_undo)


def unregister():
    for lst in (bpy.app.handlers.undo_post, bpy.app.handlers.redo_post):
        if _after_undo in lst:
            lst.remove(_after_undo)
    if _hud[0] is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_hud[0], "WINDOW")
        _hud[0] = None
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
