# SPDX-License-Identifier: Apache-2.0
"""Maps: named values painted on the scalp (guide layers, vertex maps, textures), the params they
feed, their image files (relinked, reloaded, found again when missing), the freeze mask, and map
values for set_param."""

import json
import os
import re

from maya import cmds

import groomlab_core

from groomlab.base import no_undo, undo_chunk, undoable
from groomlab.params import get_graph, set_graph, set_param
from groomlab.paths import asset_folder, asset_name, portable_path, resolve_path, _bump
from groomlab.guides import list_guide_groups, selected_guides, _targets
from groomlab.brushes import brush_tool, _rgba


@undoable("clump paint")
def clump_paint(groom, clump_node="clump", activate=True, **brush_options):
    """Paint clumping per guide: a shortcut for a painted map named "clump" (made
    if missing, starting unclumped) driving the Clump node's strength, and the
    Paint brush on it."""
    graph = get_graph(groom, expand_default=True)
    if not any(n["name"] == clump_node for n in graph["nodes"]):
        raise ValueError("%s has no node named %r (add a Clump node first)" % (groom, clump_node))
    if "clump" not in list_maps(groom):
        add_map(groom, "clump", 0.0)  # children inherit it, strands blend it
    set_param(groom, clump_node, "strength", attribute_map("clump"))
    return brush_tool("paint", attribute="clump", activate=activate, **brush_options)


# Freeze mask: the "freeze" map. Brushes leave frozen guides alone (children
# inherit it); frozen guides draw blue.


def freeze_guides(groom, guides=None, value=1.0):
    """Freeze guides (default: the selected ones); value 0 unfreezes."""
    if not guides and not selected_guides(groom):
        raise ValueError("select the guides to freeze")
    return cmds.groomLabGuides(groom, setAttribute="freeze", value=value, **_targets(guides))


def unfreeze_all(groom=None):
    """Every guide brushable again: the freeze map removed (one undo step). `groom`: the groom
    the tools work on by default (the GroomLab Unfreeze All hotkey)."""
    if groom is None:
        from groomlab.groom import active_groom
        groom = active_groom()
        if not groom:
            raise ValueError("select a groom to unfreeze")
    if _sculpt_freeze(groom, "unfreeze_all"):
        return
    if "freeze" in list_maps(groom):
        remove_map(groom, "freeze")


def invert_frozen(groom=None):
    """The frozen guides unfrozen and the rest frozen: each guide's freeze value v becomes 1 - v
    (one undo step; with nothing frozen, every guide freezes). `groom`: the groom the tools work
    on by default."""
    if groom is None:
        from groomlab.groom import active_groom
        groom = active_groom()
        if not groom:
            raise ValueError("select a groom to invert its frozen guides")
    if _sculpt_freeze(groom, "invert"):
        return
    cmds.groomLabGuides(groom, invertAttribute="freeze")


def _sculpt_freeze(groom, op):
    """With a Sculpt or SculptStrands node targeted (sculpts.target): Unfreeze All or Invert Frozen
    on its guides or strands, kept in the node (one undo step); False otherwise."""
    from groomlab import sculpts
    if not sculpts.target(groom):
        return False
    cmds.groomLabBrushStroke(groom, sculptFreeze=op)
    return True


def freeze_tool(value=1.0, **brush_options):
    """Paint the freeze mask (Ctrl while painting: unfreeze)."""
    return brush_tool("paint", attribute="freeze", value=value, **brush_options)


# ---------------------------------------------------------------------------
# Maps: named 0-1 values painted on the scalp (Groom Tools > Paint), of three kinds that convert
# into each other: a layer on the guides ("guides": children inherit it, strands blend it from
# their guides, it survives a new scalp), a value per scalp vertex ("vertices": kept with the
# groom in the scene) or an image at the scalp's UVs ("texture": a PNG in the scene's
# masks folder, masks/<asset name>/<map>.png, editable elsewhere, exportable). A Map node reads one by name into any
# mappable param (use_map, paint_param); the param's own value multiplies it. Vertex and texture
# maps are gray or color (r g b a: painted hair color through HairColor's map, or masks packed in
# channels, each read from its own output of the map's Map node); guide layers are gray.

MAP_KINDS = ("guides", "vertices", "texture")


def maps(groom):
    """The groom's maps: [{"name", "kind", "texture", "channels"}] (texture: a texture map's file,
    as stored; channels: 1 gray, 4 color)."""
    out = []
    for entry in cmds.groomLabScalpMaps(groom, list=True) or []:
        name, kind, texture, channels = (entry.split("|") + ["", "", ""])[:4]
        out.append({"name": name, "kind": kind, "texture": texture, "channels": int(channels or 1)})
    return out


def list_maps(groom):
    """The names of the groom's maps, every kind."""
    return [m["name"] for m in maps(groom)]


def map_kind(groom, name):
    """"guides", "vertices" or "texture", or "" for no such map."""
    return cmds.groomLabScalpMaps(groom, kindOf=name) or ""


