# SPDX-License-Identifier: Apache-2.0
"""Groom files (.groom): writing one, rendering from one, and reading one back into Maya (import,
reload, save)."""

import json
import os
import re

from maya import cmds, mel

import groomlab_core

from groomlab.base import ensure_plugin, no_undo, undo_chunk, _original_shape, _scalp_shape, _shape_of
from groomlab.params import CONTROL_PREFIX, get_graph, set_graph, sync_controls
from groomlab.paths import asset_name, portable_path, resolve_path
from groomlab.groom import create_groom, scalp_mesh, set_rest_mesh, _connect_rest
from groomlab.transfer import _deformed


def export_groom(groom, path, scalp_cache=False, start=None, end=None, step=1.0, guide_cache="auto", merge=False,
                 name=None):
    """Write a groom file (.groom) that groomlab-cli, the Arnold procedural and the standalone app
    read without Maya, and import_groom brings back into Maya.

    A groom file is a document of named grooms (the core's groom_package.h, as every host writes
    it): the groom goes in under `name` (its asset name), with `merge` beside the file's other
    grooms (the one of its name replaced), else alone. It holds the graph, guides, parts, flow,
    correctives, painted maps and scalp at the current frame, in the scene's up axis; the files the
    groom owns (texture maps, the card bake, Simulate bakes) go in the file's own folder,
    groomlab/<file>/<masks|cards|caches>/<groom> (the scene's own, nothing copied, when the file is
    named after the scene), every path named relative to the file. Caches cover [start, end]
    (default: the playback range) beside it:
    - `scalp_cache`: the scalp's animation, <name>.scalp.abc (AbcExport; <file>_<groom>.scalp.abc
      with `merge`), so the groom follows it. A path to an existing cache (relative to the file),
      or a (path, object) pair, names that cache instead of writing one (save_groom keeps its own).
    - `guide_cache`: the guide poses (simulation), <name>.guides.abc ("_<groom>" with `merge`).
      "auto" writes it when sim curves or a guide cache pose the groom.
    Returns the paths written."""
    from groomlab import sim

    path = os.path.abspath(path)
    name = name or asset_name(groom)
    kwargs = {"file": path, "name": name, "merge": bool(merge)}
    stem = os.path.splitext(path)[0] + ("_" + name if merge else "")  # the caches' names: one groom's in a file of several
    written = []
    if guide_cache == "auto":
        guide_cache = sim.has_poses(groom)
    if isinstance(scalp_cache, (str, tuple, list)):  # an existing cache, named as it is
        cache, obj = (scalp_cache, "") if isinstance(scalp_cache, str) else tuple(scalp_cache)
        kwargs["scalpCache"] = cache.replace("\\", "/")
        if obj:
            kwargs["scalpObject"] = obj
        scalp_cache = False
    if scalp_cache or guide_cache:
        if start is None:
            start = cmds.playbackOptions(query=True, minTime=True)
        if end is None:
            end = cmds.playbackOptions(query=True, maxTime=True)
    if scalp_cache:
        scalp = _scalp_shape(groom)
        if not scalp:
            raise RuntimeError("%s has no scalp connected" % groom)
        cache = stem + ".scalp.abc"
        cmds.loadPlugin("AbcExport", quiet=True)
        root = cmds.listRelatives(scalp, parent=True, fullPath=True)[0]
        # With the UVs and color sets: import_groom then takes the cache's mesh as the scalp itself.
        job = "-frameRange %r %r -step %r -worldSpace -uvWrite -writeColorSets -dataFormat ogawa -root \"%s\" -file \"%s\"" % (
            float(start), float(end), float(step), root, cache.replace("\\", "/"))
        cmds.AbcExport(jobArg=job)
        kwargs["scalpCache"] = os.path.basename(cache)
        kwargs["scalpObject"] = scalp.split("|")[-1]
        written.append(cache)
    if guide_cache:
        cache = stem + ".guides.abc"
        cmds.groomLabExportAlembic(groom, file=cache, startFrame=start, endFrame=end, step=step, guidePoses=True)
        kwargs["guideCache"] = os.path.basename(cache)
        written.append(cache)
    cmds.groomLabExportGroom(groom, **kwargs)
    return [path] + written


def export_grooms(grooms, path, scalp_cache=False, start=None, end=None, step=1.0, link=True):
    """Several grooms into one groom file (a document of named grooms: the app opens them together,
    Maya and Blender import each), each under its asset name; the file's other grooms stay. With
    `link`, each groom is linked to the file (Save to Groom File, Reload from Groom File, renders
    from the file find their own by name). Returns the paths written."""
    written = []
    with undo_chunk("save grooms"):
        for i, groom in enumerate(grooms):
            out = export_groom(groom, path, scalp_cache=scalp_cache, start=start, end=end, step=step, merge=i > 0)
            written += [p for p in out if p not in written]
            if link:
                cmds.setAttr(groom + ".groomFile", portable_path(out[0]), type="string")
    return written


