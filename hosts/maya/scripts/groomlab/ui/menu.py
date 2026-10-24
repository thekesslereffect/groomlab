# SPDX-License-Identifier: Apache-2.0
"""The GroomLab menu in Maya's main menu bar (installed when the plugin loads)."""

import functools
import os

from maya import cmds, mel

from groomlab.links import DOCS_URL, DONATE_URL, REPO_URL
from groomlab.ui import definitions

MENU = "groomLabMenu"


def _selected_groom():
    """The groom menu actions work on: the pinned one, else the selected one."""
    import groomlab
    groom = groomlab.active_groom()
    if not groom:
        cmds.warning("GroomLab: select a groom")
    return groom


def default_file(groom, kind, suffix):
    """A save dialog's starting file: <the scene's `kind` folder>/<the groom's asset name><suffix>
    (file safe: no namespace colons or DAG bars reach the name)."""
    import groomlab
    return groomlab.asset_folder(kind) + "/" + groomlab.asset_name(groom) + suffix


ASK_TO_SAVE = True  # tests turn the untitled-scene note off


def ensure_scene_saved(what):
    """GroomLab keeps a scene's files (masks, bakes, exports) in a groomlab folder beside the
    scene file. While the scene is untitled there is no such place yet: the files go under the
    project's groomlab/untitled folder and move beside the scene when it is first saved
    (follow_scene_files). Never a dialog: a note over the viewport says so. Always True (the
    return stays for the callers that ask)."""
    import groomlab
    if not ASK_TO_SAVE or cmds.about(batch=True) or groomlab.scene_folder():
        return True
    cmds.inViewMessage(amg="GroomLab: the scene is untitled, so %s go under %s/groomlab for now and move "
                           "beside the scene when you save it" % (what, groomlab.project_root().rstrip("/")),
                       pos="topCenter", fade=True, fadeStayTime=4000)
    return True


def create_groom_from_selection():
    """Selected mesh = scalp; selected curves become primary guides."""
    import groomlab
    meshes = cmds.ls(selection=True, dag=True, type="mesh", noIntermediate=True)
    curves = cmds.ls(selection=True, dag=True, type="nurbsCurve")
    if not meshes:
        cmds.warning("GroomLab: select a scalp mesh (and optionally guide curves)")
        return
    if not ensure_scene_saved("this groom's painted masks, card bakes and exports"):
        return
    groom = groomlab.create_groom(meshes[0], fit=True)  # the default hair in proportion to the scalp
    if curves:
        groomlab.guides_from_curves(groom, curves)
    groomlab.pin_new_groom(groom)
    cmds.select(groom)


def add_sample_action(sample):
    import groomlab
    part = groomlab.add_sample(sample)
    cmds.inViewMessage(amg="Added <hl>%s</hl> (selected): Create Groom from Selection grows hair on it." % part,
                       pos="topCenter", fade=True)


def guides_from_selected_curves():
    import groomlab
    groom = _selected_groom()
    curves = cmds.ls(selection=True, dag=True, type="nurbsCurve")
    if groom and curves:
        groomlab.guides_from_curves(groom, curves)
    elif groom:
        cmds.warning("GroomLab: select curves along with the groom")


CHILDREN_COUNT = 5  # the Guides tab's inline count (its spin box sets it)


def generate_children_dialog(count=None, group=None, groom=None):
    """Generate Children: `count` children (the Guides tab's inline count by default) for each
    selected guide, else for every guide of `group` (the last group by default). No prompt."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    groups = [g["name"] for g in groomlab.list_guide_groups(groom)]
    if not groups:
        cmds.warning("GroomLab: the groom has no guides yet")
        return None
    count = int(CHILDREN_COUNT if count is None else count)
    chosen = groomlab.selected_guides(groom)
    try:
        if chosen and not group:
            # Only the selected guides get children (Tonic-style: pick which parents have children).
            made = groomlab.generate_children(groom, count=count, selected=True)
            _say("%d children for %d selected guides" % (made, len(chosen)))
        else:
            parent = group or groups[-1]
            made = groomlab.generate_children(groom, parent, count=count)
            _say("%d children under %s" % (made, parent))
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: %s" % e)
        return None
    return made


def _playback_range():
    return cmds.playbackOptions(query=True, minTime=True), cmds.playbackOptions(query=True, maxTime=True)


def _frames(start, end):
    return int(round(end - start)) + 1


def _remove_partial(path):
    """A file a cancelled export left half written."""
    try:
        if path and os.path.isfile(path):
            os.remove(path)
    except OSError:
        pass


def _run_export(status, path, fn, done_text):
    """An export with feedback: the wait cursor and `status` in Maya's progress bar (the frame loop
    counts its frames there; Esc stops it and the half-written file goes), then `done_text` over the
    viewport. Returns fn()'s result, None when cancelled."""
    from groomlab.ui import messages
    try:
        with messages.busy(status):
            result = fn()
    except RuntimeError as e:
        if messages.is_cancelled(e):
            _remove_partial(path)
            messages.say("%s cancelled: nothing was written" % status)
            return None
        raise
    messages.done(done_text)
    return result


def export_curves_dialog(kind):
    """Strands over the playback range as Alembic or USD curves, or a V-Ray scene (Maya's progress
    bar counts the frames; Esc cancels)."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return None
    file_filter, export, ext, name = {"alembic": ("Alembic (*.abc)", groomlab.export_alembic, ".abc", "Alembic"),
                                      "usd": ("USD (*.usda)", groomlab.export_usd, ".usda", "USD"),
                                      "vrscene": ("V-Ray Scene (*.vrscene)", groomlab.export_vrscene, ".vrscene",
                                                  "V-Ray")}[kind]
    path = cmds.fileDialog2(fileFilter=file_filter, dialogStyle=2, fileMode=0, caption="Export Strands",
                            startingDirectory=default_file(groom, "exports", ext))
    if not path:
        return None
    start, end = _playback_range()
    return _run_export("Exporting %s curves" % name, path[0], lambda: export(groom, path[0], start=start, end=end),
                       "wrote %s (%d frames)" % (path[0], _frames(start, end)))


def export_unreal_dialog():
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="Alembic (*.abc)", dialogStyle=2, fileMode=0, caption="Export for Unreal",
                            startingDirectory=default_file(groom, "exports", "_unreal.abc"))
    if path:
        groomlab.export_unreal_groom(groom, path[0])
        cmds.inViewMessage(amg="GroomLab: wrote %s for Unreal's groom importer" % path[0], pos="topCenter", fade=True)


def export_unity_strands_dialog():
    """Alembic strands for Unity's hair package (com.unity.demoteam.hair), at the current frame."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="Alembic (*.abc)", dialogStyle=2, fileMode=0, caption="Export Unity Strands",
                            startingDirectory=default_file(groom, "exports", "_unity.abc"))
    if path:
        groomlab.export_unity_strands(groom, path[0])
        cmds.inViewMessage(amg="GroomLab: wrote %s for Unity's hair package" % path[0], pos="topCenter", fade=True)


def export_unity_feathers_dialog():
    """Feathers for Unity: the strands .abc (a strand group per groom group, barbs without their
    shafts), the rachis FBX and a manifest of the Hair Asset / Instance settings, into a folder."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    folder = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the Unity feathers",
                              startingDirectory=groomlab.asset_folder("exports"))
    if folder:
        result = groomlab.export_unity_feathers(groom, folder[0])
        cmds.inViewMessage(amg="GroomLab: wrote %s (%d strand groups)" % (result["abc"], len(result["groups"])),
                           pos="topCenter", fade=True)


def export_unity_cards_dialog():
    """The cards for Unity: FBX (meters, Y up), PNG maps and a manifest into a folder."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    folder = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the Unity cards",
                              startingDirectory=groomlab.asset_folder("exports"))
    if folder:
        result = groomlab.export_cards_unity(groom, folder[0])
        cmds.inViewMessage(amg="GroomLab: wrote %s" % result["fbx"], pos="topCenter", fade=True)


def export_gltf_cards_dialog():
    """The cards as glTF 2.0 (.glb with the maps inside, or .gltf beside its files)."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="glTF binary (*.glb);;glTF (*.gltf)", dialogStyle=2, fileMode=0,
                            caption="Export glTF Cards", startingDirectory=default_file(groom, "exports", "_cards.glb"))
    if path:
        written = groomlab.export_cards_gltf(groom, path[0], binary=not path[0].lower().endswith(".gltf"))
        cmds.inViewMessage(amg="GroomLab: wrote %s" % written[0], pos="topCenter", fade=True)


def export_usd_scale_instancer_dialog():
    """The strands and the scales as a USD PointInstancer of their plates (Unreal: instanced static
    meshes), at the current frame."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="USD (*.usda)", dialogStyle=2, fileMode=0, caption="Export Scales Instancer",
                            startingDirectory=default_file(groom, "exports", "_scales.usda"))
    if path:
        groomlab.export_usd(groom, path[0], scales="instancer")
        cmds.inViewMessage(amg="GroomLab: wrote %s" % path[0], pos="topCenter", fade=True)


def export_scales_fbx_dialog():
    """The scales at the current frame as an FBX mesh (Unreal's static or skeletal mesh import)."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="FBX (*.fbx)", dialogStyle=2, fileMode=0, caption="Export Scales Mesh",
                            startingDirectory=default_file(groom, "exports", "_scales.fbx"))
    if path:
        groomlab.export_scales(groom, path[0])
        cmds.inViewMessage(amg="GroomLab: wrote %s" % path[0], pos="topCenter", fade=True)


def bake_scale_maps_action():
    """The scales baked into the skin's normal, height and color maps, in the groom's bake folder."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    from groomlab.ui import messages
    with messages.busy("Baking the scale maps"):
        maps = groomlab.bake_scale_maps(groom)
    messages.done("baked the scale maps to %s (height %.4g)" % (maps["pattern"]["ScaleNormal"], maps["max_height"]))
    return maps


def export_groom_dialog():
    """A groom file for groomlab-cli, the Arnold procedural and the standalone app, with the scalp's
    animation cached; the groom is linked to it (Save to Groom File, Reload from Groom File)."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="GroomLab groom (*.groom)", dialogStyle=2, fileMode=0,
                            caption="Save Groom File As", startingDirectory=default_file(groom, "exports", ".groom"))
    if not path:
        return None
    start, end = _playback_range()
    written = _run_export("Saving the groom file and caching the scalp", path[0],
                          lambda: groomlab.export_groom(groom, path[0], scalp_cache=True, start=start, end=end),
                          "wrote %s (the scalp cached over %d frames)" % (path[0], _frames(start, end)))
    if written:
        cmds.setAttr(groom + ".groomFile", groomlab.portable_path(written[0]), type="string")
    return written


_GROOM_FILES = "GroomLab groom (*.groom)"


def import_xgen_window():
    """GroomLab > Import > From XGen...: the non-modal window listing the scene's XGen descriptions."""
    from groomlab.ui import xgen_import_window
    return xgen_import_window.show()


def import_yeti_window():
    """GroomLab > Import > From Yeti...: the non-modal window listing the scene's Yeti graphs."""
    from groomlab.ui import yeti_import_window
    return yeti_import_window.show()


