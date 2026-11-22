# SPDX-License-Identifier: Apache-2.0
"""Game engines: Unreal (groom Alembic, cards), Unity (hair package strands, cards, feathers) and
glTF cards."""

import json
import os

import groomlab_core

from maya import cmds, mel

from groomlab.paths import asset_folder, asset_name
from groomlab.shading import hair_look
from groomlab.cards import (
    CARD_MAPS, SCALP_MAPS, bake_cards, bake_scalp, card_material,
    card_stats, cards_preset, scalp_cap, _card_maps, _cards_nodes, _existing_scalp_cap,
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



def _engine_cards(groom, engine, directory, name, resolution, occlusion, scalp, lod):
    """Every host's engine cards (the editor's writeEngineCards, in groomLabBakeCards -engine): the
    maps, the hair cap's, the manifest and the meshes as FBX (the core's writer, no fbxmaya)."""
    name = name or asset_name(groom) + ("_" + lod if lod else "")
    directory = (directory or asset_folder("exports")).replace("\\", "/")
    if not os.path.isdir(directory):
        os.makedirs(directory)
    kwargs = {"directory": directory, "name": name, "occlusion": float(occlusion), "engine": engine, "hairCap": bool(scalp)}
    if resolution:
        kwargs["resolution"] = int(resolution)
    done = json.loads(cmds.groomLabBakeCards(groom, **kwargs))
    for warning in done["warnings"]:
        cmds.warning("GroomLab: %s" % warning)
    return {"fbx": done["fbx"], "scalpCap": done["scalpCap"], "maps": done["maps"], "manifest": done["manifest"]}

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
    return _engine_cards(groom, "unreal", directory, name, resolution, occlusion, scalp, lod)


# Unity's hair package (com.unity.demoteam.hair) resamples strands to 3..128 particles
# (HairSim.MIN/MAX_STRAND_PARTICLE_COUNT) and takes up to 64,000 strands a group (MAX_STRAND_COUNT).
UNITY_STRAND_POINTS = (3, 128)
UNITY_MAX_STRANDS = 64000
UNITY_CARD_MAPS = ("BaseColor", "Mask", "AO", "AO_world", "Tangent", "Depth", "Attribute", "Normal")
# The world-space ambient occlusion every card bake writes beside the maps: each card's own island
# in the mesh's second UV set (aoWorld), so the occlusion of the whole groom, not the shared texture's.
AO_WORLD = "AO_world"
AO_WORLD_NOTE = ("R (gray): ambient occlusion of the whole groom (1 open), linear, in the second UV set (UV1, "
                 "aoWorld: each card its own island), so it varies card to card where the shared maps can't")


def _unity_points(value, what):
    lo, hi = UNITY_STRAND_POINTS
    if not lo <= int(value) <= hi:
        raise ValueError("%s must be %d to %d (Unity's strand particle counts)" % (what, lo, hi))
    return int(value)


def _barb_lod(lod, barb_density, barb_width_scale):
    """(barb_density, barb_width_scale) for a lighter groom, as export_unreal_groom's `lod`."""
    if lod is not None and lod not in UNREAL_LODS:
        raise ValueError("lod must be one of %s" % (sorted(UNREAL_LODS),))
    if lod is None and barb_density is None:
        return None, barb_width_scale
    # Every host's rule (engine_export.h setBarbLod): the lod's share, each kept barb 1/sqrt wider.
    import groomlab_core
    return groomlab_core.barb_lod(lod or "", None if barb_density is None else float(barb_density),
                                  None if barb_width_scale is None else float(barb_width_scale))


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


def _require_card_uv_sets(node):
    """Evaluate a card mesh and fail closed unless both exported atlas UV sets are present."""
    shape = (cmds.listRelatives(node, shapes=True, fullPath=True) or [node])[0]
    sets = cmds.polyUVSet(shape, query=True, allUVSets=True) or []
    missing = [name for name in ("map1", "aoWorld") if name not in sets]
    if missing:
        raise RuntimeError("card mesh is missing UV set%s %s" %
                           ("s" if len(missing) > 1 else "", ", ".join(missing)))
    cmds.polyUVSet(shape, currentUVSet=True, uvSet="map1")
    return shape


def _static_card_export_mesh(node):
    """A history-free evaluated copy, so FBX receives every UV set on the mesh data itself."""
    _require_card_uv_sets(node)
    copy = cmds.duplicate(node, returnRootsOnly=True, name=node.split("|")[-1] + "Export")[0]
    cmds.delete(copy, constructionHistory=True)
    _require_card_uv_sets(copy)
    shape = (cmds.listRelatives(copy, shapes=True, fullPath=True) or [copy])[0]
    sets = cmds.polyUVSet(shape, query=True, allUVSets=True) or []
    if "map2" in sets:
        cmds.polyUVSet(shape, delete=True, uvSet="map2")
    # Export exactly two concrete sets. Keeping aoWorld and adding a copied alias produced three
    # sets, which Autodesk FBX could collapse back to UV0. Renaming the evaluated second set makes
    # it unambiguously FBX UV channel 1 while preserving its unique per-card coordinates.
    cmds.polyUVSet(shape, rename=True, uvSet="aoWorld", newUVSet="map2")
    sets = cmds.polyUVSet(shape, query=True, allUVSets=True) or []
    if "map2" not in sets:
        raise RuntimeError("could not rename the AO-world UV set to FBX UV1 map2")
    cmds.polyUVSet(shape, currentUVSet=True, uvSet="map1")
    return copy


def export_cards_unity(groom, directory=None, name=None, resolution=None, occlusion=1.0, scalp=True, lod=None):
    """Game cards for Unity: the card mesh as FBX in Unity's conventions (Y up, meters, a material
    named <name>_cards_mat with the base color map) and its maps as PNG into `directory`
    (default: the scene's exports folder), plus <name>_unity.json describing every channel and
    suggested HDRP and URP material settings. `name` defaults to the groom's asset_name, with
    _<lod> when `lod` names the level of detail ("hero", "mid", "low"), as export_cards_unreal.

    Maps: <name>_BaseColor (RGB color, sRGB; A coverage, for alpha clipping), _Mask (HDRP's
    mask map: R metallic 0, G ambient occlusion, B root (0) to tip (1), A smoothness from the
    hair look), _AO, _AO_world (the whole groom's occlusion in the second UV set, aoWorld),
    _Tangent (strand direction in tangent space), _Depth, _Attribute and _Normal
    (the strands' normal in tangent space, OpenGL's green). With
    `scalp` (and a scalp with UVs), <name>_ScalpBaseColor (color, coverage in alpha) and the
    hair cap <name>_scalpCap.fbx. The groom keeps its own bake (cardMaps and materials are
    left as they were). Returns {"fbx", "scalpCap", "maps", "manifest"}."""
    _stale_atlas_warning(groom, "FBX")
    return _engine_cards(groom, "unity", directory, name, resolution, occlusion, scalp, lod)


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
    tip), TEXCOORD_0 (the shared texture atlas) and TEXCOORD_1 (the unique aoWorld atlas).

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
    # Every host's manifest (engine_cards.h unityFeathersManifestJson).
    manifest = json.loads(groomlab_core.unity_feathers_manifest_json(
        json.dumps(written["objects"]), os.path.basename(abc), os.path.basename(fbx) if fbx else "", lod or "",
        float(barb_density or 1.0)))
    groups = manifest["groups"]
    path = "%s/%s_unity_feathers.json" % (folder, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    return {"abc": abc, "rachis": fbx, "manifest": path, "groups": groups}