def map_channels(groom, name):
    """1 for a gray map, 4 for a color map (r g b a), 0 for no such map."""
    return int(cmds.groomLabScalpMaps(groom, channelsOf=name) or 0)


@undoable("map channels")
def set_map_channels(groom, name, channels):
    """Make a map gray (1: its luminance) or color (4: the value in r g b, alpha 1). A guide
    layer stays gray (convert it to vertices or a texture first). A texture map's image is
    rewritten (this groom's own copy when another groom reads it). One undo step. Returns the
    channels. Every host's Channels (the editor's setMapChannels, in groomLabScalpMaps)."""
    return int(cmds.groomLabScalpMaps(groom, setChannels=(name, int(channels))))


def map_texture(groom, name):
    """A texture map's image file (absolute), else ""."""
    for m in maps(groom):
        if m["name"] == name and m["kind"] == "texture":
            return (resolve_path(m["texture"], groom) or m["texture"]).replace("\\", "/")
    return ""


def _map_texture_default(groom, name):
    """A new texture map's file: <masks>/<asset name>/<map>.png, stored relative to the scene
    (the command takes a free name when a file is already there)."""
    return portable_path(asset_folder("masks") + "/%s/%s.png" % (asset_name(groom), name))


def _check_png(texture):
    """A texture map's file is a PNG: painting and filling write PNG bytes to it."""
    if texture and not groomlab_core.is_png_path(texture):  # (every host's: texture.h isPngPath)
        raise ValueError("a texture map's file is a PNG (painting writes PNG), not %s: set_map_texture links "
                         "another image by copying it as one" % texture)


def _check_map_name(name):
    """Map names follow the one rule every host keeps (scalp_maps.h): letters, digits and _, not
    starting with a digit."""
    error = groomlab_core.map_name_error(name or "")
    if error:
        raise ValueError(error)


_FACE_RANGE = re.compile(r"\.f\[(\d+)(?::(\d+))?\]$")


def selected_faces(mesh):
    """The faces of `mesh` (a mesh shape or its transform) selected now, as polygon indices
    (ascending): faces as they are; vertices and edges as the faces all of whose corners they
    hold, else as the faces they touch. [] when none of the mesh's components is selected."""
    from groomlab.base import _shape_of
    try:
        shape = cmds.ls(_shape_of(mesh, "mesh"), long=True)[0]
    except (ValueError, RuntimeError, IndexError):
        return []
    components = [c for c in cmds.ls(selection=True, long=True) or []
                  if "." in c and (cmds.ls(c, objectsOnly=True, long=True) or [None])[0] == shape]
    if not components:
        return []
    found = (cmds.polyListComponentConversion(components, toFace=True, internal=True) or
             cmds.polyListComponentConversion(components, toFace=True) or [])
    faces = set()
    for item in cmds.ls(found) or []:
        m = _FACE_RANGE.search(item)
        if m:
            first = int(m.group(1))
            faces.update(range(first, int(m.group(2) or first) + 1))
    return sorted(faces)


def selected_scalp_faces(groom):
    """The faces of the groom's scalp mesh selected now (selected_faces); [] when none are."""
    from groomlab.base import _scalp_shape
    shape = _scalp_shape(groom)
    return selected_faces(shape) if shape else []


def _faces_flag(faces):
    return " ".join(str(int(f)) for f in faces)


def add_map(groom, name, value=0.0, kind="guides", resolution=1024, texture=None, color=None, faces=None):
    """A new map filled with `value`: a guide layer (`kind` "guides", on every primary guide,
    children inheriting), a vertex map ("vertices") or a texture map ("texture": a PNG of
    `resolution` square in the scene's masks folder (masks/<asset_name>/<name>.png; a free
    <name>_2.png when that file is taken), or at `texture`, stored relative
    to the scene). `color` makes a color map (vertices or texture; r g b a): (r, g, b[, a]) fills
    it with that color, True with (value, value, value, 1). With `faces` (polygon indices of the
    scalp: selected_scalp_faces) only those faces get the value or color, the rest 0 (a color
    map: black, alpha 1); a guide layer, the guides rooted on them. Returns the name."""
    _check_map_name(name)
    if kind not in MAP_KINDS:
        raise ValueError("a map's kind is guides, vertices or texture, not %r" % kind)
    if name in list_maps(groom):
        raise ValueError("the groom already has a map named %r" % name)
    if color is not None and color is not False and kind == "guides":
        raise ValueError("a guide layer is gray: make a color map on the scalp's vertices or as a texture")
    kwargs = {"make": name, "kind": kind, "value": float(value)}
    if color is not None and color is not False:
        kwargs["color"] = True
        if color is not True:
            kwargs["fillColor"] = _rgba(color)
    if kind == "texture":
        _check_png(texture)
        kwargs["texture"] = portable_path(texture) if texture else _map_texture_default(groom, name)
        kwargs["resolution"] = int(resolution)
    if faces:
        kwargs["faces"] = _faces_flag(faces)
    cmds.groomLabScalpMaps(groom, **kwargs)
    return name