def import_alembic_curves_dialog(path=None, groom=None):
    """GroomLab > Import > Alembic Curves...: an Alembic file's curves become the groom's guides (an
    ImportCurves node in place of its Guides node, rooted on the scalp; one undo step). With a mesh
    selected and no groom, a groom is made on the mesh first. Bake Node to Guides... makes them
    brushable guides. Returns the groom, None when nothing was imported."""
    import groomlab
    from groomlab.ui import messages
    if not groom:
        groom = groomlab.active_groom()
    meshes = [] if groom else cmds.ls(selection=True, dag=True, type="mesh", noIntermediate=True)
    if not groom and not meshes:
        messages.say("select a groom, or a scalp mesh to make one on, then Import > Alembic Curves... again")
        return None
    if not path:
        picked = cmds.fileDialog2(fileFilter="Alembic (*.abc)", dialogStyle=2, fileMode=1,
                                  caption="Import Alembic Curves as Guides")
        if not picked:
            return None
        path = picked[0]
    cmds.undoInfo(openChunk=True, chunkName="GroomLab Import Alembic Curves")
    try:
        with messages.busy("Reading the Alembic curves"):
            if not groom:
                ensure_scene_saved("this groom's painted masks, card bakes and exports")
                groom = groomlab.create_groom(meshes[0], fit=True)
                groomlab.pin_new_groom(groom)
            groomlab.import_guides_from_alembic(groom, path)
        cmds.select(groom)
    finally:
        cmds.undoInfo(closeChunk=True)
    messages.done("the guides now come from %s (Utilities > Bake Node to Guides... makes them brushable)"
                  % os.path.basename(path))
    return groom


def import_groom_dialog():
    """A groom from a groom file (Save Groom File As's, or one the standalone app saved): on the
    selected mesh when it is the file's scalp, else on a mesh made from the file."""
    import groomlab
    path = cmds.fileDialog2(fileFilter=_GROOM_FILES, dialogStyle=2, fileMode=1, caption="Import Groom File",
                            startingDirectory=groomlab.asset_folder("exports"))
    if not path or not ensure_scene_saved("this groom's painted masks and card bake"):
        return
    try:
        groom = groomlab.import_groom(path[0])
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: %s" % e)
        return
    groomlab.pin_new_groom(groom)
    cmds.inViewMessage(amg="GroomLab: imported %s from %s" % (groom.split("|")[-1], os.path.basename(path[0])),
                       pos="topCenter", fade=True)


def reload_groom_file(groom=None):
    """Reload from Groom File: the groom's graph, guides and maps from the file it is linked to (the
    app's edits), asked for when it is linked to none."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return
    path = groomlab.groom_file(groom)
    if not path or not os.path.isfile(path):
        picked = cmds.fileDialog2(fileFilter=_GROOM_FILES, dialogStyle=2, fileMode=1, caption="Reload from Groom File",
                                  startingDirectory=groomlab.asset_folder("exports"))
        if not picked:
            return
        path = picked[0]
    try:
        groomlab.reload_groom(groom, path)
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: %s" % e)
        return
    cmds.inViewMessage(amg="GroomLab: %s reloaded from %s" % (groom.split("|")[-1], os.path.basename(path)),
                       pos="topCenter", fade=True)


def save_groom_file(groom=None):
    """Save to Groom File: the groom written to the file it is linked to (asked for when none), for
    the standalone app to open."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return
    path = groomlab.groom_file(groom)
    if not path:
        picked = cmds.fileDialog2(fileFilter=_GROOM_FILES, dialogStyle=2, fileMode=0, caption="Save to Groom File",
                                  startingDirectory=default_file(groom, "exports", ".groom"))
        if not picked:
            return
        path = picked[0]
    from groomlab.ui import messages
    try:
        with messages.busy("Saving the groom file"):
            written = groomlab.save_groom(groom, path)
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: %s" % e)
        return
    cmds.inViewMessage(amg="GroomLab: wrote %s" % ", ".join(written), pos="topCenter", fade=True)


def render_from_groom_file_dialog():
    """Render through groomlab_procedural: export a groom file and point the groom at it."""
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="GroomLab groom (*.groom)", dialogStyle=2, fileMode=0,
                            caption="Render from Groom File", startingDirectory=default_file(groom, "caches", ".groom"))
    if not path:
        return None
    start, end = _playback_range()
    return _run_export("Writing the groom file for Arnold", path[0],
                       lambda: groomlab.render_from_groom_file(groom, path[0], start=start, end=end),
                       "Arnold renders %s from %s now (Render > Render as Curves goes back)" % (groom, path[0]))


RENDER_AS_CURVES_ITEM = "groomLabRenderAsCurvesItem"
RENDERMAN_ITEM = "groomLabRenderManProceduralItem"


def add_renderman_procedural_action():
    """Render > RenderMan Procedural: RenderMan for Maya renders the groom through groomlab_renderman
    (groomlab.renderman.add_renderman_procedural; experimental)."""
    from groomlab import renderman
    groom = _selected_groom()
    if groom:
        node = renderman.add_renderman_procedural(groom)
        cmds.inViewMessage(amg="GroomLab: %s renders the groom in RenderMan (%s)" % (node.split("|")[-1],
                                                                                    renderman.EXPERIMENTAL_NOTE),
                           pos="topCenter", fade=True, fadeStayTime=4000)


def _sync_renderman_item(parent):
    """The Render menu's RenderMan item, there while RenderMan for Maya's plugin is loaded. Never
    fails: the menu opens whatever happens."""
    try:
        from groomlab import renderman
        shown = cmds.menuItem(RENDERMAN_ITEM, exists=True)
        wanted = renderman.rfm_loaded()
        if wanted and not shown:
            kwargs = {"insertAfter": RENDER_AS_CURVES_ITEM} if cmds.menuItem(RENDER_AS_CURVES_ITEM, exists=True) else {}
            cmds.menuItem(RENDERMAN_ITEM, parent=parent, label="RenderMan Procedural (experimental)",
                          command=_guarded(add_renderman_procedural_action),
                          annotation="RenderMan for Maya renders the groom at render time through GroomLab's "
                                     "procedural: a RenderManProcedural node exporting the groom each frame, "
                                     "with a PxrMarschnerHair network (" + renderman.EXPERIMENTAL_NOTE + ")",
                          **kwargs)
        elif shown and not wanted:
            cmds.deleteUI(RENDERMAN_ITEM, menuItem=True)
    except Exception as e:  # noqa: BLE001 (a menu must open)
        cmds.warning("GroomLab: the RenderMan menu item: %s" % e)


def render_curves():
    import groomlab
    groom = _selected_groom()
    if groom:
        groomlab.render_curves(groom)


_SIMS = {}  # groom -> nodes made by Simulate Guides, for Remove Simulation (this scene only)
_SIMS_SCENE_JOBS = []


def _sims():
    """The live simulations of this scene: forgotten when a new scene is made or another opened
    (the nodes they name are gone with the old one)."""
    if not _SIMS_SCENE_JOBS:
        from maya.api import OpenMaya as om

        def forget(*_):
            _SIMS.clear()
        for kind in (om.MSceneMessage.kAfterNew, om.MSceneMessage.kAfterOpen):
            _SIMS_SCENE_JOBS.append(om.MSceneMessage.addCallback(kind, forget))
    return _SIMS


def add_simulate_node():
    """A Simulate node (the core's simulation, every host's) between the guides and Interpolate."""
    from groomlab import sim
    groom = _selected_groom()
    if groom:
        node = sim.add_simulate(groom)
        print("GroomLab: added %s to %s: play the timeline, or Simulate Range" % (node, groom))


def _main_progress_bar():
    """Maya's main progress bar ("" in batch, where there is none)."""
    if cmds.about(batch=True):
        return ""
    try:
        return mel.eval("$tmp = $gMainProgressBar")
    except RuntimeError:
        return ""


def _simulate_with_progress(groom, status, end=None):
    """Every Simulate node stepped frame by frame to `end` (the playback range's end by default)
    with Maya's main progress bar counting the frames (Esc cancels after the frame being made).
    Returns the frames stepped (fewer when cancelled)."""
    from groomlab import sim
    start, last = _playback_range()
    end = last if end is None else end
    first, end = int(round(start)), int(round(end))
    bar = _main_progress_bar()
    if bar:
        cmds.progressBar(bar, edit=True, beginProgress=True, isInterruptable=True, status=status,
                         minValue=0, maxValue=max(1, end - first + 1))
    frames = 0
    try:
        for frame in range(first, end + 1):
            if bar and cmds.progressBar(bar, query=True, isCancelled=True):
                break
            sim.simulate(groom, frame)
            frames += 1
            if bar:
                cmds.progressBar(bar, edit=True, step=1)
    finally:
        if bar:
            cmds.progressBar(bar, edit=True, endProgress=True)
    return frames


def simulate_range(groom=None):
    """The Simulate nodes stepped over the playback range into their cache (scrubbing reads it),
    the main progress bar counting the frames (`groom`, else the active one)."""
    groom = groom or _selected_groom()
    if groom:
        frames = _simulate_with_progress(groom, "GroomLab: simulating %s" % groom)
        _say("simulated %d frames of %s" % (frames, groom))
        return frames
    return 0


def bake_core_simulation(groom=None):
    """The Simulate nodes simulated over the playback range (the main progress bar counting the
    frames) and baked to the scene's caches folder: renders, exports and groom files read the
    baked frames (`groom`, else the active one)."""
    from groomlab import sim
    groom = groom or _selected_groom()
    if not groom:
        return {}
    frames = _simulate_with_progress(groom, "GroomLab: simulating %s for the bake" % groom)
    _, end = _playback_range()
    if frames < int(round(end)) - int(round(_playback_range()[0])) + 1:
        _say("simulation cancelled after %d frames; nothing baked" % frames)
        return {}
    written = sim.bake_simulation(groom)
    for node, path in written.items():
        print("GroomLab: baked %s of %s to %s" % (node, groom, path))
    _say("simulated %d frames and baked %d Simulate node(s) of %s" % (frames, len(written), groom))
    return written


def clear_core_simulation(groom=None):
    """The Simulate nodes' frames and bakes dropped: the next frame shown simulates again (`groom`,
    else the active one)."""
    from groomlab import sim
    groom = groom or _selected_groom()
    if groom:
        sim.clear_baked_simulation(groom)
        sim.invalidate_simulation(groom)


def simulate_guides_dialog():
    """nHair on one guide group; children follow the simulated guides."""
    import groomlab
    from groomlab import sim
    groom = _selected_groom()
    if not groom:
        return
    groups = [g["name"] for g in groomlab.list_guide_groups(groom)]
    if not groups:
        cmds.warning("GroomLab: the groom has no guides yet")
        return
    presets = sorted(sim.SIM_PRESETS)
    result = cmds.promptDialog(
        title="Simulate Guides",
        message="Guide group (%s) and preset (%s).\nThe hair collides with the scalp and settles over 10 "
                "frames of pre-roll." % (", ".join(groups), ", ".join(presets)),
        text="%s long" % groups[0], button=["Simulate", "Cancel"], defaultButton="Simulate",
        cancelButton="Cancel", dismissString="Cancel")
    if result == "Simulate":
        words = cmds.promptDialog(query=True, text=True).split()
        group = words[0] if words else groups[0]
        preset = words[1] if len(words) > 1 else "long"
        try:
            _sims()[groom] = sim.create_sim_curves(groom, group, preset=preset, collide=True, preroll=10)
        except (RuntimeError, ValueError) as e:
            cmds.warning("GroomLab: %s" % e)


