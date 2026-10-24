# SPDX-License-Identifier: Apache-2.0
"""Game engines: Unreal (groom Alembic, cards), Unity (hair package strands, cards, feathers) and
glTF cards."""

import json
import math
import os

from maya import cmds, mel

from groomlab.paths import asset_folder, asset_name
from groomlab.shading import hair_look
from groomlab.cards import (
    CARD_MAPS, CARD_NORMALS_NOTE, CARD_TINT_NOTE, FEATHER_CARD_NOTE, SCALP_MAPS, bake_cards, bake_scalp, card_material,
    card_mesh, card_stats, cards_preset, create_card_mesh, scalp_cap, _card_maps, _cards_nodes, _existing_scalp_cap,
    _file_texture, _pointed_textures)
from groomlab.exports import export_scales, has_scales
from groomlab.feathers import export_rachis, rachis_width


# Lighter feather grooms for heavy shots (Meerkat's swap): the share of barbs kept.
UNREAL_LODS = {"high": 1.0, "mid": 0.5, "low": 0.25}


def export_unreal_groom(groom, path, start=None, end=None, step=1.0, density=1.0, groups="auto", shafts=None,
                        feather_guides=False, lod=None, barb_density=None, barb_width_scale=None, iridescence_as_ao=False,
                        scales="auto"):
    """Alembic for Unreal Engine's groom importer: strands and primary guides with groom_* attributes.

    One frame (the default: the current one) is a static groom; a range makes
    a groom cache. The points are written in Unreal's space (Z up, left-handed, centimeters) from
    the scene's up axis, as Unreal's FBX importer turns this scene's meshes: import the .abc in
    Unreal (Groom) with the default conversion settings (Rotation 0, Scale 1) and it lines up with
    the head's FBX; bind it to the skeletal mesh with a Groom Binding.

    groups: "auto" (default) writes each groom group as a curves object of its own, with its
      groom_group_id and groom_group_name: the hair is group 0 (named after the groom), each
      feather design its own (add_feathers numbers and names them: the Propagate's groupId and
      groupName), so in Unreal each gets its own material, LODs and physics. A channel name
      groups by that channel instead; None writes one object, groom_group_id from the groupId
      channel. A groom of one group writes the same file either way.
    shafts: the feathers' shafts as strands. None (default): left out when the feathers have a
      rachis mesh (a shaftWidth; create_rachis_mesh), which carries them instead; True / False.
    feather_guides: feather groups get their shafts as simulation guides (default: none, the
      barbs ride the skinned rachis and Unreal makes guides of its own if asked); the hair's
      group always gets the primary guides.
    lod: "high", "mid" or "low": a lighter groom for heavy shots (Weta swapped the Meerkat
      eagle to one): the barbs thinned to 100%, 50% or 25% (by ID, stable), each kept one wider
      by 1/sqrt(share) so the vanes keep their look. barb_density / barb_width_scale set them
      directly. Hair keeps `density`.
    iridescence_as_ao: a FeatherColor's iridescence (always written as groom_iridescence) goes in
      groom_ao too, where Unreal's Hair Attributes node hands it to a material (the Unreal hair
      material recipe, docs/UNREAL_HAIR_MATERIAL.md).
    scales: a groom with scales (has_scales) also writes them beside the .abc as
      <name>_scales.fbx (export_scales: a static mesh for Unreal, or a skeletal one once skinned),
      unless False. Unreal's groom importer takes curves only.
    """
    if scales == "auto":
        scales = has_scales(groom)
    if scales:
        export_scales(groom, os.path.splitext(path)[0] + "_scales.fbx")
    frame = cmds.currentTime(query=True)
    start = frame if start is None else start
    end = start if end is None else end
    kwargs = {}
    if groups:
        kwargs["groups"] = groups
    if shafts is not None:
        kwargs["shafts"] = bool(shafts)
    if feather_guides:
        kwargs["featherGuides"] = True
    if iridescence_as_ao:
        kwargs["iridescenceAo"] = True
    barb_density, barb_width_scale = _barb_lod(lod, barb_density, barb_width_scale)
    if barb_density is not None:
        kwargs["barbDensity"] = barb_density
    if barb_width_scale is not None:
        kwargs["barbWidthScale"] = float(barb_width_scale)
    return cmds.groomLabExportAlembic(groom, file=path, startFrame=start, endFrame=end, step=step, density=density,
                                      unreal=True, **kwargs)