def fill_map(groom, name, value=None, selected_only=False, color=None, channels="rgba", mode="replace", faces=None):
    """Set a map to `value` everywhere, or (a guide layer) on the selected guides only. A color
    map takes `color` (r, g, b[, a]) in the channels of `channels` (a subset of "rgba"), or
    `value` in its r g b (its alpha kept). With `faces` (polygon indices of the scalp:
    selected_scalp_faces) only those faces are filled (a guide layer: the guides rooted on them).
    `mode`: replace, add, subtract or clamp (the most a value may be). One undo step. Every host's
    Fill (the editor's fillMap, in groomLabScalpMaps)."""
    if value is None and color is None:
        raise ValueError("fill a map with a value or a color")
    kwargs = {"fill": name, "paintMode": mode}
    if color is not None:
        kwargs.update(fillColor=_rgba(color), fillChannels=channels or "rgba")
    else:
        kwargs["value"] = float(value)
    if faces:
        kwargs["faces"] = _faces_flag(faces)
    if selected_only:
        kwargs["selected"] = True
    return cmds.groomLabScalpMaps(groom, **kwargs)


def remove_map(groom, name, delete_file=False):
    """Remove a map. What reads it lets go of it: its Map nodes are removed, the params they fed
    read their own (slider) values again, and map params naming it go back to the node's
    default. A texture map's file stays on disk, unless `delete_file` and GroomLab made it (it
    is in the scene's masks folder; a PNG of yours linked in place stays) and no other map or
    groom reads it. One undo step (undo puts the file back too). Returns the (node, param)
    pairs that read the map. Every host's Remove (the editor's removeMap, in groomLabScalpMaps)."""
    if not map_kind(groom, name):
        raise RuntimeError("no map named %r" % name)
    with undo_chunk("remove map"):
        flat = cmds.groomLabScalpMaps(groom, remove=name, **({"deleteFile": True} if delete_file else {})) or []
        changed = [(flat[i], flat[i + 1]) for i in range(0, len(flat) - 1, 2)]
        if cmds.getAttr(groom + ".strandColorSource") == name and cmds.getAttr(groom + ".strandColorMode") == 2:
            cmds.setAttr(groom + ".strandColorMode", 0)
        if cmds.getAttr(groom + ".scalpMapDisplay") == name:
            cmds.setAttr(groom + ".scalpMapDisplay", "", type="string")
    return changed


def rename_map(groom, old, new):
    """Rename a map; the Map nodes, map params and views reading it follow. A texture map's
    file follows too when its name is the map's (as GroomLab names them: <masks>/<asset>/<map>.png,
    or <map>_2.png when that was taken; older scenes' <groom>_<map>.png too), e.g.
    masks/hair/density.png -> masks/hair/thickness.png: moved, or copied when another groom
    reads the same file (it keeps its own), and to a free name when one is in the way. Every
    host's Rename (the editor's renameMap, in groomLabScalpMaps: the graph's readers follow in the
    same step)."""
    with undo_chunk("rename map"):
        cmds.groomLabScalpMaps(groom, rename=(old, new))
        if cmds.getAttr(groom + ".strandColorSource") == old:
            cmds.setAttr(groom + ".strandColorSource", new, type="string")
        if cmds.getAttr(groom + ".scalpMapDisplay") == old:
            cmds.setAttr(groom + ".scalpMapDisplay", new, type="string")
    return new


def convert_map(groom, name, kind, resolution=1024, texture=None):
    """Resample a map into another kind (its name kept, so the graph reads it as before):
    guides <-> vertices <-> texture. To a texture: `resolution` square, at `texture` or the map's
    file or the scene's masks folder (a new file never replaces one that is there). Returns the kind."""
    if kind not in MAP_KINDS:
        raise ValueError("a map's kind is guides, vertices or texture, not %r" % kind)
    if not map_kind(groom, name):
        raise ValueError("no map named %r" % name)
    kwargs = {"convert": name, "kind": kind, "resolution": int(resolution)}
    if kind == "texture":
        _check_png(texture)
        stored = next((m["texture"] for m in maps(groom) if m["name"] == name), "")
        kwargs["texture"] = portable_path(texture) if texture else (stored or _map_texture_default(groom, name))
    cmds.groomLabScalpMaps(groom, **kwargs)  # Map nodes name the map only: they read the new kind as is
    return kind


def map_nodes(groom, name):
    """The Map nodes reading map `name`."""
    return list(groomlab_core.map_readers(json.dumps(get_graph(groom, expand_default=True)), name)["nodes"])


def map_users(groom, name):
    """(node, param) pairs the map `name` feeds: through its Map nodes, and the params naming it (a
    map param's source, HairColor's map, Interpolate's regionMap). The core's (scalp_maps.h
    mapReaders), every host's "Read by"."""
    readers = groomlab_core.map_readers(json.dumps(get_graph(groom, expand_default=True)), name)
    return [tuple(pair) for pair in readers["users"]]