def groom_entry(groom, path):
    """The index of `groom`'s own entry in the groom file at `path` (by its asset name; a file of one
    groom: that one), or -1."""
    names = groom_file_info(path).get("grooms") or [""]
    name = asset_name(groom)
    if name in names:
        return names.index(name)
    return 0 if len(names) == 1 else -1


def render_from_groom_file(groom, path, scalp_cache=True, start=None, end=None, step=1.0):
    """Export a groom file and render the groom from it in Arnold (groomlab_procedural).

    Exported .ass files then reference the file instead of holding every
    strand, and render on a farm without Maya. Re-run after editing the
    groom. Returns the paths written.
    """
    written = export_groom(groom, path, scalp_cache=scalp_cache, start=start, end=end, step=step, merge=True)
    cmds.setAttr(groom + ".groomFile", portable_path(written[0]), type="string")
    cmds.setAttr(groom + ".renderMode", 1)
    return written


def render_curves(groom):
    """Render the groom as curves evaluated in Maya (the default)."""
    cmds.setAttr(groom + ".renderMode", 0)


# ---------------------------------------------------------------------------
# Groom files back into Maya: import_groom (a new groom), reload_groom (the same node), save_groom


def groom_file_info(path, index=0):
    """What a groom file holds (groomLabImportGroom -info): its groom `index`'s scalp, maps, caches,
    card bake, correctives' weight curves and controls, read in the scene's up axis, its "name",
    and "grooms": every groom's name in the file's order (a file holds one groom or several: the
    standalone app's document of several grooms)."""
    ensure_plugin()
    return json.loads(cmds.groomLabImportGroom(file=os.path.abspath(path), info=True, index=int(index)))


def groom_file(groom):
    """The groom file the groom is linked to (import_groom, reload_groom, save_groom, or a render
    from it: the groomFile attribute), resolved; None when it names none."""
    stored = cmds.getAttr(groom + ".groomFile") or ""
    return resolve_path(stored, groom).replace("\\", "/") if stored else None


def _selected_mesh():
    from groomlab.base import selected_meshes
    found = selected_meshes()  # (faces of it selected: the mesh, never the component)
    return found[0] if found else None


def import_grooms(path, caches=True):
    """Every groom of a groom file (the standalone app saves a document of several grooms in one
    file), each made as import_groom makes one, on a mesh made from its own scalp and named after
    it. A file of one groom imports as import_groom does (on the selected mesh when it is the
    file's scalp). One undo step. Returns the groom shapes, in the file's order."""
    names = groom_file_info(path).get("grooms") or [""]
    if len(names) == 1:
        return [import_groom(path, caches=caches)]
    stem = re.sub(r"\W", "_", os.path.splitext(os.path.basename(path))[0]) or "groom"
    out = []
    with undo_chunk("import grooms"):
        for index, entry in enumerate(names):
            name = re.sub(r"\W", "_", entry or "") or "%s_%d" % (stem, index + 1)
            out.append(import_groom(path, name=name, caches=caches, use_selection=False, index=index))
    return out


def _make_extra_scalps(groom, path, info, name, index):
    """The file's other scalps (Add > Scalp's: "scalps") as meshes of their own, made the groom's
    other scalps in order (their rest poses with them), before the file's data (the graph's Scalp
    nodes read them by name, the vertex maps span them)."""
    from groomlab import extra_scalps
    for extra in info.get("scalps") or []:
        made = _shape_of(cmds.groomLabImportGroom(file=path, index=index, makeScalp=True, scalp=extra["name"],
                                                  name="%s_%sScalp" % (name, extra["name"])), "mesh")
        rest = False
        if extra.get("rest"):
            rest = _shape_of(cmds.groomLabImportGroom(file=path, index=index, makeScalp=True, scalp=extra["name"], rest=True,
                                                      name="%s_%sScalpRest" % (name, extra["name"])), "mesh")
            cmds.hide(rest)
        extra_scalps.add_scalp(groom, made, name=extra["name"], rest=rest, wire=False)


