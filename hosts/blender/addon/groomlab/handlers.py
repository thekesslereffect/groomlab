# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender's handlers: sculpted guides and edited scalps (depsgraph updates, debounced), frames
(the scalp's pose and the time), final renders (every strand; V-Ray's: the groom's V-Ray scene,
vray.py), .blend saves and loads, undo, quit."""

import bpy
from bpy.app.handlers import persistent

from . import groom, process, vray


@persistent
def on_depsgraph(scene, depsgraph):
    if groom._rendering or groom._exporting or getattr(depsgraph, "mode", "VIEWPORT") == "RENDER":
        return
    found = groom.scalps()
    if not found:
        return
    for update in depsgraph.updates:
        idd = update.id.original if update.id is not None else None
        if idd is None:
            continue
        if isinstance(idd, bpy.types.Object):
            role = idd.groomlab.role
            if role == "GUIDES" and (update.is_updated_geometry or update.is_updated_transform) and idd.groomlab.scalp:
                groom.mark(idd.groomlab.scalp, "guides")
            elif role == "SCALP" and update.is_updated_geometry:
                groom.mark(idd, "pose")  # its evaluated mesh (an armature, shape keys): the points
                                        # again; the mesh data itself comes as a Mesh update below
        elif isinstance(idd, bpy.types.Curves):
            for s in found:
                if s.groomlab.guides is not None and s.groomlab.guides.data == idd:
                    groom.mark(s, "guides")
        elif isinstance(idd, bpy.types.Mesh):
            for s in found:
                if s.data == idd:
                    groom.mark(s, "scalp")


@persistent
def on_frame(scene, depsgraph=None):
    if groom.scalps():
        groom.on_frame(scene, depsgraph)


@persistent
def on_render_init(scene, *args):
    vray.render_init(scene)


@persistent
def on_render_pre(scene, *args):
    if not groom.scalps():
        return
    # V-Ray renders each groom's V-Ray scene (written here at full density); the hair Curves keep
    # their preview unless a groom's scene could not be written (V-Ray converts the Curves then).
    if not vray.renders(scene) or not vray.render_pre(scene):
        groom.set_rendering(scene, True)


@persistent
def on_render_post(scene, *args):
    vray.render_post(scene)
    if groom._rendering:
        groom.set_rendering(scene, False)


@persistent
def on_save_pre(*args):
    groom.snapshot_all()


@persistent
def on_load_post(*args):
    groom.forget_all()
    subscribe_active()  # (a file load clears the message bus)


# Selecting a groom (its scalp, its guides or its hair as the active object) makes it the current
# groom: the Groom Tools and the unpinned Graph Editors show it (Maya's selection does the same).
_ACTIVE_OWNER = object()


def follow_active():
    """The active object's groom made the current one; that groom (None: not a groom's)."""
    context = bpy.context
    layer = getattr(context, "view_layer", None)
    scalp = groom.scalp_of(layer.objects.active if layer is not None else None)
    scene = context.scene
    if scalp is not None and groom.is_groom(scalp) and scene is not None and scene.groomlab.groom != scalp:
        scene.groomlab.groom = scalp  # (its update shows its graph)
    return scalp


def active_changed():
    follow_active()  # (the message bus wants no value back: Blender printed an error for one)


def subscribe_active():
    bpy.msgbus.clear_by_owner(_ACTIVE_OWNER)
    bpy.msgbus.subscribe_rna(key=(bpy.types.LayerObjects, "active"), owner=_ACTIVE_OWNER, args=(),
                             notify=active_changed)


@persistent
def on_undo(*args):
    groom.forget_cache()
    groom.invalidate_all()
    # The photos (pairs, cameras) as the undo left them, into the groom's own copy of them.
    from . import photos
    from .ops import active_groom
    try:
        scalp = active_groom(bpy.context)
        if scalp is not None and photos.list_photos(bpy.context.scene):
            photos.sync(scalp)
    except (AttributeError, ReferenceError):
        pass


@persistent
def on_exit(*args):
    process.shutdown()


_HANDLERS = (
    ("depsgraph_update_post", on_depsgraph),
    ("frame_change_post", on_frame),
    ("render_init", on_render_init),
    ("render_pre", on_render_pre),
    ("render_complete", on_render_post),
    ("render_cancel", on_render_post),
    ("save_pre", on_save_pre),
    ("load_post", on_load_post),
    ("undo_post", on_undo),
    ("redo_post", on_undo),
    ("exit_pre", on_exit),
)


def register():
    for name, fn in _HANDLERS:
        lst = getattr(bpy.app.handlers, name)
        if fn not in lst:
            lst.append(fn)
    if not bpy.app.background and not bpy.app.timers.is_registered(groom.poll):
        bpy.app.timers.register(groom.poll, first_interval=1.0, persistent=True)
    subscribe_active()


def unregister():
    for name, fn in _HANDLERS:
        lst = getattr(bpy.app.handlers, name)
        if fn in lst:
            lst.remove(fn)
    if bpy.app.timers.is_registered(groom.poll):
        bpy.app.timers.unregister(groom.poll)
    bpy.msgbus.clear_by_owner(_ACTIVE_OWNER)
