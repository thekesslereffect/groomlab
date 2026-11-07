# SPDX-License-Identifier: GPL-3.0-or-later
"""The cards view and its bakes (Maya's compare_cards, card_display, bake_cards, bake_scalp and
scalp_cap; the app's Cards mode): A/B the game cards against the strands in the viewport.

Show cards (the groom's groomlab_tools.show_cards) hides the strands and shows the Cards nodes'
cards as a mesh object (<scalp>_cards, role CARDS), rebuilt from the helper's card_view whenever
the groom is evaluated; with Show textures (card_textures, on by default) and a bake, its material
shows the baked preview map (the color darkened by the occlusion, the coverage cutting out the
hair's silhouette: Material Preview and renders show it, Solid shading the flat card color), else
the flat card color. After Bake Scalp, the hair cap (<scalp>_scalpCap, role SCALP_CAP: the scalp
just above itself, textured with ScalpColor and ScalpCoverage) shows under the cards, so low LODs
show no bare skin between cards. Both objects are the viewport's (hidden in renders), parented to
the scalp, not selectable, and hidden while the strands show.

Bake Maps writes the cards' maps (Unreal's hair card layout, the atlas, the preview) into the
.blend's groomlab/<name>/cards/<groom> folder and the groom keeps the bake (its file's cardBake:
rebuilt cards keep their atlas cells, Card Stats says when they are out of date); Bake Scalp
writes <groom>_ScalpColor.tga and <groom>_ScalpCoverage.tga there (the groom keeps their prefix
in groomlab_tools.scalp_bake). An untitled .blend bakes into a temp folder its first save takes
along (groom.follow_files)."""

import os

import bpy
from bpy.props import IntProperty

from . import actions, groom, process
from .helper_client import HelperError
from .ops import GroomOperator, _fail, active_groom

CARD_COLOR = (0.62, 0.55, 0.47, 1.0)  # flat cards (no maps, or Show textures off)
SCALP_MAPS = ("ScalpColor", "ScalpCoverage")


# ---- Where the bakes go ----------------------------------------------------------------------------

def bake_folder(scalp):
    """The groom's own folder in the .blend's cards folder (Maya's _bake_folder: a folder per groom),
    or in the temp root while the .blend is untitled."""
    root = groom.asset_folder("") or groom.temp_root()
    return "%s/cards/%s" % (root, groom._file_safe(scalp.name))


def scalp_maps(scalp):
    """{ScalpColor, ScalpCoverage: path} of the groom's last scalp bake ({} without one on disk)."""
    prefix = scalp.groomlab_tools.scalp_bake
    if not prefix:
        return {}
    maps = {m: prefix + m + ".tga" for m in SCALP_MAPS}
    return maps if all(os.path.isfile(p) for p in maps.values()) else {}


# ---- The objects -----------------------------------------------------------------------------------

def _object(scalp, attr, suffix, role):
    """The groom's cards or hair cap object (made the first time, with an empty mesh)."""
    g = scalp.groomlab
    obj = getattr(g, attr)
    if obj is not None and obj.name in bpy.data.objects and obj.type == "MESH":
        return obj
    mesh = bpy.data.meshes.new("%s_%s" % (scalp.name, suffix))
    obj = bpy.data.objects.new(mesh.name, mesh)
    groom._link_like(obj, scalp, groom.scene_of(scalp) or bpy.context.scene)
    groom._parent_identity(obj, scalp)
    obj.hide_select = True   # the add-on's output, rebuilt as the groom changes
    obj.hide_render = True   # the viewport's A/B: renders show the hair
    obj.groomlab.role = role
    obj.groomlab.scalp = scalp
    setattr(g, attr, obj)
    return obj


def _set_mesh(obj, ref, material):
    from . import pipeline
    old = obj.data
    mesh = pipeline.mesh_from_arrays(old.name, ref)
    mesh.materials.append(material)
    obj.data = mesh
    if old.users == 0:
        bpy.data.meshes.remove(old)


def _image(path, color_space="sRGB"):
    """The image at `path`, read again when the file changed since it was loaded."""
    image = bpy.data.images.load(path, check_existing=True)
    stamp = str(os.path.getmtime(path)) if os.path.isfile(path) else ""
    if image.get("groomlab_stamp") not in (None, stamp):
        image.reload()
    image["groomlab_stamp"] = stamp
    try:
        image.colorspace_settings.name = color_space
    except TypeError:
        pass
    return image


def _cut_out(material):
    """Coverage as a cut-out, cards seen from both sides (as engines draw them)."""
    if hasattr(material, "surface_render_method"):  # Blender 4.2+ (EEVEE Next)
        material.surface_render_method = "DITHERED"
    if hasattr(material, "blend_method"):
        try:
            material.blend_method = "HASHED"
        except TypeError:
            pass
    material.use_backface_culling = False