def import_groom(path, scalp=None, name=None, caches=True, use_selection=True, index=0):
    """Make a groom from a groom file (.groom: export_groom's, or one the standalone app saved).

    The groom gets the file's graph (nodes, params, ramps, maps, bypass, inputs, controls and
    their values), guide hierarchy (groups, layers), correctives (a weight per layer on the
    groom's transform, keyed as the file's weight curve), parts, flow, painted maps and
    followScalp, and is linked to the file (reload_groom, save_groom):
    - The scalp: `scalp` (or, with `use_selection`, a selected mesh) when given; it must be the
      file's scalp (as many vertices, the same faces, as the guides are rooted on them), else a
      ValueError says why (move a groom to another mesh with transfer_groom afterwards).
      Otherwise a mesh is made from the file's scalp (its triangles, UVs and own color sets),
      with a hidden rest mesh when the file has a rest pose.
    - Painted maps: vertex maps go to the groom's scalpMaps; a texture map's image (found beside
      the file, or through its search paths) is copied into the scene's masks folder for the
      groom (asset_folder("masks")), the graph's other files (texture params, ImportCurves) are
      linked where they are found.
    - The card bake the file names is copied into the groom's cards folder (cardMaps), its stamp
      kept, so the cards keep their atlas cells and the stale check holds.
    - With `caches`: the file's guide cache poses the groom (guideCache), and its scalp cache
      animates the scalp, as Maya's Alembic workflow does: the cache is read with AbcImport
      (under a group named <name>ScalpCache), and its mesh is the scalp when it is the file's
      (export_groom's caches carry the UVs and color sets), else it drives the scalp through a
      blendShape in world space (the group hidden); a given mesh that deforms already is left
      as it is.
    Groom files are Y-up or Z-up ("upAxis"): a file in the other axis than the scene's is turned
    (a quarter turn about X), its colliders and the graph's world-direction params (Gravity's,
    Wind's, Bend's directions, Mirror's axis) with it. The file's colliders are not made in the
    scene: connect the scene's own (add_collider). One undo step,
    except the scalp cache's AbcImport (Maya's AbcImport does not undo; undoing the import
    leaves what it read, as File > Import does). `index` picks one groom of a file of several
    (import_grooms imports them all); such a groom is not linked to the file (saving it there would
    drop the others). Returns the groom shape."""
    ensure_plugin()
    path = os.path.abspath(path)
    info = groom_file_info(path, index)
    if not info.get("scalp"):
        raise ValueError("%s holds no scalp" % path)
    if scalp is None and use_selection:
        scalp = _selected_mesh()
    # The groom's own name, as the file keeps it (its asset name when Maya saved it, the app's or
    # Blender's groom name); a file from before names had the file's.
    stem = os.path.splitext(os.path.basename(path))[0]
    name = name or re.sub(r"\W", "_", info.get("name") or "") or (re.sub(r"\W", "_", stem) or "groom")
    if name[0].isdigit():
        name = "g" + name
    mesh = None
    if scalp:
        mesh = _shape_of(scalp, "mesh")
        why = cmds.groomLabImportGroom(file=path, index=index, checkScalp=mesh)
        if why:
            raise ValueError("%s is not the scalp of %s: %s. Import onto the mesh the groom was made on, or with no "
                             "mesh (the file's scalp is made) and move the groom with GroomLab > Utilities > Transfer "
                             "Groom to Scalp." % (mesh, path, why))
    cache = info.get("scalpCache") if caches else None
    if cache and not cache["found"]:
        cmds.warning("GroomLab: %s's scalp cache %s is not there; the scalp stays still" % (path, cache["file"]))
        cache = None
    if cache and mesh and _deformed(mesh):
        cache = None  # the given scalp moves already
    cached = _import_scalp_cache(cache["file"], cache["object"], name, info["scalp"]["points"]) if cache else None
    with undo_chunk("import groom"):
        if cached and not mesh:
            # The cache's mesh is the scalp when it is the file's (the same faces, UVs when the file has them).
            for candidate in cached[1]:
                if not cmds.groomLabImportGroom(file=path, index=index, checkScalp=candidate) and \
                        (not info["scalp"]["uvs"] or cmds.polyEvaluate(candidate, uvcoord=True)):
                    mesh = candidate
                    break
        made = not mesh
        if made:
            mesh = _shape_of(cmds.groomLabImportGroom(file=path, index=index, makeScalp=True, name=name + "Scalp"), "mesh")
        if cached and mesh not in cached[1]:
            # The cache's points drive the scalp, which keeps its own faces, UVs and color sets.
            cmds.blendShape(cmds.listRelatives(cached[1][0], parent=True, fullPath=True)[0],
                            cmds.listRelatives(mesh, parent=True, fullPath=True)[0], origin="world", weight=(0, 1.0),
                            name=name + "ScalpCacheBlend")
            cmds.hide(cached[0])
        groom = create_groom(name=name)
        cmds.connectAttr(mesh + ".worldMesh[0]", groom + ".inMesh")
        _make_extra_scalps(groom, path, info, name, index)
        if info["scalp"]["rest"] and (made or cached or not _original_shape(mesh)):
            rest = cmds.groomLabImportGroom(file=path, index=index, makeScalp=True, rest=True, name=name + "ScalpRest")
            cmds.hide(rest)
            _connect_rest(groom, _shape_of(rest, "mesh"))  # still if it is parented under a moving rig
        elif not made and _original_shape(mesh):
            set_rest_mesh(groom)  # as create_groom does on a deformed scalp
        cmds.groomLabImportGroom(groom, file=path, index=index, caches=bool(caches))
        _after_groom_file(groom, info, keep_keys=False)
        if info.get("flatGuides"):
            _flat_guide_curves(groom, info["flatGuides"], name)
        cmds.select(cmds.listRelatives(groom, parent=True, fullPath=True)[0])
    return groom