def bake_simulation_dialog():
    """Cache the live nHair sim over the playback range and read the groom from the cache."""
    from groomlab import sim
    groom = _selected_groom()
    if not groom:
        return
    if groom not in _sims():
        cmds.warning("GroomLab: no live simulation on %s (Simulate Guides first)" % groom)
        return
    path = cmds.fileDialog2(fileFilter="Alembic (*.abc)", dialogStyle=2, fileMode=0, caption="Bake Simulation",
                            startingDirectory=default_file(groom, "caches", "_sim.abc"))
    if path:
        sim.bake_sim(groom, path[0], _sims().pop(groom))


def remove_simulation():
    from groomlab import sim
    groom = _selected_groom()
    if groom:
        sim.remove_sim(groom, _sims().pop(groom, None))
        sim.clear_guide_cache(groom)


def export_guides_dialog():
    """A guide group as Alembic curves (strandId = guide ID), to simulate in another package."""
    from groomlab import sim
    groom = _selected_groom()
    if not groom:
        return
    path = cmds.fileDialog2(fileFilter="Alembic (*.abc)", dialogStyle=2, fileMode=0, caption="Export Guides",
                            startingDirectory=default_file(groom, "caches", "_guides.abc"))
    if not path:
        return None
    start, end = _playback_range()
    return _run_export("Exporting the guides", path[0],
                       lambda: sim.export_guides(groom, path[0], "primary", start=start, end=end),
                       "wrote the guides to %s (%d frames)" % (path[0], _frames(start, end)))


def load_guide_cache_dialog():
    from groomlab import sim
    groom = _selected_groom()
    if not groom:
        return
    import groomlab
    path = cmds.fileDialog2(fileFilter="Alembic (*.abc)", dialogStyle=2, fileMode=1, caption="Load Guide Cache",
                            startingDirectory=groomlab.asset_folder("caches"))
    if path:
        sim.set_guide_cache(groom, path[0])


def _prompt(title, message, text=""):
    result = cmds.promptDialog(title=title, message=message, text=text, button=["OK", "Cancel"],
                               defaultButton="OK", cancelButton="Cancel", dismissString="Cancel")
    return cmds.promptDialog(query=True, text=True).strip() if result == "OK" else None


def _guarded(fn, what=None):
    """A menu item's or button's command: `fn()`, any failure said in plain words with a next step
    over the viewport and as a warning (groomlab.ui.messages), its traceback in the Script Editor;
    never a bare traceback. `what`: the action's name in the message (its label)."""
    what = (what or "").replace("...", "").strip() or "GroomLab"

    def run(*_):
        try:
            return fn()
        except Exception as e:  # noqa: BLE001 (a menu action never ends in a bare traceback)
            from groomlab.ui import messages
            messages.error(what, e)
            return None
    return run


def _item(label, fn, annotation=None, name=None, **kwargs):
    """A GroomLab menu item running `fn()` guarded (_guarded: errors in plain words)."""
    if annotation:
        kwargs["annotation"] = annotation
    args = (name,) if name else ()
    return cmds.menuItem(*args, label=label, command=_guarded(fn, label), **kwargs)


def mirror_selection():
    import groomlab
    groom = _selected_groom()
    if groom:
        groomlab.mirror_guide_selection(groom, "x")


def clear_selection():
    import groomlab
    groom = _selected_groom()
    if groom:
        groomlab.clear_guide_selection(groom)


def symmetry_sync(from_negative=False):
    import groomlab
    groom = _selected_groom()
    if groom:
        created = groomlab.symmetry_sync(groom, "x", from_negative=from_negative)
        cmds.inViewMessage(amg="GroomLab: symmetry synced %s (%d guides)"
                           % ("-X to +X" if from_negative else "+X to -X", len(created)), pos="topCenter", fade=True)


def toggle_density_heatmap():
    import groomlab
    groom = _selected_groom()
    if groom:
        on = cmds.getAttr(groom + ".strandColorMode") != groomlab.review.DENSITY_MODE
        groomlab.density_heatmap(groom, on)


def toggle_hud():
    from groomlab import review
    review.show_hud(not review.hud_visible())


HEATMAP_ITEM = "groomLabDensityHeatmapItem"
HUD_ITEM = "groomLabStrandCountHudItem"


def density_heatmap_on():
    """Whether the active groom shows the density heatmap (False with no groom)."""
    import groomlab
    groom = groomlab.active_groom()
    try:
        return bool(groom) and cmds.getAttr(groom + ".strandColorMode") == groomlab.review.DENSITY_MODE
    except (RuntimeError, ValueError, AttributeError):
        return False


def _sync_review_items(*_):
    """Review's check items as things are when the menu opens (the active groom's heatmap, the HUD).
    Never fails: the menu opens whatever happens."""
    try:
        from groomlab import review
        if cmds.menuItem(HEATMAP_ITEM, exists=True):
            cmds.menuItem(HEATMAP_ITEM, edit=True, checkBox=density_heatmap_on())
        if cmds.menuItem(HUD_ITEM, exists=True):
            cmds.menuItem(HUD_ITEM, edit=True, checkBox=bool(review.hud_visible()))
    except Exception as e:  # noqa: BLE001 (a menu must open)
        cmds.warning("GroomLab: the Review menu's check marks: %s" % e)


def make_turntable():
    import groomlab
    groom = _selected_groom()
    if groom:
        rig = groomlab.turntable(groom)
        cmds.lookThru(rig["camera"])
        _say("the viewport looks through %s now (Panels > Perspective to go back); play the timeline"
             % rig["camera"].split("|")[-1])


def studio_action(preset):
    """GroomLab > Studio > a preset: the studio round the active groom (else the studio's own),
    built or relit in place, and the viewport looking through its camera."""
    from groomlab import studio
    import groomlab
    groom = groomlab.active_groom() or studio.state().get("groom")
    if not groom or not cmds.objExists(groom):
        cmds.warning("GroomLab: select a groom")
        return
    rig = studio.studio(groom, preset=preset)
    if rig["camera"]:
        studio.look_through()
    _say("studio: %s; the viewport looks through %s (Render to see it in Arnold; Studio > Remove Studio puts "
         "things back)" % (studio.get_preset(preset)["label"], studio.CAMERA))


def look_through_studio():
    from groomlab import studio
    if not studio.look_through():
        cmds.warning("GroomLab: no studio camera yet: pick a GroomLab > Studio preset first")


def remove_studio_action():
    from groomlab import studio
    if studio.remove_studio():
        _say("studio removed: the head's material and the render settings are back as they were")
    else:
        cmds.warning("GroomLab: the scene has no studio")


_TURNTABLES = []  # (job, progress dialog, timer) of renders running


def render_turntable_dialog():
    """How long, how big and how good; then the turntable renders in the background with a
    progress window, and its folder opens when it is done."""
    import groomlab
    from groomlab.ui import qt
    from groomlab.ui.qt import QtCore, QtGui, QtWidgets
    from groomlab.ui import theme
    groom = _selected_groom()
    if not groom:
        return
    if not groomlab.scene_folder() and not ensure_scene_saved("turntable renders"):
        return
    if not cmds.file(query=True, sceneName=True):
        cmds.warning("GroomLab: save the scene first: the turntable renders the saved file")
        return
    dialog = QtWidgets.QDialog()
    dialog.setWindowTitle("Render Turntable")
    form = QtWidgets.QFormLayout(dialog)
    frames = QtWidgets.QSpinBox()
    frames.setRange(2, 1000)
    frames.setValue(120)
    step = QtWidgets.QComboBox()
    for label, value in (("Every frame", 1), ("Every 2nd frame", 2), ("Every 4th frame (quick)", 4)):
        step.addItem(label, value)
    size = QtWidgets.QComboBox()
    for label, value in (("960 x 540", (960, 540)), ("1920 x 1080", (1920, 1080)), ("720 x 720", (720, 720)),
                         ("480 x 270 (quick)", (480, 270))):
        size.addItem(label, value)
    quality = QtWidgets.QComboBox()
    quality.addItem("Preview", "preview")
    quality.addItem("Final", "final")
    form.addRow("Frames round", frames)
    form.addRow("Render", step)
    form.addRow("Size", size)
    form.addRow("Quality", quality)
    note = QtWidgets.QLabel("Saves the scene, then renders in Arnold in the background (Maya stays free) into "
                            "the scene's groomlab/<scene>/renders folder; with ffmpeg on the PATH, a turntable.mp4 too.")
    note.setWordWrap(True)
    note.setProperty("role", "about")
    form.addRow(note)
    buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
    buttons.button(QtWidgets.QDialogButtonBox.Ok).setText("Render")
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    form.addRow(buttons)
    theme.style(dialog)
    if qt.exec_(dialog) != QtWidgets.QDialog.Accepted:
        return
    width, height = size.currentData()
    try:
        job = groomlab.render_turntable(groom, frames=frames.value(), step=step.currentData(), width=width,
                                        height=height, quality=quality.currentData())
    except (RuntimeError, ValueError, OSError) as e:
        cmds.warning("GroomLab: %s" % e)
        return
    progress = _progress_window("GroomLab Turntable", "Rendering the turntable…", job.frames)
    timer = QtCore.QTimer()
    entry = (job, progress, timer)

    def tick():
        written, total, done = job.poll()
        progress.setValue(min(written, total))
        progress.setLabelText("Rendering the turntable: frame %d of %d" % (written, total))
        if progress.wasCanceled() and not done:
            job.cancel()
        if not done:
            return
        timer.stop()
        progress.close()
        if entry in _TURNTABLES:
            _TURNTABLES.remove(entry)
        if job.ok():
            cmds.inViewMessage(amg="GroomLab: turntable ready%s" % (" (turntable.mp4)" if job.movie else ""),
                               pos="topCenter", fade=True)
            QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(job.folder))
        elif not progress.wasCanceled():
            cmds.warning("GroomLab: the turntable render stopped early; see %s" % job.log)

    timer.timeout.connect(tick)
    timer.start(1000)
    _TURNTABLES.append(entry)
    from groomlab.ui import widgets
    widgets.at_exit(_drop_turntables)
    return job


def _drop_turntables():
    """Maya quits: the progress windows stop polling and go (the renders run on by themselves)."""
    from groomlab.ui import widgets
    for _, progress, timer in _TURNTABLES:
        if widgets.alive(timer):
            timer.stop()
        widgets.dispose(timer)
        widgets.dispose(progress)
    del _TURNTABLES[:]


_RENDER_JOBS = []  # (job, progress dialog, timer) of standalone renders (V-Ray, RenderMan) running


