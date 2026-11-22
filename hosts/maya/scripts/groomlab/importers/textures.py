# SPDX-License-Identifier: Apache-2.0
"""A foreign groom's textures as GroomLab maps: found once, converted, copied into the scene's
masks folder (groomlab/<scene>/masks/<asset>/<map>.png, as every GroomLab texture map), linked.

    name = import_map_source(groom, source, report)   # a MapSource (foreign.py) -> a map name

An image GroomLab reads (PNG, JPEG, TGA, BMP, PSD, GIF, HDR, PGM/PPM) is copied as a PNG by
groomlab.import_map(copy=True); another format Maya reads (TIFF, EXR, IFF...) is first converted
to PNG with Maya's MImage. A Ptex file is painted at the scalp's UVs into a PNG (groomlab_core's
ptex_to_png: polygon i is Ptex face i), or, on a scalp without UVs, becomes a vertex map (the
texels at each vertex's corners). The copy never points back at the source: the groom keeps
working when the source folder moves or goes. A file that cannot be found is reported with every
place it was looked for (report.missing) and the import goes on without it.
"""

import json
import os
import re
import shutil
import tempfile

from maya import cmds

# What groomlab_core's texture reader (stb) reads; anything else goes through Maya's MImage.
_READABLE = (".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pgm", ".ppm", ".pnm")


def find_source(source):
    """(the file `source` names, or None; places tried; variables set nowhere)."""
    resolver = source.resolver
    if resolver is None:
        from groomlab.importers.paths import PathResolver
        resolver = PathResolver()
    if source.is_dir:
        folder, tried, unset = resolver.find(source.path, is_dir=True)
        if not folder:
            return None, tried, unset
        for name in source.file_names:
            path = os.path.join(folder, name).replace("\\", "/")
            tried.append(path)
            if os.path.isfile(path):
                return path, tried, unset
        ext = os.path.splitext(source.file_names[0])[1].lower() if source.file_names else ".ptx"
        found = sorted(f for f in os.listdir(folder) if f.lower().endswith(ext))
        if len(found) == 1:  # one map file in the folder: the scalp under another name
            return os.path.join(folder, found[0]).replace("\\", "/"), tried, unset
        return None, tried, unset
    return resolver.find(source.path)


def free_map_name(groom, wanted):
    """A map name from `wanted` (letters, digits, _) the groom does not use yet."""
    import groomlab
    import groomlab_core
    base = groomlab_core.sanitize_map_name(wanted or "map")
    taken = set(groomlab.list_maps(groom))
    name, n = base, 2
    while name in taken:
        name, n = "%s_%d" % (base, n), n + 1
    return name


def scalp_topology(scalp):
    """{"counts", "vertices", "vertex_count", "uv_ids", "us", "vs"} of a mesh (uv_ids empty when
    some polygon has no UVs), the current UV set."""
    import maya.api.OpenMaya as om
    sel = om.MSelectionList()
    sel.add(scalp)
    mesh = om.MFnMesh(sel.getDagPath(0))
    counts, vertices = mesh.getVertices()
    out = {"counts": list(counts), "vertices": list(vertices), "vertex_count": mesh.numVertices,
           "uv_ids": [], "us": [], "vs": []}
    try:
        uv_counts, uv_ids = mesh.getAssignedUVs()
        if list(uv_counts) == list(counts) and mesh.numUVs() > 0:
            us, vs = mesh.getUVs()
            out.update(uv_ids=list(uv_ids), us=list(us), vs=list(vs))
    except RuntimeError:
        pass
    return out


def _to_png(path, folder):
    """`path` as a PNG GroomLab reads: itself when stb reads it, else Maya's MImage's PNG of it in `folder`."""
    if os.path.splitext(path)[1].lower() in _READABLE:
        return path
    import maya.OpenMaya as om1
    image = om1.MImage()
    image.readFromFile(path)
    out = os.path.join(folder, os.path.splitext(os.path.basename(path))[0] + ".png").replace("\\", "/")
    image.writeToFile(out, "png")
    return out


def _gray_image(path):
    """Whether an image holds no color (r = g = b everywhere: every host's, texture.h isGrayImage);
    None when unreadable."""
    import groomlab_core
    try:
        return bool(groomlab_core.is_gray_image(path))
    except Exception:
        return None


def import_map_source(groom, source, report, resolution=1024):
    """The GroomLab map for `source` (a file or Ptex MapSource), made on `groom` from a copy of
    the file: its name, or None when the file is missing or unreadable (reported)."""
    import groomlab
    import groomlab_core
    what = source.source or source.name or source.path
    path, tried, unset = find_source(source)
    if not path:
        report.missing(what, source.path, tried, unset)
        return None
    name = free_map_name(groom, source.name or os.path.splitext(os.path.basename(path))[0])
    work = tempfile.mkdtemp(prefix="groomlab_import_")
    try:
        if source.kind == "ptex":
            info = groomlab_core.ptex_info(path)
            color = source.color if source.color is not None else (info["channels"] >= 3 and source.channel == "rgb")
            topo = scalp_topology(groomlab.scalp_mesh(groom))
            if topo["uv_ids"]:
                png = os.path.join(work, name + ".png").replace("\\", "/")
                result = groomlab_core.ptex_to_png(path, topo["counts"], topo["uv_ids"], topo["us"], topo["vs"], png,
                                                   resolution, resolution, 4 if color else 1)
                groomlab.import_map(groom, png, name=name, color=bool(color), copy=True)
                if result["covered"] < 0.05:
                    report.approximated(what, "the scalp's UVs cover %.0f%% of the 0-1 tile: the map may be "
                                              "mostly padding" % (100 * result["covered"]))
            else:
                channels = 4 if color else 1
                values = groomlab_core.ptex_vertex_values(path, topo["counts"], topo["vertices"], topo["vertex_count"],
                                                          channels)
                groomlab.add_map(groom, name, kind="vertices", color=True if color else None)
                maps = json.loads(cmds.getAttr(groom + ".scalpMaps") or "{}")
                maps[name]["values"] = [round(v, 4) for v in values]
                cmds.setAttr(groom + ".scalpMaps", json.dumps(maps, separators=(",", ":")), type="string")
                report.approximated(what, "the scalp has no UVs: the Ptex map became a vertex map (one value per "
                                          "vertex)")
        else:
            readable = _to_png(path, work)
            gray = _gray_image(readable)
            color = source.color if source.color is not None else (source.channel == "rgb" or gray is False)
            groomlab.import_map(groom, readable, name=name, color=bool(color), copy=True)
        copy = groomlab.map_texture(groom, name) or "(vertex map on the groom)"
        report.file(path, copy, name)
        return name
    except Exception as e:  # an unreadable file: the rest of the import goes on
        report.missing(what, "%s (unreadable: %s)" % (path, e), [path], [])
        if name in groomlab.list_maps(groom):
            groomlab.remove_map(groom, name)
        return None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def bake_texture_node(node, scalp, folder, resolution=1024):
    """A texture network (a Maya texture node feeding a foreign param: a ramp, a layered texture)
    baked at the scalp's UVs into a PNG in `folder` with convertSolidTx; its path, or None."""
    out = os.path.join(folder, re.sub(r"[^A-Za-z0-9_]", "_", node) + ".png").replace("\\", "/")
    try:
        cmds.convertSolidTx(node, scalp, fileImageName=out, resolutionX=resolution, resolutionY=resolution,
                            fileFormat="png", antiAlias=False, force=True, backgroundMode="extend")
    except RuntimeError:
        return None
    return out if os.path.isfile(out) else None