def reload_groom(groom, path=None):
    """Replace the groom's graph, guides, correctives, parts, flow, painted maps, card bake and
    followScalp with a groom file's (default: the file it is linked to, groom_file), keeping
    the node, its connections, its scalp and its caches: the round trip Maya -> the standalone
    app -> Maya. The file's scalp must be the groom's (ValueError otherwise). Correctives keep
    their keys where the file's weight curves are what they key; control values come from the
    file except keyed ones. One undo step. Returns the groom."""
    ensure_plugin()
    path = path or groom_file(groom)
    if not path:
        raise ValueError("%s is linked to no groom file: give one" % groom)
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise ValueError("no groom file at %s" % path)
    index = groom_entry(groom, path)
    if index < 0:
        raise ValueError("%s holds no groom named %s: reload from a groom file it was saved to" % (path, asset_name(groom)))
    info = groom_file_info(path, index)
    scalp = scalp_mesh(groom)
    if not scalp:
        raise ValueError("%s has no scalp" % groom)
    why = cmds.groomLabImportGroom(file=path, checkScalp=scalp, index=index)
    if why:
        raise ValueError("%s's scalp is not the scalp of %s: %s. Reload onto the groom the file was made from, or "
                         "import it as a new groom." % (groom, path, why))
    from groomlab import shots
    with undo_chunk("reload groom"):
        before = {l["name"]: l.get("slot") for l in shots._load(groom)["layers"]}
        cmds.groomLabImportGroom(groom, file=path, caches=False, index=index)
        kept = {l["name"] for l in shots._load(groom)["layers"]}
        for layer, slot in before.items():  # the weights of layers the file no longer has go with them
            if layer not in kept and slot is not None:
                shots._drop_weight(groom, layer, slot)
        _after_groom_file(groom, info, keep_keys=True)
    return groom


def save_groom(groom, path=None, scalp_cache=None, guide_cache="auto", start=None, end=None, step=1.0):
    """Write the groom to a groom file (export_groom) and link it there: by default the file it
    is linked to (Save to Groom File), so the standalone app opens what Maya made and
    reload_groom brings its edits back. `scalp_cache` None keeps naming the scalp cache the file
    named before (True writes a new one, False names none). Returns the paths written."""
    path = path or groom_file(groom)
    if not path:
        raise ValueError("%s is linked to no groom file: give a path" % groom)
    path = os.path.abspath(path)
    if scalp_cache is None:
        scalp_cache = False
        if os.path.isfile(path):
            try:
                index = groom_entry(groom, path)
                cache = groom_file_info(path, index).get("scalpCache") if index >= 0 else None
            except RuntimeError:
                cache = None
            if cache and cache["found"]:
                rel = groomlab_core.path_from_groom_folder(cache["file"], os.path.dirname(path))  # as the file stores it
                scalp_cache = (rel, cache["object"])
    # The groom's own entry: the file's other grooms stay.
    written = export_groom(groom, path, scalp_cache=scalp_cache, guide_cache=guide_cache, start=start, end=end, step=step,
                           merge=True)
    cmds.setAttr(groom + ".groomFile", portable_path(written[0]), type="string")
    return written


