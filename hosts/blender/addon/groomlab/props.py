# SPDX-License-Identifier: GPL-3.0-or-later
"""The add-on's properties: a groom's settings on its scalp object (saved with the .blend), the
node list and the selected node's params as the panel shows them, the scene's groom choice and
preset browser."""

import contextlib
import json

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty,
                       IntProperty, PointerProperty, StringProperty)

_quiet = [0]


@contextlib.contextmanager
def quiet():
    """Properties set by the add-on itself: their update callbacks stay silent."""
    _quiet[0] += 1
    try:
        yield
    finally:
        _quiet[0] -= 1


def is_quiet():
    return _quiet[0] > 0


def _push(self, context):
    if is_quiet():
        return
    if isinstance(self.id_data, bpy.types.NodeTree):  # a Compound's param in the Graph Editor
        from . import graph_tree
        graph_tree.push_collection_param(self, context)
        return
    from . import graph
    graph.push_param(self, context)


_enum_cache = {}


def _choices(self, context):
    key = self.choices or "[]"
    items = _enum_cache.get(key)
    if items is None:
        items = _enum_cache[key] = [(c, c, "") for c in json.loads(key)] or [("", "", "")]
    return items


class GroomLabParam(bpy.types.PropertyGroup):
    # name: the param's name (PropertyGroup's own)
    kind: StringProperty()
    unit: StringProperty()
    group: StringProperty()
    advanced: BoolProperty()
    description: StringProperty()
    choices: StringProperty()
    show_when: StringProperty()
    raw: StringProperty()      # the value as JSON (showWhen reads it)
    widget: StringProperty()   # which value_* the panel shows; "fixed": not editable here
    note: StringProperty()
    value_bool: BoolProperty(update=_push)
    value_int: IntProperty(update=_push)
    value_float: FloatProperty(update=_push, precision=4)
    value_length: FloatProperty(update=_push, subtype="DISTANCE", unit="LENGTH", precision=5)
    value_factor: FloatProperty(update=_push, subtype="FACTOR", soft_min=0.0, soft_max=1.0)
    value_color: FloatVectorProperty(update=_push, subtype="COLOR", size=3, min=0.0, soft_max=1.0)
    value_vector: FloatVectorProperty(update=_push, size=3)
    value_enum: EnumProperty(items=_choices, update=_push)
    value_string: StringProperty(update=_push)


class GroomLabNode(bpy.types.PropertyGroup):
    type: StringProperty()
    note: StringProperty()
    error: StringProperty()
    unsupported: StringProperty()
    in_chain: BoolProperty()
    is_output: BoolProperty()


class GroomLabCollider(bpy.types.PropertyGroup):
    """A mesh the groom's ColliderMesh nodes read by name (Maya's colliderMeshes): sent as it is posed
    on every frame (pipeline.sync_colliders)."""
    # name: the graph's name for it (a ColliderMesh node's `name`)
    object: PointerProperty(type=bpy.types.Object)


def _node_selected(self, context):
    if is_quiet():
        return
    from . import graph
    from .helper_client import HelperError
    try:
        graph.refresh_params(self.id_data)
    except HelperError as e:
        print("GroomLab: %s" % e)


def _settings_changed(self, context):
    if is_quiet() or self.role != "SCALP":
        return
    from . import groom
    groom.mark(self.id_data, "eval")