def _standalone_renderers():
    """The render dialogs, by name: what differs between Render > V-Ray Standalone, Render >
    RenderMan (prman) and Render > Maya Hardware 2.0 (groomlab.vray_render, groomlab.prman_render,
    groomlab.hardware2_render)."""
    import groomlab
    from groomlab import prman_render, prman_scene, vray_render, vray_scene
    exe = ".exe" if os.name == "nt" else ""
    return {
        "vray": {"title": "Render in V-Ray Standalone", "label": "V-Ray", "window": "GroomLab: V-Ray Standalone",
                 "exe": "vray" + exe, "filter": "V-Ray Standalone (vray%s)" % exe, "path": vray_render.vray_path,
                 "set_path": vray_render.set_vray_path, "not_found": vray_scene.NOT_FOUND,
                 "render": groomlab.render_vray, "busy": "Writing the V-Ray scene", "motion_blur": False,
                 "note": "Renders what the scene shows: the grooms (V-Ray's hair shader fitted to the Arnold look), the "
                         "meshes in their shaders' colors, the studio's (else the view's) camera and the Arnold area "
                         "lights and dome as V-Ray lights. vray.exe runs in the background (Maya stays free); the "
                         "images go to the scene's groomlab/<scene>/renders folder."},
        "prman": {"title": "Render in RenderMan (prman)", "label": "RenderMan", "window": "GroomLab: RenderMan",
                  "exe": "prman" + exe, "filter": "RenderMan's prman (prman%s)" % exe, "path": prman_render.prman_path,
                  "set_path": prman_render.set_prman_path, "not_found": prman_scene.NOT_FOUND,
                  "render": groomlab.render_prman, "busy": "Writing the RenderMan scene", "motion_blur": True,
                  "note": "Renders what the scene shows: the grooms through GroomLab's RenderMan procedural "
                          "(PxrMarschnerHair fitted to the Arnold look), the meshes in their shaders' colors, the "
                          "studio's (else the view's) camera and the Arnold area lights and dome as RenderMan lights. "
                          "prman runs in the background (Maya stays free); the images go to the scene's "
                          "groomlab/<scene>/renders folder. RenderMan is free for non-commercial use."},
        # Maya's own Hardware 2.0, in this Maya session (groomlab.hardware2_render): no renderer to
        # find, no meshes to choose (it draws the scene as the viewport does), frames one at a time.
        "hw2": {"title": "Render with Maya Hardware 2.0", "label": "Hardware 2.0", "window": "GroomLab: Maya Hardware 2.0",
                "exe": None, "render": functools.partial(groomlab.render_hardware2, wait=False, show=False),
                "busy": "Setting up the render", "motion_blur": False,
                "qualities": (("Preview (8 samples a pixel)", "preview"), ("Draft (4 samples, quick)", "draft"),
                              ("Final (16 samples)", "final")),
                "note": "Renders in this Maya with its own Hardware 2.0 (nothing to install): the scene as the viewport "
                        "draws it, the grooms at their render density with GroomLab's hair shading, anti-aliased; a "
                        "studio's Arnold lights stand in as Maya lights. Seconds a frame; Stop ends it between "
                        "frames. The images go to the scene's groomlab/<scene>/renders folder."},
    }


def _render_standalone_dialog(renderer):
    """Render > V-Ray Standalone, RenderMan (prman) or Maya Hardware 2.0 (any Maya version): which
    frames, how big and how good, where the renderer is (not for Hardware 2.0: Maya's own); then it
    renders with a progress window (Stop cancels) and the image opens in the Render View."""
    from groomlab import render_gather
    from groomlab.ui import qt, theme
    from groomlab.ui.qt import QtWidgets
    spec = _standalone_renderers()[renderer]
    if not render_gather.render_grooms():
        cmds.warning("GroomLab: no visible groom to render")
        return None
    dialog = QtWidgets.QDialog()
    dialog.setWindowTitle(spec["title"])
    form = QtWidgets.QFormLayout(dialog)
    frames = QtWidgets.QComboBox()
    start, end = _playback_range()
    now = cmds.currentTime(query=True)
    frames.addItem("Current frame (%g)" % now, (now, now))
    frames.addItem("Playback range (%g-%g)" % (start, end), (start, end))
    rstart, rend = cmds.getAttr("defaultRenderGlobals.startFrame"), cmds.getAttr("defaultRenderGlobals.endFrame")
    frames.addItem("Render range (%g-%g)" % (rstart, rend), (rstart, rend))
    width, height = render_gather.resolution()
    size = QtWidgets.QComboBox()
    for scale, note in ((1.0, ""), (0.5, " (half)"), (0.25, " (quarter, quick)")):
        w, h = max(16, int(width * scale)), max(16, int(height * scale))
        size.addItem("%d x %d%s" % (w, h, note), (w, h))
    quality = QtWidgets.QComboBox()
    for label, value in spec.get("qualities", (("Preview", "preview"), ("Draft (quick, noisy)", "draft"), ("Final", "final"))):
        quality.addItem(label, value)
    meshes = QtWidgets.QComboBox()
    meshes.addItem("Every visible mesh", "visible")
    meshes.addItem("The selection and the scalps", "selected")
    blur = QtWidgets.QCheckBox("Deformation blur over half a frame")
    path_row = QtWidgets.QHBoxLayout()
    path = QtWidgets.QLineEdit(spec["path"]() or "" if spec["exe"] else "")
    path.setPlaceholderText("%s not found: browse for it" % spec["exe"])
    browse = QtWidgets.QPushButton("Browse...")

    def pick():
        found = cmds.fileDialog2(fileFilter=spec["filter"], dialogStyle=2, fileMode=1, caption="Find " + spec["exe"])
        if found:
            path.setText(found[0])
    browse.clicked.connect(pick)
    path_row.addWidget(path)
    path_row.addWidget(browse)
    form.addRow("Frames", frames)
    form.addRow("Size", size)
    form.addRow("Quality", quality)
    if spec["exe"]:  # (Hardware 2.0 draws the scene as the viewport does: every visible mesh)
        form.addRow("Meshes", meshes)
    if spec["motion_blur"]:
        form.addRow("Motion blur", blur)
    if spec["exe"]:
        form.addRow(spec["exe"], path_row)
    note = QtWidgets.QLabel(spec["note"])
    note.setWordWrap(True)
    note.setProperty("role", "about")
    form.addRow(note)
    buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
    buttons.button(QtWidgets.QDialogButtonBox.Ok).setText("Render")
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    form.addRow(buttons)
    theme.style(dialog)
    if qt.exec_(dialog) != QtWidgets.QDialog.Accepted:
        return None
    chosen = path.text().strip()
    try:
        if spec["exe"] and chosen and os.path.normcase(chosen) != os.path.normcase(spec["path"]() or ""):
            spec["set_path"](chosen)
    except ValueError as e:
        cmds.warning("GroomLab: %s" % e)
        return None
    if spec["exe"] and not spec["path"]():
        cmds.warning("GroomLab: " + spec["not_found"])
        return None
    first, last = frames.currentData()
    w, h = size.currentData()
    options = {"motion_blur": blur.isChecked()} if spec["motion_blur"] else {}
    if spec["exe"]:
        options["meshes"] = meshes.currentData()
    from groomlab.ui import messages
    try:
        with messages.busy(spec["busy"]):
            job = spec["render"](start=first, end=last, width=w, height=h, quality=quality.currentData(), **options)
    except (RuntimeError, ValueError, OSError) as e:
        cmds.warning("GroomLab: %s" % e)
        return None
    return _watch_render(job, spec)


def render_vray_dialog():
    """Render > V-Ray Standalone (any Maya version): groomlab.render_vray from a dialog."""
    return _render_standalone_dialog("vray")


def render_prman_dialog():
    """Render > RenderMan (prman) (any Maya version): groomlab.render_prman from a dialog."""
    return _render_standalone_dialog("prman")


def render_hardware2_dialog():
    """Render > Maya Hardware 2.0 (any Maya version, no renderer needed): groomlab.render_hardware2
    from a dialog, frame by frame in this session."""
    return _render_standalone_dialog("hw2")


def _progress_window(title, text, maximum):
    """A background render's progress window: a tool window over Maya's (not a bare top-level one,
    which flashed as a small white box while Maya kept taking the front), a fixed width so new
    label texts don't resize it, shown once and then only updated, left open at the end until
    the caller closes it."""
    from groomlab.ui import theme, widgets
    from groomlab.ui.qt import QtCore, QtWidgets
    progress = QtWidgets.QProgressDialog(text, "Stop", 0, maximum, widgets._maya_main_window())
    progress.setWindowFlags(progress.windowFlags() | QtCore.Qt.Tool)
    progress.setWindowTitle(title)
    progress.setWindowModality(QtCore.Qt.NonModal)
    progress.setAutoClose(False)
    progress.setAutoReset(False)
    progress.setMinimumDuration(0)
    progress.setFixedWidth(theme.px(420))
    theme.style(progress)
    progress.setValue(0)
    progress.show()
    return progress


def _watch_render(job, spec):
    """A render's progress window (non-modal; Stop cancels it); the image in the Render View when
    it is done. A background renderer's job is polled twice a second; Hardware 2.0's renders a
    frame each time it is polled, as often as Maya is idle (its poll_interval)."""
    from groomlab import render_gather
    from groomlab.ui import widgets
    from groomlab.ui.qt import QtCore
    total = len(job.frames)
    label = spec["label"]
    progress = _progress_window(spec["window"], "Starting %s..." % label, 100 * total)
    timer = QtCore.QTimer()
    entry = (job, progress, timer)
    in_session = getattr(job, "poll_interval", None) is not None

    def tick():
        written, frames, done, percent = job.poll()
        progress.setValue(min(100 * frames, int(100 * written + (0 if done else percent))))
        if in_session:
            progress.setLabelText("%s: frame %d of %d" % (label, min(written + 1, frames), frames))
        else:
            progress.setLabelText("%s: frame %d of %d, %d%%" % (label, min(written + 1, frames), frames, int(percent)))
        if progress.wasCanceled() and not done:
            job.cancel()
            return
        if not done:
            return
        timer.stop()
        progress.close()
        if entry in _RENDER_JOBS:
            _RENDER_JOBS.remove(entry)
        images = job.images()
        if job.ok():
            render_gather.show_image(images[-1])
            _say("%s rendered %s" % (label, images[0] if len(images) == 1 else "%d frames into %s" % (len(images),
                                                                                                    job.folder)))
        elif job.cancelled:
            _say("%s stopped; %d of %d frames in %s" % (label, len(images), frames, job.folder))
        else:
            cmds.warning("GroomLab: %s stopped early: %s (see %s)" % (label, "; ".join(job.errors()) or "no image",
                                                                     job.log))

    timer.timeout.connect(tick)
    timer.start(getattr(job, "poll_interval", 500))
    _RENDER_JOBS.append(entry)
    widgets.at_exit(_drop_render_jobs)
    return job


def _drop_render_jobs():
    """Maya quits: the progress windows go (the renders run on by themselves)."""
    from groomlab.ui import widgets
    for _, progress, timer in _RENDER_JOBS:
        if widgets.alive(timer):
            timer.stop()
        widgets.dispose(timer)
        widgets.dispose(progress)
    del _RENDER_JOBS[:]