def _after_groom_file(groom, info, keep_keys):
    """What import_groom and reload_groom do after groomLabImportGroom set the groom's texts: the
    graph's files linked where the file's folder finds them, the controls' attributes made and set
    (a keyed one kept with keep_keys), the correctives' weights keyed."""
    graph = get_graph(groom)
    dirs = [info.get("directory") or ""] + [os.path.expandvars(p) for p in info.get("searchPaths") or []]
    from groomlab import presets
    relinked = False
    for _label, params, param, stored in list(presets._file_params(graph)):
        if os.path.isabs(os.path.expandvars(stored)) or presets.file_exists(resolve_path(stored, groom)):
            continue
        for folder in dirs:
            candidate = os.path.join(folder, stored) if folder else None
            if candidate and presets.file_exists(candidate):
                presets.set_file_param(params, param, portable_path(os.path.normpath(candidate).replace("\\", "/")))
                relinked = True
                break
    if relinked:
        set_graph(groom, graph)  # and the controls' attributes
    else:
        sync_controls(groom, graph)
    for name, value in sorted((info.get("controls") or {}).items()):
        plug = "%s.%s%s" % (groom, CONTROL_PREFIX, name)
        if not cmds.objExists(plug) or cmds.getAttr(plug, lock=True):
            continue
        if cmds.listConnections(plug, source=True, destination=False):
            continue  # keyed or driven: its keys stay
        kind = cmds.getAttr(plug, type=True)
        if kind == "bool":
            cmds.setAttr(plug, bool(value))
        elif kind in ("long", "short", "byte", "enum"):
            cmds.setAttr(plug, int(round(value)))
        else:
            cmds.setAttr(plug, float(value))
    fps = float(mel.eval("currentTimeUnitToFPS"))
    for layer in info.get("correctives") or []:
        _corrective_weight(groom, layer, fps, keep_keys)


def _corrective_weight(groom, layer, fps, keep_keys):
    """A corrective layer's weight on the groom's transform (shots.py), keyed as its weight curve
    (seconds, weight; linear between samples as the file evaluates it). With keep_keys, a weight
    whose keys already give the curve keeps them."""
    from groomlab import shots
    name, slot, curve = layer["name"], int(layer["slot"]), layer.get("weightCurve") or []
    source = shots._source(groom, name)
    if keep_keys and curve and cmds.objExists(source) and \
            all(abs(cmds.getAttr(source, time=t * fps) - w) < 1e-3 for t, w in curve):
        target = shots._shape_plug(groom, slot)
        if not cmds.isConnected(source, target):
            cmds.connectAttr(source, target, force=True)
        return
    try:
        shots._make_weight(groom, name, slot, value=float(layer.get("weight", 1.0)))
    except RuntimeError as e:
        cmds.warning("GroomLab: corrective %s keeps its weight unkeyed (%s)" % (name, e))
        return
    for t, w in curve:
        cmds.setKeyframe(source, time=t * fps, value=w, inTangentType="linear", outTangentType="linear")


def _import_scalp_cache(cache, obj, name, count):
    """A scalp cache read with AbcImport under a group <name>ScalpCache: (the group, its meshes
    of `count` vertices, the one named `obj` first), or None (a warning, nothing left) when it has
    none. Outside the undo queue: AbcImport does not undo, and a step that half undoes would
    leave redo pointing at nodes that are gone."""
    cmds.loadPlugin("AbcImport", quiet=True)
    with no_undo():
        group = cmds.createNode("transform", name=name + "ScalpCache")
        reader = cmds.AbcImport(cache.replace("\\", "/"), mode="import", reparent=group)
        meshes = [m for m in cmds.listRelatives(group, allDescendents=True, type="mesh", fullPath=True) or []
                  if not cmds.getAttr(m + ".intermediateObject") and cmds.polyEvaluate(m, vertex=True) == count]
        leaf = (obj or "").split("|")[-1].split(":")[-1]
        meshes.sort(key=lambda m: leaf not in [p.split(":")[-1] for p in m.split("|")[-2:]])
        if not meshes:
            cmds.delete(group)
            if reader and cmds.objExists(reader):
                cmds.delete(reader)
            cmds.warning("GroomLab: %s has no mesh of %d vertices; the scalp stays still" % (cache, count))
            return None
    return group, meshes


def _flat_guide_curves(groom, guides, name):
    """The file's flat guides as curves through their points, connected to the groom's
    guideCurves at their IDs (curves are read as their arc-length samples: the points)."""
    group = cmds.createNode("transform", name=name + "GuideCurves")
    for guide in guides:
        p = guide["points"]
        points = [tuple(p[i:i + 3]) for i in range(0, len(p) - 2, 3)]
        if len(points) < 2:
            continue
        curve = cmds.curve(degree=1, point=points, name="%sGuide%d" % (name, guide["id"]))
        curve = cmds.parent(curve, group)[0]
        cmds.connectAttr(_shape_of(curve, "nurbsCurve") + ".worldSpace[0]", "%s.guideCurves[%d]" % (groom, guide["id"]))
    return group