def export_cards_unreal(groom, directory=None, name=None, resolution=None, occlusion=1.0, scalp=True, lod=None):
    """Game cards for Unreal: the card mesh as FBX and its maps (TGA, Unreal's hair card
    layout, see bake_cards) into `directory` (default: the scene's exports folder), plus
    <name>_cards.json saying which map is which. `name` defaults to the groom's asset_name,
    with _<lod> when `lod` names the level of detail ("hero", "mid", "low"; see cards_preset),
    so a groom's LODs exported to one folder keep apart. In Unreal, import the FBX and set the
    textures on the groom's Cards LOD (or a hair card material). With `scalp` (and a scalp with
    UVs), the groom baked onto its scalp goes too (<name>_ScalpColor / _ScalpCoverage) with the
    hair cap mesh <name>_scalpCap.fbx. The groom itself keeps showing its own bake (its
    cardMaps and materials are left as they were). Returns {"fbx", "scalpCap", "maps", "manifest"}."""
    name = name or asset_name(groom) + ("_" + lod if lod else "")
    directory = (directory or asset_folder("exports")).replace("\\", "/")
    if not os.path.isdir(directory):
        os.makedirs(directory)
    before = set(cmds.ls(long=True))
    paths = [p for p in bake_cards(groom, directory, resolution=resolution, name=name, occlusion=occlusion,
                                   material=False, store=False)
             if p.endswith(".tga")]  # the maps; the atlas record beside them is the groom's own business
    name = os.path.basename(paths[0]).rsplit("_", 1)[0] if paths else name  # as written (another groom's bake kept)
    fbx = os.path.join(directory, name + "_cards.fbx").replace("\\", "/")
    cap_fbx = None
    if scalp:
        try:
            paths = list(paths) + list(bake_scalp(groom, directory, resolution=resolution or 2048, name=name,
                                                  cap=False))
        except RuntimeError as e:  # e.g. a scalp without UVs: the cards still go
            cmds.warning("GroomLab: no scalp maps (%s)" % str(e).strip())
            scalp = False
    if not cmds.pluginInfo("fbxmaya", query=True, loaded=True):
        cmds.loadPlugin("fbxmaya", quiet=True)
    by_map = {m: next((p for p in paths if p.endswith("_%s.tga" % m)), None) for m in CARD_MAPS + SCALP_MAPS}
    # The geometry to write: the live mesh and hair cap if there are, their materials pointed
    # at the exported maps while they are written, else meshes of their own, removed afterwards.
    mesh = card_mesh(groom)
    temporary = mesh is None
    put_back = []
    selection = cmds.ls(selection=True)
    view = cmds.getAttr(groom + ".displayCards")
    try:
        if temporary:
            mesh = create_card_mesh(groom)
            card_material(mesh, {m: by_map[m] for m in CARD_MAPS})
        else:
            put_back += _pointed_textures(mesh, by_map)
        cap = None
        if scalp:
            cap = _existing_scalp_cap(groom)
            if cap:
                put_back += _pointed_textures(cap, by_map)
            else:
                cap = scalp_cap(groom, {m: by_map[m] for m in SCALP_MAPS})
        cmds.setAttr(groom + ".displayCards", True)  # written visible (their visibility follows the view)
        cmds.select(mesh, replace=True)
        for command in ("FBXResetExport", "FBXExportInputConnections -v false", "FBXExportSmoothingGroups -v true",
                        "FBXExportTriangulate -v false", "FBXExportEmbeddedTextures -v false"):
            mel.eval(command)
        mel.eval('FBXExport -f "%s" -s' % fbx)
        if cap:
            cap_fbx = os.path.join(directory, name + "_scalpCap.fbx").replace("\\", "/")
            cmds.select(cap, replace=True)
            mel.eval('FBXExport -f "%s" -s' % cap_fbx)
    finally:
        for texture, old in put_back:
            cmds.setAttr(texture + ".fileTextureName", old, type="string")
        cmds.setAttr(groom + ".displayCards", view)
        cmds.select(selection, replace=True) if selection else cmds.select(clear=True)
        made = [n for n in cmds.ls(long=True) if n not in before]  # what the export made for itself
        cmds.delete([n for n in made if cmds.objExists(n)])
    stats = card_stats(groom)
    maps = {m: os.path.basename(p) for m in CARD_MAPS + SCALP_MAPS
            for p in paths if p.endswith("_%s.tga" % m)}
    manifest = {
        "mesh": os.path.basename(fbx),
        "cards": stats["cards"], "triangles": stats["triangles"], "textures": stats["textures"],
        "feathers": stats["feathers"],
        "maps": maps,
        "channels": {
            "Depth": "R: distance off the card plane (0.5 on it, higher in front)",
            "Coverage": "R: opacity",
            "Tangent": "RGB: strand direction in card space (x across U, y root to tip along V, z out), 0.5 + 0.5 * t",
            "Attribute": "R: root U, G: root V (scalp UVs), B: seed (random per strand), A: coordU (0 root, 1 tip "
                         "along each strand; on a feather's card along each barb, from the shaft out)",
            "Color": "RGB: base color, sRGB",
            "AO": "R: ambient occlusion (1 open)",
            "Normal": "RGB: the strands' normal as round fibers in tangent space (x across U, y along V, z out), "
                      "DirectX's green (Unreal's), linear",
            "ScalpColor": "RGB: the hair's color on the scalp (sRGB), in the scalp's UVs, for the hair cap",
            "ScalpCoverage": "R: how much hair covers the skin, in the scalp's UVs",
        },
        "scalpCap": os.path.basename(cap_fbx) if cap_fbx else None,
        "uv": "UV0: the atlas; V runs root (low) to tip (high); cards sharing a texture may be mirrored in U",
        "vertexColor": CARD_TINT_NOTE,
        "normals": CARD_NORMALS_NOTE,
        "featherCards": FEATHER_CARD_NOTE,
    }
    path = os.path.join(directory, name + "_cards.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    return {"fbx": fbx, "scalpCap": cap_fbx, "maps": paths, "manifest": path}


# Unity's hair package (com.unity.demoteam.hair) resamples strands to 3..128 particles
# (HairSim.MIN/MAX_STRAND_PARTICLE_COUNT) and takes up to 64,000 strands a group (MAX_STRAND_COUNT).
UNITY_STRAND_POINTS = (3, 128)
UNITY_MAX_STRANDS = 64000
UNITY_CARD_MAPS = ("BaseColor", "Mask", "AO", "Tangent", "Depth", "Attribute", "Normal")


def _unity_points(value, what):
    lo, hi = UNITY_STRAND_POINTS
    if not lo <= int(value) <= hi:
        raise ValueError("%s must be %d to %d (Unity's strand particle counts)" % (what, lo, hi))
    return int(value)


def _barb_lod(lod, barb_density, barb_width_scale):
    """(barb_density, barb_width_scale) for a lighter groom, as export_unreal_groom's `lod`."""
    if lod is not None:
        if lod not in UNREAL_LODS:
            raise ValueError("lod must be one of %s" % (sorted(UNREAL_LODS),))
        barb_density = UNREAL_LODS[lod] if barb_density is None else barb_density
    if barb_density is not None:
        barb_density = float(barb_density)
        if not 0.0 < barb_density <= 1.0:
            raise ValueError("barb_density must be above 0 and at most 1")
        if barb_width_scale is None:
            barb_width_scale = 1.0 / math.sqrt(barb_density)
    return barb_density, barb_width_scale


def _unity_strands_kwargs(path, scale, resample_to, start, end, step, density, groups, shafts, barb_points, lod,
                          barb_density, barb_width_scale, max_strands):
    kwargs = {"file": path, "step": step, "density": density, "scale": float(scale), "unity": True}
    if resample_to is not None:
        kwargs["pointsPerStrand"] = _unity_points(resample_to, "resample_to")
    if groups:
        kwargs["groups"] = groups
        if barb_points is not None:
            kwargs["barbPoints"] = _unity_points(barb_points, "barb_points")
        kwargs["maxStrandsPerGroup"] = int(max_strands or 0)
    elif barb_points is not None:
        raise ValueError("barb_points needs groups (the barbs' own curves objects)")
    if shafts is not None:
        kwargs["shafts"] = bool(shafts)
    barb_density, barb_width_scale = _barb_lod(lod, barb_density, barb_width_scale)
    if barb_density is not None:
        kwargs["barbDensity"] = barb_density
    if barb_width_scale is not None:
        kwargs["barbWidthScale"] = float(barb_width_scale)
    frame = cmds.currentTime(query=True)
    kwargs["startFrame"] = frame if start is None else start
    kwargs["endFrame"] = kwargs["startFrame"] if end is None else end
    return kwargs


def export_unity_strands(groom, path, scale=1.0, resample_to=None, start=None, end=None, step=1.0, density=1.0,
                         groups="auto", shafts=None, barb_points=None, lod=None, barb_density=None,
                         barb_width_scale=None, max_strands=UNITY_MAX_STRANDS):
    """Alembic curves for Unity's strand hair (com.unity.demoteam.hair, which reads them through
    com.unity.formats.alembic): the strands with per-point widths (every root above 0: Unity
    drops a curve set's widths for one root at 0) and, when the scalp has UVs, the root UV
    repeated at every point (Unity reads curve UVs per point and drops a curve set whose UVs
    are fewer than its points; export_alembic writes one per curve).

    `scale` multiplies points and widths. The default 1.0 keeps Maya's centimeters, which is
    what Unity expects with its default import settings: the Alembic importer's Scale Factor
    0.01 turns the points into meters and the Hair Asset reads widths as centimeters (Alembic
    Scale Diameters: Data In Centimeters). With scale=0.01 (meters in the file) set the
    importer's Scale Factor to 1 and the diameters to Data In Meters. `resample_to` (3 to 128) gives every strand that
    many points evenly by arc length, so Unity can leave "Resample Curves" off (it resamples
    to 16 by default, and must when strands have different point counts). Frames default to
    the current one (a Hair Asset takes its rest pose from the first). Returns frames written.

    Feathers (see export_unity_feathers): `groups` "auto" (default) writes each groom group (the
    hair, each feather design) as a curves object of its own, which Unity's Hair Asset makes a
    strand group of (Alembic Asset Groups: Preserve), named after the group; a group of more than
    `max_strands` (Unity's 64,000; 0 or None: no limit) goes in parts <name>_0, <name>_1, ... A
    groom of one group within the limit writes the same file as groups=None (one object).
    `barb_points` (3 to 128) resamples the feather groups' strands to their own count (barbs are
    short: fewer points than the hair's `resample_to`). `shafts`, `lod`, `barb_density` and
    `barb_width_scale` work as for export_unreal_groom: the shafts are left out when the
    feathers have a rachis mesh, and a lod thins the barbs."""
    kwargs = _unity_strands_kwargs(path, scale, resample_to, start, end, step, density, groups, shafts, barb_points,
                                   lod, barb_density, barb_width_scale, max_strands)
    return cmds.groomLabExportAlembic(groom, **kwargs)


def _stale_atlas_warning(groom, what):
    """Warn when the groom's own bake is stale: its card mesh keeps that bake's atlas while an
    export bakes a fresh one, so the mesh's UVs and the exported maps would not match."""
    if "Atlas" in _card_maps(groom) and card_stats(groom)["stale"]:
        cmds.warning("GroomLab: the cards changed since the groom's last Bake Maps; the %s keeps that "
                     "bake's UVs, so bake the maps again before exporting" % what)


def _unity_material(mesh, name, base_color):
    """A lambert on `mesh` for Unity's FBX import: the BaseColor PNG as its color, its alpha as
    transparency (Unity's material import picks both up)."""
    shapes = cmds.listRelatives(mesh, shapes=True, fullPath=True) or [mesh]
    material = cmds.shadingNode("lambert", asShader=True, name=name)
    group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=name + "SG")
    cmds.connectAttr(material + ".outColor", group + ".surfaceShader")
    if base_color:
        texture = _file_texture(name + "BaseColor", base_color, "sRGB")
        cmds.connectAttr(texture + ".outColor", material + ".color", force=True)
        cmds.connectAttr(texture + ".outTransparency", material + ".transparency", force=True)
    cmds.sets(shapes[0], edit=True, forceElement=group)
    return material