def vray_hook_action():
    """Render > V-Ray for Maya Hook (2024+, experimental): V-Ray's post-translate script renders the
    grooms (groomlab.vray_hook); picked again, it is taken out. In Maya 2022, or without V-Ray for
    Maya, a message says what is needed."""
    from groomlab import vray_hook
    try:
        if vray_hook.hook_enabled():
            vray_hook.disable_hook()
            _say("V-Ray for Maya no longer renders the grooms through GroomLab's hook")
            return False
        vray_hook.enable_hook()
    except vray_hook.VRayRequired as e:
        cmds.warning("GroomLab: %s; V-Ray Standalone (Render > V-Ray Standalone) renders the groom in any Maya" % e)
        return None
    _say("V-Ray for Maya renders the grooms at render time now (%s)" % vray_hook.EXPERIMENTAL_NOTE)
    return True


def vray_load_scene_action():
    """Render > Load as VRayScene (2024+, experimental): the groom as a .vrscene and a VRayScene node."""
    from groomlab import vray_hook
    groom = _selected_groom()
    if not groom:
        return None
    try:
        vray_hook.check()
        transform = _run_export("Exporting the V-Ray scene", None, lambda: vray_hook.export_and_load(groom),
                                "a VRayScene node renders the groom in V-Ray for Maya (%s)" % vray_hook.EXPERIMENTAL_NOTE)
    except vray_hook.VRayRequired as e:
        cmds.warning("GroomLab: %s; V-Ray Standalone (Render > V-Ray Standalone) renders the groom in any Maya" % e)
        return None
    return transform


def mirror_group_dialog():
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    text = _prompt("Mirror Group", "Root group to mirror from +X to -X:", "primary")
    if text:
        groomlab.mirror_group(groom, text, "x")


def promote_group_dialog():
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    text = _prompt("Promote Group", "Group to move one level up:")
    if text:
        groomlab.promote_group(groom, text)


def demote_group_dialog():
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    text = _prompt("Demote Group", "Group, then the sibling group to move it under (e.g. \"extra primary\"):")
    if text:
        words = text.split()
        if len(words) != 2:
            raise ValueError("type two group names: the group to move, a space, then the sibling group to move it "
                             "under (e.g. \"extra primary\"), and try again")
        groomlab.demote_group(groom, words[0], words[1])


def reparent_dialog():
    import groomlab
    groom = _selected_groom()
    if not groom:
        return
    text = _prompt("Reparent Selected Guides", "ID of the new parent guide:")
    if text:
        groomlab.reparent_guides(groom, text)




# ---------------------------------------------------------------------------
# Utilities (groomlab.utils). Each action takes its choices as arguments; a choice left None is
# asked in a small dialog, which only a click on the menu item gets to (scripts pass them all).


def _say(text):
    """A result: in the Script Editor, and over the viewport in a UI session."""
    print("GroomLab: " + text)
    if not cmds.about(batch=True):
        cmds.inViewMessage(amg="GroomLab: " + text, pos="topCenter", fade=True)


def delete_all_flow_arrows(groom=None):
    """GroomLab > Utilities > Delete All Flow Arrows: every flow arrow and sink off the groom (one undo
    step); said how many. Returns (arrows, sinks) removed, or None with no groom."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    arrows, sinks = groomlab.clear_flow(groom)
    if arrows or sinks:
        _say("deleted %d flow arrow%s and %d sink%s from %s" % (arrows, "" if arrows == 1 else "s", sinks,
                                                              "" if sinks == 1 else "s", groom.split("|")[-1]))
    else:
        _say("%s has no flow arrows" % groom.split("|")[-1])
    return arrows, sinks


def _ask(title, fields, note=None):
    """A small form: fields are (key, label, kind, value[, choices][, tip]) with kind "choice"
    (a drop-down), "edit" (a drop-down you may type in), "int", "float" or "bool". A choice may
    be (value, label): the label shown, the value returned. Returns {key: value} on OK, None on
    Cancel."""
    from groomlab.ui import qt
    from groomlab.ui.qt import QtWidgets
    from groomlab.ui import theme
    dialog = QtWidgets.QDialog()
    dialog.setWindowTitle(title)
    form = QtWidgets.QFormLayout(dialog)
    getters = {}
    for field in fields:
        key, label, kind, value = field[:4]
        choices = field[4] if len(field) > 4 else None
        tip = field[5] if len(field) > 5 else ""
        if kind in ("choice", "edit"):
            w = QtWidgets.QComboBox()
            w.setEditable(kind == "edit")
            for c in choices or []:
                value, text = c if isinstance(c, tuple) else (c, str(c))
                w.addItem(text, value)
            index = w.findData(value)
            if index >= 0:
                w.setCurrentIndex(index)
            elif kind == "edit":
                w.setEditText(str(value))
            getters[key] = (lambda w=w: w.currentText().strip()) if kind == "edit" else w.currentData
        elif kind == "int":
            w = QtWidgets.QSpinBox()
            w.setRange(1, 10000000)
            w.setValue(int(value))
            getters[key] = w.value
        elif kind == "float":
            w = QtWidgets.QDoubleSpinBox()
            w.setDecimals(3)
            w.setRange(0.001, 1000.0)
            w.setSingleStep(0.1)
            w.setValue(float(value))
            getters[key] = w.value
        else:
            w = QtWidgets.QCheckBox()
            w.setChecked(bool(value))
            getters[key] = w.isChecked
        w.setToolTip(tip)
        form.addRow(label, w)
    if note:
        text = QtWidgets.QLabel(note)
        text.setWordWrap(True)
        text.setProperty("role", "about")
        form.addRow(text)
    buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    form.addRow(buttons)
    theme.style(dialog)
    if qt.exec_(dialog) != QtWidgets.QDialog.Accepted:
        return None
    return {key: get() for key, get in getters.items()}


_OUTPUT = "(output)"


def _output_node(groom):
    """The graph's output node, whatever its name."""
    from groomlab import graph as gm
    try:
        return gm.GraphModel.from_groom(groom).output
    except gm.GraphError:
        return "output"


def _node_choices(groom):
    """"(output)" (the groom's output) first, then every node giving strands or guides but the
    output node itself (which "(output)" is)."""
    from groomlab import utils
    output = _output_node(groom)
    return [_OUTPUT] + [n for n in utils.bake_nodes(groom) if n != output]


def _selected_curve_nodes(groom):
    """Selected nodes holding curves (curves, or groups of them), the groom's own nodes aside."""
    own = set(cmds.ls([groom] + (cmds.listRelatives(groom, parent=True, fullPath=True) or []), long=True))
    out = []
    for node in cmds.ls(selection=True, long=True) or []:
        if node in own:
            continue
        if cmds.nodeType(node) == "nurbsCurve" or cmds.listRelatives(node, allDescendents=True, type="nurbsCurve"):
            out.append(node)
    return out


def guides_to_curves_action(groom=None, degree=1):
    """Guides to Curves: the selected guides, else every guide, as curves grouped by guide group."""
    from groomlab import utils
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    selection = bool(groomlab.selected_guides(groom))
    result = utils.guides_to_curves(groom, selection=selection, degree=degree)
    cmds.select(result["group"])
    _say("%d guide curves in %s (%s); edit them, then Curves to Guides brings them back"
         % (result["count"], result["group"].split("|")[-1], "the selected guides" if selection else "every guide"))
    return result


def curves_to_guides_dialog(group=None, update=True, curves=None, groom=None):
    """Curves to Guides: the selected curves (or groups of them) back into the groom."""
    from groomlab import utils
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    curves = curves or _selected_curve_nodes(groom)
    if not curves:
        cmds.warning("GroomLab: select curves (or their group) along with the groom")
        return None
    if group is None:
        roots = [g["name"] for g in groomlab.list_guide_groups(groom) if g["depth"] == 0] or ["primary"]
        answer = _ask("Curves to Guides", [
            ("group", "New guides into", "edit", roots[0], roots,
             "The root group curves without a guide ID (or with one the groom lacks) join; typed: a new group"),
            ("update", "Update tagged guides", "bool", update, None,
             "Curves made by Guides to Curves (tagged glGuideId) reshape their own guide in place")],
            note="Curves from Guides to Curves update their guides (same IDs; children follow); any other "
                 "curve becomes a new guide.")
        if not answer:
            return None
        group, update = answer["group"] or roots[0], answer["update"]
    result = utils.curves_to_guides(groom, curves, group=group, update=update)
    _say("%d guides reshaped, %d unchanged, %d new in %s"
         % (len(result["updated"]), len(result["unchanged"]), len(result["added"]), group))
    return result


def strands_to_curves_dialog(node=None, density=None, max_curves=None, attributes=None, degree=None, groom=None):
    """Strands to Curves: the groom's (or a node's) strands as Maya curves, capped."""
    from groomlab import utils
    groom = groom or _selected_groom()
    if not groom:
        return None
    if None in (node, density, max_curves, attributes, degree):
        answer = _ask("Strands to Curves", [
            ("node", "Strands of", "choice", node or _OUTPUT, _node_choices(groom), "The groom's output, or any node's"),
            ("density", "Density", "float", density or 1.0, None, "1: every strand the node makes"),
            ("max_curves", "At most", "int", max_curves or utils.DEFAULT_MAX_CURVES, None,
             "Beyond this many, strands are thinned by strand ID (as a lower density thins them)"),
            ("attributes", "Strand attributes", "bool", True if attributes is None else attributes, None,
             "glStrandId, glWidth and the strands' channels on each curve's shape"),
            ("degree", "Degree", "choice", degree or 1, [1, 3], "1: CVs on the strand's points; 3: a cubic through them")])
        if not answer:
            return None
        node, density, max_curves = answer["node"], answer["density"], answer["max_curves"]
        attributes, degree = answer["attributes"], answer["degree"]
    result = utils.strands_to_curves(groom, node=None if node in (None, _OUTPUT) else node, density=density,
                                     max_curves=max_curves, attributes=attributes, degree=int(degree))
    _say("%d curves in %s%s" % (result["count"], result["group"].split("|")[-1],
                                " (%d strands thinned to the cap)" % result["total"] if result["capped"] else ""))
    return result


def strands_to_mesh_dialog(node=None, style=None, sides=None, width_scale=None, max_strands=None, strand_uvs=None,
                           groom=None):
    """Strands to Mesh: tubes or ribbons along the strands, one mesh: a snapshot of this frame (a
    mesh following the animation: a StrandMesh node's Mesh in Scene)."""
    from groomlab import utils
    groom = groom or _selected_groom()
    if not groom:
        return None
    if None in (node, style, sides, width_scale, max_strands, strand_uvs):
        answer = _ask("Strands to Mesh", [
            ("node", "Strands of", "choice", node or _OUTPUT, _node_choices(groom), "The groom's output, or any node's"),
            ("style", "Style", "choice", style or "tube", list(utils.MESH_STYLES),
             "Tubes round each strand, or flat ribbons lying along the scalp"),
            ("sides", "Tube sides", "int", sides or 4),
            ("width_scale", "Width scale", "float", width_scale or 1.0, None, "Times the strands' widths"),
            ("max_strands", "At most", "int", max_strands or utils.DEFAULT_MAX_STRANDS, None,
             "Beyond this many strands, thinned by strand ID"),
            ("strand_uvs", "Per-strand UVs", "bool", bool(strand_uvs), None,
             "A second UV set, strandUVs: each strand in its own cell of a grid")])
        if not answer:
            return None
        node, style, sides = answer["node"], answer["style"], answer["sides"]
        width_scale, max_strands, strand_uvs = answer["width_scale"], answer["max_strands"], answer["strand_uvs"]
    result = utils.strands_to_mesh(groom, node=None if node in (None, _OUTPUT) else node, style=style, sides=sides,
                                   width_scale=width_scale, max_strands=max_strands, strand_uvs=strand_uvs)
    cmds.select(result["mesh"])
    _say("%s: %d strands as %ss, %d faces" % (result["mesh"].split("|")[-1], result["strands"], style, result["faces"]))
    return result