def _material(name, color=None, alpha=None, alpha_channel=False):
    """A material of the cards view, diffuse only (Maya's lambert: it lights as the viewport's strands
    do, so the two compare fairly): `color` an image (else the flat card color); its alpha (the color
    image's own with `alpha_channel`, else the `alpha` image) cuts the surface out."""
    material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    material.use_nodes = True
    material.diffuse_color = CARD_COLOR  # Solid shading
    tree = material.node_tree
    tree.nodes.clear()
    out = tree.nodes.new("ShaderNodeOutputMaterial")
    diffuse = tree.nodes.new("ShaderNodeBsdfDiffuse")
    diffuse.location = (-400, 100)
    cover = None
    if color is None:
        diffuse.inputs["Color"].default_value = CARD_COLOR
    else:
        texture = tree.nodes.new("ShaderNodeTexImage")
        texture.location = (-800, 100)
        texture.image = color
        tree.links.new(texture.outputs["Color"], diffuse.inputs["Color"])
        if alpha_channel:
            cover = texture.outputs["Alpha"]
        elif alpha is not None:
            node = tree.nodes.new("ShaderNodeTexImage")
            node.location = (-800, -200)
            node.image = alpha
            cover = node.outputs["Color"]
    if cover is None:
        tree.links.new(diffuse.outputs["BSDF"], out.inputs["Surface"])
    else:
        clear = tree.nodes.new("ShaderNodeBsdfTransparent")
        clear.location = (-400, -100)
        mix = tree.nodes.new("ShaderNodeMixShader")
        mix.location = (-150, 0)
        tree.links.new(cover, mix.inputs["Fac"])
        tree.links.new(clear.outputs["BSDF"], mix.inputs[1])
        tree.links.new(diffuse.outputs["BSDF"], mix.inputs[2])
        tree.links.new(mix.outputs["Shader"], out.inputs["Surface"])
    _cut_out(material)
    return material


def _hide(obj, hide):
    if obj is not None and obj.name in bpy.data.objects and obj.hide_viewport != hide:
        obj.hide_viewport = hide


def refresh(scalp, rt=None):
    """The cards view as the groom is now: the card mesh (and the hair cap after Bake Scalp) rebuilt,
    their materials, the strands hidden; Show cards off: the card objects hidden, the strands back.
    Returns the helper's card_view answer (None when the view is off). A graph with no Cards node
    shows the strands (the error kept for the panel)."""
    from . import display
    g, t = scalp.groomlab, scalp.groomlab_tools
    if not t.show_cards:
        _hide(g.cards, True)
        _hide(g.scalp_cap, True)
        display.apply_strand_visibility(scalp)
        return None
    rt = rt or groom.ensure(scalp)
    cap = scalp_maps(scalp)
    try:
        r = process.call("card_view", session=rt.session, cap=bool(cap))
    except HelperError as e:
        rt.error = e.message
        _hide(g.cards, True)
        _hide(g.scalp_cap, True)
        display.apply_strand_visibility(scalp)
        return None
    bake = r.get("bake") or {}
    textured = bool(t.card_textures and bake.get("preview"))
    cards = _object(scalp, "cards", "cards", "CARDS")
    material = _material("%s_cards_view" % scalp.name, _image(bake["preview"]) if textured else None, alpha_channel=True)
    _set_mesh(cards, r["meshes"]["cards"], material)
    _hide(cards, False)
    if cap and "scalpCap" in r["meshes"]:
        obj = _object(scalp, "scalp_cap", "scalpCap", "SCALP_CAP")
        mat = _material("%s_scalpCap_view" % scalp.name, _image(cap["ScalpColor"]),
                        _image(cap["ScalpCoverage"], "Non-Color"))
        _set_mesh(obj, r["meshes"]["scalpCap"], mat)
        _hide(obj, False)
    else:
        _hide(g.scalp_cap, True)
    display.apply_strand_visibility(scalp)
    rt.cards_view = {"stale": bool(r.get("stale")), "baked": bool(bake), "cards": r.get("cards", 0),
                     "triangles": r.get("triangles", 0)}
    return r


def refresh_safely(scalp, rt=None):
    try:
        return refresh(scalp, rt)
    except Exception as e:  # noqa: BLE001 - called from evaluations in timers and handlers
        print("GroomLab: the cards view: %r" % e)
        return None


def show(scalp, on=None):
    """A/B the cards against the strands (Maya's compare_cards): True the cards view, False the
    strands, None flips. Returns "cards" or "strands"."""
    t = scalp.groomlab_tools
    on = (not t.show_cards) if on is None else bool(on)
    if t.show_cards != on:
        t.show_cards = on  # its update refreshes
    else:
        refresh(scalp)
    return "cards" if on else "strands"


