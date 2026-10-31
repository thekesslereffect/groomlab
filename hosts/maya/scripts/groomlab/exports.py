# SPDX-License-Identifier: Apache-2.0
"""Strand exports: Alembic and USD curves, the scales mesh and its baked maps, and the strands the
viewport shows."""

import os
import re

from maya import cmds, mel

from groomlab.params import get_graph
from groomlab.paths import asset_name
from groomlab.cards import _bake_folder


def viewport_points(groom):
    """The points of the strands the viewport shows now (this frame, at the preview density), as
    [(x, y, z)]: what an export or a render at that density should match."""
    xyz = cmds.groomLabGraphStats(groom, points=True) or []
    return [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]


def export_alembic(groom, path, start=None, end=None, step=1.0, density=1.0, scales="mesh", render_overrides=False):
    """Write the groom's strands to an Alembic file as animated curves.

    Frames default to the current frame. Density 1.0 exports every strand
    (render density); previewDensity is ignored. Returns frames written.
    render_overrides: the groom's render overrides too, as a render has them (renderDensity times
    `density`, renderWidthScale, renderLengthScale); off, the strands as groomed.
    scales: "mesh" (default) writes the groom's scales beside the curves as an animated mesh,
    bent onto the skin as rendered (<name>/scales), with their data as arbitrary params (scaleColor
    per vertex; scaleRootColor, scaleId, scaleUV, scaleVariant per face); "none" leaves them out.
    """
    kwargs = {"file": path, "step": step, "density": density, "scales": scales, "renderOverrides": bool(render_overrides)}
    if start is not None:
        kwargs["startFrame"] = start
    if end is not None:
        kwargs["endFrame"] = end
    return cmds.groomLabExportAlembic(groom, **kwargs)


def export_usd(groom, path, start=None, end=None, step=1.0, density=1.0, scales="mesh", render_overrides=False):
    """Write the groom's strands to a USD layer (.usda) as BasisCurves.

    Same frames, density and render_overrides rules as export_alembic. Returns frames written.
    scales: "mesh" (default): the scales' bent mesh (exact, with primvars scaleColor per vertex
    and scaleRootColor, scaleId, scaleUV, scaleVariant per face); "instancer": a PointInstancer
    of their prototypes, each placed by its scale's tilted frame, with scaleRootColor, scaleId,
    scaleUV and scaleVariant per instance (light and editable in an engine: Unreal imports it as
    instanced static meshes; the plates are not bent onto the skin); "both"; "none". The layer's
    layout is versioned: web/content/docs/usd-layout.mdx.
    """
    kwargs = {"file": path, "step": step, "density": density, "scales": scales, "renderOverrides": bool(render_overrides)}
    if start is not None:
        kwargs["startFrame"] = start
    if end is not None:
        kwargs["endFrame"] = end
    return cmds.groomLabExportUsd(groom, **kwargs)


def export_vrscene(groom, path, start=None, end=None, step=1.0, density=1.0, render_overrides=False, min_pixel_width=0.0):
    """Write the groom as a V-Ray scene (.vrscene): the strands (GeomMayaHair, the groom's object
    space, centimeters), its hair shader (VRayHairNextMtl's BRDFHair4, each strand's color through
    a TexHairSampler, the highlight from the groom's look) and a Node with the groom's world matrix.
    Load it in V-Ray for Maya with a VRayScene node, or render it in V-Ray Standalone (#include it in
    a scene with a camera and lights).

    Same frames, density and render_overrides rules as export_alembic; more than one frame writes
    the moving values as interpolate() keys, which V-Ray renders and blurs (to blur frame f, write
    f to f + 1). min_pixel_width: V-Ray's (strands thinner than that many pixels drawn that wide and
    more transparent). Returns frames written."""
    kwargs = {"file": path, "step": step, "density": density, "renderOverrides": bool(render_overrides),
              "minPixelWidth": float(min_pixel_width)}
    if start is not None:
        kwargs["startFrame"] = start
    if end is not None:
        kwargs["endFrame"] = end
    return cmds.groomLabExportVrscene(groom, **kwargs)


def has_scales(groom):
    """Whether the groom's graph has a Scales node."""
    return any(n.get("type") == "Scales" for n in get_graph(groom).get("nodes", []))


def scale_mesh(groom, name=None):
    """The groom's scales at the current frame as a Maya mesh (undoable), bent onto the skin as
    they render, under a new transform with the groom's matrix: UV sets map1 (the plates' uvs),
    scaleUV (the skin's uv at each scale's root) and scaleData (scaleId, scaleVariant), color sets
    scaleColor (per vertex) and scaleRootColor (per scale), locked normals. Returns the transform."""
    kwargs = {"name": name} if name else {}
    return cmds.groomLabScaleMesh(groom, **kwargs)


def export_scales(groom, path, unity=False):
    """The groom's scales at the current frame to `path` as a mesh: .fbx (Maya's FBX export, as
    export_rachis writes it: for Unreal's static or skeletal mesh import, its uv and color sets
    carrying the per-scale data; skin it with the body in Maya first for a skeletal mesh) or .abc
    (export_alembic over a range is the animated one). With `unity` the FBX is in meters, Y up.
    Returns the path."""
    ext = os.path.splitext(path)[1].lower()
    if ext not in (".fbx", ".abc"):
        raise ValueError("the scales go to .fbx or .abc, not %r" % ext)
    directory = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(directory):
        os.makedirs(directory)
    path = path.replace("\\", "/")
    selection = cmds.ls(selection=True)
    mesh = scale_mesh(groom)
    try:
        _write_mesh(mesh, path, unity=unity)
    finally:
        cmds.delete(mesh)
        cmds.select(selection, replace=True) if selection else cmds.select(clear=True)
    return path