def bake_node_dialog(node=None, group=None, density=1.0, groom=None):
    """Bake Node to Guides: a graph node's strands (or guides) become editable guides."""
    from groomlab import utils
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    if node is None or group is None:
        nodes = _node_choices(groom)
        roots = [g["name"] for g in groomlab.list_guide_groups(groom) if g["depth"] == 0]
        answer = _ask("Bake Node to Guides", [
            ("node", "Node", "choice", node or _OUTPUT, nodes,
             "The groom's output, or any node's strands or guides, as they are now"),
            ("group", "Into group", "edit", group or "baked", ["baked"] + [r for r in roots if r != "baked"],
             "A root group: made when new, added to otherwise"),
            ("density", "Density", "float", density, None, "Of the node's strands (1: every one)")],
            note="Each strand becomes a primary guide you can brush, mirror and give children.")
        if not answer:
            return None
        node, group, density = answer["node"], answer["group"] or "baked", answer["density"]
    if node in (None, _OUTPUT):
        node = _output_node(groom)
    ids = utils.bake_guides(groom, node, group=group, density=density)
    _say("baked %d guides from %s into %s" % (len(ids), node, group))
    return ids


def transfer_dialog(scalp=None, fit=None, groom=None, maps=None, color_sets=None, method=None, markers=None,
                    keep_lengths=None):
    """Transfer Groom to Scalp: the groom onto another mesh (selected with it, or picked).

    Scripts pass the new `scalp` and a `method` (closest, bounds, markersAlign, markersWarp, uv;
    or `fit` for bounds) and it transfers at once (marker methods: `markers`, else the groom's
    stored ones). Without them the Transfer Groom window opens (never modal: marker points are
    picked in the viewport while it is open) and is returned."""
    from groomlab import utils
    groom = groom or _selected_groom()
    if not groom:
        return None
    if scalp is None or (fit is None and method is None):
        from groomlab.ui import transfer_window
        return transfer_window.show(groom, scalp=scalp)
    maps = True if maps is None else bool(maps)
    result = utils.transfer_to_scalp(groom, scalp, fit=bool(fit), maps=maps, color_sets=bool(color_sets), method=method,
                                     markers=markers, keep_lengths=bool(keep_lengths))
    _say(result["message"])
    if result["missing"]:
        cmds.warning("GroomLab: not carried onto the new scalp: %s" % ", ".join(result["missing"]))
    return result


def copy_graph_dialog(guides=None, maps=None, source=None, targets=None):
    """Copy Graph to Other Grooms: the active groom's look onto the other selected grooms."""
    from groomlab import utils
    import groomlab
    source = source or _selected_groom()
    if not source:
        return None
    if targets is None:
        mine = cmds.ls(source, long=True)
        targets = [g for g in cmds.ls(selection=True, dag=True, type=groomlab.GROOM_TYPE, long=True) or []
                   if g not in mine]
    if not targets:
        cmds.warning("GroomLab: select the grooms to copy %s's graph to (pin %s, or select it first)" % (source, source))
        return None
    if guides is None or maps is None:
        answer = _ask("Copy Graph to Other Grooms", [
            ("guides", "Guides", "choice", guides or "keep", list(utils.GUIDE_MODES),
             "keep: each groom keeps its guides; replace: they take %s's (moved onto their scalps)" % source),
            ("maps", "Copy painted maps", "bool", bool(maps), None,
             "Maps the graph reads that a groom lacks: copied onto a scalp of the same topology, projected by "
             "position onto another (texture maps as new images in the groom's masks folder)")],
            note="%d groom(s) take %s's graph and control values." % (len(targets), source))
        if not answer:
            return None
        guides, maps = answer["guides"], answer["maps"]
    report = utils.copy_graph(source, targets, guides=guides, maps=maps)
    missing = sorted({"%s (%s)" % (m, why) for r in report.values() for m, why in r["missing"].items()})
    _say("copied %s's graph to %d groom(s)" % (source, len(report)))
    if missing:
        cmds.warning("GroomLab: maps the graph reads that some grooms lack: " + "; ".join(missing))
    return report


def _count(section):
    return sum(len(entries) for kinds in section.values() for entries in kinds.values())


def collect_files_report(grooms=None):
    """Collect Files: the files the selected grooms (else every groom) use from outside the scene's
    folder copied into it and the grooms repointed (groomlab.collect_files). Reports in the Script
    Editor and over the viewport, never in a dialog. Returns the report (None for an untitled scene)."""
    import groomlab
    if grooms is None:
        grooms = cmds.ls(selection=True, dag=True, type=groomlab.GROOM_TYPE, long=True) or \
            cmds.ls(type=groomlab.GROOM_TYPE, long=True) or []
    if not grooms:
        cmds.warning("GroomLab: no grooms in the scene")
        return None
    try:
        report = groomlab.collect_files(grooms)
    except ValueError as e:
        cmds.warning("GroomLab: %s" % e)
        return None
    for groom, kinds in sorted(report["copied"].items()):
        for kind, entries in sorted(kinds.items()):
            for key, path in sorted(entries.items()):
                print("GroomLab: %s: %s %s -> %s" % (groom, kind, key, path))
    for groom, kinds in sorted(report["missing"].items()):
        for kind, entries in sorted(kinds.items()):
            for key, path in sorted(entries.items()):
                cmds.warning("GroomLab: %s: %s %s is missing (%s): Find Missing Files relinks it" % (groom, kind, key, path))
    _say("collected %d file(s) into the scene's folder; %d there already, %d missing (Script Editor)"
         % (_count(report["copied"]), report["inside"], _count(report["missing"])))
    return report


def find_missing_files_dialog(folder=None, grooms=None, recursive=True):
    """Find Missing Files: the files the selected grooms (else every groom) name that are missing
    (texture maps, graph params, guide caches) relinked to files of the same name in `folder` and
    its subfolders. The folder is asked (a folder dialog) only when None and something is
    missing. Reports in the Script Editor and over the viewport, never in a dialog. Returns
    groomlab.find_missing_files' report (None when cancelled)."""
    import groomlab
    if grooms is None:
        grooms = cmds.ls(selection=True, dag=True, type=groomlab.GROOM_TYPE, long=True) or \
            cmds.ls(type=groomlab.GROOM_TYPE, long=True) or []
    if not grooms:
        cmds.warning("GroomLab: no grooms in the scene")
        return None
    if not any(any(kinds.values()) for kinds in (groomlab.missing_files(g) for g in grooms)):
        _say("no missing files in %d groom(s)" % len(grooms))
        return {"relinked": {}, "still_missing": {}, "ambiguous": {}}
    if folder is None:
        picked = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder to look for the missing files in",
                                  startingDirectory=groomlab.scene_folder() or groomlab.project_root())
        if not picked:
            return None
        folder = picked[0]
    report = groomlab.find_missing_files(grooms, folder, recursive=recursive)
    for groom, kinds in sorted(report["relinked"].items()):
        for kind, entries in sorted(kinds.items()):
            for key, path in sorted(entries.items()):
                print("GroomLab: %s: %s %s -> %s" % (groom, kind[:-1], key, path))
    for groom, kinds in sorted(report["ambiguous"].items()):
        for kind, entries in sorted(kinds.items()):
            for key, candidates in sorted(entries.items()):
                cmds.warning("GroomLab: %s: %s %s: several files fit, none relinked: %s"
                             % (groom, kind[:-1], key, ", ".join(candidates)))
    for groom, kinds in sorted(report["still_missing"].items()):
        for kind, entries in sorted(kinds.items()):
            for key, path in sorted(entries.items()):
                cmds.warning("GroomLab: %s: %s %s: no %s in %s" % (groom, kind[:-1], key, os.path.basename(path), folder))
    _say("relinked %d missing file(s); %d still missing, %d with several matches (Script Editor)"
         % (_count(report["relinked"]), _count(report["still_missing"]), _count(report["ambiguous"])))
    return report


# ---- GroomLab > Add: the graph's building blocks from the menu -----------------------------------

FEATHER_DESIGNS = ("contour", "down", "flight", "afterfeather", "barred")


def feathers_form(groom, curves=()):
    """Add > Feathers' form for `groom` with `curves` selected: (fields, note) as _ask takes them.
    The choice reads "Scattered feathers (shaped by the guides)" or "One feather per guide
    (hand-placed)" (engine/ui/choices.json's featherSource), one per guide when curves are
    selected or the groom has a feathers group already; the guide group the hand-placed feathers
    go into is picked from the groom's groups or typed (a new one: flight_left, tail), the
    groom's feathers group first (default "feathers")."""
    import groomlab
    from groomlab.ui import definitions
    feather_groups = groomlab.feather_guide_groups(groom)
    group = feather_groups[0] if feather_groups else "feathers"
    groups = [group] + [g for g in feather_groups + [x["name"] for x in groomlab.list_guide_groups(groom)]
                        if g != group]
    groups = [g for i, g in enumerate(groups) if g not in groups[:i]]
    source = "guides" if (curves or feather_groups) else definitions.choice_default("featherSource")
    sources = definitions.choices("featherSource")
    fields = [
        ("design", "Design", "edit", "contour", list(FEATHER_DESIGNS),
         "One design, or several separated by commas (an assortment: contour, down)"),
        ("weights", "Weights", "edit", "", [], "The assortment's shares, one per design (3, 1); empty: equal"),
        ("source", definitions.get("choices/featherSource").get("label", "Feathers"), "choice", source,
         [(c["value"], c["label"]) for c in sources], "\n".join("%s: %s" % (c["label"], c["tip"]) for c in sources)),
        ("group", "Guide group", "edit", group, groups,
         "One feather per guide: the guide group whose guides are the feathers, left out of the hair (pick one, "
         "or type a new name: flight_left, flight_right, tail)")]
    if curves:
        note = "%d curve(s) selected: with One feather per guide each becomes a feather in the guide group." % len(curves)
    else:
        note = ("One feather per guide: each guide of the group is a feather. A group without guides yet: the "
                "Draw tool draws into it, every guide drawn from the skin a feather.")
    return fields, note