class GroomLabObject(bpy.types.PropertyGroup):
    role: EnumProperty(items=[("NONE", "None", ""), ("SCALP", "Scalp", "The mesh a GroomLab groom grows on"),
                              ("GUIDES", "Guides", "A GroomLab groom's guide curves"),
                              ("HAIR", "Hair", "A GroomLab groom's strands (written by the add-on)"),
                              ("VRAY", "V-Ray Hair", "A GroomLab groom's V-Ray scene: what V-Ray for Blender renders "
                                                     "for it (vray.py)"),
                              ("CARDS", "Cards", "A GroomLab groom's game cards in the cards view (cards_view.py)"),
                              ("SCALP_CAP", "Hair Cap", "A GroomLab groom's hair cap in the cards view: the scalp "
                                                        "just above itself with the baked scalp maps")],
                       default="NONE")
    uid: StringProperty()
    scalp: PointerProperty(type=bpy.types.Object)
    guides: PointerProperty(type=bpy.types.Object)
    hair: PointerProperty(type=bpy.types.Object)
    cards: PointerProperty(type=bpy.types.Object)      # the cards view's card mesh (cards_view.py)
    scalp_cap: PointerProperty(type=bpy.types.Object)  # ... and its hair cap, after Bake Scalp
    uv_map: StringProperty(description="The scalp's UV map the groom reads (empty: the active one)")
    guide_target: StringProperty(default="flat")
    # Maya's previewDensity default (every strand); the budget keeps a dense groom (fur, a big
    # creature) drawable: Blender's curves cost more to update and draw than Maya's lines. Both thin
    # to a subset of the render's strands (the same strands, fewer); final renders and exports get
    # every strand.
    preview_density: FloatProperty(name="Preview Density", default=1.0, min=0.0, max=1.0, subtype="FACTOR",
                                   description="Share of the strands the viewport shows; final renders get all of them",
                                   update=_settings_changed)
    preview_point_budget: IntProperty(name="Point Budget (k)", default=0, min=0, soft_max=5000,
                                      description="At most this many thousand points in the viewport: a denser preview "
                                                  "shows fewer of the same strands (0: no limit). Final renders get every "
                                                  "strand", update=_settings_changed)
    follow_scalp: BoolProperty(name="Follow Scalp", default=True, update=_settings_changed,
                               description="On a deforming scalp (armature, shape keys) the groom is made on the rest "
                                           "pose and each strand carried along by its root")
    graph_json: StringProperty()  # the graph, kept current (rebuilds a session if the helper restarts)
    snapshot: StringProperty()    # the whole groom as a .groom text, written when the .blend is saved
    groom_file: StringProperty(subtype="FILE_PATH",  # Save to / Reload from Groom File (Maya's linked file)
                               description="The groom file this groom was opened from or saved to last")
    nodes: CollectionProperty(type=GroomLabNode)
    node_index: IntProperty(update=_node_selected)
    params: CollectionProperty(type=GroomLabParam)
    params_node: StringProperty()
    node_type_description: StringProperty()
    open_groups: StringProperty(default="")
    show_advanced: BoolProperty(name="Advanced", default=False)
    view_node: StringProperty(description="The node the viewport shows instead of the output (the Graph Editor's View)")
    colliders: CollectionProperty(type=GroomLabCollider)  # Add > Collider's meshes
    extra_scalps: CollectionProperty(type=GroomLabCollider)  # Add > Scalp's meshes (Maya's extraScalps), in order


def _is_scalp(self, obj):
    return obj.type == "MESH" and obj.groomlab.role == "SCALP"


_category_items = [("ALL", "All", "")]


def _categories(self, context):
    from . import graph
    cats = []
    for p in graph.cached_presets():
        if p[3] and p[3] not in cats:
            cats.append(p[3])
    _category_items[:] = [("ALL", "All", "Every preset")] + [(c, c, "") for c in cats]
    return _category_items


_preset_items = [("hair_basics", "Hair Basics", "")]


def _presets(self, context):
    from . import graph
    cat = self.preset_category
    found = [(p[0], p[1], p[2]) for p in graph.cached_presets() if cat in ("ALL", "", p[3])]
    _preset_items[:] = found or [("hair_basics", "Hair Basics", "")]
    return _preset_items


_type_items = [("Noise", "Noise", "")]


def _types(self, context):
    from . import graph
    specs = graph._specs
    if specs:
        _type_items[:] = sorted(((t, t, s.get("description", "")) for t, s in specs.items()
                                 if t not in ("Output", "Scalp", "Compound", "Script", "Input")), key=lambda x: x[0])
    return _type_items


def _groom_chosen(self, context):
    if is_quiet():
        return
    from . import graph_editor
    graph_editor.show_groom(context, self.groom)


class GroomLabScene(bpy.types.PropertyGroup):
    groom: PointerProperty(type=bpy.types.Object, poll=_is_scalp, name="Groom", update=_groom_chosen,
                           description="The groom the panels and the Graph Editor show (its scalp object)")
    preset_category: EnumProperty(name="Category", items=_categories)
    preset: EnumProperty(name="Preset", items=_presets)
    add_type: EnumProperty(name="Node", items=_types, description="The kind of node to add")


classes = (GroomLabParam, GroomLabNode, GroomLabCollider, GroomLabObject, GroomLabScene)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Object.groomlab = PointerProperty(type=GroomLabObject)
    bpy.types.Scene.groomlab = PointerProperty(type=GroomLabScene)


def unregister():
    del bpy.types.Scene.groomlab
    del bpy.types.Object.groomlab
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