def paint_map(groom, name, value=1.0, strength=0.5, color=None, channels="rgba", **brush_options):
    """Start the Paint brush painting `value` into map `name`, showing the map (on the strands,
    and on the scalp for a scalp map): drag over the guides for a guide layer, over the scalp
    for a vertex or texture map. A color map is painted `color` (r, g, b[, a]; white when None)
    in the channels of `channels` ("rgba" or a subset, e.g. "r" to paint one packed mask)."""
    kind = map_kind(groom, name)
    if not kind:
        raise ValueError("no map named %r" % name)
    _remember_view(groom)
    view_map(groom, name)
    show_map_on_scalp(groom, name if kind != "guides" else None)
    texture = next((m["texture"] for m in maps(groom) if m["name"] == name), "") if kind == "texture" else ""
    if map_channels(groom, name) == 4 and color is None:
        color = (1.0, 1.0, 1.0, 1.0)
    ctx = brush_tool("paint", attribute=name, value=value, strength=strength, paint_kind=kind,
                     paint_texture=texture, paint_color=color, paint_channels=channels, **brush_options)
    _watch_paint_tool()
    return ctx


# What the groom showed before painting showed a map: put back when the Paint brush ends (another
# tool, Q, or another brush), so painting never leaves the strands colored by the map.
_VIEW_ATTRS = (("strandColorMode", None), ("strandColorSource", "string"), ("strandColorMin", None),
               ("strandColorMax", None), ("scalpMapDisplay", "string"))
_painting = {"groom": None, "view": None, "job": None}


def _remember_view(groom):
    if _painting["groom"] == groom and _painting["view"] is not None:
        return  # painting already: keep what showed before the first map
    end_paint_view()
    _painting["groom"] = groom
    _painting["view"] = [(a, t, cmds.getAttr(groom + "." + a)) for a, t in _VIEW_ATTRS]


def _watch_paint_tool():
    if _painting["job"] is None or not cmds.scriptJob(exists=_painting["job"]):
        _painting["job"] = cmds.scriptJob(event=["ToolChanged", _tool_changed])


def _tool_changed():
    from groomlab.brushes import BRUSH_CONTEXT
    if cmds.currentCtx() != BRUSH_CONTEXT:
        end_paint_view(from_job=True)


def _kill_job(job):
    if job is not None and cmds.scriptJob(exists=job):
        cmds.scriptJob(kill=job, force=True)


def end_paint_view(from_job=False):
    """Put back what the groom showed before painting showed a map (on the strands and the
    scalp). Runs by itself when the Paint brush ends: another tool, Q, or another brush."""
    groom, view, job = _painting["groom"], _painting["view"], _painting["job"]
    _painting.update(groom=None, view=None, job=None)
    if from_job:
        # Maya refuses to kill a script job while it runs: after it returns, then.
        cmds.evalDeferred(lambda: _kill_job(job))
    else:
        _kill_job(job)
    if not view or not groom or not cmds.objExists(groom):
        return
    with no_undo():  # a display, not an edit: no undo step of its own
        for attr, kind, value in view:
            if kind == "string":
                cmds.setAttr(groom + "." + attr, value or "", type="string")
            else:
                cmds.setAttr(groom + "." + attr, value)


def view_map(groom, name=None):
    """Color the strands by a map (black to white ramp over 0-1); None goes back to solid."""
    if name is None:
        cmds.setAttr(groom + ".strandColorMode", 0)
        return
    cmds.setAttr(groom + ".strandColorSource", name, type="string")
    cmds.setAttr(groom + ".strandColorMin", 0.0)
    cmds.setAttr(groom + ".strandColorMax", 1.0)
    cmds.setAttr(groom + ".strandColorMode", 2)


MAP_DISPLAY_CHANNELS = ("rgb", "r", "g", "b", "a")


def show_map_on_scalp(groom, name=None, channel=None):
    """Show a map on the scalp in the viewport (a gray map black to white, a color map in color,
    any kind); None hides it. `channel` ("rgb", or "r", "g", "b", "a" as gray) picks what of a
    color map shows; None leaves it as it is."""
    if channel is not None:
        if channel not in MAP_DISPLAY_CHANNELS:
            raise ValueError("channel is one of %s, not %r" % (MAP_DISPLAY_CHANNELS, channel))
        cmds.setAttr(groom + ".scalpMapChannel", channel, type="string")
    cmds.setAttr(groom + ".scalpMapDisplay", name or "", type="string")