def remove_objects(scalp):
    """The cards view's objects deleted (the groom removed)."""
    g = scalp.groomlab
    for attr in ("cards", "scalp_cap"):
        obj = getattr(g, attr)
        if obj is not None and obj.name in bpy.data.objects:
            mesh = obj.data
            bpy.data.objects.remove(obj)
            if mesh is not None and mesh.users == 0:
                bpy.data.meshes.remove(mesh)


# ---- The bakes -------------------------------------------------------------------------------------

def bake_cards(scalp, directory=None, name=None, resolution=0, occlusion=1.0):
    """Bake Maps (Maya's bake_cards): the cards' maps into `directory` (bake_folder) as <name>_<map>.tga
    (`name`: the groom's), the atlas and the preview; the groom keeps the bake and the cards view
    shows it. Returns the helper's answer {paths, prefix, name, cards, triangles, textures, size}."""
    from . import pipeline
    default = directory is None and name is None
    directory = (directory or bake_folder(scalp)).replace("\\", "/")
    args = {"directory": directory, "name": name or groom._file_safe(scalp.name), "occlusion": float(occlusion)}
    if resolution:
        args["resolution"] = int(resolution)
    if not default:
        args["exact"] = True
    r = pipeline.call(scalp, "bake_cards", **args)
    refresh(scalp)
    return r


def bake_scalp(scalp, directory=None, name=None, resolution=2048, reach=0.0, density=1.0):
    """Bake Scalp (Maya's bake_scalp): the groom baked onto its scalp's UVs as <name>_ScalpColor and
    <name>_ScalpCoverage (TGA) into `directory` (bake_folder), the hair cap's maps: the cards view
    shows the hair cap under the cards from then on. Returns the helper's answer {paths, prefix...}."""
    from . import pipeline
    default = directory is None and name is None
    directory = (directory or bake_folder(scalp)).replace("\\", "/")
    args = {"directory": directory, "name": name or groom._file_safe(scalp.name), "resolution": int(resolution),
            "density": float(density), "own": scalp.groomlab_tools.scalp_bake}
    if reach:
        args["reach"] = float(reach)
    if not default:
        args["exact"] = True
    r = pipeline.call(scalp, "bake_scalp", **args)
    from . import props
    with props.quiet():
        scalp.groomlab_tools.scalp_bake = r["prefix"]
    refresh(scalp)
    return r


# ---- Operators -------------------------------------------------------------------------------------

class _Tipped(object):
    @classmethod
    def description(cls, context, properties):
        return actions.tip_for(cls.bl_idname, properties, cls.__doc__ or "")


class GROOMLAB_OT_cards_bake(_Tipped, GroomOperator):
    """Bake the cards' maps for the atlas into the .blend's groomlab cards folder; the cards view shows them"""
    bl_idname = "groomlab.cards_bake"
    bl_label = "Bake Maps"

    resolution: IntProperty(name="Resolution", default=0, min=0, max=16384,
                            description="The maps' texels square (0: the Cards nodes' atlasSize)")

    def execute(self, context):
        scalp = active_groom(context)
        context.window.cursor_set("WAIT")
        try:
            r = bake_cards(scalp, resolution=self.resolution)
        except HelperError as e:
            return _fail(self, e)
        finally:
            context.window.cursor_set("DEFAULT")
        self.report({"INFO"}, "GroomLab: baked %d card maps into %s" % (len(r.get("paths") or []),
                                                                       os.path.dirname(r["prefix"])))
        return {"FINISHED"}


class GROOMLAB_OT_cards_bake_scalp(_Tipped, GroomOperator):
    """Bake the groom onto its scalp's UVs (hair color and coverage), with a hair cap that shows it under the cards"""
    bl_idname = "groomlab.cards_bake_scalp"
    bl_label = "Bake Scalp"

    resolution: IntProperty(name="Resolution", default=2048, min=16, max=16384, description="The maps' texels square")

    def execute(self, context):
        scalp = active_groom(context)
        context.window.cursor_set("WAIT")
        try:
            r = bake_scalp(scalp, resolution=self.resolution)
        except HelperError as e:
            return _fail(self, e)
        finally:
            context.window.cursor_set("DEFAULT")
        text = "baked %d scalp maps and a hair cap" % len(r.get("paths") or [])
        if not scalp.groomlab_tools.show_cards:
            text += " (Show cards shows it under the cards)"
        self.report({"INFO"}, "GroomLab: " + text)
        return {"FINISHED"}


classes = (GROOMLAB_OT_cards_bake, GROOMLAB_OT_cards_bake_scalp)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)