MESH_FORMATS = (".obj", ".fbx", ".abc")


def export_mesh(groom, path, node=None):
    """A ToMesh node's mesh at the current frame to `path`: .obj, .fbx (Maya's FBX export) or .abc
    (this frame; export_alembic of a mesh on to_mesh_shape over a range is the animated one). The
    node's settings make it (make, style, sides, width, density, cap): `node`, else the first ToMesh
    node making the output, else the output as to_mesh's defaults (auto) make it. Returns the path."""
    from groomlab import graph as gm, utils
    ext = os.path.splitext(path)[1].lower()
    if ext not in MESH_FORMATS:
        raise ValueError("the mesh goes to %s, not %r" % (", ".join(MESH_FORMATS), ext))
    model = gm.GraphModel.from_groom(groom)
    nodes = [n for n in model.feeding_output() if model.node_type(n) == "ToMesh"]
    if node is None:
        node = nodes[0] if nodes else None
    elif model.node_type(node) != "ToMesh":
        raise ValueError("%r is not a ToMesh node" % node)
    kwargs = {}
    if node is not None:
        p = lambda key: model.param(node, key)  # noqa: E731
        kwargs = {"node": node, "make": p("make"), "style": p("style"), "sides": int(p("sides")),
                  "width_scale": float(p("widthScale")), "density": float(p("density")),
                  "strand_uvs": bool(p("strandUVs"))}
        cap = int(p("maxStrands"))
        kwargs["max_strands"] = cap if cap > 0 else 100000000
    directory = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(directory):
        os.makedirs(directory)
    path = path.replace("\\", "/")
    selection = cmds.ls(selection=True)
    made = utils.to_mesh(groom, **kwargs)["mesh"]
    try:
        _write_mesh(made, path)
    finally:
        cmds.delete(made)
        cmds.select(selection, replace=True) if selection else cmds.select(clear=True)
    return path


def _write_mesh(mesh, path, unity=False):
    """A Maya mesh (its transform) to `path`: .fbx, .abc (this frame) or .obj."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".obj":
        if not cmds.pluginInfo("objExport", query=True, loaded=True):
            cmds.loadPlugin("objExport", quiet=True)
        cmds.select(mesh, replace=True)
        cmds.file(path, force=True, exportSelected=True, type="OBJexport", preserveReferences=False,
                  options="groups=1;ptgroups=1;materials=0;smoothing=1;normals=1")
        return
    if ext == ".fbx":
        if not cmds.pluginInfo("fbxmaya", query=True, loaded=True):
            cmds.loadPlugin("fbxmaya", quiet=True)
        cmds.select(mesh, replace=True)
        commands = ["FBXResetExport", "FBXExportInputConnections -v false", "FBXExportSmoothingGroups -v true",
                    "FBXExportTriangulate -v false", "FBXExportEmbeddedTextures -v false"]
        if unity:
            commands += ["FBXExportUpAxis y", "FBXExportConvertUnitString m", "FBXExportScaleFactor 1.0"]
        try:
            for command in commands:
                mel.eval(command)
            mel.eval('FBXExport -f "%s" -s' % path)
        finally:
            mel.eval("FBXResetExport")
    else:
        if not cmds.pluginInfo("AbcExport", query=True, loaded=True):
            cmds.loadPlugin("AbcExport", quiet=True)
        frame = cmds.currentTime(query=True)
        cmds.AbcExport(j='-frameRange %s %s -uvWrite -writeUVSets -writeColorSets -worldSpace -root "%s" -file "%s"'
                       % (frame, frame, cmds.ls(mesh, long=True)[0], path))


SCALE_MAPS = ("ScaleNormal", "ScaleHeight", "ScaleColor")


def bake_scale_maps(groom, directory=None, name=None, resolution=2048, max_height=0.0, padding=8):
    """Bake the groom's scales at the current frame onto its scalp's uvs, for scales too small or
    far to model: <name>_ScaleNormal.png (tangent space, OpenGL's +Y: flip green for Unreal),
    <name>_ScaleHeight.png (16-bit: black the skin, white `max_height`; 0 measures the scales'
    highest point) and <name>_ScaleColor.png (sRGB, alpha the scales' coverage), one of each per
    UDIM tile (<name>_<Map>.1001.png...) when the scalp's uvs leave the 0-1 square. `directory`
    defaults to the groom's bake folder (where bake_cards writes), `name` to its asset name.
    Returns {"ScaleNormal": [paths], "ScaleHeight": [...], "ScaleColor": [...], "max_height": h,
    "pattern": {map: path or <UDIM> pattern}} (max_height: the displacement scale for the height)."""
    directory = (directory or _bake_folder(groom)).replace("\\", "/")
    name = name or asset_name(groom)
    result = cmds.groomLabBakeScaleMaps(groom, directory=directory, name=name, resolution=int(resolution),
                                        maxHeight=float(max_height), padding=int(padding))
    files, height = result[:-1], float(result[-1])
    out = {m: [f for f in files if os.path.basename(f).startswith("%s_%s." % (name, m))] for m in SCALE_MAPS}
    udim = any(re.search(r"\.\d{4}\.png$", f) for f in files)
    out["max_height"] = height
    out["pattern"] = {m: "%s/%s_%s%s.png" % (directory, name, m, ".<UDIM>" if udim else "") for m in SCALE_MAPS}
    return out