def export_map(groom, name, path, resolution=1024):
    """Write map `name` as a PNG at `path` (for an engine: a packed mask, a painted color): a
    texture map's image as it is (a gray one stays gray), a vertex map or guide layer rasterized
    at the scalp's UVs, `resolution` square, with its channels. The groom is left as it was.
    Returns the path."""
    import shutil
    kind = map_kind(groom, name)
    if not kind:
        raise ValueError("no map named %r" % name)
    path = os.path.abspath(path).replace("\\", "/")
    folder = os.path.dirname(path)
    if folder and not os.path.isdir(folder):
        os.makedirs(folder)
    if kind == "texture":
        from groomlab import presets
        source = map_texture(groom, name)
        if not source or not presets.file_exists(source):
            raise ValueError("map %s's image is missing: %s" % (name, source))
        if os.path.normcase(os.path.abspath(source)) != os.path.normcase(path):
            # (A tile set, udim.h: each tile, into the set `path` names: <path stem>.1001.png...)
            for src, dst in groomlab_core.map_image_moves(source, path):
                shutil.copyfile(src, dst)
        return path
    # Rasterized through a conversion to a texture at `path`, then the map put back as it was.
    before = cmds.getAttr(groom + ".scalpMaps")
    guides = cmds.getAttr(groom + ".guideData")
    display = cmds.getAttr(groom + ".scalpMapDisplay")
    if os.path.exists(path):
        os.remove(path)  # the command never writes over a file: this one is asked for
    with no_undo():
        try:
            cmds.groomLabScalpMaps(groom, convert=name, kind="texture", resolution=int(resolution), texture=path)
            written = map_texture(groom, name)
        finally:
            cmds.setAttr(groom + ".scalpMaps", before or "", type="string")
            cmds.setAttr(groom + ".guideData", guides or "", type="string")
            cmds.setAttr(groom + ".scalpMapDisplay", display or "", type="string")
    if written and os.path.normcase(os.path.abspath(written)) != os.path.normcase(path):
        shutil.move(written, path)
    return path


# ---------------------------------------------------------------------------
# A map's image: relink it to another file, read it again after an edit outside Maya, and find
# the files a groom names that went missing (a botched copy, a moved project).


def set_map_texture(groom, name, path, copy=None):
    """Point map `name` at the image `path`. A PNG is linked in place (stored relative to the
    scene, as GroomLab stores its files): painting then edits that file. Any other image (JPEG,
    TGA, PSD, BMP, GIF, HDR...) is copied into the scene's masks folder as a PNG
    (masks/<asset>/<name>.png, a free name when that is taken) and the map reads the copy:
    painting writes PNG, so the source is never painted over. `copy` True copies a PNG too;
    False refuses another format. A guide layer or vertex map becomes a texture map reading the
    image (Map nodes follow by name). The map keeps its channels: a gray map reads the image's
    luminance, a color map its r g b a. One undo step (a copy is removed on undo). Returns the
    stored path."""
    kind = map_kind(groom, name)
    if not kind:
        raise ValueError("no map named %r" % name)
    if not path:
        raise ValueError("give the image's path")
    full = (resolve_path(path, groom) or path).replace("\\", "/")
    if not groomlab_core.file_stamp(full):  # (a UDIM tile set: any of its tiles)
        raise ValueError("no image at %s" % full)
    png = full.lower().endswith(".png")
    if copy is None:
        copy = not png
    if not copy and not png:
        raise ValueError("%s is not a PNG: painting writes PNG, so another format is copied as one (copy=True)" % full)
    # Every host's Relink Image (the editor's relinkMap, in groomLabScalpMaps): texture params
    # reading the old image read the new one in the same step.
    if copy:
        asset_folder("masks")  # made, and the untitled scene's warning given
        return cmds.groomLabScalpMaps(groom, relink=name, texture=_map_texture_default(groom, name), copyFrom=full)
    return cmds.groomLabScalpMaps(groom, relink=name, texture=portable_path(full))


def import_map(groom, path, name=None, color=None, copy=None):
    """A new texture map reading the image `path`, in one step (no blank map made first): named
    `name`, else after the file (its letters, digits and _; a free name when taken). `color` None
    makes a color map when the image has color and a gray one (its luminance) when it is gray; a
    PNG is used in place and any other format copied into the masks folder as a PNG, as
    set_map_texture does (`copy`). One undo step. Returns the map's name."""
    full = (resolve_path(path, groom) or path).replace("\\", "/") if path else ""
    if not full or not groomlab_core.file_stamp(full):  # (a UDIM tile set: any of its tiles)
        raise ValueError("no image at %s" % (full or path))
    if name is None:  # the core's rule, every host's (scalp_maps.h mapNameFromFile)
        name = groomlab_core.map_name_from_file(full, list(list_maps(groom)))
    if color is None:
        color = not _image_is_gray(full)
    with undo_chunk("import map"):
        # Made on the vertices (no file written), then turned into a texture map reading the image.
        add_map(groom, name, kind="vertices", color=True if color else None)
        set_map_texture(groom, name, full, copy=copy)
    return name


def _image_is_gray(path):
    """Whether the image holds no color (every pixel r = g = b; a UDIM tile set: every tile, the
    core's isGrayImage, as the app and Blender ask); unreadable: False."""
    try:
        return groomlab_core.is_gray_image(path)
    except (RuntimeError, ValueError, groomlab_core.TextureError):
        return False

def reload_map(groom, name=None):
    """Read texture map `name`'s image again (None: every texture map's) after it changed outside
    Maya, in an image editor: the groom evaluates again and every file it reads whose stamp
    changed is read anew (the graph's own images too, HairColor's image...); the others stay
    cached. Not an undo step. Returns the images (absolute) of the maps reloaded."""
    if name is not None and map_kind(groom, name) != "texture":
        raise ValueError("%s is not a texture map" % name)
    files = [map_texture(groom, m["name"]) for m in maps(groom)
             if m["kind"] == "texture" and (name is None or m["name"] == name)]
    with no_undo():
        _bump(groom)
    return files