def _shading_groups(node):
    shapes = cmds.listRelatives(node, shapes=True, fullPath=True) or [node]
    return shapes[0], cmds.listConnections(shapes[0], type="shadingEngine") or []


def export_cards_unity(groom, directory=None, name=None, resolution=None, occlusion=1.0, scalp=True, lod=None):
    """Game cards for Unity: the card mesh as FBX in Unity's conventions (Y up, meters, a material
    named <name>_cards_mat with the base color map) and its maps as PNG into `directory`
    (default: the scene's exports folder), plus <name>_unity.json describing every channel and
    suggested HDRP and URP material settings. `name` defaults to the groom's asset_name, with
    _<lod> when `lod` names the level of detail ("hero", "mid", "low"), as export_cards_unreal.

    Maps: <name>_BaseColor (RGB color, sRGB; A coverage, for alpha clipping), _Mask (HDRP's
    mask map: R metallic 0, G ambient occlusion, B root (0) to tip (1), A smoothness from the
    hair look), _AO, _Tangent (strand direction in tangent space), _Depth, _Attribute and _Normal
    (the strands' normal in tangent space, OpenGL's green). With
    `scalp` (and a scalp with UVs), <name>_ScalpBaseColor (color, coverage in alpha) and the
    hair cap <name>_scalpCap.fbx. The groom keeps its own bake (cardMaps and materials are
    left as they were). Returns {"fbx", "scalpCap", "maps", "manifest"}."""
    name = name or asset_name(groom) + ("_" + lod if lod else "")
    directory = (directory or asset_folder("exports")).replace("\\", "/")
    if not os.path.isdir(directory):
        os.makedirs(directory)
    _stale_atlas_warning(groom, "FBX")
    kwargs = {"directory": directory, "name": name, "occlusion": float(occlusion), "noStore": True, "layout": "unity"}
    if resolution:
        kwargs["resolution"] = int(resolution)
    paths = list(cmds.groomLabBakeCards(groom, **kwargs))
    name = os.path.basename(paths[0]).rsplit("_", 1)[0] if paths else name  # as written (another groom's bake kept)
    if scalp:
        try:
            paths += list(cmds.groomLabBakeCards(groom, directory=directory, name=name, scalp=True, layout="unity",
                                                 resolution=int(resolution or 2048)))
        except RuntimeError as e:  # e.g. a scalp without UVs: the cards still go
            cmds.warning("GroomLab: no scalp maps (%s)" % str(e).strip())
            scalp = False
    by_map = {m: next((p for p in paths if p.endswith("_%s.png" % m)), None) for m in UNITY_CARD_MAPS + ("ScalpBaseColor",)}
    if not cmds.pluginInfo("fbxmaya", query=True, loaded=True):
        cmds.loadPlugin("fbxmaya", quiet=True)
    fbx = os.path.join(directory, name + "_cards.fbx").replace("\\", "/")
    cap_fbx = None
    before = set(cmds.ls(long=True))
    selection = cmds.ls(selection=True)
    view = cmds.getAttr(groom + ".displayCards")
    live = card_mesh(groom)
    live_cap = _existing_scalp_cap(groom) if scalp else None
    # The meshes to write: the live card mesh and hair cap when there are (their own shading
    # put back afterwards), else meshes of their own, removed afterwards; either way with a
    # material made for Unity while they are written.
    put_back = [(_shading_groups(n)) for n in (live, live_cap) if n]
    try:
        mesh = live or create_card_mesh(groom)
        _unity_material(mesh, name + "_cards_mat", by_map["BaseColor"])
        cap = None
        if scalp:
            cap = live_cap or scalp_cap(groom)
            if cap:
                _unity_material(cap, name + "_scalpCap_mat", by_map["ScalpBaseColor"])
        cmds.setAttr(groom + ".displayCards", True)  # written visible (their visibility follows the view)
        for command in ("FBXResetExport", "FBXExportInputConnections -v false", "FBXExportSmoothingGroups -v true",
                        "FBXExportTriangulate -v false", "FBXExportEmbeddedTextures -v false",
                        "FBXExportUpAxis y", "FBXExportConvertUnitString m", "FBXExportScaleFactor 1.0"):
            mel.eval(command)
        cmds.select(mesh, replace=True)
        mel.eval('FBXExport -f "%s" -s' % fbx)
        if cap:
            cap_fbx = os.path.join(directory, name + "_scalpCap.fbx").replace("\\", "/")
            cmds.select(cap, replace=True)
            mel.eval('FBXExport -f "%s" -s' % cap_fbx)
    finally:
        mel.eval("FBXResetExport")  # the next FBX export (the user's own) starts from the defaults
        for shape, groups in put_back:
            for group in groups:
                cmds.sets(shape, edit=True, forceElement=group)
        cmds.setAttr(groom + ".displayCards", view)
        cmds.select(selection, replace=True) if selection else cmds.select(clear=True)
        made = [n for n in cmds.ls(long=True) if n not in before]  # what the export made for itself
        cmds.delete([n for n in made if cmds.objExists(n)])
    stats = card_stats(groom)
    smoothness = round(1.0 - hair_look(groom)["roughness"], 3)
    maps = {m: os.path.basename(p) for m, p in by_map.items() if p}
    manifest = {
        "engine": "Unity",
        "mesh": os.path.basename(fbx),
        "scalpCap": os.path.basename(cap_fbx) if cap_fbx else None,
        "units": "meters, Y up (the FBX is converted from Maya's centimeters)",
        "cards": stats["cards"], "triangles": stats["triangles"], "textures": stats["textures"],
        "feathers": stats["feathers"],
        "maps": maps,
        "channels": {
            "BaseColor": "RGB: base color (sRGB); A: coverage (opacity: alpha clip at 0.5)",
            "Mask": "HDRP mask map layout. R: metallic (0); G: ambient occlusion (1 open); "
                    "B: root (0) to tip (1) along the strands; A: smoothness (1 - the hair look's roughness)",
            "AO": "R (gray): ambient occlusion (1 open)",
            "Tangent": "RGB: strand direction in tangent space (R along +U, G along +V root to tip, B out), "
                       "0.5 + 0.5 * t, linear (import without sRGB)",
            "Depth": "R (gray): distance off the card plane (0.5 on it, higher in front), linear",
            "Attribute": "R, G: root UV on the scalp; B: seed per strand; A: root (0) to tip (1) along each strand "
                         "(a feather's barbs: from the shaft out), linear",
            "Normal": "RGB: the strands' normal as round fibers in tangent space (R along +U, G along +V, B out), "
                      "OpenGL's green, linear (import as a Normal map)",
            "ScalpBaseColor": "RGB: the hair's color on the scalp (sRGB); A: how much hair covers the skin",
        },
        "import": {
            "textures": "BaseColor and ScalpBaseColor sRGB with Alpha Is Transparency; Mask, AO, Tangent, "
                        "Depth and Attribute linear (sRGB off); Normal as Texture Type Normal map",
            "model": "Convert Units on (the FBX is in meters, so its scale stays 1)",
        },
        "materials": {
            "HDRP": {
                "shader": "a Shader Graph with the HDRP Hair material (or HDRP/Lit)",
                "Base Color Map": maps.get("BaseColor"),
                "Alpha Clipping": True, "Alpha Clip Threshold": 0.5, "Double-Sided": True,
                "Mask Map": maps.get("Mask"),
                "Normal Map": maps.get("Normal"),
                "Hair Strand Direction": "Tangent map * 2 - 1 (strands run +V, root to tip)",
                "Smoothness": smoothness,
            },
            "URP": {
                "shader": "Universal Render Pipeline/Lit",
                "Base Map": maps.get("BaseColor"),
                "Alpha Clipping": True, "Threshold": 0.5, "Render Face": "Both",
                "Occlusion Map": maps.get("AO"),
                "Normal Map": maps.get("Normal"),
                "Smoothness": smoothness,
            },
            "scalpCap": "Transparent (alpha blend) with ScalpBaseColor, drawn under the cards",
        },
        "uv": "UV0: the atlas; V runs root (low) to tip (high); cards sharing a texture may be mirrored in U",
        "vertexColor": CARD_TINT_NOTE,
        "normals": CARD_NORMALS_NOTE,
        "featherCards": FEATHER_CARD_NOTE,
    }
    path = os.path.join(directory, name + "_unity.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    return {"fbx": fbx, "scalpCap": cap_fbx, "maps": paths, "manifest": path}


def export_cards_gltf(groom, path=None, lod=None, lods=None, resolution=None, binary=True, cap=True, scale=0.01,
                      alpha_mode="mask", alpha_cutoff=0.5, occlusion=1.0, name=None):
    """The groom's game cards as glTF 2.0: a .glb (binary, the maps inside it) or, with
    binary=False, a .gltf with its .bin and PNG maps beside it. Y up, meters (`scale` 0.01 from
    Maya's centimeters); one mesh per level of detail, each a node under one root node named
    `name` (default the groom's asset_name). Each mesh's material is PBR metallic-roughness:
    baseColorTexture <mesh>_BaseColor (RGB color, sRGB; A coverage), `alpha_mode` "mask" (cut
    at `alpha_cutoff`), "blend" or "opaque", double-sided, <mesh>_ORM as occlusion (R) and
    metallic-roughness (G and B at 1: the factors apply, metallic 0 and the hair look's
    roughness). Vertices carry NORMAL, TANGENT (+U across the card; the bitangent runs root to
    tip), TEXCOORD_0 (the atlas) and TEXCOORD_1 (x across the card, 0.5 on a feather card's
    spine; y root 0 to tip 1).

    `lods` (e.g. ["hero", "mid", "low"]) bakes a mesh <name>_<lod> for each preset (see
    cards_preset) from a copy of the graph, the groom untouched; without, one mesh <name>_cards
    of the cards as they are, `lod` only naming the file as the other exports do. With `cap`
    (and a scalp with UVs) the hair cap <name>_scalpCap goes too. `path` defaults to the scene's
    exports folder, <name>_cards.glb. The groom's own bake is left alone. Returns the files written."""
    from groomlab import graph as gm
    name = name or asset_name(groom) + ("_" + lod if lod else "")
    if path is None:
        path = "%s/%s_cards.%s" % (asset_folder("exports"), name, "glb" if binary else "gltf")
    path = os.path.abspath(path).replace("\\", "/")
    folder = os.path.dirname(path)
    if not os.path.isdir(folder):
        os.makedirs(folder)
    if alpha_mode not in ("mask", "blend", "opaque"):
        raise ValueError("alpha_mode is mask, blend or opaque")
    kwargs = {"file": path, "name": name, "binary": bool(binary), "scalpCap": bool(cap), "scale": float(scale),
              "alphaMode": alpha_mode, "alphaCutoff": float(alpha_cutoff), "occlusion": float(occlusion)}
    if resolution:
        kwargs["resolution"] = int(resolution)
    if lods:
        graphs = []
        for level in lods:
            model = gm.GraphModel.from_groom(groom)
            if not _cards_nodes(model):
                raise ValueError("the graph has no Cards node: add one (create_cards) first")
            cards_preset(groom, level, model=model)  # into the copy only
            graphs.append(model.to_json(layout=False))
        kwargs["levelOfDetail"] = list(lods)
        kwargs["lodGraph"] = graphs
    return cmds.groomLabExportGltf(groom, **kwargs)


# Feathers in Unity: the barbs as strands of Unity's hair package, rooted on the rachis mesh (skinned
# with the character), which com.unity.demoteam.digital-human's SkinAttachment carries them on.
UNITY_HAIR_PACKAGES = {
    "com.unity.demoteam.hair": "https://github.com/Unity-Technologies/com.unity.demoteam.hair.git",
    "com.unity.formats.alembic": "2.2.2 or later (Unity's Package Manager)",
    "com.unity.demoteam.digital-human": "https://github.com/Unity-Technologies/com.unity.demoteam.digital-human.git "
                                        "(0.2.1-preview or later: roots attach with the mesh's twist)",
}


def export_unity_feathers(groom, folder=None, lod=None, name=None, points=16, barb_points=6, density=1.0,
                          shafts=None, rachis=True, max_strands=UNITY_MAX_STRANDS):
    """Feathers for Unity the way they go to Unreal: the rachis is skinned geometry, the barbs are
    strands of Unity's hair package attached to it. Writes into `folder` (default: the scene's
    exports folder), named <name> (default: the groom's asset_name, with _<lod>):

      <name>_strands.abc  the strands (export_unity_strands): a curves object, so a Unity strand
                          group, per groom group (the hair, each feather design; parts past
                          `max_strands`), the shafts left out when the feathers have a rachis, the
                          hair at `points` points a strand and the barbs at `barb_points` (3 to 128;
                          short barbs need few), root UVs per point, centimeters (Scale Factor 0.01)
      <name>_rachis.fbx   the rachis mesh (export_rachis, unity=True: Y up, meters), when the
                          feathers have a rachis width and `rachis` is on
      <name>_unity_feathers.json  the files, every strand group in Unity's order (object, group,
                          strands, points per strand) and the Hair Asset / Hair Instance settings to
                          give them: groups preserved, no resampling, root UVs from the curves, each
                          feather group's roots attached to the skinned rachis and held to it.

    `lod` ("high", "mid", "low") thins the barbs as export_unreal_groom does (a lighter groom to
    swap in). Returns {"abc", "rachis", "manifest", "groups"}."""
    name = name or asset_name(groom) + ("_" + lod if lod else "")
    folder = (folder or asset_folder("exports")).replace("\\", "/")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    abc = "%s/%s_strands.abc" % (folder, name)
    barb_density, barb_width_scale = _barb_lod(lod, None, None)
    kwargs = _unity_strands_kwargs(abc, 1.0, points, None, None, 1.0, density, "auto", shafts, barb_points, None,
                                   barb_density, barb_width_scale, max_strands)
    kwargs["report"] = True
    written = json.loads(cmds.groomLabExportAlembic(groom, **kwargs))
    fbx = None
    if rachis and rachis_width(groom) > 0.0:
        fbx = export_rachis(groom, "%s/%s_rachis.fbx" % (folder, name), unity=True)
    elif any(o["feathers"] for o in written["objects"]):
        cmds.warning("GroomLab: %s's feathers have no rachis (set_rachis_width): their shafts go as strands, and "
                     "there is no mesh for the barbs to ride" % groom)
    uniform = all(o["minPoints"] == o["maxPoints"] for o in written["objects"])
    groups = []
    for index, o in enumerate(written["objects"]):
        if o["feathers"]:
            instance = {
                "Skinning": {"Roots Attach": True,
                             "Roots Attach Target": "the rachis's SkinAttachmentTarget (%s)" % (
                                 os.path.basename(fbx) if fbx else "no rachis written")},
                "Physics": {"Global Position": True, "Global Position Influence": 1.0,
                            "Global Position Interval": "Per 1ms", "Global Rotation": True,
                            "Global Rotation Influence": 1.0, "Gravity": 0.0,
                            "why": "barbs ride the skinned rachis: held to their groomed shape, not simulated"},
                "Rendering": {"Material": "a hair material of its own (the feather design's look)"},
            }
        else:
            instance = {
                "Skinning": {"Roots Attach": True,
                             "Roots Attach Target": "the body's SkinAttachmentTarget (the scalp's skinned mesh)"},
                "Physics": "the package's defaults (simulated)",
                "Rendering": {"Material": "the hair material"},
            }
        groups.append({"index": index, "object": o["object"], "group": o["group"], "groupId": o["groupId"],
                       "feathers": o["feathers"], "strands": o["strands"],
                       "pointsPerStrand": o["pointsPerStrand"] or o["maxPoints"], "hairInstance": instance})
    manifest = {
        "engine": "Unity",
        "packages": UNITY_HAIR_PACKAGES,
        "strands": os.path.basename(abc),
        "rachis": os.path.basename(fbx) if fbx else None,
        "lod": lod,
        "barbDensity": barb_density or 1.0,
        "units": "the .abc in centimeters (the Alembic importer's Scale Factor 0.01 makes meters), widths in "
                 "centimeters; the rachis FBX in meters, Y up: both land in the same place",
        "import": {
            "strands": {"Scale Factor": 0.01, "Swap Handedness": True, "Import Curves": True},
            "rachis": {"Convert Units": True, "then": "skin it to the character's skeleton (or combine it with the "
                       "body before skinning) and give its SkinnedMeshRenderer a SkinAttachmentTarget"},
        },
        "hairAsset": {
            "Type": "Alembic", "Alembic Asset": os.path.basename(abc),
            "Alembic Asset Groups": "Preserve",
            "Alembic Scale Positions": "Data In Meters", "Alembic Scale Diameters": "Data In Centimeters",
            "Resample Curves": not uniform,
            "Root UV": "Resolve From Curves", "Strand Diameter": "Resolve From Curves",
            "Tip Scale": "Resolve From Curves",
            "why": "Preserve keeps every curves object its own strand group (Combine merges neighbours of the same "
                   "point count); Resample Curves on would give every group one Resample Resolution",
        },
        "groups": groups,
        "roots": "Unity attaches a strand group's roots (SkinAttachment, attachment type Mesh) to the nearest vertex "
                 "of the target mesh and the triangles round it: every barb root lies on its own rachis's surface "
                 "(rootOnShaft), so attach the feather groups to the rachis mesh, the hair to the body",
    }
    path = "%s/%s_unity_feathers.json" % (folder, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    return {"abc": abc, "rachis": fbx, "manifest": path, "groups": groups}