def draw_guides_into(groom, group):
    """The Draw tool drawing into guide group `group` of `groom` (the Groom Tools' group picked
    when they are open), so every guide drawn is one of the group's: a hand-placed feather. Not in
    batch mode (no viewport). Returns whether the tool started."""
    import groomlab
    if cmds.about(batch=True):
        return False
    from groomlab.ui import palette
    window = palette.current()
    try:
        if window is not None:
            window.fill_levels(groom, select=group)
            window.target_changed()
            window.activate("draw")
        else:
            groomlab.brush_tool("draw", group=group)
    except (RuntimeError, ValueError) as e:
        cmds.warning("GroomLab: cannot start the Draw tool: %s" % e)
        return False
    cmds.inViewMessage(amg="GroomLab: draw guides from the skin into <hl>%s</hl>: each one is a feather" % group,
                       pos="topCenter", fade=True)
    return True


def add_feathers_dialog(design=None, source=None, weights=None, groom=None, group=None):
    """Add > Feathers...: feathers on the groom (groomlab.add_feathers). `source` "scatter":
    scattered feathers shaped by the guides; "guides": one feather per guide of guide group
    `group` (default: the groom's feathers group, else "feathers"), the selected curves joining
    it first; with no curves selected and a group without guides, the Draw tool draws into it.
    `design` is one name, or several (an assortment, `weights` their shares). Asked in a small form
    (feathers_form) when not given (a menu click); scripted callers pass them."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    curves = cmds.ls(selection=True, dag=True, type="nurbsCurve", long=True) or []
    if design is None:
        fields, note = feathers_form(groom, curves)
        answer = _ask("Add Feathers", fields, note=note)
        if not answer:
            return None
        design = [d.strip() for d in answer["design"].split(",") if d.strip()]
        weights = [float(w) for w in answer["weights"].replace(",", " ").split()] or None
        source = answer["source"]
        group = answer["group"].strip() or None
    designs = [design] if isinstance(design, str) else list(design)
    defaults = groomlab.feather_guide_groups(groom)
    source = source or ("guides" if (curves or defaults) else "scatter")
    group = group or (defaults[0] if defaults else "feathers")
    made = groomlab.add_feathers(groom, designs if len(designs) > 1 else designs[0], source=source, weights=weights,
                                 curves=curves if source == "guides" and curves else None, group=group)
    if source == "guides":
        count = len(groomlab.guide_ids(groom, group))
        _say("added %s feathers to %s: one per guide of %s (%d now)" % (", ".join(designs), groom, group, count))
        if not count:
            cmds.select(groom, replace=True)
            draw_guides_into(groom, group)
    else:
        _say("added scattered %s feathers to %s" % (", ".join(designs), groom))
    return made


def add_braid_dialog(style=None, groom=None):
    """Add > Braid on Curves...: the selected curves (drawn from the scalp along the braid) become the
    braids group and a Braid node braids the strands along them (groomlab.add_braid). The style
    is asked when not given (a menu click)."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    curves = cmds.ls(selection=True, dag=True, type="nurbsCurve", long=True) or []
    if style is None:
        answer = _ask("Braid on Curves", [
            ("style", "Style", "choice", "three", ["three", "fishtail", "rope", "cornrow"],
             "three: a classic plait; fishtail; rope: two strands twisted; cornrow: fed along the scalp")],
            note="%d curve(s) selected become braid paths." % len(curves) if curves else
                 "No curves selected: the braids group's guides are used (draw them with the Draw tool).")
        if not answer:
            return None
        style = answer["style"]
    node = groomlab.add_braid(groom, curves=curves or None, style=style)
    _say("added %s (%s braid) to %s" % (node, style, groom))
    return node


def add_cards_action(lod, groom=None):
    """Add > Cards > Hero / Mid / Low: a Cards node before the output at that level of detail
    (groomlab.create_cards with its preset), shown in the groom's cards view."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    node = groomlab.create_cards(groom, lod=lod)
    _say("added %s (%s cards) to %s" % (node, lod, groom))
    return node


def add_collider_action(groom=None, mesh=None):
    """Add > Collider: the selected mesh that is not the groom's scalp becomes a collider of the
    groom (groomlab.add_collider: a ColliderMesh node and a Collide node before the output)."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    if mesh is None:
        scalp = cmds.ls(groomlab.scalp_mesh(groom) or "", long=True) or [""]
        meshes = [m for m in cmds.ls(selection=True, dag=True, type="mesh", noIntermediate=True, long=True) or []
                  if m != scalp[0]]
        if not meshes:
            cmds.warning("GroomLab: select a mesh (besides the groom) to collide with")
            return None
        mesh = meshes[0]
    name = groomlab.add_collider(groom, mesh)
    _say("added collider %s to %s" % (name, groom))
    return name


def add_wind_action(groom=None):
    """Add > Wind: a Wind node before the output (groomlab.add_wind); play the timeline to see it."""
    import groomlab
    groom = groom or _selected_groom()
    if groom:
        node = groomlab.add_wind(groom)
        _say("added %s to %s: play the timeline" % (node, groom))
        return node
    return None


def add_jiggle_action(groom=None):
    """Add > Jiggle: a Jiggle node before the output (groomlab.add_jiggle): strands lag behind
    the scalp's motion and settle."""
    import groomlab
    groom = groom or _selected_groom()
    if groom:
        node = groomlab.add_jiggle(groom)
        _say("added %s to %s: strands follow the scalp's motion" % (node, groom))
        return node
    return None


def export_card_maps_window(groom=None):
    """Export > Card Maps...: the Export Card Maps window on the active groom (never modal)."""
    from groomlab.ui import card_maps_window
    return card_maps_window.show(groom or _selected_groom())


def export_unreal_cards_dialog(folder=None, groom=None):
    """Export > Unreal Cards...: the card mesh as FBX and its maps (TGA, Unreal's hair card
    layout) into a folder (groomlab.export_cards_unreal); asked when not given (a menu click)."""
    import groomlab
    groom = groom or _selected_groom()
    if not groom:
        return None
    if folder is None:
        picked = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the Unreal cards",
                                  startingDirectory=groomlab.asset_folder("exports"))
        if not picked:
            return None
        folder = picked[0]
    result = groomlab.export_cards_unreal(groom, folder)
    _say("wrote %s for Unreal's hair cards" % result["fbx"])
    return result


# ---- Help ----------------------------------------------------------------------------------------

def docs_folder():
    """The docs folder shipped with the plugin (hosts/maya/scripts/../../../docs in a source tree,
    the plugin folder's docs beside an installed one), "" when none is found."""
    here = os.path.dirname(os.path.abspath(__file__))
    for up in (5, 4, 3, 2, 1):  # the source tree: scripts/groomlab/ui -> the checkout's docs
        folder = os.path.normpath(os.path.join(here, *([".."] * up), "docs"))
        if os.path.isfile(os.path.join(folder, "USER_GUIDE.md")):
            return folder.replace("\\", "/")
    return ""


def open_docs():
    """Help > Docs: the web docs (getgroomlab.com/docs); the docs folder shipped with the plugin when
    no browser opens them."""
    from groomlab import links
    try:
        opened = links.open_url(DOCS_URL)
    except Exception:
        opened = False
    if opened is not False:
        _say("opened the docs at %s" % DOCS_URL)
        return DOCS_URL
    folder = docs_folder()
    if folder:
        if hasattr(os, "startfile"):
            os.startfile(folder)  # Explorer on Windows
        else:
            cmds.showHelp(folder, absolute=True)
        _say("opened the docs folder %s" % folder)
        return folder
    cmds.showHelp(DOCS_URL, absolute=True)
    return DOCS_URL


def open_discord():
    """GroomLab > Help > Discord: the GroomLab community."""
    from groomlab import links
    return links.open_discord()


def open_donate():
    """GroomLab > Help > Support GroomLab: GroomLab is free for any use, funded by donations (Ko-fi)."""
    from groomlab import links
    url = links.open_donate()
    _say("opened %s: thank you for supporting GroomLab" % url)
    return url


def open_updates():
    """GroomLab > Help > Check for Updates...: this version, the newest one and its notes, Update Now."""
    from groomlab.ui import updates_window
    return updates_window.show()


def shortcuts_text():
    """Help > Keyboard Shortcuts: the table every host shares (groomlab.ui.shortcuts)."""
    from groomlab.ui import shortcuts
    return shortcuts.text()


def about_text():
    import groomlab
    version = ""
    try:
        version = cmds.groomLabVersion() or ""
    except (RuntimeError, AttributeError, TypeError):
        pass
    return ("GroomLab %s\n\nHair, fur and feather grooming for Maya: the Groom Tools for the guides, the Graph Editor "
            "for the look, exports to Arnold, Unreal, Unity and glTF, and a .groom file the standalone app shares.\n\n"
            "Free for any use, commercial included. Support GroomLab: %s\n\n"
            "Open source (Apache-2.0): %s\n\n"
            "Plugin folder: %s" % (version or "(development build)", DONATE_URL, REPO_URL,
                                   os.path.dirname(os.path.abspath(groomlab.__file__))))


def _text_window(name, title, text):
    """A small non-modal window with read-only text (Help > Keyboard Shortcuts, About)."""
    if cmds.window(name, exists=True):
        cmds.deleteUI(name)
    window = cmds.window(name, title=title, widthHeight=(560, 360), sizeable=True)
    cmds.columnLayout(adjustableColumn=True)
    cmds.scrollField(editable=False, wordWrap=False, text=text, height=340, font="fixedWidthFont")
    cmds.showWindow(window)
    return window


def show_shortcuts():
    """Help > Keyboard Shortcuts: the brush and Graph Editor keys in a small window."""
    return _text_window("groomLabShortcuts", "GroomLab: Keyboard Shortcuts", shortcuts_text())


def show_about():
    """Help > About GroomLab: the text and a Support GroomLab button (no modal dialog)."""
    name = "groomLabAbout"
    if cmds.window(name, exists=True):
        cmds.deleteUI(name)
    window = cmds.window(name, title="About GroomLab", widthHeight=(560, 380), sizeable=True)
    cmds.columnLayout(adjustableColumn=True)
    cmds.scrollField(editable=False, wordWrap=True, text=about_text(), height=330)
    cmds.button("groomLabAboutSupport", label="Support GroomLab", command=lambda *_: open_donate(),
                annotation="Opens %s in the browser" % DONATE_URL)
    cmds.showWindow(window)
    return window



def open_graph_editor():
    from groomlab.ui import graph_editor
    graph_editor.show()


def show_all(what, on):
    """GroomLab > Display (and the Groom Tools' eyes, Shift+click or right-click): every groom's hair
    (`what` "hair": the strands alone) or guides ("guides"), shown or hidden in one undo step; said
    in the viewport."""
    import groomlab
    grooms = (groomlab.set_all_guides_visible if what == "guides" else groomlab.set_all_hair_visible)(on)
    if cmds.about(batch=True):
        return grooms
    cmds.inViewMessage(amg="GroomLab: %s %s" % (
        "every groom's guides" if what == "guides" else "every groom's hair", "shown" if on else "hidden"),
        pos="topCenter", fade=True)
    return grooms


def install_hotkeys():
    """GroomLab > Install GroomLab Hotkeys: the GroomLab hotkey set, and a small window (not a dialog)
    listing what it changed: the set switched to and each key it rebound."""
    from groomlab.ui import hotkeys
    name = hotkeys.install()
    cmds.inViewMessage(amg="GroomLab: hotkey set '%s' is active. Hold Y + left click for the marking menu." % name,
                       pos="topCenter", fade=True)
    if not cmds.about(batch=True):
        _text_window("groomLabHotkeyChanges", "GroomLab: Hotkeys Installed", hotkeys.changes_text())
    return hotkeys.last_changes