def missing_map_files(groom):
    """Texture maps whose image is not there: {map: path as stored}."""
    from groomlab import presets
    return {m["name"]: m["texture"] for m in maps(groom)
            if m["kind"] == "texture" and m["texture"]
            and not presets.file_exists(resolve_path(m["texture"], groom))}


MISSING_KINDS = ("maps", "params", "attributes")


def missing_files(groom):
    """The files a groom names that are not there: {"maps": {map: path} (texture maps), "params":
    {"node.param": path} (graph params naming files: texture map params, HairColor's image, FeatherColor's,
    ImportCurves' Alembic file; compound/inner.param inside compounds), "attributes": {attr: path}
    (the guide cache, the render's groom file)}, paths as stored. All empty when nothing is missing."""
    from groomlab import presets
    out = {"maps": missing_map_files(groom), "params": presets.missing_files(groom, get_graph(groom), network=True),
           "attributes": {}}
    for attr in ("guideCache", "groomFile"):
        value = cmds.getAttr(groom + "." + attr) or ""
        if value and not presets.file_exists(resolve_path(value, groom)):
            out["attributes"][attr] = value
    return out


def find_missing_files(grooms, folder, recursive=True):
    """Relink the files grooms name that are missing (missing_files: texture maps, graph params,
    the guide cache and groom file) to files of the same name in `folder` (and its subfolders
    with `recursive`): names compare as the file system does (case-insensitive on Windows); when
    several files share a name, the one whose folders end like the stored path's wins, and when
    none does the file is reported as ambiguous, not guessed. A texture map relinks as
    set_map_texture does (a PNG in place). `grooms`: a groom or a list. One undo step. Returns
    {"relinked": {groom: {kind: {key: new path}}}, "still_missing": {groom: {kind: {key: path}}},
    "ambiguous": {groom: {kind: {key: [candidates]}}}} with kind "maps", "params" or
    "attributes" (missing_files' keys); a groom with nothing to report is left out."""
    from groomlab import presets
    if isinstance(grooms, str):
        grooms = [grooms]
    if not folder or not os.path.isdir(folder):
        raise ValueError("no folder %s" % folder)
    index = groomlab_core.index_folder(os.path.abspath(folder), recursive)  # (every host's matching: asset_files.h)
    report = {"relinked": {}, "still_missing": {}, "ambiguous": {}}

    def note(section, groom, kind, key, value):
        report[section].setdefault(groom, {}).setdefault(kind, {})[key] = value

    with undo_chunk("find missing files"):
        for groom in grooms:
            missing = missing_files(groom)

            def look(kind, key, stored):
                found, candidates = groomlab_core.match_missing_file(index, stored)
                if found:
                    return found
                if candidates:
                    note("ambiguous", groom, kind, key, candidates)
                else:
                    note("still_missing", groom, kind, key, stored)
                return None

            for name, stored in sorted(missing["maps"].items()):
                found = look("maps", name, stored)
                if found:
                    note("relinked", groom, "maps", name, set_map_texture(groom, name, found))
            if missing["params"]:
                graph = get_graph(groom)
                relinked = {}
                for label, stored in presets.graph_files(graph):
                    if missing["params"].get(label) != stored:
                        continue
                    found = look("params", label, stored)
                    if found:
                        relinked[label] = portable_path(found)
                        note("relinked", groom, "params", label, portable_path(found))
                if relinked:
                    set_graph(groom, presets.with_graph_files(graph, relinked))
            for attr, stored in sorted(missing["attributes"].items()):
                found = look("attributes", attr, stored)
                if found:
                    cmds.setAttr(groom + "." + attr, portable_path(found), type="string")
                    note("relinked", groom, "attributes", attr, portable_path(found))
    return report


def _map_node_for(model, groom, name, near=None):
    """The Map node reading map `name` in `model`, made if there is none (one per map: its
    outputs give the luminance and each channel). The node holds the name alone: where the map
    lives (and a texture's file) the groom tells the graph."""
    from groomlab import graph as gm
    if not map_kind(groom, name):
        raise ValueError("no map named %r" % name)
    for other in model.order:
        if model.node_type(other) == "Map" and model.param(other, "name") == name:
            return other
    node = model.add_node("Map", name=model.unique_name(name), params={"name": name})
    anchor = near if near in model.order else (model.order[0] if model.order else None)
    if anchor:
        x, y = model.position(anchor)
        model.set_position(node, x - 2 * gm.LAYOUT_DX, y)
    return node


def feed_map_node(groom, map_node, target, param, model=None, output=None):
    """Feed the Map node `map_node` into `target`'s mappable `param` (exposed as an input and
    connected; the param's own value multiplies the map), from its output `output` ("luminance",
    the default, or one channel "r", "g", "b", "a"). With `model` (being edited) the change is
    not saved. Returns "target.param"."""
    from groomlab import graph as gm
    own = model is None
    model = model or gm.GraphModel.from_groom(groom)
    if map_node not in model.order or model.node_type(map_node) != "Map":
        raise ValueError("%s is not a Map node" % map_node)
    if target not in model.order:
        raise ValueError("graph has no node named %r" % target)
    if param not in model.param_inputs(target):
        model.expose_param(target, param)
    model.connect(map_node, target, param, output=output)
    if own:
        model.save(groom)
    return "%s.%s" % (target, param)


MAP_CHANNELS = ("luminance", "r", "g", "b", "a", "rgb")


@undoable("use map")
def use_map(groom, node, param, name, kind="guides", resolution=1024, channel="luminance", faces=None):
    """Drive a mappable param with map `name` (made, filled 1, if there is none: `kind`,
    `resolution` for a texture) through a Map node (one per map, reused): the param is exposed
    as an input the node feeds, and its own value multiplies the map (white = the value, black
    = 0). `channel` picks the Map node's output the param reads: one channel of a color map
    ("r", "g", "b", "a": masks packed in one map, so one Map node feeds several params) or its
    luminance; "rgb", its color, for a color param (HairColor's color, dye or tipColor: the map
    colors each strand by its root, times the param's color). A map made here with `faces` (scalp
    polygon indices: selected_scalp_faces) is 1 on those faces and 0 elsewhere: hair grows on a face
    selection only (a Density map). Returns the Map node's name."""
    from groomlab import graph as gm
    if channel not in MAP_CHANNELS:
        raise ValueError("channel is one of %s, not %r" % (MAP_CHANNELS, channel))
    if name not in list_maps(groom):
        if channel == "rgb" and kind == "guides":
            raise ValueError("a guide layer is gray: a color map lives on the scalp's vertices or in a texture")
        add_map(groom, name, 1.0, kind=kind, resolution=resolution, color=channel == "rgb", faces=faces)
    model = gm.GraphModel.from_groom(groom)
    if node not in model.order:
        raise ValueError("graph has no node named %r" % node)
    map_node = _map_node_for(model, groom, name, near=node)
    feed_map_node(groom, map_node, node, param, model=model, output=channel)
    model.save(groom)
    return map_node


def param_map(groom, node, param):
    """The map node.param reads: (map, channel) through a Map node wired to it (channel the
    node's output: "luminance", "r", "g", "b" or "a"), or a map param naming a guide layer or a
    vertex map (the maps the Width and Density brushes used to make); None when it reads none."""
    graph = get_graph(groom, expand_default=True)
    names = set(list_maps(groom))
    for c in graph.get("connections", []):
        if c.get("to") == node and c.get("input") == param:
            source = next((n for n in graph["nodes"] if n["name"] == c.get("from")), None)
            if source and source.get("type") == "Map":
                name = (source.get("params") or {}).get("name", "mask")
                if name in names:
                    return name, c.get("output") or "luminance"
            return None
    target = next((n for n in graph["nodes"] if n["name"] == node), None)
    value = (target.get("params") or {}).get(param) if target else None
    if isinstance(value, str):  # a param naming a map (Interpolate's regionMap)
        return (value, "luminance") if value in names else None
    if isinstance(value, dict):
        name = value.get("guide") or value.get("colorSet")
        if name in names:
            return name, "luminance"
        if value.get("texture"):  # the app's and Blender's painted params read a texture map by its file
            full = os.path.normcase(os.path.normpath(resolve_path(value["texture"], groom) or value["texture"]))
            for m in maps(groom):
                if m["kind"] == "texture" and os.path.normcase(os.path.normpath(map_texture(groom, m["name"]))) == full:
                    component = value.get("component") or "luminance"
                    gray = map_channels(groom, m["name"]) != 4
                    return m["name"], "luminance" if gray or component not in ("r", "g", "b", "a") else component
    return None


def paint_param(groom, node, param, kind="guides", name=None, resolution=1024, paint=True, channel="luminance",
                faces=None):
    """Paint a mappable param (+ New Map > for a setting in the Paint tab, and the Graph Editor's
    right-click a param > Paint): the map it reads already when it reads one, else the map `name`
    (default "<node>_<param>"; made white, so the look stays as it is) on the guides, the scalp's
    vertices or a texture (`kind`), read by the param as every host links it (the core's
    wireParamMap: a Map node of the map wired into the param, whose own value the map's 0-1
    multiplies, so its slider still scales it). With `paint` the
    Paint brush starts on it toward 1 (the param's own value); Ctrl paints toward 0 (on a density
    map: no hair). With `channel` ("r", "g", "b" or "a") the param reads that output of the map's
    Map node (one Map node feeds several params), a channel of a color map `name` (made if
    missing: packed masks), and the brush paints that channel only. A map made here with `faces` (scalp polygon indices: selected_scalp_faces)
    is white on those faces only, black elsewhere (a map the param already reads is left as it
    is). The map, its link and what painting shows are one undo step. Returns the map's name."""
    # Every host's Paint on a setting (the editor's paintParam, in groomLabScalpMaps): what drives the
    # param must be a painted map, or nothing (an expression, a channel or an image no map of the
    # groom holds is refused); the map made and its link are one step.
    kwargs = {"paintParam": (node, param), "kind": kind, "resolution": int(resolution)}
    if name:
        kwargs["mapName"] = name
    if channel and channel != "luminance":
        kwargs["channel"] = channel
    if faces:
        kwargs["faces"] = _faces_flag(faces)
    if kind == "texture":
        asset_folder("masks")  # made, and the untitled scene's warning given
    with undo_chunk("paint %s.%s" % (node, param)):
        name, kind, channel = cmds.groomLabScalpMaps(groom, **kwargs)
        if paint:
            if channel:
                paint_map(groom, name, value=1.0, color=(1.0, 1.0, 1.0, 1.0), channels=channel)
            else:
                paint_map(groom, name, value=1.0)
    return name