def open_palette():
    from groomlab.ui import palette
    palette.show()


def open_guides_from_reference(groom=None):
    """The Photos window's Guides stage (Guides from Photos) on the active groom; returns it."""
    from groomlab.ui import reference_window
    groom = groom or _selected_groom()
    if not groom:
        return None
    return reference_window.show(groom, stage="guides")


MENU_ONLY, TOOLS_ONLY = "menu", "tools"


def _studio_entries(entry):
    """The Studio menus' presets (groomlab.studio's, in its order), the generator's tip filled per preset."""
    from groomlab import studio
    return [{"id": entry["action"], "value": name, "label": studio.get_preset(name)["label"],
             "tip": definitions.fill(entry["tip"], {"description": studio.get_preset(name)["description"]})}
            for name in studio.preset_names()]


# The generators the GroomLab menu expands (engine/ui/menus.json) and the values its texts name.
GENERATORS = {"studioPresets": _studio_entries}
VALUES = {"donateUrl": DONATE_URL}

# What each action id of the GroomLab menu and the Groom Tools' Output strip runs (engine/ui:
# actions.json says the labels and tips, menus.json the order). Every command is guarded (_item,
# _guarded): a failure is said in plain words with a next step, its traceback in the Script Editor.
ACTIONS = {
    "tools.open": open_palette,
    "graph.open": open_graph_editor,
    "groom.create": create_groom_from_selection,
    "sample.nika": lambda: add_sample_action("nika"),
    "sample.manny": lambda: add_sample_action("manny"),
    "sample.wolf": lambda: add_sample_action("wolf"),
    "sample.buddy": lambda: add_sample_action("buddy"),
    "photos.guides": open_guides_from_reference,
    "file.import": import_groom_dialog,
    "file.reload": reload_groom_file,
    "file.save": save_groom_file,
    "file.saveAs": export_groom_dialog,
    "import.xgen": import_xgen_window,
    "import.yeti": import_yeti_window,
    "import.alembicCurves": import_alembic_curves_dialog,
    "add.feathers": add_feathers_dialog,
    "add.braid": add_braid_dialog,
    "add.cards.hero": lambda: add_cards_action("hero"),
    "add.cards.mid": lambda: add_cards_action("mid"),
    "add.cards.low": lambda: add_cards_action("low"),
    "add.collider": add_collider_action,
    "add.wind": add_wind_action,
    "add.jiggle": add_jiggle_action,
    "sim.add": add_simulate_node,
    "sim.range": simulate_range,
    "sim.bake": bake_core_simulation,
    "sim.clear": clear_core_simulation,
    "nhair.simulate": simulate_guides_dialog,
    "nhair.bake": bake_simulation_dialog,
    "nhair.remove": remove_simulation,
    "nhair.exportGuides": export_guides_dialog,
    "nhair.loadCache": load_guide_cache_dialog,
    "display.showAllHair": lambda: show_all("hair", True),
    "display.hideAllHair": lambda: show_all("hair", False),
    "display.showAllGuides": lambda: show_all("guides", True),
    "display.hideAllGuides": lambda: show_all("guides", False),
    "review.heatmap": toggle_density_heatmap,
    "review.hud": toggle_hud,
    "review.turntable": make_turntable,
    "render.turntable": render_turntable_dialog,
    "studio.preset": studio_action,
    "studio.lookThrough": look_through_studio,
    "studio.remove": remove_studio_action,
    "render.fromGroomFile": render_from_groom_file_dialog,
    "render.asCurves": render_curves,
    "render.vrayStandalone": render_vray_dialog,
    "render.prman": render_prman_dialog,
    "render.hardware2": render_hardware2_dialog,
    "render.vrayHook": vray_hook_action,
    "render.vrayLoadScene": vray_load_scene_action,
    "export.alembic": lambda: export_curves_dialog("alembic"),
    "export.usd": lambda: export_curves_dialog("usd"),
    "export.vrscene": lambda: export_curves_dialog("vrscene"),
    "export.unrealGroom": export_unreal_dialog,
    "export.unrealCards": export_unreal_cards_dialog,
    "export.unityStrands": export_unity_strands_dialog,
    "export.unityFeathers": export_unity_feathers_dialog,
    "export.unityCards": export_unity_cards_dialog,
    "export.gltfCards": export_gltf_cards_dialog,
    "export.cardMaps": export_card_maps_window,
    "export.scalesFbx": export_scales_fbx_dialog,
    "export.scalesUsd": export_usd_scale_instancer_dialog,
    "guides.toCurves": guides_to_curves_action,
    "guides.curvesToGuides": curves_to_guides_dialog,
    "strands.toCurves": strands_to_curves_dialog,
    "strands.toMesh": strands_to_mesh_dialog,
    "node.bakeToGuides": bake_node_dialog,
    "scales.bakeMaps": bake_scale_maps_action,
    "flow.deleteAll": delete_all_flow_arrows,
    "groom.transfer": transfer_dialog,
    "graph.copyToGrooms": copy_graph_dialog,
    "files.findMissing": find_missing_files_dialog,
    "files.collect": lambda *_: collect_files_report(),
    "hotkeys.install": install_hotkeys,
    "help.docs": open_docs,
    "help.shortcuts": show_shortcuts,
    "help.updates": open_updates,
    "help.discord": open_discord,
    "help.donate": open_donate,
    "help.about": show_about,
}
# Maya's names for the items other code finds (the RenderMan item goes after Render as Curves; the
# Review check marks follow the groom as the menu opens), and the check items' state.
ITEM_NAMES = {"render.asCurves": RENDER_AS_CURVES_ITEM, "review.heatmap": HEATMAP_ITEM, "review.hud": HUD_ITEM}
CHECKED = {"review.heatmap": density_heatmap_on,
           "review.hud": lambda: bool(__import__("groomlab.review", fromlist=["review"]).hud_visible())}
# Submenus with a command as they open (made from the submenu's name), and the ones that tear off.
POST_MENU = {"review": lambda _menu: _sync_review_items,
             # RenderMan for Maya's item only while RfM is loaded (it may load after GroomLab).
             "render": lambda menu: (lambda *_: _sync_renderman_item(menu))}
TEAR_OFF = {"utilities"}


def shown_items(kind, where):
    """The Import, Export, Render or Studio items of one place (MENU_ONLY: the GroomLab menu's
    menus/<kind>, TOOLS_ONLY: the Groom Tools' Output strip's menus/output.<kind>): (label, fn,
    annotation, where, item name) per item, None for a divider (only between items)."""
    path = "menus/%s" % kind if where == MENU_ONLY else "menus/output.%s" % kind
    bind = definitions.binder(ACTIONS)
    return [None if item["kind"] == "divider" else
            (item["label"], bind(item), item["tip"], where, ITEM_NAMES.get(item["id"]))
            for item in definitions.resolve(definitions.get(path), values=VALUES, generators=GENERATORS)]


def _build(items):
    """GroomLab menu items (definitions.resolve's) under the current menu."""
    from groomlab.ui import widgets
    bind = definitions.binder(ACTIONS)
    for item in items:
        if item["kind"] == "divider":
            cmds.menuItem(divider=True)
        elif item["kind"] == "submenu":
            if item["id"] == "menus/theme":
                _theme_submenu()
                continue
            kwargs = {"annotation": item["tip"]} if item["tip"] else {}
            if item["id"] in TEAR_OFF:
                kwargs["tearOff"] = True
            sub = cmds.menuItem(label=item["label"], subMenu=True, **kwargs)
            if item["id"] in POST_MENU:
                cmds.menuItem(sub, edit=True, postMenuCommand=POST_MENU[item["id"]](sub))
            _build(item["items"])
            cmds.setParent("..", menu=True)
        elif item["kind"] == "action":
            kwargs = {}
            if item["icon"]:
                kwargs["image"] = widgets.icon_path(item["icon"])
            if item["checkable"]:
                kwargs["checkBox"] = CHECKED[item["id"]]()
            _item(item["label"], bind(item), item["tip"] or None, name=ITEM_NAMES.get(item["id"]), **kwargs)
        else:
            raise ValueError("the GroomLab menu has no generator %r" % item["name"])


def install():
    """The GroomLab menu (engine/ui/menus.json's main): the two windows, making a groom, and
    pipeline steps. Grooming itself lives in the Groom Tools; the look in the Graph Editor."""
    if cmds.about(batch=True):
        return
    if cmds.menu(MENU, exists=True):  # a rebuild (a new theme): the simulations' callbacks stay
        cmds.deleteUI(MENU)
    main = mel.eval("$tmp = $gMainWindow")
    cmds.menu(MENU, parent=main, label="GroomLab", tearOff=True)
    # "..." ends the items that open a dialog or a window to fill in; the others act at once.
    _build(definitions.resolve(definitions.get("menus/main"), values=VALUES, generators=GENERATORS))
    try:
        from groomlab.ui import hotkeys
        hotkeys.create_runtime_commands()
    except RuntimeError:
        pass
    try:  # once a session: an update's previous folder goes; at most daily, a quiet check for a new version
        from groomlab import updates
        updates.on_startup()
    except Exception:
        pass
    try:  # the theme's guide colors in the viewport; the menu's icons follow the theme
        from groomlab.ui import theme
        theme.apply_viewport()
        theme.on_change(_theme_changed)
    except (ImportError, RuntimeError, ValueError, IOError, OSError) as e:
        cmds.warning("GroomLab: theme not applied: %s" % e)


def _theme_submenu():
    """GroomLab > Theme (menus/theme): every theme (the active one checked); picking one restyles
    everything at once (the menu rebuilds itself after, so the switch waits for the click to finish)."""
    from groomlab.ui import widgets
    from maya import utils
    container = definitions.get("menus/theme")
    cmds.menuItem(label=container["label"], subMenu=True, annotation=container["tip"])
    cmds.radioMenuItemCollection()
    # The windows' Theme menus' items (widgets.theme_menu_items: one list); each runs after the click.
    for item in widgets.theme_menu_items():
        if item is None:
            cmds.menuItem(divider=True)
            continue
        label, run, checked, tip = item
        command = _guarded(lambda run=run: utils.executeDeferred(run), label)
        if checked is None:
            cmds.menuItem(label=label, command=command, annotation=tip or label)
        else:
            cmds.menuItem(label=label, radioButton=checked, command=command, annotation=tip)
    cmds.setParent("..", menu=True)


def _theme_changed(_theme):
    if cmds.menu(MENU, exists=True):
        install()


def uninstall():
    """The plugin unloads: the menu goes, and the scene callbacks behind Remove Simulation."""
    if cmds.menu(MENU, exists=True):
        cmds.deleteUI(MENU)
    if _SIMS_SCENE_JOBS:
        from maya.api import OpenMaya as om
        for job in _SIMS_SCENE_JOBS:
            try:
                om.MMessage.removeCallback(job)
            except RuntimeError:
                pass
        del _SIMS_SCENE_JOBS[:]
    _SIMS.clear()  # its grooms went with the scene (the plugin unloads only without them)