def setting_targets(groom):
    """What + New Map > for a setting offers on this groom: the common settings (groomlab_core's
    paintable_settings: Density, Width, Length, Clump strength, Curl, Noise amplitude, Flyaways,
    Guide weight, Interpolation regions),
    each [{"id", "label", "node", "param", "hint", "kind"}] with node "" when the graph has no node of
    its type (kind: where its new map lives unless picked otherwise, "guides" for the regions); then
    every mappable param of the graph (Other) as {"id": "node.param", ...}. Returns
    (settings, others)."""
    # The core's list, every host's (stroke.h settingTargetsJson).
    targets = json.loads(groomlab_core.setting_targets_json(json.dumps(get_graph(groom, expand_default=True))))
    return targets["settings"], targets["others"]


# ---------------------------------------------------------------------------
# Maps (use as parameter values; see set_param)


def texture_map(path, component="r", min=0.0, max=1.0):
    """A parameter read from an image at each root's UV, remapped from [0, 1] to [min, max].

    component: "r", "g", "b", "a" or "luminance". Formats: PNG, JPEG, TGA, BMP,
    PSD, GIF, HDR, PIC, PNM. The scalp needs UVs.
    """
    return {"texture": portable_path(path), "component": component, "min": min, "max": max}


def color_set_map(name, component="r", min=0.0, max=1.0):
    """A parameter read from a color set on the scalp (Mesh Display > Paint Vertex Color)."""
    return {"colorSet": name, "component": component, "min": min, "max": max}


def attribute_map(name, min=0.0, max=1.0):
    """A parameter read from a per-strand channel written upstream (e.g. by Attribute)."""
    return {"attribute": name, "min": min, "max": max}


def guide_map(name, min=0.0, max=1.0):
    """A parameter read from a painted map on the guides (see add_map), remapped to [min, max].

    After Interpolate it is the strand channel of that name; before strands
    exist (Scatter density) it blends the nearest guides' values at each root,
    which needs the node's guides input connected (use_map does that).
    """
    return {"guide": name, "min": min, "max": max}


def expression_map(expression, min=0.0, max=1.0):
    """A per-strand expression for a mappable param, remapped from [0, 1] to [min, max].

    Variables: u v (root UV), x y z (root position), nx ny nz (root normal),
    rand (stable per strand), index, and any strand channel by name (e.g.
    clumpId). The language is SeExpr's (as XGen's): smoothstep(x, a, b), noise(p),
    fbm(p), fit, curve, rand(min, max), c ? a : b, x->f(a) and the rest.
    """
    return {"expression": expression, "min": min, "max": max}


# ---------------------------------------------------------------------------
# Painting maps live in Maya (scalp color sets + Maya's Paint Vertex Color tool)


@undoable("paint scalp map")
def paint_scalp_map(groom, name="mask", after="interpolate", paint=True):
    """A painted mask you can reuse as a strand attribute: a vertex map `name` on the scalp
    (made black if new) written into the strand channel `name` by an Attribute node (mode map)
    after `after`, so any param reads it with attribute_map(name). With `paint` the Paint brush
    starts on it (Groom Tools > Paint; white = 1). Returns the Attribute node's name."""
    from groomlab import graph as gm
    if name not in list_maps(groom):
        add_map(groom, name, 0.0, kind="vertices")
    elif map_kind(groom, name) == "guides":
        raise ValueError("%s is a guide layer; Interpolate already writes it into the strands" % name)
    model = gm.GraphModel.from_groom(groom)
    existing = [n for n in model.order if model.node_type(n) == "Attribute"
                and isinstance(model.param(n, "value"), dict) and model.param(n, "name") == name]
    if existing:
        node = existing[0]
    else:
        source = color_set_map(name) if map_kind(groom, name) == "vertices" else texture_map(map_texture(groom, name))
        node = model.add_node("Attribute", name=model.unique_name(name + "Map"),
                              params={"name": name, "value": source})
        model.insert_after(after, node)
        model.save(groom)
    if paint:
        paint_map(groom, name)
    return node
