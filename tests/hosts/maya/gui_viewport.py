# SPDX-License-Identifier: Apache-2.0
"""GUI budget test: orbiting the viewport around 200k preview strands (spec: >= 24 fps).

A suite of the GUI session (gui_runner.py, ctest maya.gui; harness: gui_support.py).

The orbit moves the camera and forces a redraw per frame, so the time is the
viewport draw alone: the groom is not re-evaluated while the camera moves.
"""

import json
import math
import os
import time

from maya import cmds

from gui_support import CLASSIC_GRAPH, check, finish, persp_panel, process_events, say, soon, to_view

FRAMES = 60
BUDGET_FPS = 24.0
GPU_PREVIEW_APIS = ("DirectX11", "OpenGLCoreProfile")  # where the GPU look preview runs (gpu_preview_check)


def start():
    soon(run)


def run():
    import groomlab

    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "live_drag":  # that case alone (a quick look while fixing it)
        run_steps(live_drag_steps(), finish)
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "async":  # commits off the UI thread alone
        run_steps(async_commit_steps(), finish)
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "gpu":  # the GPU look preview's cases alone
        gpu_preview_check()
        run_steps(async_commit_steps(), finish)
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "cards":  # the cards view alone (baked, MSAA, wireframe)
        cards_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "colors":  # the strands' own colors alone
        strand_colors_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "haircolor":  # HairColor's randomness alone
        hair_color_randomness_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "scalefreeze":  # the groom scaled 1 -> 0.1 -> 1 with freezes
        scale_freeze_shaders_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "freezecycle":  # frozen down and back up (a scene probe)
        freeze_cycle_scene_check(os.environ["GROOMLAB_PROBE_SCENE"], os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "strokescale":  # a stroke while scaled, then scaled back
        if os.environ.get("GROOMLAB_PROBE_SCENE"):
            stroke_scaled_scene_check(os.environ["GROOMLAB_PROBE_SCENE"], os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        else:
            stroke_scaled_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "colorscale":  # the hair color scaled with the scalp alone
        hair_color_scale_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        hair_color_scale_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"), deformed=True)
        hair_color_scale_presets_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "follow":  # following the scalp on the GPU alone
        follow_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "coverage":  # width as coverage's cases alone
        snapshots = os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS")
        coverage_check(snapshots)
        dense_coverage_check(snapshots)
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "hair":  # GroomLab's hair shading alone
        import gui_hair
        gui_hair.hair_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "eyes":  # the overlays and the Groom Tools' eyes alone
        overlay_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    if os.environ.get("GROOMLAB_VIEWPORT_CASE") == "lod":  # the screen-space level of detail alone
        lod_check(os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS"))
        finish()
        return
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=80, subdivisionsY=40)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    groomlab.set_param(groom, "scatter", "density", 16000.0)  # ~200k strands
    cmds.setAttr(groom + ".previewDensity", 1.0)
    count = cmds.getAttr(groom + ".outStrandCount")
    say("strands:", count)

    panel = persp_panel()
    cmds.setFocus(panel)
    cmds.viewFit("persp", allObjects=True)
    cmds.refresh(force=True)  # first draw builds the buffers

    start_time = time.time()
    for _ in range(FRAMES):
        cmds.orbit("persp", rotationAngles=(0, 6))
        cmds.refresh(currentView=True, force=True)
    seconds = time.time() - start_time
    fps = FRAMES / seconds
    say("[ budget ] viewport orbit: %d preview strands at %.1f fps (%d frames in %.2f s)"
        % (count, fps, FRAMES, seconds))
    check("orbiting %d preview strands draws at %.1f fps (budget %g, over 190000 strands)" % (count, fps, BUDGET_FPS),
          count > 190000 and fps >= BUDGET_FPS)
    # The same orbit with width as coverage off (opaque lines, as before it), then on again (the
    # first orbit above takes the first frames' warm-up: these two compare).
    cmds.setAttr(groom + ".displayWidthCoverage", False)
    cmds.refresh(currentView=True, force=True)
    start_time = time.time()
    for _ in range(FRAMES):
        cmds.orbit("persp", rotationAngles=(0, 6))
        cmds.refresh(currentView=True, force=True)
    say("[ budget ] viewport orbit with width as coverage off: %d preview strands at %.1f fps (reported only)"
        % (count, FRAMES / (time.time() - start_time)))
    cmds.setAttr(groom + ".displayWidthCoverage", True)
    cmds.refresh(currentView=True, force=True)
    start_time = time.time()
    for _ in range(FRAMES):
        cmds.orbit("persp", rotationAngles=(0, 6))
        cmds.refresh(currentView=True, force=True)
    say("[ budget ] viewport orbit with width as coverage on again (warm, as the one off): %.1f fps (reported only)"
        % (FRAMES / (time.time() - start_time)))

    # A tweak (other points, the same point counts) writes the lit strands' points into their GPU
    # buffers in place, the GPU writing their frames from them (gpu_lines.h; the CPU writes them
    # where the GPU cannot: groomLabGpuPreview -frames off); their indices and uvs stay (they depend
    # on the counts alone), but for new widths.
    snapshots = os.environ.get("GROOMLAB_VIEWPORT_SNAPSHOTS")
    points = json.loads(cmds.groomLabGraphStats(groom))["points"]
    rows = []
    for fraction in (0.8, 0.9, 0.85):
        groomlab.set_param(groom, "length", "keep", fraction)
        start_time = time.time()
        cmds.refresh(currentView=True, force=True)
        ms = (time.time() - start_time) * 1000.0
        rows.append((ms, json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))))
    if snapshots:
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=os.path.join(snapshots, "tweaked.png"))
    uploaded = [row[1]["draw"]["items"].get("groomLabStrands", {}).get("megabytes") for row in rows]
    gpu_frames = [row[1]["draw"]["items"].get("groomLabStrands", {}).get("gpuFrames") for row in rows]
    say("[ budget ] a tweak on %d preview strands (%d points) redraws in %s ms: compute %s ms, display update %s ms "
        "(build %s, upload %s; %s MB written) (reported only)"
        % (count, points, ", ".join("%.0f" % r[0] for r in rows),
           ", ".join("%.0f" % r[1]["computeMs"] for r in rows), ", ".join("%.0f" % r[1]["draw"]["updateMs"] for r in rows),
           ", ".join("%.0f" % r[1]["draw"]["buildMs"] for r in rows), ", ".join("%.0f" % r[1]["draw"]["uploadMs"] for r in rows),
           ", ".join("%.0f" % (u or 0) for u in uploaded)))
    # With the GPU's frames, the points alone (12 bytes a point) and the widths (4 bytes, width as
    # coverage reads them) where the new lengths gave other widths. Else three streams of 12 bytes
    # a point: the points, the normals and the tangents (no bitangent: the strands' hair graph
    # takes none, groomLabHairTubeSpecular); and the uvs (u, width: 8 bytes) with other widths.
    api = cmds.groomLabGpuPreview(drawApi=True)
    expected = (12, 16) if all(gpu_frames) else (36, 44)
    check("a tweak keeping the point counts uploads the lit strands' points (%s), and the widths where they changed "
          "(%s MB, %d points, frames by the GPU %s)" % ("alone, the GPU writing their frames" if all(gpu_frames) else "and frames",
                                                         uploaded, points, gpu_frames),
          all(u is not None and min(abs(u - points * expected[0] / 1e6), abs(u - points * expected[1] / 1e6)) < 0.01
              for u in uploaded))
    if cmds.about(windows=True) and api in GPU_PREVIEW_APIS:
        check("on %s the GPU writes the lit strands' frames (%s)" % (api, gpu_frames), all(gpu_frames))
        frames = json.loads(cmds.groomLabGpuPreview(groom, checkFrames=True))
        say("the lit strands' buffers against the CPU's:", frames)
        check("the lit strands' buffers hold the CPU's points, uvs and widths and its frames to rounding (%s)" % frames,
              frames.get("positions") == 0.0 and frames.get("u") == 0.0 and frames.get("widths") == 0.0
              and frames.get("normals", 1) < 1e-5 and frames.get("tangents", 1) < 1e-5)
        # Off, the CPU writes them, the same picture (to a level's rounding).
        if snapshots:
            cmds.groomLabGpuPreview(frames=False)
            cmds.refresh(currentView=True, force=True)
            cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                           percent=100, widthHeight=(480, 360), completeFilename=os.path.join(snapshots, "tweaked_cpu_frames.png"))
            cmds.groomLabGpuPreview(frames=True)
            cmds.refresh(currentView=True, force=True)

    # Strand colors and ribbons (VP-1, VP-3) draw, and redraw without re-evaluating.
    cmds.setAttr(groom + ".previewDensity", 0.1)
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "clump", "type": "Clump", "params": {"count": 60, "strength": 0.8}})
    graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
    graph["connections"] += [{"from": "resample", "to": "clump", "input": "strands"},
                             {"from": "clump", "to": "output", "input": "strands"}]
    groomlab.set_graph(groom, graph)
    cmds.polyColorPerVertex(scalp, rgb=(0.9, 0.3, 0.1), colorDisplayOption=True)
    cmds.viewFit("persp", allObjects=True)
    looks = [("solid", {}), ("colorSet", {"strandColorSource": "colorSet1"}),
             ("attribute", {"strandColorSource": "clumpId", "strandColorMax": 60.0}),
             ("random", {}), ("clump", {"strandColorSource": "clumpId"}), ("density", {}), ("ribbons", {}),
             ("lit_ribbons", {"strandColorMode": 0}), ("lit_self_shadow", {"displayShadows": 2})]
    # Lit looks: a shadowing light, so the lit items' shadow and AO passes draw too.
    light = cmds.directionalLight(intensity=1.0)
    cmds.setAttr(light + ".useDepthMapShadows", True)
    cmds.modelEditor(panel, edit=True, displayLights="all", shadows=True)
    cmds.setAttr("hardwareRenderingGlobals.ssaoEnable", True)
    for name, attrs in looks:
        if name.startswith("lit_"):
            pass
        elif name == "ribbons":
            cmds.setAttr(groom + ".strandColorMode", 4)
            cmds.setAttr(groom + ".displayRibbons", True)
            cmds.setAttr(groom + ".ribbonWidthScale", 3.0)
        else:
            cmds.setAttr(groom + ".strandColorMode",
                         ["solid", "colorSet", "attribute", "random", "clump", "density"].index(name))
        for attr, value in attrs.items():
            if isinstance(value, str):
                cmds.setAttr(groom + "." + attr, value, type="string")
            else:
                cmds.setAttr(groom + "." + attr, value)
        cmds.refresh(force=True)
        if name in ("ribbons", "lit_ribbons") and cmds.about(windows=True) and cmds.groomLabGpuPreview(drawApi=True) in GPU_PREVIEW_APIS:
            # The GPU makes the ribbons from the lines (two vertices a point): the CPU's triangles.
            made = json.loads(cmds.groomLabGpuPreview(groom, checkFrames=True) or "{}").get("ribbons") or {}
            item = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"].get("groomLabRibbons", {})
            check("%s: the GPU makes the ribbons from the lines, as the CPU builds them (%s; drawn %s)" % (name, made, item),
                  "why" not in made and made.get("positions", 1) < 1e-4 and made.get("normals", 0) < 1e-4
                  and made.get("tangents", 0) < 1e-4 and made.get("lit") == (name == "lit_ribbons"))
        if snapshots:
            cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                           percent=100, widthHeight=(480, 360), completeFilename=os.path.join(snapshots, name + ".png"))
    cmds.setAttr(groom + ".previewDensity", 1.0)
    start_time = time.time()
    for _ in range(FRAMES):
        cmds.orbit("persp", rotationAngles=(0, 6))
        cmds.refresh(currentView=True, force=True)
    say("[ budget ] ribbons at %d preview strands: %.1f fps (reported only)"
        % (cmds.getAttr(groom + ".outStrandCount"), FRAMES / (time.time() - start_time)))
    # Colored lines take a new color range by their colors alone (their lines kept): the picture
    # changes, and so does it for a change of their geometry's settings afterwards.
    import tempfile
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr(groom + ".displayShadows", 0)
    cmds.setAttr(groom + ".strandColorMode", 2)
    cmds.setAttr(groom + ".strandColorSource", "clumpId", type="string")
    cmds.setAttr(groom + ".strandColorMin", 0.0)
    cmds.setAttr(groom + ".strandColorMax", 4194304.0)  # clump numbers come from the centers' IDs: 0 to 2^22
    cmds.refresh(force=True)
    pictures, recolor = [], []
    for top in (4194304.0, 262144.0, 1048576.0):
        cmds.setAttr(groom + ".strandColorMax", top)
        start_time = time.time()
        cmds.refresh(currentView=True, force=True)
        recolor.append(time.time() - start_time)
        picture = os.path.join(tempfile.gettempdir(), "groomlab_recolor_%d.png" % int(top))
        cmds.playblast(frame=[cmds.currentTime(query=True)], format="image", compression="png", viewer=False,
                       showOrnaments=False, percent=100, widthHeight=(240, 180), completeFilename=picture)
        with open(picture, "rb") as f:
            pictures.append(f.read())
    rebuilt = []
    for scale in (2.5, 3.0):  # a setting of the strands' geometry: the lines are built again
        cmds.setAttr(groom + ".ribbonWidthScale", scale)
        start_time = time.time()
        cmds.refresh(currentView=True, force=True)
        rebuilt.append(time.time() - start_time)
    say("[ budget ] a new color range on %d strands: %s ms a redraw; lines built again: %s ms (reported only)"
        % (cmds.getAttr(groom + ".outStrandCount"), ", ".join("%.0f" % (t * 1000) for t in recolor[1:]),
           ", ".join("%.0f" % (t * 1000) for t in rebuilt)))
    check("a new color range re-colors the strand lines", pictures[0] != pictures[1] != pictures[2])
    start_time = time.time()
    for _ in range(FRAMES):
        cmds.orbit("persp", rotationAngles=(0, 6))
        cmds.refresh(currentView=True, force=True)
    say("[ budget ] colored lines at %d preview strands: %.1f fps (reported only)"
        % (cmds.getAttr(groom + ".outStrandCount"), FRAMES / (time.time() - start_time)))
    # Motion without a cache, played back through Maya's own evaluation (the evaluation manager
    # as the session has it): Wind, and a Jiggle that reads the scalp's earlier frames from
    # inside compute, on a scalp keyed to move. While playing, each earlier frame was the
    # current one before, so the scalp is read at other times only to start the history.
    cmds.setAttr(groom + ".previewDensity", 0.25)
    cmds.setAttr(groom + ".displayShadows", 0)  # the plain look: the playback time is the evaluation's
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr(groom + ".strandColorMode", 0)
    cmds.setAttr("hardwareRenderingGlobals.ssaoEnable", False)
    for frame, x in ((1, 0.0), (10, 0.0), (20, 2.0), (60, 2.0)):
        cmds.setKeyframe(scalp, attribute="translateX", time=frame, value=x)
    groomlab.add_wind(groom, strength=0.4)
    groomlab.add_jiggle(groom, history=10)
    cmds.playbackOptions(minTime=1, maxTime=60, loop="once", playbackSpeed=0, maxPlaybackSpeed=0)
    cmds.currentTime(1)
    cmds.refresh(force=True)
    reads = json.loads(cmds.groomLabGraphStats(groom))["scalpHistory"]["reads"]
    start_time = time.time()
    cmds.play(forward=True, wait=True)
    seconds = time.time() - start_time
    stats = json.loads(cmds.groomLabGraphStats(groom))
    played = cmds.currentTime(query=True)
    say("evaluation mode:", cmds.evaluationManager(query=True, mode=True))
    say("[ budget ] playback with Wind and Jiggle: %d preview strands, 59 frames at %.1f fps (reported only); "
        "the scalp read at %d other times; ended on frame %g" % (cmds.getAttr(groom + ".outStrandCount"),
                                                                59 / max(seconds, 1e-6),
                                                                stats["scalpHistory"]["reads"] - reads, played))
    # A jump into the move, stopped: the scalp's earlier frames are read again, in their own
    # contexts, from inside the groom's compute under the session's evaluation manager.
    cmds.currentTime(15)
    cmds.refresh(force=True)
    jumped = json.loads(cmds.groomLabGraphStats(groom))["scalpHistory"]["reads"] - stats["scalpHistory"]["reads"]
    say("jump to frame 15: the scalp read at %d other times; %d strands" % (jumped, cmds.getAttr(groom + ".outStrandCount")))
    check("motion playback: Wind and Jiggle play to the end, the scalp's history read only to start it and on a jump",
          played == 60 and stats["scalpHistory"]["frames"] == 10
          and stats["scalpHistory"]["reads"] - reads <= 20 and jumped == 10
          and cmds.getAttr(groom + ".outStrandCount") > 0 and not groomlab.errors(groom))

    # The review HUD draws the active groom's counts.
    cmds.select(groom)
    groomlab.show_hud(True)
    cmds.refresh(force=True)
    from groomlab import review
    hud = review.hud_text()
    say("hud:", hud)
    check("the review HUD shows the groom's counts", review.hud_visible() and "strands" in hud and "guides" in hud)
    groomlab.show_hud(False)
    say("errors:", groomlab.errors(groom))
    check("no errors on the groom", not groomlab.errors(groom))
    if snapshots:
        overlay_snapshots(snapshots)
    coverage_check(snapshots)
    dense_coverage_check(snapshots)
    lod_check(snapshots)
    true_width_check(snapshots)
    overlay_check(snapshots)
    clump_centers_check(snapshots)
    cards_check(snapshots)
    strand_colors_check(snapshots)
    hair_color_randomness_check(snapshots)
    hair_color_scale_check(snapshots)
    hair_color_scale_check(snapshots, deformed=True)
    stroke_scaled_check(snapshots)
    scale_freeze_shaders_check(snapshots)
    scales_check(snapshots)
    import gui_hair
    gui_hair.hair_check(snapshots)
    follow_check(snapshots)
    gpu_preview_check()
    # Maya's event loop runs between their steps.
    run_steps(async_commit_steps(), lambda: run_steps(live_drag_steps(), finish))


def follow_check(snapshots=None):
    """Following the scalp on the GPU (line_frames.h runFollow): a groom with Follow Scalp on a bent
    scalp draws its strands at rest carried by the GPU, frame by frame, as lit lines and as ribbons:
    each frame sends the scalp alone (not the strands' points), the drawn buffers hold what the CPU
    carries (groomLabGpuPreview -checkFrames, against the evaluated strands), and the lines' pictures
    are those of the CPU's carried strands (the GPU frames off: placed on the CPU, all written)."""
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    # (The drawing's paths compared picture for picture: the hair's own shadows, from the grid each path
    # writes when its strands move, aside; gui_hair checks them.)
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    groomlab.set_param(groom, "scatter", "density", 3000.0)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    groomlab.set_rest_mesh(groom)
    groomlab.set_follow_scalp(groom, True)
    bend = cmds.nonLinear(scalp, type="bend")[0]
    for frame, curvature in ((1, 0.0), (5, 70.0)):
        cmds.setKeyframe(bend, attribute="curvature", time=frame, value=curvature)
    cmds.select(clear=True)
    cmds.setAttr("persp.rotate", -24.0, 45.0, 0.0)
    cmds.viewFit("persp", allObjects=True)
    api = cmds.groomLabGpuPreview(drawApi=True)
    gpu = cmds.about(windows=True) and api in GPU_PREVIEW_APIS
    import tempfile
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_follow_")

    def shot(name):
        path = os.path.join(folder, "follow_%s.png" % name)
        cmds.playblast(frame=[cmds.currentTime(query=True)], format="image", compression="png", viewer=False,
                       showOrnaments=False, percent=100, widthHeight=(320, 240), completeFilename=path)
        return path

    for look, attrs in (("lines", {"displayRibbons": False}), ("ribbons", {"displayRibbons": True, "ribbonWidthScale": 3.0})):
        for attr, value in attrs.items():
            cmds.setAttr(groom + "." + attr, value)
        item = "groomLabRibbons" if look == "ribbons" else "groomLabStrands"
        rows, checks, gpu_shots = [], [], {}
        cmds.currentTime(1)
        cmds.refresh(currentView=True, force=True)
        for frame in (2, 3, 4, 5):
            cmds.currentTime(frame)
            cmds.refresh(currentView=True, force=True)
            drawn = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"].get(item, {})
            rows.append(drawn)
            frames = json.loads(cmds.groomLabGpuPreview(groom, checkFrames=True) or "{}")
            checks.append(frames.get("ribbons") if look == "ribbons" else frames)
            if frame in (3, 5):
                gpu_shots[frame] = shot("%s_%d" % (look, frame))
        points = json.loads(cmds.groomLabGraphStats(groom))["points"]
        say("follow %s: %d points; drawn %s; buffers against the CPU's carried strands %s" % (
            look, points, [(round(r.get("megabytes", 0), 3), r.get("follow"), r.get("gpuFrames")) for r in rows], checks))
        if not gpu:
            continue
        carried = all(r.get("follow") for r in rows[1:])
        check("follow %s: the GPU carries the strands at rest frame by frame, the scalp alone sent (%s MB for %d points)"
              % (look, [round(r.get("megabytes", 0), 3) for r in rows], points),
              carried and all(r.get("megabytes", 1e9) < points * 12 / 1e6 / 10 for r in rows[1:]))
        if look == "ribbons":
            ok = all(c and "why" not in c and c.get("positions", 1) < 1e-4 and c.get("normals", 1) < 1e-3
                     and c.get("tangents", 1) < 1e-3 for c in checks)
        else:
            ok = all(c.get("positions", 1) < 1e-4 and c.get("normals", 1) < 1e-3 and c.get("tangents", 1) < 1e-3
                     and c.get("u") == 0.0 and c.get("widths") == 0.0 for c in checks)
        check("follow %s: the drawn buffers hold the CPU's carried strands to rounding (%s)" % (look, checks), ok)
        # The CPU's: the GPU frames off, the strands carried and written by the CPU. (Ribbons: the
        # buffers above alone; without the GPU they are the CPU's ribbons along the scalp, not facing
        # the camera, another picture.)
        if look == "ribbons":
            continue
        cmds.groomLabGpuPreview(frames=False)
        same = True
        for frame in (3, 5):
            cmds.currentTime(frame)
            cmds.refresh(currentView=True, force=True)
            changed = differ(gpu_shots[frame], shot("%s_%d_cpu" % (look, frame)))
            say("follow %s, frame %d: %.5f of the pixels differ from the CPU's" % (look, frame, changed))
            same = same and changed < 0.002
        cmds.groomLabGpuPreview(frames=True)
        cmds.refresh(currentView=True, force=True)
        check("follow %s: the pictures are the CPU's carried strands'" % look, same)
    cmds.setAttr(groom + ".displayRibbons", False)
    check("follow: no errors on the groom (%s)" % groomlab.errors(groom), not groomlab.errors(groom))


def scales_check(snapshots=None):
    """The Dragon scales preset draws its scales on the device the session runs: the scales item
    (lit through its own graph: a fragment that does not register fails the session through
    FAILING_WARNINGS) covers the body in place of the strands, the preview quads too."""
    import tempfile
    import groomlab
    from groomlab import presets
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=10, subdivisionsX=60, subdivisionsY=30)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    presets.apply_preset(groom, "dragon_scales")
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.select(scalp)
    cmds.viewFit("persp", fitFactor=0.9)
    cmds.select(clear=True)
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_scales_")

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "scales_check_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    meshes = shot("meshes")
    items = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"]
    stats = json.loads(cmds.groomLabGraphStats(groom))
    cmds.setAttr(groom + ".displayInstances", False)
    hidden = shot("hidden")
    cmds.setAttr(groom + ".displayInstances", True)
    groomlab.set_param(groom, "scales", "preview", "quads")
    quads = shot("quads")
    api = cmds.groomLabGpuPreview(drawApi=True)
    # The share of the body's pixels (off the background, as its corner shows it) the scales change.
    from PySide2 import QtGui
    image = QtGui.QImage(hidden).convertToFormat(QtGui.QImage.Format_RGB32)
    data = bytes(image.constBits())[:image.byteCount()]
    background = data[0:3]
    body = sum(1 for i in range(0, len(data), 4) if abs(data[i] - background[0]) + abs(data[i + 1] - background[1])
               + abs(data[i + 2] - background[2]) > 24) / float(image.width() * image.height())
    shown, flat = differ(meshes, hidden) / max(body, 1e-6), differ(quads, hidden) / max(body, 1e-6)
    say("scales on %s: %s instances, %.0f%% of the body's pixels theirs (%s), quads %.0f%% (%s); items %s"
        % (api, stats.get("instances"), 100 * shown, meshes, 100 * flat, quads, sorted(items)))
    check("the Dragon scales preset draws its scales on %s (%.0f%% of the body)" % (api, 100 * shown),
          "groomLabScales" in items and body > 0.02 and shown > 0.6)
    check("its preview quads draw too on %s (%.0f%% of the body)" % (api, 100 * flat), flat > 0.6)
    check("the scales made no errors", not groomlab.errors(groom))
    cmds.file(new=True, force=True)
    scales_presets_check(snapshots)


def scales_presets_check(snapshots=None):
    """The scales presets driven by guides and flow on the device the session runs: a comb stroke's
    dabs redraw the scales of Scales from guides live (the scales item written on every dab), the
    stroke and flow arrows change the picture, and Swept spikes (follow strand) draws. The pictures'
    paths go to the log."""
    import tempfile
    import groomlab
    from groomlab import presets
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    curves = []
    for i in range(8):
        a = 2 * math.pi * i / 8
        x, z = 0.5 * math.cos(a), 0.5 * math.sin(a)
        curves.append(cmds.curve(degree=2, point=[(x, 0.87, z), (1.05 * x, 1.0, 1.05 * z), (1.1 * x, 1.12, 1.1 * z)]))
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    groomlab.guides_from_curves(groom, curves, tube_radius=0.12)
    cmds.delete(curves)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr("persp.translate", 0.0, 1.6, 4.2)
    cmds.setAttr("persp.rotate", -18, 0, 0)
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_scales_presets_")
    api = cmds.groomLabGpuPreview(drawApi=True)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "scales_preset_%s_%s.png" % (name, api))
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        say("scales preset picture:", path)
        return path

    presets.apply_preset(groom, "scales_from_guides")
    check("Scales from guides evaluates on the guides", not groomlab.errors(groom))
    guided = shot("from_guides")
    cmds.select(cmds.listRelatives(groom, parent=True)[0])
    ctx = groomlab.brush_tool("comb", radius=260, strength=1.0)
    x0, y0 = to_view((0.0, 0.9, 0.45))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="draw " + " ".join("%d %d" % (x0 + 20 * k, y0) for k in range(10)))
    steps = json.loads(cmds.groomLabBrushCtx(ctx, query=True, debugStrokeDraws=True))
    dabs = ["groomLabScales" in s["draw"]["items"] for s in steps[1:-1]]
    check("each dab of a comb stroke redraws the scales on %s (%s)" % (api, dabs), dabs and all(dabs))
    combed = shot("from_guides_combed")
    check("the combed guides turn the scales on %s (%.0f%% of the pixels)" % (api, 100 * differ(guided, combed)),
          differ(guided, combed) > 0.01)
    cmds.setToolTo("selectSuperContext")
    cmds.select(clear=True)

    presets.apply_preset(groom, "scales_from_flow")
    flat = shot("from_flow")
    groomlab.flow_stroke(groom, [(-0.8, 0.7, 0.4), (0.0, 0.85, 0.5), (0.8, 0.7, 0.4)], radius=1.5)
    flowed = shot("from_flow_arrows")
    check("flow arrows turn the scales on %s (%.0f%% of the pixels)" % (api, 100 * differ(flat, flowed)),
          differ(flat, flowed) > 0.01)

    presets.apply_preset(groom, "swept_spikes")
    spikes = shot("swept_spikes")
    items = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"]
    check("Swept spikes draws its spikes on %s" % api, "groomLabScales" in items and differ(flowed, spikes) > 0.01)
    check("the scales presets made no errors", not groomlab.errors(groom))
    cmds.file(new=True, force=True)


TAPER = {"keys": [[0.0, 1.0], [1.0, 0.0]]}  # Width's profile: tapering to nothing
COVERAGE_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"},
              {"name": "scatter", "type": "Scatter", "params": {"density": 12.0, "length": 3.0}},
              {"name": "width", "type": "Width", "params": {"width": 0.006, "profile": {"keys": [[0.0, 1.0], [1.0, 0.0]]}}},
              {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "width", "input": "strands"},
                    {"from": "width", "to": "output", "input": "strands"}],
}


def band_ink(path, rows):
    """How far a capture's pixels in each band of rows (fractions of its height, top 0) are from
    its background (the mean color of its top 4 rows, above the strands), mean over the band."""
    from PySide2 import QtGui
    image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
    w, h = image.width(), image.height()
    data = bytes(image.constBits())[:image.byteCount()]

    def pixel(x, y):
        i = (y * w + x) * 4
        return data[i + 2], data[i + 1], data[i]

    top = [pixel(x, y) for y in range(4) for x in range(w)]
    bg = [sum(p[c] for p in top) / float(len(top)) for c in range(3)]
    out = []
    for lo, hi in rows:
        y0, y1 = int(lo * h), int(hi * h)
        total = 0.0
        for y in range(y0, y1):
            for x in range(w):
                p = pixel(x, y)
                total += sum(abs(p[c] - bg[c]) for c in range(3))
        out.append(total / float(max((y1 - y0) * w, 1)))
    return out


def coverage_check(snapshots=None):
    """Width as coverage (displayWidthCoverage, on by default): the strand lines draw a pixel wide
    with each point's opacity its width's share of the pixel. Straight strands tapering to nothing
    (Width's profile 1 to 0, about half a pixel wide at the root), seen side on through an
    orthographic camera (roots at the bottom of the picture, tips at the top) and a perspective one:
    their tips draw lighter than their roots, flat, in the color modes' colors and lit; with it off
    the picture is today's (the widths read nowhere: as wide strands draw with it on, opaque). The
    coverage is hashed (each fragment kept where it reaches a threshold of its strand's own); with
    multisample anti-aliasing the fragments cover samples by their coverage (coverageDraws counts
    each kind). On the device the session runs (the suite runs again on DirectX 11)."""
    import tempfile
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polyPlane(width=4, height=4, subdivisionsX=8, subdivisionsY=8)[0]
    groom = groomlab.create_groom(scalp)
    # (The drawing's paths compared picture for picture: the hair's own shadows, from the grid each path
    # writes when its strands move, aside; gui_hair checks them.)
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    groomlab.set_graph(groom, COVERAGE_GRAPH)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_coverage_")
    api = cmds.groomLabGpuPreview(drawApi=True)
    panel = persp_panel()
    # Side on: the front camera (orthographic, down -Z) framing the strands (0 to 3 up) with room
    # above them for the background; then the perspective camera from the same side.
    cmds.setAttr("front.translate", 0.0, 1.5, 100.0)
    cmds.setAttr("front.rotate", 0.0, 0.0, 0.0)
    cmds.setAttr("frontShape.orthographicWidth", 3.0 * 1.25 * 480.0 / 360.0)
    cmds.modelPanel(panel, edit=True, camera="front")
    roots, tips = (0.70, 0.86), (0.16, 0.32)  # rows: the strands span 0.1 (tips) to 0.9 (roots)
    draws = lambda: json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["coverageDraws"]

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "coverage_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    modes = (("flat", {"displayLighting": False, "strandColorMode": 0}),
             ("colored", {"displayLighting": True, "strandColorMode": 3}),
             ("lit", {"displayLighting": True, "strandColorMode": 0}))
    for mode, attrs in modes:
        for attr, value in attrs.items():
            cmds.setAttr(groom + "." + attr, value)
        groomlab.set_param(groom, "width", "width", 0.006)
        groomlab.set_param(groom, "width", "profile", TAPER)
        cmds.setAttr(groom + ".displayWidthCoverage", True)
        before = draws()
        on = shot(mode + "_on")
        hashed = draws()["hashed"] - before["hashed"]
        cmds.setAttr(groom + ".displayWidthCoverage", False)
        off = shot(mode + "_off")
        groomlab.set_param(groom, "width", "width", 1.0)  # a unit wide, root to tip: every point a pixel or more
        groomlab.set_param(groom, "width", "profile", {"keys": [[0.0, 1.0], [1.0, 1.0]]})
        wide_off = shot(mode + "_wide_off")
        cmds.setAttr(groom + ".displayWidthCoverage", True)
        wide_on = shot(mode + "_wide_on")
        root, tip = band_ink(on, (roots, tips))
        off_root, off_tip = band_ink(off, (roots, tips))
        say("width as coverage, %s lines (%s): ink at the roots %.1f, tips %.1f (off: %.1f, %.1f); %d hashed draws; "
            "pictures %s, %s" % (mode, api, root, tip, off_root, off_tip, hashed, on, off))
        check("width as coverage on %s, %s lines: the tapered tips draw lighter than the roots (%.1f against %.1f), "
              "both lighter than with it off (%.1f, %.1f), hashed (%d draws)" % (api, mode, tip, root, off_tip, off_root, hashed),
              tip < 0.6 * root and root < 0.95 * off_root and tip < 0.5 * off_tip and hashed > 0)
        check("width as coverage off on %s, %s lines: the widths read nowhere, the picture as opaque lines draw it "
              "(%.4f of the pixels differ from the wide strands', %.4f from them with it on)"
              % (api, mode, differ(off, wide_off), differ(wide_off, wide_on)),
              differ(off, wide_off) == 0.0 and differ(wide_off, wide_on) < 0.002)
    # Perspective, from the same side: the coverage over the view depth.
    groomlab.set_param(groom, "width", "width", 0.006)
    groomlab.set_param(groom, "width", "profile", TAPER)
    cmds.setAttr(groom + ".displayLighting", True)
    cmds.setAttr(groom + ".strandColorMode", 0)
    cmds.setAttr("persp.translate", 0.0, 1.5, 9.0)
    cmds.setAttr("persp.rotate", 0.0, 0.0, 0.0)
    cmds.modelPanel(panel, edit=True, camera="persp")
    persp = shot("lit_persp")
    root, tip = band_ink(persp, ((0.62, 0.74), (0.26, 0.38)))
    check("width as coverage on %s through a perspective camera: the tips lighter than the roots (%.1f against %.1f)"
          % (api, tip, root), tip < 0.6 * root)
    # Multisample anti-aliasing: each fragment covers its share of the samples.
    cmds.modelPanel(panel, edit=True, camera="front")
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", True)
    before = draws()
    sampled_path = shot("lit_msaa")
    sampled = draws()["sampled"] - before["sampled"]
    root, tip = band_ink(sampled_path, (roots, tips))
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", False)
    check("width as coverage with multisample anti-aliasing on %s: drawn by samples (%d draws), the tips lighter "
          "than the roots (%.1f against %.1f)" % (api, sampled, tip, root), sampled > 0 and tip < 0.6 * root)
    # A Width drag the GPU previews writes its widths into the lines' uvs (in place on OpenGL, into
    # the preview's own uvs on DirectX 11): the coverage it draws is its commit's, every strand
    # drawn (displayClumpLod off: the level of detail chooses its strands from the CPU's, so a drag
    # draws the strands chosen for the widths before it, each widened to stand for the rest, and
    # its commit those chosen for the new ones: the same ink, other pixels); with the level of
    # detail on, the same ink.
    if cmds.about(windows=True) and api in GPU_PREVIEW_APIS:
        for lod in (False, True):
            cmds.setAttr(groom + ".displayClumpLod", lod)
            for mode, lit in (("flat", False), ("lit", True)):
                cmds.setAttr(groom + ".displayLighting", lit)
                groomlab.set_param(groom, "width", "width", 0.006)
                name = "gpu_%s%s" % (mode, "_lod" if lod else "")
                before = shot(name + "_before")
                taken = cmds.groomLabGpuPreview(groom, node="width", param="width", value=0.012)
                dragged = shot(name + "_drag")
                stats = json.loads(cmds.groomLabGpuPreview(groom, stats=True))
                cmds.groomLabGpuPreview(groom, endDrag=True)
                groomlab.set_param(groom, "width", "width", 0.012)
                committed = shot(name + "_commit")
                if not lod:
                    check("width as coverage on %s, %s lines: a Width drag on the GPU draws its commit's coverage (taken %s, "
                          "%d runs; %.4f of the pixels differ from the commit, %.4f from before)"
                          % (api, mode, taken, stats.get("runs", 0), differ(dragged, committed), differ(dragged, before)),
                          taken and stats.get("runs", 0) > 0 and differ(dragged, committed) < 0.01 and differ(dragged, before) > 0.02)
                else:
                    say("level of detail after the Width drag's commit: %s; coverage draws %s"
                        % (json.dumps(json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["lod"]), draws()))
                    ink_drag, ink_commit, ink_before = image_ink(dragged), image_ink(committed), image_ink(before)
                    # (GroomLab's hair shades the lit lines dark, as rendered: against the light background
                    # the commit's blended segments and the drag's hashed ones differ by more ink.)
                    tolerance = 0.12 if lit else 0.1
                    check("width as coverage on %s, %s lines, level of detail on: a Width drag on the GPU draws its commit's "
                          "ink (taken %s; %.3f against %.3f, %.3f before)" % (api, mode, taken, ink_drag, ink_commit, ink_before),
                          taken and abs(ink_drag - ink_commit) <= tolerance * ink_commit and ink_drag > 1.2 * ink_before)
        cmds.setAttr(groom + ".displayClumpLod", True)
    check("width as coverage made no errors", not groomlab.errors(groom))
    cmds.modelPanel(panel, edit=True, camera="persp")
    cmds.file(new=True, force=True)


def image_ink(path):
    """How far a capture's pixels are from its background (the mean color of its top 4 rows), mean
    over the whole picture."""
    return band_ink(path, ((0.0, 1.0),))[0]


def dense_coverage_check(snapshots=None):
    """Width as coverage on a dense groom (100k strands on a sphere, thin and tapered, at full
    density) seen from afar and from nearer: the strands crossing a pixel each cover their share of
    it (hashed, depth written), so the hair keeps about the ink opaque lines (coverage off) give it
    instead of fading to its front strand's coverage (blended with depth writes it kept half the
    ink flat, under a third lit). The pattern holds: the same frame twice draws the same picture,
    and a small orbit changes few pixels. A thinned preview (density 0.25) reads as dense as the
    full one with displayCoverageDensity, lighter without."""
    import tempfile
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    graph = json.loads(json.dumps(COVERAGE_GRAPH))
    graph["nodes"][1]["params"] = {"density": 8000.0, "length": 1.0}  # ~100k strands
    graph["nodes"][2]["params"]["width"] = 0.0015
    groomlab.set_graph(groom, graph)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr(groom + ".strandColorMode", 0)
    cmds.setAttr(scalp + ".visibility", False)  # the hair over the background alone
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_dense_coverage_")
    api = cmds.groomLabGpuPreview(drawApi=True)
    panel = persp_panel()
    cmds.modelPanel(panel, edit=True, camera="persp")
    grid = cmds.modelEditor(panel, query=True, grid=True)
    cmds.modelEditor(panel, edit=True, grid=False)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "dense_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    def frame(distance):
        cmds.setAttr("persp.translate", 0.0, 0.0, distance)
        cmds.setAttr("persp.rotate", 0.0, 0.0, 0.0)

    # (Strands 0.0015 wide cover 0.03 to 0.07 of a pixel here: drawn at their own widths since the
    # coverage floor went (display.h kLineCoverageFloor, 0.05 before), they keep less of the opaque
    # lines' ink than the floor made them, and still far more than the front strand's alone.)
    for mode, lit, least in (("flat", False, 0.7), ("lit", True, 0.5)):
        cmds.setAttr(groom + ".displayLighting", lit)
        for distance in (20.0, 8.0):
            frame(distance)
            cmds.setAttr(groom + ".displayWidthCoverage", True)
            on = shot("%s_%d_on" % (mode, distance))
            cmds.setAttr(groom + ".displayWidthCoverage", False)
            off = shot("%s_%d_off" % (mode, distance))
            ink_on, ink_off = image_ink(on), image_ink(off)
            check("width as coverage on %s, %s lines, %d strands from %g units: the dense hair keeps its ink "
                  "(%.2f against %.2f opaque: %.0f%%, at least %.0f%%)"
                  % (api, mode, cmds.getAttr(groom + ".outStrandCount"), distance, ink_on, ink_off,
                     100.0 * ink_on / max(ink_off, 1e-6), 100.0 * least),
                  ink_on >= least * ink_off)
    # The pattern holds: the same frame twice; a small orbit, against opaque lines'.
    cmds.setAttr(groom + ".displayLighting", False)
    moved = {}
    for on in (True, False):
        cmds.setAttr(groom + ".displayWidthCoverage", on)
        frame(8.0)
        first, second = shot("still_a_%d" % on), shot("still_b_%d" % on)
        cmds.orbit("persp", rotationAngles=(0, 0.2))
        orbited = shot("orbit_%d" % on)
        moved[on] = (differ(first, second), differ(first, orbited))
    check("width as coverage on %s: the same frame draws the same picture (%.4f of the pixels differ), a 0.2 degree "
          "orbit changes %.4f of them (opaque lines %.4f)" % (api, moved[True][0], moved[True][1], moved[False][1]),
          moved[True][0] == 0.0 and moved[True][1] < 0.03)
    # A thinned preview, from afar: as dense as the full one when compensated.
    cmds.setAttr(groom + ".displayWidthCoverage", True)
    frame(20.0)
    full = image_ink(shot("density_1"))
    cmds.setAttr(groom + ".previewDensity", 0.25)
    thinned = image_ink(shot("density_025"))
    cmds.setAttr(groom + ".displayCoverageDensity", False)
    plain = image_ink(shot("density_025_plain"))
    cmds.setAttr(groom + ".displayCoverageDensity", True)
    check("width as coverage on %s at preview density 0.25: as dense as the full preview with displayCoverageDensity "
          "(%.2f against %.2f), lighter without (%.2f)" % (api, thinned, full, plain),
          abs(thinned - full) < 0.05 * full and plain < 0.95 * thinned)
    check("width as coverage on a dense groom made no errors", not groomlab.errors(groom))
    cmds.modelEditor(panel, edit=True, grid=grid)
    cmds.file(new=True, force=True)


def lod_check(snapshots=None):
    """The screen-space level of detail (displayClumpLod, strand_lod.h) on a clumped groom (the
    hair_basics preset on a radius-10 sphere, ~120k strands), lit lines with width as coverage, the
    groom's bounding sphere across 3.1, 0.75, 0.19 and 0.0625 of the view (the research's near, mid,
    far and very far): mid and farther draw a share of the strands, very far some clumps as one line
    (impostors), near draws every strand with the blendable segments' second pass; the ink stays
    within a tenth of every strand's (LOD off); a still view draws the same picture twice; an orbit
    keeps the strands drawn (no indices written) and changes few pixels; the colored lines, a
    multisampled target, a GPU preview drag and the groom selected draw with it; ribbons and
    displayClumpLod off do not."""
    import tempfile
    import groomlab
    from groomlab import presets
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=10, subdivisionsX=60, subdivisionsY=30)[0]
    groom = groomlab.create_groom(scalp)
    presets.apply_preset(groom, "hair_basics")
    groomlab.set_param(groom, "scatter", "density", 120000.0 / (4.0 * math.pi * 100.0))
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".previewPointBudget", 0.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr(groom + ".displayLighting", True)
    cmds.setAttr(groom + ".displayWidthCoverage", True)
    cmds.setAttr(scalp + ".visibility", False)  # the hair over the background alone
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_lod_")
    api = cmds.groomLabGpuPreview(drawApi=True)
    strands = cmds.getAttr(groom + ".outStrandCount")
    panel = persp_panel()
    cmds.modelPanel(panel, edit=True, camera="persp")
    grid = cmds.modelEditor(panel, query=True, grid=True)
    cmds.modelEditor(panel, edit=True, grid=False)
    msaa = cmds.getAttr("hardwareRenderingGlobals.multiSampleEnable")
    # The camera as it was (the suites after this one frame their own grooms with it).
    camera = {"translate": cmds.getAttr("persp.translate")[0], "rotate": cmds.getAttr("persp.rotate")[0],
              "centerOfInterest": cmds.getAttr("persp.centerOfInterest"),
              "near": cmds.camera("persp", query=True, nearClipPlane=True), "far": cmds.camera("persp", query=True, farClipPlane=True)}

    def lod():
        return json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["lod"]

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "lod_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    def frame(fill):
        box = cmds.exactWorldBoundingBox(cmds.listRelatives(groom, parent=True)[0])
        center = [(box[i] + box[i + 3]) / 2.0 for i in range(3)]
        radius = 0.5 * math.sqrt(sum((box[i + 3] - box[i]) ** 2 for i in range(3)))
        fov = math.radians(cmds.camera("persp", query=True, verticalFieldOfView=True))
        distance = radius / (fill * math.tan(0.5 * fov))
        cmds.setAttr("persp.rotate", -15.0, 30.0, 0.0)
        direction = (math.sin(math.radians(30.0)) * math.cos(math.radians(15.0)), math.sin(math.radians(15.0)),
                     math.cos(math.radians(30.0)) * math.cos(math.radians(15.0)))
        cmds.setAttr("persp.translate", *[center[i] + direction[i] * distance for i in range(3)])
        cmds.setAttr("persp.centerOfInterest", distance)
        cmds.camera("persp", edit=True, nearClipPlane=max(0.01, 0.01 * distance), farClipPlane=10.0 * distance + 10.0 * radius)

    def both(name):
        """A picture with the level of detail on (and its stats), and one with it off."""
        cmds.setAttr(groom + ".displayClumpLod", True)
        on = shot(name + "_on")
        stats = lod()
        cmds.setAttr(groom + ".displayClumpLod", False)
        off = shot(name + "_off")
        cmds.setAttr(groom + ".displayClumpLod", True)
        return on, off, stats

    def inks(name, views):
        """The mean ink with the level of detail on and off over `views` pictures a degree of orbit
        apart (a groom a few dozen pixels across is a few hundred hashed pixels: one picture's ink
        is as noisy as a tenth)."""
        total_on = total_off = 0.0
        for v in range(views):
            on, off, stats = both(name if v == 0 else "%s_%d" % (name, v))
            total_on += image_ink(on)
            total_off += image_ink(off)
            cmds.orbit("persp", rotationAngles=(0, 1.0))
        return total_on / views, total_off / views, stats

    framings = (("near", 3.1), ("mid", 0.75), ("far", 0.19), ("veryfar", 0.0625))
    for name, fill in framings:
        frame(fill)
        lines_on = None
        if name in ("far", "veryfar"):  # the same view with thinned lines alone (the impostors off)
            cmds.optionVar(intValue=("groomLabLodImpostors", 0))
            lines_on, lines_off, lines_stats = inks(name + "_lines", 6)
            cmds.optionVar(remove="groomLabLodImpostors")
            frame(fill)
            say("level of detail on %s, %s without impostors: %d strands drawn, the ink %.3f against every strand's %.3f"
                % (api, name, lines_stats["drawnStrands"], lines_on, lines_off))
        ink_on, ink_off, stats = inks(name, 1 if name in ("near", "mid") else 6)
        say("level of detail on %s, %s: %s" % (api, name, json.dumps(stats, sort_keys=True)))
        # Very far (the head ten pixels across) every strand drawn here reads about a fifth lighter
        # than the thinned strands and the impostors, which agree with each other; on the CPU
        # harness at that scale (docs/research/clump_lod.md: --sphere --tiny, this groom) the level of detail is within
        # 4% of the physical reference and every strand 11% over it. There: the impostors within a
        # tenth of the thinned lines, both within a third of every strand.
        if name == "veryfar":
            close = abs(ink_on - lines_on) <= 0.1 * max(lines_on, 1e-3) and abs(ink_on - ink_off) <= 0.33 * max(ink_off, 1e-3)
        else:
            close = abs(ink_on - ink_off) <= 0.1 * max(ink_off, 1e-3)
        check("level of detail on %s, %s (%d strands): drawn %d strands, %d impostors, %d blendable segments; the ink "
              "%.3f against every strand's %.3f%s"
              % (api, name, strands, stats["drawnStrands"], stats["impostors"], stats["blendSegments"], ink_on, ink_off,
                 " and the thinned lines' %.3f" % lines_on if lines_on is not None else ""),
              stats["on"] and close and
              (stats["drawnStrands"] < 0.6 * strands if name in ("mid", "far", "veryfar") else stats["blendSegments"] > 0) and
              (stats["impostors"] > 0 if name == "veryfar" else True))
    # A still view draws the same picture twice; an orbit keeps the strands drawn and changes few pixels.
    frame(0.19)
    first, second = shot("still_a"), shot("still_b")
    before = lod()
    changes, last = [], first
    for step in range(6):
        cmds.orbit("persp", rotationAngles=(0, 2.0))
        now = shot("orbit_%d" % step)
        changes.append(differ(last, now))
        last = now
    after = lod()
    cmds.setAttr(groom + ".displayClumpLod", False)
    frame(0.19)
    plain_changes, last = [], shot("orbit_off_start")
    for step in range(6):
        cmds.orbit("persp", rotationAngles=(0, 2.0))
        now = shot("orbit_off_%d" % step)
        plain_changes.append(differ(last, now))
        last = now
    cmds.setAttr(groom + ".displayClumpLod", True)
    check("level of detail on %s: the same frame draws the same picture (%.4f of the pixels differ); a 12 degree orbit "
          "in 2 degree steps writes no indices (%d rebuilds before, %d after; level %d, %d) and changes %.4f of the "
          "pixels a step at most (%.4f with every strand)"
          % (api, differ(first, second), before["rebuilds"], after["rebuilds"], before["level"], after["level"],
             max(changes), max(plain_changes)),
          differ(first, second) == 0.0 and before["rebuilds"] == after["rebuilds"] and before["level"] == after["level"] and
          max(changes) <= max(0.05, 1.5 * max(plain_changes)))
    lod_orbit_checks(groom, api, frame)
    # The colored lines, a multisampled target.
    frame(0.75)
    cmds.setAttr(groom + ".strandColorMode", 3)
    cmds.setAttr(groom + ".displayLighting", False)
    on, off, stats = both("colored")
    check("level of detail on %s draws the colored lines (%d strands drawn, %d items; ink %.3f against %.3f)"
          % (api, stats["drawnStrands"], stats["items"], image_ink(on), image_ink(off)),
          stats["on"] and stats["drawnStrands"] < strands and abs(image_ink(on) - image_ink(off)) <= 0.1 * image_ink(off))
    cmds.setAttr(groom + ".strandColorMode", 0)
    cmds.setAttr(groom + ".displayLighting", True)
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", True)
    on, off, stats = both("msaa")
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", msaa)
    check("level of detail on %s draws on a multisampled target (%d strands drawn; ink %.3f against %.3f)"
          % (api, stats["drawnStrands"], image_ink(on), image_ink(off)),
          stats["on"] and abs(image_ink(on) - image_ink(off)) <= 0.1 * image_ink(off))
    # The groom selected; a GPU preview drag (where the preview runs).
    cmds.select(groom)
    selected = shot("selected")
    stats = lod()
    cmds.select(clear=True)
    check("level of detail on %s draws the groom selected (%d strands drawn; ink %.3f)" % (api, stats["drawnStrands"], image_ink(selected)),
          stats["on"] and image_ink(selected) > 0.0)
    if api in GPU_PREVIEW_APIS:
        took = cmds.groomLabGpuPreview(groom, node="noise", param="amplitude", value=0.2)
        dragged = shot("drag")
        stats = lod()
        cmds.groomLabGpuPreview(groom, end=True)
        cmds.refresh(currentView=True, force=True)
        check("level of detail on %s draws a GPU preview drag (taken %s; %d strands drawn, %d impostors: none while "
              "the GPU moves them; ink %.3f)" % (api, took, stats["drawnStrands"], stats["impostors"], image_ink(dragged)),
              stats["on"] and image_ink(dragged) > 0.0 and (stats["impostors"] == 0 or not took))
    # Ribbons, and the attribute off: every strand as before.
    cmds.setAttr(groom + ".displayRibbons", True)
    shot("ribbons")
    ribbons = lod()
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr(groom + ".displayClumpLod", False)
    shot("off")
    off = lod()
    cmds.setAttr(groom + ".displayClumpLod", True)
    check("level of detail on %s: ribbons draw every strand (%s), as displayClumpLod off does (%s)"
          % (api, ribbons["on"], off["on"]), not ribbons["on"] and not off["on"])
    check("the level of detail made no errors", not groomlab.errors(groom))
    cmds.modelEditor(panel, edit=True, grid=grid)
    cmds.file(new=True, force=True)
    cmds.setAttr("persp.translate", *camera["translate"])
    cmds.setAttr("persp.rotate", *camera["rotate"])
    cmds.setAttr("persp.centerOfInterest", camera["centerOfInterest"])
    cmds.camera("persp", edit=True, nearClipPlane=camera["near"], farClipPlane=camera["far"])


def true_width_check(snapshots=None):
    """Ribbons at the strands' true width (displayRibbons, off by default): the strands draw as
    ribbons facing the camera at their width (groomLabRibbons), at least a pixel, their thinner
    segments as lines with width as coverage (groomLabRibbonsThin), so they cover more of the picture
    than the lines; far away as near (no switch by distance). An orbit makes the ribbons again for
    each view (refaced); flat (lighting off) alike; off, the lines."""
    import tempfile
    import groomlab
    from PySide2 import QtGui
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_truewidth_")
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_param(groom, "scatter", "density", 300.0)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr(groom + ".displayLighting", True)
    cmds.setAttr(scalp + ".visibility", False)  # the hair over the background alone
    api = cmds.groomLabGpuPreview(drawApi=True)
    panel = persp_panel()
    cmds.modelPanel(panel, edit=True, camera="persp")
    grid = cmds.modelEditor(panel, query=True, grid=True)
    cmds.modelEditor(panel, edit=True, grid=False)
    kept = (cmds.getAttr("persp.translate")[0], cmds.getAttr("persp.rotate")[0])

    def draw():
        cmds.refresh(currentView=True, force=True)
        return json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]

    def ink(name):
        """The share of the picture's pixels that are not the background (its corner's)."""
        path = os.path.join(folder, "truewidth_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
        corner = QtGui.QColor(image.pixel(2, 2))
        inked = 0
        for y in range(0, image.height(), 2):
            for x in range(0, image.width(), 2):
                c = QtGui.QColor(image.pixel(x, y))
                if abs(c.red() - corner.red()) + abs(c.green() - corner.green()) + abs(c.blue() - corner.blue()) > 24:
                    inked += 1
        return inked / float((image.width() // 2) * (image.height() // 2))

    def near():
        cmds.setAttr("persp.translate", 0.3, 1.6, 0.9)
        cmds.viewLookAt("persp", position=(0.0, 1.0, 0.0))

    try:
        # Off by default: the lines.
        near()
        default = draw()
        lines = ink("near_lines")
        check("true width on %s: off by default, the strands draw as lines (facing %s, ribbons %s)"
              % (api, default.get("facing"), cmds.getAttr(groom + ".displayRibbons")),
              default.get("facing") is False and not cmds.getAttr(groom + ".displayRibbons"))
        # On: facing ribbons, their thin segments as lines with width as coverage, more ink than the lines.
        cmds.setAttr(groom + ".displayRibbons", True)
        close = draw()
        thin = close.get("shaders", {}).get("groomLabRibbonsThin", {})
        check("true width on %s: the strands draw as ribbons facing the camera, their thin segments as lines with "
              "width as coverage (facing %s, items %s, thin %s)" % (api, close.get("facing"), sorted(close["items"]), thin),
              close.get("facing") is True and "groomLabRibbons" in close["items"] and thin.get("coverage") is True)
        wide = ink("near")
        check("true width on %s: the strands cover more of the picture at their width than as lines (%.3f, lines %.3f)"
              % (api, wide, lines), wide > 1.25 * lines)
        checked = json.loads(cmds.groomLabGpuPreview(groom, checkFrames=True) or "{}").get("ribbons") or {}
        check("true width on %s: the GPU's facing ribbons are each point's, at its width or a pixel (%s)" % (api, checked),
              checked.get("facing") is True and checked.get("positions", 1) < 1e-4 and checked.get("normals", 1) < 1e-4
              and checked.get("tangents", 1) < 1e-4)
        # Far: the same ribbons (no switch by distance).
        cmds.viewFit("persp", allObjects=True)
        cmds.dolly("persp", distance=-30.0)
        far = draw()
        check("true width on %s: far away, still the facing ribbons (facing %s)" % (api, far.get("facing")), far.get("facing") is True)
        # An orbit: the ribbons made again for each view, the strands not built again.
        near()
        draw()
        refaced, facing = 0, []
        for _ in range(4):
            cmds.orbit("persp", rotationAngles=(0, 10))
            d = draw()
            refaced += d["items"].get("groomLabRibbons", {}).get("refaced", 0)
            facing.append(d.get("facing"))
        orbited = ink("orbited")
        check("true width on %s: an orbit makes the facing ribbons again for each view (%d, facing %s), at the strands' "
              "width throughout (%.3f, lines %.3f)" % (api, refaced, facing, orbited, lines),
              refaced >= 3 and all(facing) and orbited > 1.25 * lines)
        # Flat (lighting off): the same.
        near()
        cmds.setAttr(groom + ".displayLighting", False)
        flat = draw()
        flatInk = ink("flat")
        cmds.setAttr(groom + ".displayRibbons", False)
        flatLines = ink("flat_lines")
        flatThin = flat.get("shaders", {}).get("groomLabRibbonsThin", {})
        check("true width on %s: flat (lighting off) too, ribbons and thin lines with width as coverage, more ink than "
              "the lines (facing %s, thin %s, %.3f, lines %.3f)" % (api, flat.get("facing"), flatThin, flatInk, flatLines),
              flat.get("facing") is True and flatThin.get("coverage") is True and flatInk > 1.25 * flatLines)
        # Far, flat: every segment thinner than a pixel, drawn by the thin lines alone with width as
        # coverage (the uvs' u read there too), about as much ink as the lines' coverage; and in one
        # color (no strand colors: Display Color) as well.
        cmds.viewFit("persp", allObjects=True)
        cmds.dolly("persp", distance=-12.0)
        farLines = ink("far_flat_lines")
        cmds.setAttr(groom + ".displayRibbons", True)
        farRibbons = ink("far_flat_ribbons")
        cmds.setAttr(groom + ".useMaterialColor", False)
        cmds.setAttr(groom + ".strandColorMode", 0)
        farOne = ink("far_flat_one_color")
        oneColor = json.loads(cmds.groomLabGpuPreview(groom, checkFrames=True) or "{}").get("ribbons") or {}
        check("true width on %s: far away and flat, the thin segments draw with width as coverage about as the lines do "
              "(%.4f, lines %.4f), in one color too (%.4f, made by the GPU facing: %s)"
              % (api, farRibbons, farLines, farOne, oneColor),
              0.5 * farLines < farRibbons < 2.0 * farLines and farOne > 0.25 * farLines and oneColor.get("facing") is True)
        check("true width on %s: no errors" % api, not groomlab.errors(groom))
    finally:
        cmds.setAttr("persp.translate", *kept[0])
        cmds.setAttr("persp.rotate", *kept[1])
        cmds.modelEditor(panel, edit=True, grid=grid)
        cmds.file(new=True, force=True)


def lod_orbit_checks(groom, api, frame):
    """Full turns at a fixed distance write the level of detail's indices at most once, whatever the
    camera turns about, as the tumble tool turns it: near and mid, about the groom's center (its
    center of interest there, the tool's default; around, and over the top), a point of the surface it looks at (the center of
    interest moved there), the center of interest Maya's own framing leaves (viewFit), and "tumble on
    recent selection" (the tool's localTumble 0, the camera's tumble pivot on the surface off the
    view's axis); 36 steps, up and down a little as well as around. A dolly still changes the level,
    in steps."""
    import maya.api.OpenMaya as om
    scalp = cmds.ls(type="mesh", noIntermediate=True, long=True)[0]

    def lod():
        return json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["lod"]

    def surface(turn=0.0):
        """Where the view's axis (turned `turn` degrees) meets the scalp."""
        if turn:
            cmds.rotate(0, turn, 0, "persp", relative=True, objectSpace=True)
        matrix = om.MMatrix(cmds.xform("persp", query=True, worldSpace=True, matrix=True))
        if turn:
            cmds.rotate(0, -turn, 0, "persp", relative=True, objectSpace=True)
        path = om.MSelectionList().add(scalp).getDagPath(0)
        hit = om.MFnMesh(path).closestIntersection(om.MFloatPoint(matrix[12], matrix[13], matrix[14]),
                                                   om.MFloatVector(-matrix[8], -matrix[9], -matrix[10]),
                                                   om.MSpace.kWorld, 1e6, False)
        return [hit[0].x, hit[0].y, hit[0].z] if hit and hit[1] > 0.0 else None

    def coi_point():
        matrix = om.MMatrix(cmds.xform("persp", query=True, worldSpace=True, matrix=True))
        point = om.MVector(matrix[12], matrix[13], matrix[14]) + om.MVector(-matrix[8], -matrix[9], -matrix[10]) * \
            cmds.getAttr("perspShape.centerOfInterest")
        return [point.x, point.y, point.z]

    mode = cmds.tumbleCtx("tumbleContext", query=True, localTumble=True)
    pivot_was = cmds.getAttr("perspShape.tumblePivot")[0]
    results = []
    for framing, fill in (("near", 3.1), ("mid", 0.75)):
        for pivot in ("center", "over", "surface", "tumble", "selection"):
            frame(fill)
            about = None
            if pivot == "surface":
                point = surface()
                cmds.viewPlace("persp", lookAt=point)
                eye = cmds.xform("persp", query=True, worldSpace=True, translation=True)
                cmds.setAttr("perspShape.centerOfInterest", math.sqrt(sum((point[i] - eye[i]) ** 2 for i in range(3))))
            elif pivot == "tumble":
                # Maya's own framing (its center of interest the scene's center), then along the view
                # to this framing's distance.
                distance = cmds.getAttr("perspShape.centerOfInterest")
                cmds.viewFit("persp", allObjects=True)
                cmds.move(0, 0, distance - cmds.getAttr("perspShape.centerOfInterest"), "persp", relative=True, objectSpace=True)
                cmds.setAttr("perspShape.centerOfInterest", distance)
            elif pivot == "selection":
                about = surface(10.0) or [0.0, 0.0, 0.0]
                cmds.setAttr("perspShape.tumblePivot", *about)
                cmds.tumbleCtx("tumbleContext", edit=True, localTumble=0)
                time.sleep(1.1)  # (the viewport asks the tumble tool once a second)
            cmds.refresh(currentView=True, force=True)
            before = lod()
            levels = set()
            for step in range(36):
                # As the tumble tool turns the camera: about its center of interest (the tool's
                # default), or about its tumble pivot ("tumble on recent selection").
                if pivot == "over":  # (over the top: orbit's second angle)
                    cmds.orbit("persp", pivotPoint=coi_point(), rotationAngles=(0.0, 10.0))
                else:
                    cmds.tumble("persp", pivotPoint=about or coi_point(), azimuthAngle=10.0, elevationAngle=1.5 if step < 18 else -1.5)
                cmds.refresh(currentView=True, force=True)
                levels.add(lod()["level"])
            after = lod()
            cmds.tumbleCtx("tumbleContext", edit=True, localTumble=mode)
            cmds.setAttr("perspShape.tumblePivot", *pivot_was)
            results.append((framing, pivot, after["rebuilds"] - before["rebuilds"], sorted(levels), after["on"]))
    check("level of detail on %s: a full turn at a fixed distance writes its indices at most once, about any pivot "
          "(framing, pivot, rebuilds, levels, drawn by class: %s)" % (api, results),
          all(rebuilds <= 1 and len(levels) == 1 for _, _, rebuilds, levels, _ in results) and
          any(on for _, _, _, _, on in results))
    # A dolly toward the center of interest: the level moves, a step at a time.
    frame(0.75)
    cmds.refresh(currentView=True, force=True)
    seen = [lod()["level"]]
    for _ in range(12):
        coi = cmds.getAttr("perspShape.centerOfInterest")
        cmds.move(0, 0, -0.05 * coi, "persp", relative=True, objectSpace=True)
        cmds.setAttr("perspShape.centerOfInterest", 0.95 * coi)
        cmds.refresh(currentView=True, force=True)
        seen.append(lod()["level"])
    steps = [b - a for a, b in zip(seen, seen[1:]) if b != a]
    check("level of detail on %s: a dolly in changes the level in steps (%s)" % (api, seen),
          len(steps) >= 2 and all(s == 1 for s in steps))


def differ(a, b):
    """The share of pixels that differ between two captures."""
    from PySide2 import QtGui
    ia, ib = QtGui.QImage(a), QtGui.QImage(b)
    if ia.size() != ib.size() or ia.isNull():
        return 1.0
    ia = ia.convertToFormat(QtGui.QImage.Format_RGB32)
    ib = ib.convertToFormat(QtGui.QImage.Format_RGB32)
    da, db = bytes(ia.constBits())[:ia.byteCount()], bytes(ib.constBits())[:ib.byteCount()]
    pixels = ia.width() * ia.height()
    changed = sum(1 for i in range(0, len(da), 4) if da[i:i + 3] != db[i:i + 3])
    return changed / float(pixels)


def hue_counts(path):
    """Pixels of a capture where red, green or blue clearly leads (every other pixel's), and the
    rest that is not the dark background. (By a third for red, a fifth for green and blue: picked
    colors shade as Arnold renders a base color, hair_bsdf.glsl hairColorSigma, lighter and less
    saturated than the one-pass transmittance they drew as before.)"""
    from PySide2 import QtGui
    image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
    data = bytes(image.constBits())[:image.byteCount()]
    counts = {"r": 0, "g": 0, "b": 0, "other": 0}
    for i in range(0, len(data), 8):  # every other pixel (BGRA)
        b, g, r = data[i], data[i + 1], data[i + 2]
        if max(r, g, b) < 25:
            continue
        key = ("r" if r > 1.35 * g and r > 1.35 * b else "g" if g > 1.2 * r and g > 1.2 * b
               else "b" if b > 1.2 * r and b > 1.2 * g else "other")
        counts[key] += 1
    return counts


def hair_pixel_change(before, after, block=6):
    """How the hair pixels (off the gray or white background in both) of two captures of the same
    view differ, pixel by pixel (the lighting cancels): the spread (standard deviation) of the
    change of hue (turns), of saturation (HSV) and of brightness (the log of the luminance's
    ratio), their count, and how much of the hue change's spread stays within `block`-pixel squares
    (the mean variance inside the squares over the variance of all: low when neighboring strands,
    a clump, change alike)."""
    import colorsys
    from PySide2 import QtGui

    def load(path):
        image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
        return image.width(), image.height(), bytes(image.constBits())[:image.byteCount()]

    w, h, a = load(before)
    _, _, b = load(after)
    changes = {}
    for y in range(h):
        for x in range(w):
            i = 4 * (y * w + x)
            pa, pb = (a[i + 2], a[i + 1], a[i]), (b[i + 2], b[i + 1], b[i])
            if max(pa) - min(pa) < 10 or max(pb) - min(pb) < 10 or min(max(pa), max(pb)) < 12:
                continue
            ha = colorsys.rgb_to_hsv(*[c / 255.0 for c in pa])
            hb = colorsys.rgb_to_hsv(*[c / 255.0 for c in pb])
            la = 0.2126 * pa[0] + 0.7152 * pa[1] + 0.0722 * pa[2]
            lb = 0.2126 * pb[0] + 0.7152 * pb[1] + 0.0722 * pb[2]
            d = hb[0] - ha[0]
            changes[(x, y)] = (d - round(d), hb[1] - ha[1], math.log(max(lb, 1.0) / max(la, 1.0)))

    def spread(values):
        m = sum(values) / max(len(values), 1)
        return math.sqrt(sum((v - m) ** 2 for v in values) / max(len(values), 1))

    if len(changes) < 50:
        return {"pixels": len(changes), "hue": 0.0, "saturation": 0.0, "value": 0.0, "local": 1.0}
    hues = {k: c[0] for k, c in changes.items()}
    total = spread(list(hues.values())) ** 2
    inside, blocks = 0.0, 0
    for by in range(0, h, block):
        for bx in range(0, w, block):
            values = [hues[(x, y)] for y in range(by, min(by + block, h)) for x in range(bx, min(bx + block, w))
                      if (x, y) in hues]
            if len(values) >= block * block // 2:
                inside += spread(values) ** 2
                blocks += 1
    return {"pixels": len(changes), "hue": math.sqrt(total), "saturation": spread([c[1] for c in changes.values()]),
            "value": spread([c[2] for c in changes.values()]), "local": (inside / max(blocks, 1)) / max(total, 1e-12)}


def hair_color_randomness_check(snapshots=None):
    """HairColor's randomness in the viewport, on the device the session runs (DirectX 11 in
    maya.gui_dx11): hue, saturation, melanin and value randomness each spread the drawn strands'
    hue, saturation or brightness when raised from 0 (live, each edit redrawn), and clumpShare 1
    keeps a clump's strands (neighbors on screen) to one hue. The user's report: hue and
    saturation randomness showed nothing, clump randomness was gone."""
    import tempfile
    import groomlab
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_haircolor_")
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)
    groomlab.set_param(groom, "scatter", "density", 6000.0)
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "clump", "type": "Clump", "params": {"count": 60, "strength": 0.6}})
    graph["nodes"].insert(-1, {"name": "color", "type": "HairColor",
                               "params": {"melanin": 0.25, "melaninRandom": 0.0, "clumpShare": 0.0}})
    into = next(c for c in graph["connections"] if c["to"] == "output")
    graph["connections"] = [c for c in graph["connections"] if c is not into]
    graph["connections"] += [{"from": into["from"], "to": "clump", "input": "strands"},
                             {"from": "clump", "to": "color", "input": "strands"},
                             {"from": "color", "to": "output", "input": "strands"}]
    groomlab.set_graph(groom, graph)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayStrandVariation", 0.0)  # the viewport's own variation off: HairColor's alone
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    cmds.select(clear=True)
    cmds.setAttr("persp.translate", 0.0, 6.0, 0.01)
    cmds.setAttr("persp.rotate", -90.0, 0.0, 0.0)
    cmds.viewFit("persp", allObjects=True)
    api = cmds.groomLabGpuPreview(drawApi=True)

    def shot(name, **params):
        for param, value in params.items():
            groomlab.set_param(groom, "color", param, value)
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "haircolor_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    def change(before, after, name):
        stats = hair_pixel_change(before, after)
        say("haircolor %s on %s: %s" % (name, api, ", ".join("%s %.4f" % (k, v) for k, v in sorted(stats.items()))))
        return stats

    # Mode melanin: hue and melanin randomness from 0; again with nothing changed (the noise floor).
    plain = shot("melanin_plain")
    floor = change(plain, shot("melanin_again"), "melanin again")
    hue = change(plain, shot("melanin_hue", hueRandom=0.5), "melanin hue")
    check("hue randomness turns each drawn strand's hue in mode melanin on %s (%.4f, %.4f unchanged)"
          % (api, hue["hue"], floor["hue"]), hue["pixels"] > 2000 and hue["hue"] > max(5 * floor["hue"], 0.03))
    groomlab.set_param(groom, "color", "hueRandom", 0.0)
    melanin = change(plain, shot("melanin_melanin", melaninRandom=0.4), "melanin melanin")
    check("melanin randomness lightens and darkens each drawn strand on %s (%.4f, %.4f unchanged)"
          % (api, melanin["value"], floor["value"]), melanin["value"] > max(5 * floor["value"], 0.08))
    groomlab.set_param(groom, "color", "melaninRandom", 0.0)
    melanin_saturation = change(plain, shot("melanin_saturation", saturationRandom=0.9), "melanin saturation")
    check("saturation randomness changes each drawn strand's saturation in mode melanin on %s (%.4f, %.4f unchanged)"
          % (api, melanin_saturation["saturation"], floor["saturation"]),
          melanin_saturation["saturation"] > max(5 * floor["saturation"], 0.02))
    groomlab.set_param(groom, "color", "saturationRandom", 0.0)
    # Mode color: saturation, value and hue randomness; clumpShare.
    plain = shot("color_plain", mode="color", color=(0.6, 0.35, 0.2))
    saturation = change(plain, shot("color_saturation", saturationRandom=0.9), "color saturation")
    check("saturation randomness changes each drawn strand's saturation in mode color on %s (%.4f, %.4f unchanged)"
          % (api, saturation["saturation"], floor["saturation"]), saturation["saturation"] > max(5 * floor["saturation"], 0.02))
    groomlab.set_param(groom, "color", "saturationRandom", 0.0)
    value = change(plain, shot("color_value", valueRandom=0.6), "color value")
    check("value randomness lightens and darkens each drawn strand in mode color on %s (%.4f, %.4f unchanged)"
          % (api, value["value"], floor["value"]), value["value"] > max(5 * floor["value"], 0.08))
    groomlab.set_param(groom, "color", "valueRandom", 0.0)
    color_hue = change(plain, shot("color_hue", hueRandom=0.5), "color hue")
    check("hue randomness turns each drawn strand's hue in mode color on %s (%.4f, %.4f unchanged)"
          % (api, color_hue["hue"], floor["hue"]), color_hue["hue"] > max(5 * floor["hue"], 0.03))
    clumped = change(plain, shot("color_hue_clumps", clumpShare=1.0), "color hue clumps")
    check("clumpShare 1 turns a clump's strands (neighbors on screen) alike on %s (within squares %.3f of the spread, "
          "%.3f per strand), the spread kept (%.4f, %.4f)" % (api, clumped["local"], color_hue["local"], clumped["hue"],
                                                             color_hue["hue"]),
          clumped["local"] < 0.6 * color_hue["local"] and clumped["hue"] > 0.5 * color_hue["hue"])
    check("HairColor's randomness made no errors", not groomlab.errors(groom))
    cmds.file(new=True, force=True)


def hair_color_scale_presets_check(snapshots=None):
    """The same with shipped presets (HairColor's tips, wetness, painted maps) on a head-sized
    sphere with guides, at the default display settings and preview density."""
    import tempfile
    import groomlab
    from groomlab import presets
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_colorscale_")
    for preset in ("two_tone_tips", "hair_basics", "wet", "painted_maps"):
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=10, subdivisionsX=40, subdivisionsY=20)[0]
        groom = groomlab.create_groom(scalp)
        curves = []
        for i in range(12):
            a = 2 * math.pi * i / 12.0
            x, z = math.cos(a), math.sin(a)
            curves.append(cmds.curve(point=[(7 * x, 7.1, 7 * z), (9 * x, 9, 9 * z), (12 * x, 6, 12 * z)], degree=2))
        groomlab.guides_from_curves(groom, curves)
        cmds.delete(curves)
        presets.apply_preset(groom, preset)
        cmds.select(clear=True)
        cmds.viewFit("persp", allObjects=True)
        api = cmds.groomLabGpuPreview(drawApi=True)

        def shot(name):
            cmds.refresh(currentView=True, force=True)
            path = os.path.join(folder, "colorscale_%s_%s.png" % (preset, name))
            cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                           percent=100, widthHeight=(480, 360), completeFilename=path)
            return path

        plain = shot("plain")
        floor = differ(plain, shot("again"))
        for step in range(1, 9):
            s = 1.0 + step / 8.0
            cmds.setAttr(scalp + ".scale", s, s, s)
            cmds.refresh(currentView=True, force=True)
        for step in range(1, 9):
            s = 2.0 - step / 8.0
            cmds.setAttr(scalp + ".scale", s, s, s)
            cmds.refresh(currentView=True, force=True)
        back = shot("back")
        node = next(n["name"] for n in groomlab.get_graph(groom)["nodes"] if n["type"] == "HairColor")
        value = groomlab.param_value(groom, node, "melaninRandom")
        groomlab.set_param(groom, node, "melaninRandom", value + 0.01)
        groomlab.set_param(groom, node, "melaninRandom", value)
        tweaked = shot("tweaked")
        check("%s: hair color scaled up and back is as before on %s (%.4f of the pixels differ, %.4f after a tweak, %.4f again)"
              % (preset, api, differ(plain, back), differ(plain, tweaked), floor),
              differ(plain, back) <= max(floor, differ(plain, tweaked)) + 0.002)
    cmds.file(new=True, force=True)


def hair_luminance(path):
    """The mean luminance of the pixels that are neither the white background nor the gray floor."""
    from PySide2 import QtGui
    image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
    data = bytes(image.constBits())[:image.byteCount()]
    total, count = 0.0, 0
    for i in range(0, len(data), 4):
        b, g, r = data[i], data[i + 1], data[i + 2]
        if min(r, g, b) > 245 or (abs(r - 69) < 4 and abs(g - 69) < 4 and abs(b - 69) < 4):
            continue
        total += 0.2126 * r + 0.7152 * g + 0.0722 * b
        count += 1
    return total / max(count, 1)


def freeze_cycle_scene_check(scene, snapshots=None):
    """The user's report: the scalp scaled down, frozen, scaled back up, frozen: the hair drew darker
    until a HairColor value was tweaked."""
    import tempfile
    import groomlab
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_freeze_")
    cmds.file(scene, open=True, force=True)
    groom = cmds.ls(type="groomLabShape")[0]
    scalp = cmds.listRelatives(groomlab.scalp_mesh(groom), parent=True)[0]
    color = next(n["name"] for n in groomlab.get_graph(groom, expand_default=True)["nodes"] if n["type"] == "HairColor")
    cmds.select(clear=True)
    cmds.viewFit("persp", scalp)
    if os.environ.get("GROOMLAB_PROBE_CACHE") == "1":  # cached playback on (Maya's default), filled in the background
        from gui_cached_playback import cache_on
        cache_on("syncAsync")
    zoom = float(os.environ.get("GROOMLAB_PROBE_ZOOM", "0"))  # the camera that share of the way to the head
    if zoom:
        box = cmds.exactWorldBoundingBox(scalp)
        head = ((box[0] + box[3]) / 2, box[1] + 0.85 * (box[4] - box[1]), (box[2] + box[5]) / 2)
        eye = cmds.getAttr("persp.translate")[0]
        cmds.setAttr("persp.translate", *[e + (h - e) * zoom for e, h in zip(eye, head)])
    api = cmds.groomLabGpuPreview(drawApi=True)

    if os.environ.get("GROOMLAB_PROBE_MAXIMIZE") == "1":  # the user's full screen window
        import maya.OpenMayaUI as omui
        import shiboken2
        from PySide2 import QtWidgets
        window = shiboken2.wrapInstance(int(omui.MQtUtil.mainWindow()), QtWidgets.QMainWindow)
        window.showMaximized()
        process_events(500)
        view = omui.M3dView.active3dView()
        say("freeze: view %dx%d" % (view.portWidth(), view.portHeight()))

    def shot(name):
        import maya.OpenMaya as om
        import maya.OpenMayaUI as omui
        cmds.refresh(currentView=True, force=True)
        stats = cmds.groomLabGraphStats(groom, drawTimes=True)
        say("freeze %s: k %.4f, %d strands, draw %s" % (name, cmds.getAttr(groom + ".groomScale"),
                                                     cmds.getAttr(groom + ".outStrandCount"), stats[:900]))
        say("freeze %s: lod %s" % (name, json.dumps(json.loads(stats).get("draw", {}).get("lod"))))
        grid = json.loads(cmds.groomLabGraphStats(groom, hairShading=True)).get("grid", {})
        grids[name] = grid
        say("freeze %s: grid %s" % (name, json.dumps(grid)))
        path = os.path.join(folder, "freeze_%s.png" % name)
        # The live view as drawn (a playblast draws again offscreen: it can miss what the view keeps).
        view = omui.M3dView.active3dView()
        view.refresh(False, True)
        image = om.MImage()
        view.readColorBuffer(image, True)
        image.writeToFile(path, "png")
        return path

    grids = {}
    plain = shot("plain")
    factor = float(os.environ.get("GROOMLAB_PROBE_FACTOR", "0.5"))
    def settle():
        cmds.refresh(currentView=True, force=True)
        process_events(1500)  # the user's pace: idle work, cached playback's background fill
        cmds.refresh(currentView=True, force=True)
        g = json.loads(cmds.groomLabGraphStats(groom, hairShading=True)).get("grid", {})
        say("freeze settle: k %.4f, grid %s" % (cmds.getAttr(groom + ".groomScale"), {key: g.get(key) for key in (
            "size", "cell", "volumeSerial", "serial", "staleShaders", "textureSum", "ownSum", "texelsDiffering", "checkedSerial", "reuploads")}))

    cmds.setAttr(scalp + ".scale", factor, factor, factor)
    settle()
    cmds.makeIdentity(scalp, apply=True, translate=True, rotate=True, scale=True, normal=0, preserveNormals=True)
    settle()
    cmds.setAttr(scalp + ".scale", 1.0 / factor, 1.0 / factor, 1.0 / factor)
    settle()
    cmds.makeIdentity(scalp, apply=True, translate=True, rotate=True, scale=True, normal=0, preserveNormals=True)
    settle()
    back = shot("back")
    v = groomlab.param_value(groom, color, "melanin")
    groomlab.set_param(groom, color, "melanin", v + 0.01)
    groomlab.set_param(groom, color, "melanin", v)
    tweaked = shot("tweaked")
    # The user's fix: width as coverage off and on (every strand line written again).
    cmds.setAttr(groom + ".displayWidthCoverage", not cmds.getAttr(groom + ".displayWidthCoverage"))
    cmds.refresh(currentView=True, force=True)
    cmds.setAttr(groom + ".displayWidthCoverage", not cmds.getAttr(groom + ".displayWidthCoverage"))
    toggled = shot("toggled")
    lum = {name: hair_luminance(path) for name, path in (("plain", plain), ("back", back), ("tweaked", tweaked),
                                                          ("toggled", toggled))}
    say("freeze on %s: luminance %s, back vs tweaked %.4f, back vs toggled %.4f"
        % (api, lum, differ(back, tweaked), differ(back, toggled)))
    check("frozen down and up, the hair draws as after width coverage off and on on %s (%.4f of the pixels differ; "
          "luminance %.2f, %.2f)" % (api, differ(back, toggled), lum["back"], lum["toggled"]),
          differ(back, toggled) <= 0.002)
    if grids.get("back", {}).get("valid") and grids.get("toggled", {}).get("valid"):
        a, b = grids["back"]["sum"], grids["toggled"]["sum"]
        check("frozen down and up, the self-shadowing grid holds what it holds made afresh on %s (%.3f, %.3f)" % (api, a, b),
              abs(a - b) <= 1e-3 * max(abs(b), 1.0))


def stroke_scaled_scene_check(scene, snapshots=None):
    """stroke_scaled_check on a scene's first groom (a probe: the user's own scene)."""
    import tempfile
    import groomlab
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_strokescale_")
    cmds.file(scene, open=True, force=True)
    groom = cmds.ls(type="groomLabShape")[0]
    scalp = cmds.listRelatives(groomlab.scalp_mesh(groom), parent=True)[0]
    color = next(n["name"] for n in groomlab.get_graph(groom, expand_default=True)["nodes"] if n["type"] == "HairColor")
    say("scene groom %s on %s: hair shading %s, self-shadowing %s, coverage %s, follow %s, preview %s" % (
        groom, scalp, cmds.getAttr(groom + ".displayHairShading"), cmds.getAttr(groom + ".displaySelfShadowing"),
        cmds.getAttr(groom + ".displayWidthCoverage"), cmds.getAttr(groom + ".followScalp"), cmds.getAttr(groom + ".previewDensity")))
    cmds.select(clear=True)
    cmds.viewFit("persp", scalp)
    api = cmds.groomLabGpuPreview(drawApi=True)
    say("GPU preview was %s" % groomlab.gpu_preview())
    groomlab.gpu_preview(True)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        g = cmds.ls(type="groomLabShape")[0]
        info = json.loads(cmds.groomLabGraphStats(g, frameInfo=True))
        say("scenescale %s: k %.3f, %d strands, frame %s" % (name, cmds.getAttr(g + ".groomScale"),
                                                            cmds.getAttr(g + ".outStrandCount"), json.dumps(info)[:300]))
        path = os.path.join(folder, "scenescale_%s.png" % name)
        cmds.playblast(frame=[cmds.currentTime(query=True)], format="image", compression="png", viewer=False,
                       showOrnaments=False, percent=100, widthHeight=(640, 480), completeFilename=path)
        return path

    plain = shot("plain")
    pivot = cmds.xform(scalp, query=True, worldSpace=True, rotatePivot=True)
    cmds.setAttr(scalp + ".scale", 0.1, 0.1, 0.1)
    cmds.refresh(currentView=True, force=True)
    small = shot("small")
    cmds.select(cmds.listRelatives(groom, parent=True)[0])
    camera = (cmds.getAttr("persp.translate")[0], cmds.getAttr("persp.rotate")[0])
    cmds.viewFit("persp", scalp)
    box = cmds.exactWorldBoundingBox(scalp)
    middle = ((box[0] + box[3]) / 2, box[1] + 0.6 * (box[4] - box[1]), box[5])
    say("locked %s, hidden groups %r, guides %d, sculpt node %r" % (
        cmds.getAttr(groom + ".locked"), cmds.getAttr(groom + ".hiddenGuideGroups"),
        len(groomlab.get_hierarchy(groom).get("guides", [])), cmds.getAttr(groom + ".sculptNode")))
    cmds.setAttr(groom + ".locked", False)
    hidden = cmds.getAttr(groom + ".hiddenGuideGroups") or ""
    cmds.setAttr(groom + ".hiddenGuideGroups", "", type="string")
    groomlab.pin_groom(groom)
    ctx = groomlab.brush_tool("comb", radius=80, strength=1.0)
    cmds.refresh(currentView=True, force=True)
    x0, y0 = to_view(middle)
    say("stroke at %d %d (view of %s)" % (x0, y0, middle,))
    data = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="draw " + " ".join("%d %d" % (x0 - 15 + 2 * k, y0) for k in range(16)))
    combed = cmds.getAttr(groom + ".guideData") != data
    cmds.setAttr(groom + ".hiddenGuideGroups", hidden, type="string")
    cmds.refresh(currentView=True, force=True)
    process_events(500)  # the brush's idle work (the stroke document warmed)
    cmds.setAttr(scalp + ".scale", 1.0, 1.0, 1.0)  # the brush still up, as a user scaling in the channel box
    cmds.setAttr("persp.translate", *camera[0])
    cmds.setAttr("persp.rotate", *camera[1])
    with_tool = shot("back_tool")
    process_events(500)
    cmds.setToolTo("selectSuperContext")
    cmds.select(clear=True)
    back = shot("back")
    say("scenescale: brush up %.2f, put away %.2f" % (hair_luminance(with_tool), hair_luminance(back)))
    v = groomlab.param_value(groom, color, "melanin")
    groomlab.set_param(groom, color, "melanin", v + 0.01)
    groomlab.set_param(groom, color, "melanin", v)
    tweaked = shot("tweaked")
    # Fresh: the scene saved and opened again (nothing kept from the session).
    again = os.path.join(folder, "scenescale_again.ma")
    cmds.file(rename=again)
    cmds.file(save=True, type="mayaAscii", force=True)
    cmds.file(again, open=True, force=True)
    cmds.setAttr("persp.translate", *camera[0])
    cmds.setAttr("persp.rotate", *camera[1])
    fresh = shot("fresh")
    say("scenescale on %s: back vs fresh %.4f, tweaked vs fresh %.4f" % (api, differ(back, fresh), differ(tweaked, fresh)))
    for name, path in (("plain", plain), ("back", back), ("tweaked", tweaked), ("fresh", fresh)):
        say("scenescale %s: hair luminance %.2f" % (name, hair_luminance(path)))
    # (The pixels differ a little anyway: the strands' order in the buffers. The hair's brightness may not.)
    check("scene: combed while scaled to 0.1 and scaled back, the hair is as bright as the scene opened afresh on %s "
          "(luminance %.2f, %.2f)" % (api, hair_luminance(back), hair_luminance(fresh)),
          abs(hair_luminance(back) - hair_luminance(fresh)) < 0.5)
    say("scenescale on %s: pivot %s, combed %s, back vs tweaked %.4f, plain vs tweaked %.4f, small %s"
        % (api, pivot, combed, differ(back, tweaked), differ(plain, tweaked), small))
    check("scene: combed while scaled to 0.1 and scaled back, the hair draws as after a tweak on %s (%.4f)"
          % (api, differ(back, tweaked)), differ(back, tweaked) <= 0.002)


def scale_freeze_shaders_check(snapshots=None):
    """The scalp scaled to 0.1, frozen, scaled back up and frozen (the groom's scale 1 -> 0.1 -> 1):
    the strands' items draw as width as coverage turned off and on draws them (the user's way back
    from hair that drew darker): the same shader state (coverage, what the hair shader's draws know
    it by, the coverage density, no instance shared) and the same picture."""
    import tempfile
    import groomlab
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_scalefreeze_")
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    curves = []
    for i in range(8):
        a = 2 * math.pi * i / 8
        x, z = 0.5 * math.cos(a), 0.5 * math.sin(a)
        y = math.sqrt(1 - 0.25)
        curves.append(cmds.curve(degree=2, point=[(x, y, z), (1.2 * x, y + 0.5, 1.2 * z), (1.3 * x, y + 1.0, 1.3 * z)]))
    groom = groomlab.create_groom(scalp)
    groomlab.guides_from_curves(groom, curves, tube_radius=0.12)
    cmds.delete(curves)
    cmds.setAttr(groom + ".displayHairShading", 1)
    cmds.setAttr(groom + ".displaySelfShadowing", 1.0)
    cmds.setAttr(groom + ".displayWidthCoverage", True)
    cmds.setAttr("persp.translate", 0.0, 1.4, 5.0)
    cmds.setAttr("persp.rotate", -8, 0, 0)
    cmds.select(clear=True)
    api = cmds.groomLabGpuPreview(drawApi=True)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        cmds.refresh(currentView=True, force=True)  # (the update after one that wrote the lines: the grid made again)
        draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]
        path = os.path.join(folder, "scalefreeze_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path, draw.get("shaders", {}).get("groomLabStrands", {})

    def strands_item():
        draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]
        return draw.get("items", {}).get("groomLabStrands"), draw.get("lod", {})

    def grid():
        return json.loads(cmds.groomLabGraphStats(groom, hairShading=True)).get("grid", {})

    cmds.refresh(currentView=True, force=True)
    built, _ = strands_item()  # the strands' item as first built
    grids = [grid()]
    afresh = None
    for k in (0.1, 10.0):
        cmds.setAttr(scalp + ".scale", k, k, k)
        cmds.refresh(currentView=True, force=True)
        if k > 1.0:
            afresh, _ = strands_item()  # the groom's scale back at 1: the strands' item written afresh
        grids.append(grid())
        cmds.makeIdentity(scalp, apply=True, translate=True, rotate=True, scale=True, normal=0, preserveNormals=True)
        cmds.refresh(currentView=True, force=True)
        grids.append(grid())
    back, backShader = shot("back")
    _, backLod = strands_item()
    # Every hair shader holds the grid as it is now (the self-shadowing a stale texture gave drew the
    # hair darker): its texture (made anew where the grid changed size) and its contents.
    now = grid()
    say("scalefreeze grids: %s; now %s" % ([(g.get("size"), g.get("volumeSerial"), g.get("serial"), g.get("staleShaders")) for g in grids],
                                           {key: now.get(key) for key in ("size", "volumeSerial", "serial", "shadersGiven", "staleShaders")}))
    check("scaled 1 -> 0.1 -> 1 and frozen, every hair shader holds the self-shadowing grid as it is now on %s (%s)"
          % (api, {key: now.get(key) for key in ("volumeSerial", "serial", "shadersGiven", "staleShaders")}),
          now.get("valid") and now.get("staleShaders") == 0 and len(now.get("shadersGiven", {})) > 0 and
          now.get("volumeSerial", 0) > grids[0].get("volumeSerial", 0) and  # (a texture of its own at each scale)
          all(v == [now["volumeSerial"], now["serial"]] for v in now["shadersGiven"].values()) and
          all(g.get("staleShaders", 0) == 0 for g in grids))
    # Written afresh, the item holds what a fresh build holds: the same streams, and the CPU's share of
    # what it sent (the indices, the colors) the same (the GPU's lines keep their layout: not sent again).
    if built and afresh:
        cpu = lambda item: round(item["megabytes"] - item.get("gpuMB", 0.0), 2)
        check("scaled 1 -> 0.1 -> 1, the strands' item holds what a fresh build holds on %s (%s; %s)"
              % (api, {key: built.get(key) for key in ("streams", "megabytes", "gpuMB")},
                 {key: afresh.get(key) for key in ("streams", "megabytes", "gpuMB")}),
              afresh.get("streams") == built.get("streams") and cpu(afresh) == cpu(built))
    if backLod.get("on"):
        check("scaled 1 -> 0.1 -> 1 and frozen, the level of detail's items draw the strands' streams as last bound on %s (%s)"
              % (api, backLod), backLod.get("staleSlots") == 0 and backLod.get("baseSerial", 0) > 0)
    for _ in range(2):
        cmds.setAttr(groom + ".displayWidthCoverage", not cmds.getAttr(groom + ".displayWidthCoverage"))
        cmds.refresh(currentView=True, force=True)
    toggled, toggledShader = shot("toggled")
    say("scalefreeze on %s: k %.4f, strands' shader %s; toggled %s; %.4f of the pixels differ"
        % (api, cmds.getAttr(groom + ".groomScale"), backShader, toggledShader, differ(back, toggled)))
    same = {key: backShader.get(key) for key in ("kind", "hair", "coverage", "registered", "lineDensity", "widthScale", "shared")}
    fresh = {key: toggledShader.get(key) for key in same}
    check("scaled 1 -> 0.1 -> 1 and frozen, the strands' shader state is width as coverage off and on's on %s (%s, %s)"
          % (api, same, fresh), same == fresh and backShader.get("shared") == 1 and
          (not backShader.get("hair") or backShader.get("registered") == int(bool(backShader.get("coverage")))))
    check("scaled 1 -> 0.1 -> 1 and frozen, the hair draws as after width coverage off and on on %s (%.4f)"
          % (api, differ(back, toggled)), differ(back, toggled) <= 0.002)
    # Maya's texture holds the grid the kernels made (read back where the device lets it: DirectX 11);
    # one spoiled from the CPU (as a copy that did not land would leave it) is found and written again.
    if now.get("textureSum", -1.0) >= 0.0:
        check("scaled 1 -> 0.1 -> 1 and frozen, the self-shadowing texture holds the grid the kernels made on %s (%s)"
              % (api, {key: now.get(key) for key in ("textureSum", "ownSum", "texelsDiffering", "checkedSerial", "reuploads")}),
              now.get("texelsDiffering") == 0 and now.get("checkedSerial", 0) > 0 and now.get("reuploads") == 0)
        cmds.optionVar(intValue=("groomLabHairGridSpoil", 1))
        cmds.setAttr(scalp + ".scale", 2.0, 2.0, 2.0)  # (another scale: the grid made in a new texture, checked)
        cmds.refresh(currentView=True, force=True)
        cmds.refresh(currentView=True, force=True)
        spoiled = grid()
        cmds.optionVar(remove="groomLabHairGridSpoil")
        check("a self-shadowing texture that does not hold the grid is written again from the CPU on %s (%s)"
              % (api, {key: spoiled.get(key) for key in ("textureSum", "ownSum", "texelsDiffering", "reuploads", "handleSwaps")}),
              spoiled.get("reuploads") == 1 and spoiled.get("texelsDiffering") == 0)
    cmds.file(new=True, force=True)


def stroke_scaled_check(snapshots=None):
    """The user's report: the scalp scaled to 0.1, the guides combed, the scalp scaled back to 1:
    the hair drew darker until a HairColor value was tweaked. Back at 1, the view is the one a
    tweak gives."""
    import tempfile
    import groomlab
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_strokescale_")
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    curves = []
    for i in range(8):
        a = 2 * math.pi * i / 8
        x, z = 0.5 * math.cos(a), 0.5 * math.sin(a)
        y = math.sqrt(1 - 0.25)
        curves.append(cmds.curve(degree=2, point=[(x, y, z), (1.2 * x, y + 0.5, 1.2 * z), (1.3 * x, y + 1.0, 1.3 * z)]))
    groom = groomlab.create_groom(scalp)
    groomlab.guides_from_curves(groom, curves, tube_radius=0.12)
    cmds.delete(curves)
    graph = groomlab.get_graph(groom, expand_default=True)
    if not any(n["type"] == "HairColor" for n in graph["nodes"]):
        graph["nodes"].insert(-1, {"name": "color", "type": "HairColor", "params": {"melanin": 0.3}})
        into = next(c for c in graph["connections"] if c["to"] == "output")
        graph["connections"] = [c for c in graph["connections"] if c is not into]
        graph["connections"] += [{"from": into["from"], "to": "color", "input": "strands"},
                                 {"from": "color", "to": "output", "input": "strands"}]
        groomlab.set_graph(groom, graph)
    color = next(n["name"] for n in groomlab.get_graph(groom, expand_default=True)["nodes"] if n["type"] == "HairColor")
    cmds.setAttr(groom + ".displayHairShading", 1)
    cmds.setAttr(groom + ".displaySelfShadowing", 1.0)
    cmds.setAttr("persp.translate", 0.0, 1.4, 5.0)
    cmds.setAttr("persp.rotate", -8, 0, 0)
    cmds.select(cmds.listRelatives(groom, parent=True)[0])
    api = cmds.groomLabGpuPreview(drawApi=True)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "strokescale_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    def tweak():
        v = groomlab.param_value(groom, color, "melanin")
        groomlab.set_param(groom, color, "melanin", v + 0.01)
        groomlab.set_param(groom, color, "melanin", v)

    shot("plain")
    cmds.setAttr(scalp + ".scale", 0.1, 0.1, 0.1)
    cmds.refresh(currentView=True, force=True)
    ctx = groomlab.brush_tool("comb", radius=60, strength=1.0)
    cmds.refresh(currentView=True, force=True)
    x0, y0 = to_view((0.0, 0.085, 0.05))
    data = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="draw " + " ".join("%d %d" % (x0 - 30 + 4 * k, y0) for k in range(16)))
    combed = cmds.getAttr(groom + ".guideData") != data
    cmds.setToolTo("selectSuperContext")
    cmds.refresh(currentView=True, force=True)
    cmds.setAttr(scalp + ".scale", 1.0, 1.0, 1.0)
    back = shot("back")
    tweak()
    tweaked = shot("tweaked")
    say("strokescale on %s: combed %s, back %.4f of the pixels differ from after a tweak" % (api, combed, differ(back, tweaked)))
    check("combed while scaled to 0.1 and scaled back, the hair draws as after a tweak on %s (%.4f of the pixels differ; "
          "combed: %s)" % (api, differ(back, tweaked), combed), differ(back, tweaked) <= 0.002)
    cmds.file(new=True, force=True)


def hair_color_scale_check(snapshots=None, deformed=False):
    """A groom scaled with its scalp keeps its hair color: scaled up and back down, the view is the
    one before (the user's report: the color broke, a param tweak brought it back)."""
    import tempfile
    import groomlab
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_colorscale_")
    cmds.file(new=True, force=True)
    cmds.currentTime(10)  # away from the start frame: the scalp's size is read at the start frame
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    if deformed:  # a deformed scalp (its rest mesh, set by create_groom), followed
        bend = cmds.nonLinear(scalp, type="bend")[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)
    groomlab.set_param(groom, "scatter", "density", 6000.0)
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "clump", "type": "Clump", "params": {"count": 60, "strength": 0.6}})
    graph["nodes"].insert(-1, {"name": "color", "type": "HairColor",
                               "params": {"melanin": 0.3, "melaninRandom": 0.4, "hueRandom": 0.3, "clumpShare": 0.5}})
    into = next(c for c in graph["connections"] if c["to"] == "output")
    graph["connections"] = [c for c in graph["connections"] if c is not into]
    graph["connections"] += [{"from": into["from"], "to": "clump", "input": "strands"},
                             {"from": "clump", "to": "color", "input": "strands"},
                             {"from": "color", "to": "output", "input": "strands"}]
    groomlab.set_graph(groom, graph)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    if deformed:
        cmds.setAttr(bend + ".curvature", 0.4)  # away from its rest pose
    cmds.select(clear=True)
    cmds.setAttr("persp.translate", 0.0, 6.0, 0.01)
    cmds.setAttr("persp.rotate", -90.0, 0.0, 0.0)
    cmds.viewFit("persp", allObjects=True)
    api = cmds.groomLabGpuPreview(drawApi=True)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "colorscale_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    def tweak():
        groomlab.set_param(groom, "color", "melanin", 0.31)
        groomlab.set_param(groom, "color", "melanin", 0.3)

    # Each display: the lines, the hair shading with self-shadowing, ribbons, the clump level of detail.
    setups = [("lines", {}), ("hair", {"displayHairShading": 1, "displaySelfShadowing": 1.0}),
              ("ribbons", {"displayRibbons": True}), ("lod", {"displayClumpLod": True, "displayRibbons": False})]
    if deformed:
        setups = [("rest", {}), ("follow", {"followScalp": True}),
                  ("followhair", {"followScalp": True, "displayHairShading": 1, "displaySelfShadowing": 1.0})]
    for name, attrs in setups:
        for attr, value in attrs.items():
            cmds.setAttr(groom + "." + attr, value)
        plain = shot(name + "_plain")
        floor = differ(plain, shot(name + "_again"))
        for k in (2.0, 0.5):
            # In steps, as a drag of the scale manipulator evaluates it.
            for step in range(1, 9):
                s = 1.0 + (k - 1.0) * step / 8.0
                cmds.setAttr(scalp + ".scale", s, s, s)
                cmds.refresh(currentView=True, force=True)
            scaled = shot("%s_x%g" % (name, k))
            tweak()
            scaled_tweaked = shot("%s_x%g_tweaked" % (name, k))
            check("%s: hair color scaled x%g is as after a tweak on %s (%.4f of the pixels differ)"
                  % (name, k, api, differ(scaled, scaled_tweaked)), differ(scaled, scaled_tweaked) <= floor + 0.002)
            for step in range(1, 9):
                s = k + (1.0 - k) * step / 8.0
                cmds.setAttr(scalp + ".scale", s, s, s)
                cmds.refresh(currentView=True, force=True)
            back = shot("%s_back%g" % (name, k))
            tweak()
            tweaked = shot("%s_back%g_tweaked" % (name, k))
            check("%s: hair color scaled x%g and back is as before on %s (%.4f of the pixels differ, %.4f after a tweak)"
                  % (name, k, api, differ(plain, back), differ(plain, tweaked)),
                  differ(plain, back) <= max(floor, differ(plain, tweaked)) + 0.002)
    cmds.currentTime(1)
    cmds.file(new=True, force=True)


def overlay_check(snapshots=None):
    """Guides draw over dense strands (the overlay effect's pull toward the camera) on the device
    the session runs: green guides inside lit hair show many of their pixels on top, few with
    displayOverlaysOnTop off (depth-tested, the strands in front hide them). Without the effect
    (it did not compile) both draw depth-tested alike. The guides are guideWidth (4) pixels wide on
    both devices the sessions run (~900 pixels on either; a pixel-wide line, as legacy OpenGL's
    CgFX draws, shows less than a third)."""
    import tempfile
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    groomlab.set_param(groom, "scatter", "density", 4000.0)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    curves = []
    for k, angle in enumerate((0.3, 0.9, 1.4)):
        n = (math.sin(angle), math.cos(angle), 0.25 * k)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.4 * c for c in n), tuple(1.7 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.generate_children(groom, "primary", name="children", count=3)
    for group in ("primary", "children"):  # green guides: their groups' own color
        groomlab.set_guide_group_color(groom, group, (0.0, 1.0, 0.0))
    cmds.setAttr(groom + ".guidePoints", 0)  # the dots draw X-ray whatever the lines do: lines alone
    # Opaque strands (width as coverage off): thin strands drawn by their coverage let the guides
    # behind them show through, depth-tested or not.
    cmds.setAttr(groom + ".displayWidthCoverage", False)
    cmds.select(scalp)
    cmds.viewFit("persp", fitFactor=0.9)
    cmds.select(clear=True)
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_overlay_")

    def shot(name):
        """The capture and its guide pixels: green to cyan (the children's levels shift the hue),
        where the brown hair and the light background are neither."""
        from PySide2 import QtGui
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "overlay_check_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
        data = bytes(image.constBits())[:image.byteCount()]
        return path, sum(1 for i in range(0, len(data), 4) if data[i + 1] > 90 and data[i + 1] > 1.4 * data[i + 2])

    on_top, green = shot("on_top")
    cmds.setAttr(groom + ".displayOverlaysOnTop", False)
    under, hidden = shot("under")
    cmds.setAttr(groom + ".displayOverlaysOnTop", True)
    api = cmds.groomLabGpuPreview(drawApi=True)
    say("guides over %d strands (%s): %d guide pixels on top (%s), %d depth-tested (%s)"
        % (cmds.getAttr(groom + ".outStrandCount"), api, green, on_top, hidden, under))
    check("guides draw over dense strands on %s (%d guide pixels on top, %d depth-tested)" % (api, green, hidden),
          green > 400 and green > 3 * hidden and not groomlab.errors(groom))
    eyes_check(groom, folder)
    cmds.file(new=True, force=True)


def eyes_check(groom, folder):
    """The Groom Tools' eyes on overlay_check's groom (green guides in brown hair): the hair's eye
    (displayHair) hides the strands alone, the guides drawing on; the guides' eye (displayGuides)
    hides the guides alone, the strands drawing on. Neither hides the groom's transform."""
    import groomlab
    from PySide2 import QtGui
    transform = cmds.listRelatives(groom, parent=True, fullPath=True)[0]

    def shot(name):
        """The capture's guide pixels (green to cyan) and hair pixels (brown: red over blue, where the
        gray scalp and background are neither). (Format_RGB32's bytes: blue, green, red.)"""
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "eyes_maya_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
        data = bytes(image.constBits())[:image.byteCount()]
        guides = hair = 0
        for i in range(0, len(data), 4):
            if data[i + 1] > 90 and data[i + 1] > 1.4 * data[i + 2]:
                guides += 1
            elif data[i + 2] > data[i] + 20:
                hair += 1
        return guides, hair

    both = shot("both")
    groomlab.set_hair_visible(groom, False)
    no_hair = shot("hair_hidden")
    hidden_attrs = (cmds.getAttr(transform + ".visibility"), groomlab.guides_visible(groom))
    groomlab.set_hair_visible(groom, True)
    groomlab.set_guides_visible(groom, False)
    no_guides = shot("guides_hidden")
    groomlab.set_guides_visible(groom, True)
    say("eyes: (guide, hair) pixels %s both, %s hair hidden, %s guides hidden" % (both, no_hair, no_guides))
    check("the hair's eye hides the strands alone (guide pixels %d -> %d, hair %d -> %d; transform and guides %s)"
          % (both[0], no_hair[0], both[1], no_hair[1], hidden_attrs),
          no_hair[0] > 0.5 * both[0] and no_hair[0] > 200 and no_hair[1] < 0.25 * both[1] and hidden_attrs == (True, True))
    check("the guides' eye hides the guides alone (guide pixels %d -> %d, hair %d -> %d)"
          % (both[0], no_guides[0], both[1], no_guides[1]),
          no_guides[0] < 0.1 * both[0] and no_guides[1] > 0.8 * both[1])


CLUMP_CENTERS_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"},
              {"name": "scatter", "type": "Scatter", "params": {"density": 600.0, "length": 0.3}},
              {"name": "clump", "type": "Clump", "params": {"count": 40, "clumpChannel": "clumpId"}},
              {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "clump", "input": "strands"},
                    {"from": "clump", "to": "output", "input": "strands"}],
}


def clump_centers_check(snapshots=None):
    """Focus on a Clump (the groom's viewNode, the Graph Editor's eye): its clump centers draw
    as dots on the scalp, in the theme's clumpCenters color, as many as it picked (about its count
    of 40: 41 on this sphere), with the smooth
    mesh preview on too (the same centers, on the smooth surface); focus off, none."""
    import tempfile
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=24, subdivisionsY=16)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLUMP_CENTERS_GRAPH)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.select(scalp)
    cmds.viewFit("persp", fitFactor=0.9)
    cmds.select(clear=True)
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_centers_")

    def shot(name):
        """The dots counted (the viewport's) and the capture's pixels near their color."""
        from PySide2 import QtGui
        cmds.refresh(currentView=True, force=True)
        draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]
        centers = json.loads(cmds.groomLabGraphStats(groom, overlays=True))["clumpCenters"]
        path = os.path.join(folder, "clump_centers_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
        data = bytes(image.constBits())[:image.byteCount()]  # B, G, R, A
        # Every theme's clumpCenters is a magenta: red and blue well over green (the hair brown,
        # the background grey).
        pixels = sum(1 for i in range(0, len(data), 4) if data[i + 2] > 170 and data[i] > 140 and data[i + 1] < 120)
        return draw.get("clumpCenterDots", -1), len(centers), pixels, path

    off = shot("off")
    cmds.setAttr(groom + ".viewNode", "clump", type="string")
    on = shot("auto")
    cmds.setAttr(scalp + ".displaySmoothMesh", 2)
    smooth = shot("auto_smooth")
    cmds.setAttr(scalp + ".displaySmoothMesh", 0)
    cmds.setAttr(groom + ".viewNode", "", type="string")
    back = shot("back")
    say("clump centers: off %s, Clump viewed %s, smooth preview %s, back %s (dots drawn, centers, pixels, capture)"
        % (off, on, smooth, back))
    check("a viewed Clump draws its %d centers as dots (%d drawn, %d pixels), the same with the smooth preview "
          "(%d drawn), none without" % (on[1], on[0], on[2], smooth[0]),
          36 <= on[1] <= 44 and on[0] == on[1] and smooth[1] == on[1] and smooth[0] == on[1] and off[0] == 0 and back[0] == 0
          and on[2] > 100 and on[2] > 5 * max(off[2], 1) and not groomlab.errors(groom))
    cmds.file(new=True, force=True)


def cards_check(snapshots=None):
    """Baked cards look like their strands on the device the session runs (the card effect: its
    fragments register on DirectX 11 and the OpenGL Core Profile alike, a failure fails the
    session through FAILING_WARNINGS): about as many pixels covered, and of the strands' color.
    Clumps of three hues share two textures: each card's tint gives it its own (the full fidelity
    measure is tools/bench/card_fidelity)."""
    import tempfile
    import groomlab
    from PySide2 import QtGui
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    groomlab.set_param(groom, "scatter", "density", 3000.0)
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "clump", "type": "Clump", "params": {"count": 40, "strength": 0.7}})
    graph["nodes"].insert(-1, {"name": "color", "type": "HairColor",
                               "params": {"melanin": 0.2, "hueRandom": 0.6, "clumpShare": 1.0}})
    graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
    graph["connections"] += [{"from": "resample", "to": "clump", "input": "strands"},
                             {"from": "clump", "to": "color", "input": "strands"},
                             {"from": "color", "to": "output", "input": "strands"}]
    groomlab.set_graph(groom, graph)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    # The cards shade with Maya's hair tube graph: their strands compared drawn the same way.
    cmds.setAttr(groom + ".displayHairShading", 0)
    node = groomlab.create_cards(groom, lod="mid")
    groomlab.set_param(groom, node, "variations", 2)
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_cards_")
    groomlab.bake_cards(groom, directory=folder, resolution=512)
    cmds.select(scalp)
    cmds.viewFit("persp", fitFactor=0.9)
    cmds.select(clear=True)

    def shot(name):
        """Hair pixels (off the gray background and the scalp's own gray) and their mean color."""
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "cards_check_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
        data = bytes(image.constBits())[:image.byteCount()]
        hair, total = 0, [0, 0, 0]
        for i in range(0, len(data), 4):
            b, g, r = data[i], data[i + 1], data[i + 2]
            if max(r, g, b) - min(r, g, b) < 12:  # gray: the background, the scalp
                continue
            hair += 1
            total = [total[0] + r, total[1] + g, total[2] + b]
        return path, hair, [t / float(max(hair, 1)) for t in total]

    groomlab.compare_cards(groom, False)  # create_cards showed the cards
    strands_path, strands, strand_color = shot("strands")
    groomlab.compare_cards(groom, True)
    cards_path, cards, card_color = shot("cards")
    groomlab.compare_cards(groom, False)
    api = cmds.groomLabGpuPreview(drawApi=True)
    off = max(abs(a - b) for a, b in zip(strand_color, card_color))
    say("cards on %s: %d hair pixels against the strands' %d (%s, %s); mean color %s against %s"
        % (api, cards, strands, cards_path, strands_path, ["%.0f" % c for c in card_color],
           ["%.0f" % c for c in strand_color]))
    check("baked cards cover about what their strands do on %s (%d pixels against %d)" % (api, cards, strands),
          strands > 2000 and 0.75 * strands < cards < 1.25 * strands)
    check("baked cards show their strands' color on %s (off by %.0f of 255)" % (api, off), off < 25)
    # With the viewport's multisample anti-aliasing the cards' coverage becomes the samples they cover
    # (cardDraws: sampled), with Maya's hair graph and GroomLab's hair alike; about as many hair
    # pixels as cut out.
    draws = lambda: json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["cardDraws"]
    groomlab.compare_cards(groom, True)
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", True)
    try:
        smooth_paths = []
        for shading in (0, 1):
            cmds.setAttr(groom + ".displayHairShading", shading)
            cmds.refresh(currentView=True, force=True)  # (the cards rebuilt with the other shader)
            before = draws()
            smooth_path, smooth, _ = shot("cards_msaa_%d" % shading)
            smooth_paths.append(smooth_path)
            sampled = draws()["sampled"] - before["sampled"]
            say("cards with multisampling on %s (hair shading %d): %d hair pixels (%s), %d sampled draws"
                % (api, shading, smooth, smooth_path, sampled))
            check("cards with multisampling on %s (hair shading %d) draw their coverage as samples (%d draws)"
                  % (api, shading, sampled), sampled > 0)
            if shading == 0:
                check("cards with multisampling on %s cover about what cut out ones do (%d pixels against %d)"
                      % (api, smooth, cards), 0.75 * cards < smooth < 1.25 * cards)
        # Hair shading switched in the cards view swaps the cards' shader (GroomLab's hair: another picture).
        changed = differ(*smooth_paths)
        check("the cards follow the hair shading on %s (%.3f of the pixels differ)" % (api, changed), changed > 0.01)
    finally:
        cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", False)
        cmds.setAttr(groom + ".displayHairShading", 0)
    # Wireframe on shaded (issue #19): the cards' quads' edges over them, as over a mesh; in
    # wireframe the edges alone. (The scalp hidden: only the cards' wires can differ.)
    groomlab.compare_cards(groom, True)
    cmds.setAttr(scalp + ".visibility", False)
    panel = persp_panel()
    try:
        shaded_path = shot("cards_shaded")[0]
        cmds.modelEditor(panel, edit=True, wireframeOnShaded=True)
        wired_path = shot("cards_wire_on_shaded")[0]
        cmds.modelEditor(panel, edit=True, displayAppearance="wireframe")
        wire_path, wire_pixels, wire_color = shot("cards_wireframe")
    finally:
        cmds.modelEditor(panel, edit=True, wireframeOnShaded=False, displayAppearance="smoothShaded")
        cmds.setAttr(scalp + ".visibility", True)
    wired = differ(shaded_path, wired_path)
    check("wireframe on shaded on %s draws the cards' edges over them (%.4f of the pixels differ)" % (api, wired),
          wired > 0.005)
    # (Maya's dormant wireframe color is a dark blue; the cards' faces are the hair's color.)
    check("wireframe on %s draws the cards' edges, not their faces (%d pixels, mean color %s; %s)"
          % (api, wire_pixels, ["%.0f" % c for c in wire_color], wire_path),
          wire_pixels > 0 and wire_color[2] > wire_color[0] + 40)
    groomlab.compare_cards(groom, False)
    check("the cards made no errors", not groomlab.errors(groom))
    cmds.file(new=True, force=True)


def strand_colors_check(snapshots=None):
    """The lit strands in their own colors: a HairColor node reading a color map painted in three
    colors (a texture map, mode replace) shows three colors in the solid lit look, as the render
    does, not their mean; per point with a tip blend; a tweak moving the strands alone keeps the
    colors on the GPU; a GPU drag draws them too. On both APIs (the viewport suite runs again on
    DirectX 11)."""
    import tempfile
    import groomlab
    from PySide2 import QtGui
    folder = tempfile.mkdtemp(prefix="groomlab_colors_")
    cmds.file(new=True, force=True)
    cmds.file(rename=os.path.join(folder, "colors.ma"))
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    # (The drawing's paths compared picture for picture: the hair's own shadows, from the grid each path
    # writes when its strands move, aside; gui_hair checks them.)
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    shape = cmds.listRelatives(groom, shapes=True, fullPath=True)[0] if cmds.nodeType(groom) != "groomLabShape" else groom
    # Three bands around the sphere (u): red, green, blue.
    stripes = QtGui.QImage(48, 48, QtGui.QImage.Format_RGBA8888)
    for x in range(48):
        color = QtGui.QColor(*((230, 30, 30), (30, 200, 40), (40, 60, 230))[x // 16])
        for y in range(48):
            stripes.setPixelColor(x, y, color)
    png = os.path.join(folder, "stripes.png")
    stripes.save(png)
    groomlab.add_map(groom, "color", kind="texture", color=(1.0, 1.0, 1.0), resolution=48)
    groomlab.set_map_texture(groom, "color", png, copy=True)
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "noise", "type": "Noise", "params": {"amplitude": 0.03}})
    graph["nodes"].insert(-1, {"name": "color", "type": "HairColor",
                               "params": {"mode": "color", "map": "color"}})
    # (The iridescence below: FeatherColor's, nothing to do until then.)
    graph["nodes"].insert(-1, {"name": "sheen", "type": "FeatherColor", "params": {"pattern": "none"}})
    into = next(c for c in graph["connections"] if c["to"] == "output")
    graph["connections"] = [c for c in graph["connections"] if c is not into]
    graph["connections"] += [{"from": into["from"], "to": "noise", "input": "strands"},
                             {"from": "noise", "to": "color", "input": "strands"},
                             {"from": "color", "to": "sheen", "input": "strands"},
                             {"from": "sheen", "to": "output", "input": "strands"}]
    for node in graph["nodes"]:
        if node["name"] == "resample":  # even: the strands keep their counts (the GPU takes no other)
            node.setdefault("params", {})["mode"] = "even"
    groomlab.set_graph(groom, graph)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.select(clear=True)
    # From above: the three bands meet at the pole, each a third of the picture.
    cmds.setAttr("persp.translate", 0.0, 6.0, 0.01)
    cmds.setAttr("persp.rotate", -90.0, 0.0, 0.0)
    cmds.viewFit("persp", allObjects=True)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(snapshots or folder, "colors_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path, hue_counts(path)

    def three(counts):
        """Each color a good share of the clearly colored pixels."""
        total = counts["r"] + counts["g"] + counts["b"]
        return total > 1000 and all(counts[k] > 0.2 * total for k in "rgb")  # (Scatter at its default 80)

    lit, counts = shot("lit")
    stats = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"].get("groomLabStrands", {})
    say("lit strands in their own colors (%s): %s; drawn %s" % (lit, counts, stats))
    check("the lit strands show the three colors of the painted map, not one (%s)" % counts,
          three(counts) and not groomlab.errors(groom))
    points = json.loads(cmds.groomLabGraphStats(groom))["points"]
    # Written whole: points, the uvs (u, width) and the colors (RGBA floats), and the normals and
    # tangents where the CPU writes them: 36 bytes a point (60 with the frames), and the indices.
    check("their colors stream goes up with them (%s MB, %d points)" % (stats.get("megabytes"), points),
          stats.get("megabytes", 0) >= points * (36 if stats.get("gpuFrames") else 60) / 1e6 - 0.01)
    # A tweak moving the strands alone (Noise): their points go up (and frames, where the CPU
    # writes them), the colors stay.

    groomlab.set_param(groom, "noise", "amplitude", 0.04)
    _, counts = shot("tweaked")
    tweak = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"].get("groomLabStrands", {})
    check("a tweak keeping the colors uploads the lit strands' points alone, and their frames where the CPU writes them "
          "(%s MB, %d points, frames by the GPU %s), the colors kept (%s)" % (tweak.get("megabytes"), points, tweak.get("gpuFrames"),
                                                                             counts),
          abs(tweak.get("megabytes", 0) - points * (12 if tweak.get("gpuFrames") else 36) / 1e6) < 0.01 and three(counts))
    # Per point: a tip blend toward white lightens the tips, the three colors kept at the roots.
    groomlab.set_param(groom, "color", "tipStrength", 0.6)
    groomlab.set_param(groom, "color", "tipColor", (1.0, 1.0, 1.0))
    tipped, counts = shot("tip")
    with open(lit, "rb") as a, open(tipped, "rb") as b:
        other = a.read() != b.read()
    check("a tip blend draws per point: another picture, the three colors kept (%s)" % counts, other and three(counts))
    groomlab.set_param(groom, "color", "tipStrength", 0.0)
    # A material's color (or useMaterialColor off: the display color) is every strand's: one color.
    cmds.setAttr(groom + ".useMaterialColor", False)
    _, one = shot("display_color")
    cmds.setAttr(groom + ".useMaterialColor", True)
    check("useMaterialColor off draws the lit strands in the display color, one color (%s)" % one, not three(one))
    # With a shadowing light, shadows on the strands and ambient occlusion: the shadow and AO passes
    # draw the colored item too, the colors kept.
    light = cmds.directionalLight(intensity=1.2, rotation=(-60.0, 30.0, 0.0))
    cmds.setAttr(light + ".useDepthMapShadows", True)
    lights = cmds.modelEditor(persp_panel(), query=True, displayLights=True)
    shadows = cmds.modelEditor(persp_panel(), query=True, shadows=True)
    cmds.modelEditor(persp_panel(), edit=True, displayLights="all", shadows=True)
    cmds.setAttr(groom + ".displayShadows", 2)
    cmds.setAttr("hardwareRenderingGlobals.ssaoEnable", True)
    _, counts = shot("shadowed")
    cmds.setAttr("hardwareRenderingGlobals.ssaoEnable", False)
    cmds.setAttr(groom + ".displayShadows", 1)
    cmds.modelEditor(persp_panel(), edit=True, displayLights=lights, shadows=shadows)
    cmds.delete(cmds.listRelatives(light, parent=True) or light)
    check("lit by a shadowing light with shadows and ambient occlusion, the three colors show (%s)" % counts, three(counts))
    # Lit ribbons take the colors too.
    cmds.setAttr(groom + ".displayRibbons", True)
    _, counts = shot("ribbons")
    cmds.setAttr(groom + ".displayRibbons", False)
    check("lit ribbons show the three colors (%s)" % counts, three(counts))
    # A comb stroke whose strands change color as they move (the tip blend's strength reads the
    # points' height): its release writes the changed strands' points, frames and colors alone, and
    # draws what writing them whole draws.
    groomlab.set_param(groom, "color", "tipStrength", {"expression": "smoothstep(0.8, 1.6, py)", "min": 0.0, "max": 0.6})
    curves = []
    for k, angle in enumerate((0.3, 0.9, 1.4, 2.0)):
        n = (math.sin(angle), math.cos(angle), 0.25 * k - 0.3)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.4 * c for c in n), tuple(1.7 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.generate_children(groom, "primary", name="children", count=4)
    shot("guides")
    groomlab.brush_stroke(groom, "comb", [(0.3, 1.0, -0.3), (0.5, 0.95, -0.3)], radius=0.15, level=-1)
    released, counts = shot("stroke")
    stroke = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"].get("groomLabStrands", {})
    cmds.setAttr(shape + ".displayLighting", False)  # every strand written again
    cmds.refresh(currentView=True, force=True)
    cmds.setAttr(shape + ".displayLighting", True)
    redrawn, _ = shot("stroke_redrawn")
    count = cmds.getAttr(groom + ".outStrandCount")
    say("a stroke on strands changing color: %s of %s strands written alone (%s MB), %.4f of the pixels differ from a "
        "whole write" % (stroke.get("partialStrands"), count, stroke.get("megabytes"), differ(released, redrawn)))
    check("a stroke's release writes the changed strands' colors with their points and draws a whole write's "
          "picture (%s)" % counts, 0 < stroke.get("partialStrands", 0) < count and stroke.get("megabytes", 0) > 0
          and differ(released, redrawn) < 0.001 and three(counts))
    groomlab.set_param(groom, "color", "tipStrength", 0.0)
    # Iridescence (FeatherColor's structural color, in the colors' alpha): the strands take the
    # iridescent color, its hue turned as the view leaves each strand's normal (a feather's vane;
    # the scalp's here, leaned along the strand); from above, the hair at the pole faces the
    # view (blue), the hair round the sides is seen edge-on (turned to red).
    # (Strands a pixel wide or more: width as coverage leaves their colors whole, the hue reads.)
    groomlab.set_param(groom, "width", "width", 0.05)
    groomlab.set_param(groom, "sheen", "iridescence", 1.0)
    groomlab.set_param(groom, "sheen", "iridescenceColor", (0.1, 0.2, 0.9))
    groomlab.set_param(groom, "sheen", "iridescenceShift", 0.0)
    _, blue = shot("iridescent")
    colored = max(1, blue["r"] + blue["g"] + blue["b"])
    check("iridescent strands take the structural color (%s)" % blue, blue["b"] > 0.6 * colored)
    groomlab.set_param(groom, "sheen", "iridescenceShift", 0.33)
    _, turned = shot("iridescent_turned")
    colored = max(1, turned["r"] + turned["g"] + turned["b"])
    check("its hue turns with the view's angle to the strands' normals: blue facing it, red edge-on (%s)" % turned,
          turned["r"] > 0.1 * colored and turned["b"] > 0.1 * colored)
    groomlab.set_param(groom, "sheen", "iridescence", 0.0)
    groomlab.set_param(groom, "width", "width", 0.007)
    # A GPU drag (Noise) draws the strands' colors, the item's colors stream bound beside the
    # GPU's points.
    api = cmds.groomLabGpuPreview(drawApi=True)
    if cmds.about(windows=True) and api in GPU_PREVIEW_APIS:
        cmds.refresh(currentView=True, force=True)
        taken = cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.06)
        _, counts = shot("gpu_drag")
        cmds.groomLabGpuPreview(shape, endDrag=True)
        say("GPU drag in the strands' own colors (%s): taken %s, %s" % (api, taken, counts))
        check("a GPU drag draws the lit strands in their own colors (%s)" % counts, taken and three(counts))
    check("no errors on the colored groom", not groomlab.errors(groom))
    cmds.file(new=True, force=True)


def draw_serial():
    """How many viewport updates wrote or ran something so far (groomLabGraphStats -drawTimes)."""
    groom = cmds.ls(type="groomLabShape")[0]
    return json.loads(cmds.groomLabGraphStats(groom, drawTimes=True)).get("draw", {}).get("serial", 0)


def run_steps(steps, then, ms=150):
    """Runs a generator's steps, Maya's event loop running `ms` between them (its idle work, the
    viewport's idle redraw, Qt's timers: what happens between a slider's steps), then `then`."""
    from PySide2 import QtCore

    from gui_support import guarded

    def step():
        try:
            next(steps)
        except StopIteration:
            then()
            return
        QtCore.QTimer.singleShot(ms, guarded(step))
    soon(step)


def live_drag_steps():
    """A slider drag updates the view at every step before its release, by the GPU preview or, where
    the GPU refuses, by the CPU: the default graph and hair_basics, Noise's amplitude, Clump's
    strength, Width's width and Curl's radius dragged in the Graph Editor's param panel (its
    preview_param, as each step of a slider calls it), a keyed param there too, and a keyed param's
    attribute set step by step (the Attribute Editor, the Channel Box). Between steps Maya's event
    loop runs (each yield): nothing forces a redraw, what draws is what draws in a session. (A GPU
    drag changes nothing in the scene: it draws because groomLabGpuPreview schedules the redraw.)"""
    import maya.OpenMayaUI as omui

    import groomlab
    from groomlab import presets
    from groomlab.ui import graph_editor

    api = cmds.groomLabGpuPreview(drawApi=True)
    for preset in (None, "hair_basics"):
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
        groom = groomlab.create_groom(scalp)
        if preset:
            presets.apply_preset(groom, preset)
        cmds.select(clear=True)
        cmds.viewFit("persp", allObjects=True)
        cmds.refresh(currentView=True, force=True)
        yield
        label = preset or "default graph"
        nodes = {}
        for node in groomlab.get_graph(groom, expand_default=True)["nodes"]:
            nodes.setdefault(node["type"], node)
        editor = graph_editor.GraphEditor(groom)
        for kind, param in (("Noise", "amplitude"), ("Clump", "strength"), ("Width", "width"), ("Curl", "radius")):
            node = nodes.get(kind)
            if node is None:
                say("live drag, %s: no %s node" % (label, kind))
                continue
            name = node["name"]
            start = node.get("params", {}).get(param)
            if not isinstance(start, (int, float)) or isinstance(start, bool) or start == 0:
                start = 0.01 if param in ("width", "radius") else 0.5
            steps, gpu = [], False
            for k in range(4):
                before = draw_serial()
                editor.preview_param(name, param, start * (1.15 + 0.1 * k))
                gpu = gpu or getattr(editor, "_gpu_dragging", False)
                yield
                steps.append(draw_serial() > before)
            editor.set_param(name, param, start * 1.45)  # the release
            yield
            check("live drag, %s on %s: every step of a Graph Editor drag of %s.%s draws before the release (%s, "
                  "steps drawn %s)" % (label, api, name, param, "GPU" if gpu else "CPU", steps), all(steps))
        # A keyed param: through the Graph Editor, and its attribute set step by step.
        clump = nodes.get("Clump") or nodes.get("Noise")
        param = "strength" if clump["type"] == "Clump" else "amplitude"
        groomlab.set_param_key(groom, clump["name"], param)  # keyed: its attribute ctl_<node>_<param>
        yield
        steps = []
        for k in range(4):
            before = draw_serial()
            editor.preview_param(clump["name"], param, 0.3 + 0.1 * k)
            yield
            steps.append(draw_serial() > before)
        editor.set_param(clump["name"], param, 0.65)
        yield
        check("live drag, %s on %s: every step of a keyed %s.%s's Graph Editor drag draws (steps drawn %s)"
              % (label, api, clump["name"], param, steps), all(steps))
        plug = groomlab.param_plugs(groom, clump["name"], param)[0]
        steps = []
        for k in range(4):
            before = draw_serial()
            cmds.setAttr(plug, 0.2 + 0.1 * k)
            omui.M3dView.scheduleRefreshAllViews()  # what Maya does after a command its UI ran
            yield
            steps.append(draw_serial() > before)
        check("live drag, %s on %s: every step of a keyed attribute's drag (Attribute Editor, Channel Box) draws "
              "(steps drawn %s)" % (label, api, steps), all(steps))
        editor.deleteLater()
        check("live drag, %s: no errors on the groom" % label, not groomlab.errors(groom))


def gpu_preview_check():
    """The GPU look preview (hosts/maya/src/gpu_preview.h, on by default): a drag of Noise, Curl and
    Width draws from the GPU, what the viewport shows is the CPU's picture for the same value, its
    results match the CPU's, the release draws the CPU's again, and undo after it goes back; then
    what a session does around drags (bounds, strokes, keys, views, scenes, the plugin reloaded)."""
    import tempfile

    import groomlab
    if not cmds.about(windows=True):
        say("GPU preview: skipped (Windows only)")
        return
    check("GPU preview: on by default", groomlab.gpu_preview())
    api = cmds.groomLabGpuPreview(drawApi=True)
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    # (The drawing's paths compared picture for picture: the hair's own shadows, from the grid each path
    # writes when its strands move, aside; gui_hair checks them.)
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "noise", "type": "Noise", "params": {"amplitude": 0.05}})
    graph["nodes"].insert(-1, {"name": "curl", "type": "Curl", "params": {"radius": 0.02}})
    graph["connections"] = [c for c in graph["connections"] if not (c["from"] == "length" and c["to"] == "width")]
    graph["connections"] += [{"from": "length", "to": "noise", "input": "strands"},
                             {"from": "noise", "to": "curl", "input": "strands"},
                             {"from": "curl", "to": "width", "input": "strands"}]
    for node in graph["nodes"]:
        if node["name"] == "resample":  # even: the strands keep their counts (the GPU takes no other)
            node.setdefault("params", {})["mode"] = "even"
    groomlab.set_graph(groom, graph)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.select(clear=True)
    cmds.viewFit("persp", allObjects=True)
    cmds.refresh(currentView=True, force=True)
    shape = cmds.listRelatives(groom, shapes=True, fullPath=True)[0] if cmds.nodeType(groom) != "groomLabShape" else groom
    folder = tempfile.mkdtemp(prefix="groomlab_gpu_")

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, name + ".png")
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    before = shot("cpu_before")
    # Off (the preference): a drag is the CPU's; on again, the GPU's.
    groomlab.gpu_preview(False)
    refused = cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.08)
    groomlab.gpu_preview(True)
    check("GPU preview: turned off, a drag is not taken", not refused and groomlab.gpu_preview())
    taken = cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.08)
    drawn = shot("gpu_drag")
    stats = json.loads(cmds.groomLabGpuPreview(shape, stats=True, wait=True))
    say("GPU preview on %s: taken %s, %s" % (api, taken, stats))
    if api not in GPU_PREVIEW_APIS:
        check("GPU preview: not taken where Viewport 2.0 draws with %s" % api, not taken)
        cmds.groomLabGpuPreview(shape, endDrag=True)
        return
    check("GPU preview: a Noise drag runs on the GPU (%s)" % api, taken and stats.get("runs", 0) >= 1)
    check("GPU preview: the graph is left alone during the drag",
          next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == "noise")["params"]["amplitude"] == 0.05)
    cmds.groomLabGpuPreview(shape, endDrag=True)
    # The release commits on the CPU (one undo step); undo after it puts the value from before
    # the drag back, drawn by the CPU (a playblast empties Maya's undo queue: none in between).
    from gui_support import settle_undo
    settle_undo()
    groomlab.set_param(groom, "noise", "amplitude", 0.08)
    cmds.refresh(currentView=True, force=True)
    cmds.undo()
    amplitude = next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == "noise")["params"]["amplitude"]
    undone = shot("cpu_undone")
    check("GPU preview: undo after a drag's commit puts the value back and draws it (%s, %.4f differ)"
          % (amplitude, differ(before, undone)), amplitude == 0.05 and differ(before, undone) < 0.002)
    groomlab.set_param(groom, "noise", "amplitude", 0.08)
    committed = shot("cpu_commit")
    after = json.loads(cmds.groomLabGpuPreview(shape, stats=True))
    gpu_vs_cpu, cpu_vs_before = differ(drawn, committed), differ(before, committed)
    say("GPU preview frames: GPU drag vs CPU commit %.4f of the pixels differ, CPU before vs after %.4f; display buffers "
        "during the drag %s MB, after %s MB" % (gpu_vs_cpu, cpu_vs_before, stats.get("displayMB"), after.get("displayMB")))
    # Fine hair: a sub-pixel move changes many pixels (the value's change: ~1.5% at Scatter's default
    # 80, where the hashed coverage of the strands crossing a pixel keeps it covered as they move);
    # float rounding a few.
    check("GPU preview: the viewport draws the GPU's results, the CPU's picture for the value (%.4f differ, the "
          "value's change %.4f)" % (gpu_vs_cpu, cpu_vs_before), gpu_vs_cpu < 0.01 and cpu_vs_before > 0.012)
    if api == "DirectX11" and not os.environ.get("GROOMLAB_GPU_PREVIEW_COPY"):
        check("GPU preview: DirectX 11 draws the preview's own buffers, written in place, and lets them go after",
              stats.get("inPlace") and stats.get("displayCustomHandle") and stats.get("displayMB", 0) > 0
              and after.get("displayMB") == 0)
    compared = json.loads(cmds.groomLabGpuPreview(shape, check=True))
    say("GPU preview against the CPU:", compared)
    check("GPU preview: the GPU's points and frames are the CPU's (rounding)",
          compared.get("positions", {}).get("max", 1) < 1e-3 and compared.get("normals", {}).get("max", 1) < 1e-3)
    # The node feeding the look nodes changed and a drag before the next draw: the drawn frame is
    # the new strands' (uploaded again), the commit's.
    cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.07)
    shot("gpu_before_base_change")
    cmds.groomLabGpuPreview(shape, endDrag=True)
    groomlab.set_param(groom, "length", "keep", 0.8)
    cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.07)
    drawn = shot("gpu_after_base_change")
    cmds.groomLabGpuPreview(shape, endDrag=True)
    groomlab.set_param(groom, "noise", "amplitude", 0.07)
    committed = shot("cpu_after_base_change")
    say("a drag in the frame the base changed: %.4f of the pixels differ from the commit" % differ(drawn, committed))
    check("GPU preview: a drag in the frame its base changes draws the new strands", differ(drawn, committed) < 0.01)
    groomlab.set_param(groom, "noise", "amplitude", 0.08)
    # Through the Graph Editor: a slider drag previews on the GPU (the graph left alone), its
    # release commits the value on the CPU.
    from groomlab.ui import graph_editor
    editor = graph_editor.GraphEditor(groom)
    before = cmds.getAttr(groom + ".graph")
    runs = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
    editor.preview_param("curl", "radius", 0.03)
    cmds.refresh(currentView=True, force=True)
    during = json.loads(cmds.groomLabGpuPreview(shape, stats=True))
    untouched = cmds.getAttr(groom + ".graph") == before
    editor.set_param("curl", "radius", 0.03)
    cmds.refresh(currentView=True, force=True)
    committed = groomlab.get_graph(groom)
    radius = next(n for n in committed["nodes"] if n["name"] == "curl")["params"].get("radius")
    say("Graph Editor drag: GPU runs %s -> %s, graph untouched %s, committed radius %s"
        % (runs, during.get("runs"), untouched, radius))
    check("GPU preview: a Graph Editor slider drag draws from the GPU and its release commits on the CPU",
          during.get("runs", 0) > runs and untouched and radius == 0.03 and not editor.previewing())
    # A profile's ramp (the ramp editor's keys) and a seed (its slider) through the Graph Editor:
    # the GPU's too, each drawing what its commit draws.
    noise_params = next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == "noise")["params"]
    for param, value in (("profile", [[0.0, 0.2], [0.5, 1.0], [1.0, 0.3]]), ("seed", 7)):
        start = noise_params.get(param, 0 if param == "seed" else [[0.0, 1.0], [1.0, 1.0]])
        unchanged = shot("cpu_noise_before_" + param)  # (evaluated: the drag starts from the groom as it is)
        runs = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
        before = cmds.getAttr(groom + ".graph")
        editor.preview_param("noise", param, value)
        drawn = shot("gpu_noise_" + param)
        during = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
        untouched = cmds.getAttr(groom + ".graph") == before
        editor.set_param("noise", param, value)
        committed = shot("cpu_noise_" + param)
        say("Graph Editor drag of noise.%s: GPU runs %s -> %s, graph untouched %s, %.4f of the pixels differ from the commit, "
            "the value's change %.4f" % (param, runs, during, untouched, differ(drawn, committed), differ(unchanged, committed)))
        check("GPU preview: a Graph Editor drag of noise.%s (%s) draws from the GPU what its commit draws"
              % (param, "a ramp" if param == "profile" else "a seed"),
              during > runs and untouched and differ(drawn, committed) < 0.01 and differ(unchanged, committed) > 0.01
              and not editor.previewing())
        groomlab.set_param(groom, "noise", param, start)
    # A vector (a weighted Curl's weightDirection, three numbers) through the Graph Editor: the
    # GPU's too (-vector), drawing what its commit draws.
    groomlab.set_param(groom, "curl", "weight", 0.8)
    unchanged = shot("cpu_curl_before_weightDirection")
    runs = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
    before = cmds.getAttr(groom + ".graph")
    editor.preview_param("curl", "weightDirection", [1.0, -1.0, 0.0])
    drawn = shot("gpu_curl_weightDirection")
    during = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
    untouched = cmds.getAttr(groom + ".graph") == before
    editor.set_param("curl", "weightDirection", [1.0, -1.0, 0.0])
    committed = shot("cpu_curl_weightDirection")
    say("Graph Editor drag of curl.weightDirection: GPU runs %s -> %s, graph untouched %s, %.4f of the pixels differ from "
        "the commit, the value's change %.4f" % (runs, during, untouched, differ(drawn, committed), differ(unchanged, committed)))
    check("GPU preview: a Graph Editor drag of curl.weightDirection (a vector) draws from the GPU what its commit draws",
          during > runs and untouched and differ(drawn, committed) < 0.01 and not editor.previewing())
    groomlab.set_param(groom, "curl", "weightDirection", [0.0, -1.0, 0.0])
    groomlab.set_param(groom, "curl", "weight", 0.0)
    editor.deleteLater()
    # Mapped params and world-space noise: Curl's radius by an expression along the strand, Noise
    # in world space; a drag of Noise's frequency draws what its commit draws.
    groomlab.set_param(groom, "curl", "radius", {"expression": "0.01 + 0.03 * pt", "min": 0.0, "max": 1.0})
    groomlab.set_param(groom, "noise", "space", "world")
    cmds.refresh(currentView=True, force=True)
    taken = cmds.groomLabGpuPreview(shape, node="noise", param="frequency", value=3.0)
    drawn = shot("gpu_mapped")
    cmds.groomLabGpuPreview(shape, endDrag=True)
    groomlab.set_param(groom, "noise", "frequency", 3.0)
    committed = shot("cpu_mapped")
    say("GPU preview of mapped params: taken %s, %.4f of the pixels differ from the commit" % (taken, differ(drawn, committed)))
    check("GPU preview: mapped params and world-space noise draw on the GPU what the commit draws",
          taken and differ(drawn, committed) < 0.01)
    # Bounds: a drag pushing hair far out of the CPU's last bounds draws with bounds holding it
    # (Viewport 2.0 culls, fits shadows and clip planes by an item's bounds): those the GPU draws
    # with hold the commit's at the same value. (Maya 2022 did not cull the item with stale bounds
    # in this view beside the groom; the frame is compared all the same.)
    groomlab.set_param(groom, "curl", "radius", 0.02)
    groomlab.set_param(groom, "noise", "space", "strand")
    # (Curl at the points it is given: the noise stretches the strands to many more turns, and a
    # commit adding points to them would draw rounder curls than the GPU's preview of the drag.)
    groomlab.set_param(groom, "curl", "tolerance", 0.0)
    lo = cmds.exactWorldBoundingBox(groom)
    size = max(lo[3] - lo[0], lo[4] - lo[1], lo[5] - lo[2])
    camera = cmds.camera(focalLength=120.0)[0]
    cmds.xform(camera, worldSpace=True, translation=(lo[3] + 1.1 * size, 0.5 * (lo[1] + lo[4]), lo[5] + 4.0 * size))
    panel = cmds.getPanel(withFocus=True) if cmds.getPanel(typeOf=cmds.getPanel(withFocus=True)) == "modelPanel" else "modelPanel4"
    cmds.modelPanel(panel, edit=True, camera=camera)
    taken = cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=1.5 * size)
    drawn = shot("aside_gpu")
    grown = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("bounds") or [0] * 6
    cmds.groomLabGpuPreview(shape, endDrag=True)
    groomlab.set_param(groom, "noise", "amplitude", 1.5 * size)
    committed = shot("aside_commit")
    cpu = cmds.exactWorldBoundingBox(groom)
    holds = all(grown[k] <= cpu[k] + 1e-4 for k in range(3)) and all(grown[k] >= cpu[k] - 1e-4 for k in range(3, 6))
    say("GPU preview beside the groom: taken %s, bounds drawn %s hold the commit's %s: %s; frames differ by %.4f"
        % (taken, ["%.2f" % v for v in grown], ["%.2f" % v for v in cpu], holds, differ(drawn, committed)))
    check("GPU preview: a drag pushing hair out of the CPU's bounds draws it, with bounds holding it",
          taken and holds and differ(drawn, committed) < 0.01)
    groomlab.set_param(groom, "curl", "tolerance", 0.5)
    cmds.modelPanel(panel, edit=True, camera="persp")
    cmds.delete(camera)
    groomlab.set_param(groom, "noise", "amplitude", 0.05)

    # A brush stroke, then a drag: the strands entering the look nodes changed in the stroke's
    # strands alone (incremental evaluation), and only those go up again. Guides grown from a few
    # curves (the stroke combs them).
    curves = []
    for k, angle in enumerate((0.3, 0.9, 1.4, 2.0)):
        n = (math.sin(angle), math.cos(angle), 0.25 * k - 0.3)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.4 * c for c in n), tuple(1.7 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.generate_children(groom, "primary", name="children", count=4)
    cmds.refresh(currentView=True, force=True)
    cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.06)  # the strands up as they are
    cmds.refresh(currentView=True, force=True)
    cmds.groomLabGpuPreview(shape, endDrag=True)
    cmds.refresh(currentView=True, force=True)
    uploads = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("uploads", 0)
    groomlab.brush_stroke(groom, "comb", [(0.3, 1.0, -0.3), (0.5, 0.95, -0.3)], radius=0.15, level=-1)
    cmds.refresh(currentView=True, force=True)
    cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.06)
    drawn = shot("stroke_gpu")
    stroked = json.loads(cmds.groomLabGpuPreview(shape, stats=True))
    cmds.groomLabGpuPreview(shape, endDrag=True)
    groomlab.set_param(groom, "noise", "amplitude", 0.06)
    committed = shot("stroke_commit")
    count = cmds.getAttr(groom + ".outStrandCount")
    say("a drag after a brush stroke: upload %s -> %s, %s of %s strands (partial %s, %.3f MB), %.4f of the pixels differ"
        % (uploads, stroked.get("uploads"), stroked.get("uploadStrands"), count, stroked.get("uploadPartial"),
           stroked.get("uploadMB", 0), differ(drawn, committed)))
    check("GPU preview: a drag after a brush stroke uploads the stroke's strands alone and draws the commit",
          stroked.get("uploads", 0) == uploads + 1 and stroked.get("uploadPartial")
          and 0 < stroked.get("uploadStrands", 0) < count and differ(drawn, committed) < 0.01)
    groomlab.set_param(groom, "noise", "amplitude", 0.05)

    # A keyed param: its drag runs on the GPU, the attribute left alone until the release keys it
    # (Auto Key on: a key at the current time, which a playblast's time change keeps).
    import maya.mel
    groomlab.set_param_key(groom, "curl", "radius", 0.02, time=1)
    groomlab.set_param_key(groom, "curl", "radius", 0.06, time=20)
    cmds.currentTime(1)
    auto_key = cmds.autoKeyframe(query=True, state=True)
    cmds.autoKeyframe(state=True)
    plugs = groomlab.param_plugs(groom, "curl", "radius")
    editor = graph_editor.GraphEditor(groom)
    runs = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
    editor.preview_param("curl", "radius", 0.04)
    drawn = shot("keyed_gpu")
    during = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
    attribute_during = cmds.getAttr(plugs[0])
    editor.set_param("curl", "radius", 0.04)
    committed = shot("keyed_commit")
    cmds.autoKeyframe(state=auto_key)
    say("keyed drag: GPU runs %s -> %s, attribute during %s, after %s, %.4f of the pixels differ"
        % (runs, during, attribute_during, cmds.getAttr(plugs[0]), differ(drawn, committed)))
    check("GPU preview: a keyed param's drag runs on the GPU and its release sets the attribute",
          during > runs and abs(attribute_during - 0.02) < 1e-6 and abs(cmds.getAttr(plugs[0]) - 0.04) < 1e-6
          and differ(drawn, committed) < 0.01 and not editor.previewing())
    editor.deleteLater()

    gpu_phase2_check(groom, shape, shot, differ)

    # Four views drawing the groom: one run a value (the override updates once for all of them).
    maya.mel.eval('setNamedPanelLayout "Four View"')
    cmds.refresh(force=True)
    runs = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("runs", 0)
    cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.07)
    cmds.refresh(force=True)
    cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.075)
    cmds.refresh(force=True)
    four = json.loads(cmds.groomLabGpuPreview(shape, stats=True))
    cmds.groomLabGpuPreview(shape, endDrag=True)
    maya.mel.eval('setNamedPanelLayout "Single Perspective View"')
    say("four views: GPU runs %s -> %s (%s)" % (runs, four.get("runs"), four.get("why")))
    check("GPU preview: four views draw a drag with one run a value", four.get("runs", 0) - runs == 2)
    check("GPU preview: no errors on the groom", not groomlab.errors(groom))

    # A new scene in the middle of a drag (no release): the next groom starts without it.
    cmds.groomLabGpuPreview(shape, node="noise", param="amplitude", value=0.09)
    cmds.refresh(currentView=True, force=True)
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    other = groomlab.create_groom(scalp)
    cmds.refresh(currentView=True, force=True)
    other_shape = cmds.listRelatives(other, shapes=True, fullPath=True)[0] if cmds.nodeType(other) != "groomLabShape" else other
    fresh = json.loads(cmds.groomLabGpuPreview(other_shape, stats=True))
    check("GPU preview: a new scene in the middle of a drag leaves nothing behind (%s)" % fresh,
          fresh.get("runs", 0) == 0 and not groomlab.errors(other))

    # The plugin unloaded with nothing of the preview left, loaded again: it works again.
    cmds.file(new=True, force=True)
    path = cmds.pluginInfo("GroomLab", query=True, path=True)
    cmds.unloadPlugin("GroomLab")
    cmds.loadPlugin(path)
    reloaded = "groomLabGpuPreview" in (cmds.pluginInfo("GroomLab", query=True, command=True) or [])
    check("GPU preview: the plugin unloads and loads again with the preview (%s)" % reloaded, reloaded)
    cmds.file(new=True, force=True)


def gpu_phase2_check(groom, shape, shot, differ):
    """The GPU runs the graph from the first node that changes: Clump (its centers as the CPU chose
    them), Interpolate (the CPU's binding, the guides' shapes; the centers made again from the
    strands it blends) and Length drags draw what their commits draw; a brush stroke's dabs after
    its press are the GPU's (the shape evaluates the guides alone), and its release is the CPU's,
    as with the preview off."""
    import groomlab
    from gui_support import settle_undo, to_view
    graph = groomlab.get_graph(groom)
    graph["nodes"].insert(-1, {"name": "clump", "type": "Clump", "params": {"count": 60, "strength": 0.5}})
    graph["connections"] = [c for c in graph["connections"] if not (c["from"] == "length" and c["to"] == "noise")]
    graph["connections"] += [{"from": "length", "to": "clump", "input": "strands"},
                             {"from": "clump", "to": "noise", "input": "strands"}]
    groomlab.set_graph(groom, graph)
    cmds.refresh(currentView=True, force=True)
    check("GPU phase 2: the graph with a Clump evaluates", not groomlab.errors(groom))

    def param(node, name):
        return next(n for n in groomlab.get_graph(groom)["nodes"] if n["name"] == node).get("params", {}).get(name)


    for node, name, value, fallback in (("clump", "strength", 0.85, 0.5), ("interpolate", "limitDistance", 0.3, 0.0),
                                        ("length", "keep", 0.8, 1.0)):
        own = param(node, name)
        own = fallback if own is None else own
        taken = cmds.groomLabGpuPreview(shape, node=node, param=name, value=value)
        drawn = shot("gpu_%s_%s" % (node, name))
        stats = json.loads(cmds.groomLabGpuPreview(shape, stats=True, wait=True))
        cmds.groomLabGpuPreview(shape, endDrag=True)
        groomlab.set_param(groom, node, name, value)
        committed = shot("cpu_%s_%s" % (node, name))
        say("GPU drag of %s.%s: taken %s, runs %s (%s), %.4f of the pixels differ from the commit; %s"
            % (node, name, taken, stats.get("stages"), stats.get("gpu"), differ(drawn, committed), stats.get("why")))
        check("GPU phase 2: a %s.%s drag runs from %s on the GPU and draws what its commit draws" % (node, name, node),
              taken and (stats.get("stages") or "").startswith(node) and differ(drawn, committed) < 0.01)
        groomlab.set_param(groom, node, name, own)
    compared = json.loads(cmds.groomLabGpuPreview(shape, check=True, **{"from": "interpolate"}))
    say("GPU from Interpolate against the CPU:", compared)
    check("GPU phase 2: from Interpolate the GPU's points and frames are the CPU's (rounding)",
          compared.get("positions", {}).get("max", 1) < 1e-3 and compared.get("normals", {}).get("max", 1) < 1e-3)

    # A comb stroke drawn after every dab: its dabs after the press run on the GPU (evaluating the
    # guides alone), the release commits on the CPU and draws the CPU's strands: the frame after it
    # is the one a full redraw of the same groom draws (no GPU-written vertex left).
    cmds.select(cmds.listRelatives(groom, parent=True, fullPath=True) or groom)
    ctx = groomlab.brush_tool("comb", radius=150, strength=1.0)
    x0, y0 = to_view((0.4, 1.0, -0.2))
    stroke = "draw " + " ".join("%d %d" % (x0 - 40 + 8 * k, y0 + 2 * k) for k in range(10))
    before = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("stroke", {})
    data0 = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=stroke)
    after = json.loads(cmds.groomLabGpuPreview(shape, stats=True))
    draws = json.loads(cmds.groomLabBrushCtx(ctx, query=True, debugStrokeDraws=True) or "[]")
    released = shot("stroke_gpu_release")
    changed = cmds.getAttr(groom + ".guideData") != data0
    cmds.setToolTo("selectSuperContext")
    cmds.deleteUI(ctx, toolContext=True)  # (the plugin is unloaded below: no context of it left behind)
    cmds.setAttr(shape + ".displayLighting", False)  # every strand written again from the CPU's
    cmds.refresh(currentView=True, force=True)
    cmds.setAttr(shape + ".displayLighting", True)
    redrawn = shot("stroke_cpu_redrawn")
    runs = after.get("stroke", {}).get("runs", 0) - before.get("runs", 0)
    compute = [round(d.get("computeMs", 0), 2) for d in draws]
    say("GPU stroke: %s dabs drawn by the GPU (%s), compute ms per step %s, the release against a full redraw %.4f differ; %s"
        % (runs, after.get("stroke"), compute, differ(released, redrawn), after.get("why")))
    check("GPU phase 2: a comb stroke's dabs after its press are drawn by the GPU", changed and runs >= 5)
    check("GPU phase 2: the stroke's release draws the CPU's strands (as a full redraw does)", differ(released, redrawn) < 0.001)
    if after.get("api") == "DirectX11" and not os.environ.get("GROOMLAB_GPU_PREVIEW_COPY"):
        done = json.loads(cmds.groomLabGpuPreview(shape, stats=True))
        check("GPU phase 2: after the stroke the preview's own buffers are let go", done.get("displayMB") == 0)
    check("GPU phase 2: no errors on the groom", not groomlab.errors(groom))


def async_commit_steps():
    """Commits off the UI thread (groom_shape.h expectCommit, B7): a GPU drag's release and a GPU
    stroke's release draw at once what the GPU drew (the commit's look) while the commit evaluates on a
    worker, then land (on Maya's idle, or as soon as something needs the live session): the pictures
    after equal a full redraw of the commit (as before), undo and redo take one step each, and a
    second stroke, a save, playback, a node edit and a new scene while one is pending are right. Maya's
    event loop runs at each yield (its idle lands a commit)."""
    import tempfile

    import groomlab
    from gui_support import settle_undo, to_view
    if not cmds.about(windows=True):
        say("async commits: skipped (Windows only)")
        return
    api = cmds.groomLabGpuPreview(drawApi=True)
    if api not in GPU_PREVIEW_APIS:
        say("async commits: skipped (%s)" % api)
        return
    cmds.file(new=True, force=True)
    cmds.undoInfo(state=True, infinity=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    from groomlab import presets
    presets.apply_preset(groom, "hair_basics")
    curves = []
    for k, angle in enumerate((0.3, 0.9, 1.4, 2.0, 2.6, 3.3)):
        n = (math.sin(angle), math.cos(angle), 0.2 * k - 0.5)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.4 * c for c in n), tuple(1.7 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.generate_children(groom, "primary", name="children", count=4)
    shape = cmds.listRelatives(groom, shapes=True, fullPath=True)[0] if cmds.nodeType(groom) != "groomLabShape" else groom
    cmds.setAttr(shape + ".previewDensity", 1.0)
    cmds.setAttr(shape + ".displaySelfShadowing", 0.0)  # (its grid aside: gui_hair checks it)
    cmds.setAttr(shape + ".sculptDisplay", 0)  # the dabs drawn as the strands look: the release's look
    cmds.setAttr(shape + ".sculptDensity", 1.0)
    cmds.select(clear=True)
    cmds.viewFit("persp", allObjects=True)
    cmds.refresh(currentView=True, force=True)
    folder = tempfile.mkdtemp(prefix="groomlab_async_")

    def info():
        return json.loads(cmds.groomLabGraphStats(shape, frameInfo=True))

    def settle():
        cmds.groomLabGraphStats(shape)  # (it reads the live session: waits for a pending commit)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, name + ".png")
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    def redrawn(name):
        # Every strand written again from the CPU's (flat, then lit).
        settle()
        cmds.setAttr(shape + ".displayLighting", False)
        cmds.refresh(currentView=True, force=True)
        cmds.setAttr(shape + ".displayLighting", True)
        return shot(name)

    def amplitude():
        return next(n for n in groomlab.get_graph(groom, expand_default=True)["nodes"] if n["type"] == "Noise")["params"].get("amplitude")

    noise = next(n for n in groomlab.get_graph(groom, expand_default=True)["nodes"] if n["type"] == "Noise")["name"]
    start = amplitude()
    yield

    # A GPU drag released: the commit evaluates behind the drawn frame (the GPU's), lands on Maya's idle.
    def release_drag(value):
        took = cmds.groomLabGpuPreview(shape, node=noise, param="amplitude", value=value)
        cmds.refresh(currentView=True, force=True)
        before = info()
        cmds.groomLabGpuPreview(shape, endDrag=True)
        groomlab.set_param(groom, noise, "amplitude", value)
        t = time.time()
        cmds.refresh(currentView=True, force=True)
        return took, before, info(), (time.time() - t) * 1000.0

    settle_undo()
    took, before, during, ms = release_drag(start * 1.6)
    say("async commits on %s: a drag's release drawn in %.1f ms, %s" % (api, ms, during))
    check("async commits: a GPU drag's release commits off the UI thread (%s), the frame drawn still the one "
          "before it (version %s), its strand count the commit's" % (api, during.get("version")),
          took and during.get("pending") and during.get("version") == before.get("version")
          and during.get("evaluations") == before.get("evaluations")
          and cmds.getAttr(shape + ".outStrandCount") == during.get("strands"))
    for _ in range(40):  # Maya's event loop: its idle lands it once evaluated (cached playback's fill may share the cores)
        yield
        if not info().get("pending"):
            break
    landed = info()
    check("async commits: the commit lands on Maya's idle once evaluated (%s)" % landed,
          not landed.get("pending") and landed.get("settledBy") == "idle" and landed.get("commits") == during.get("commits", 0) + 1
          and landed.get("version") != during.get("version") and landed.get("evaluations") > during.get("evaluations"))
    cmds.undo()
    undone = amplitude()
    cmds.redo()
    check("async commits: the drag's commit undoes and redoes as one step (%s, %s)" % (undone, amplitude()),
          abs(undone - start) < 1e-6 and abs(amplitude() - start * 1.6) < 1e-6)
    # Pictures: while pending the GPU's frame; landed, the CPU's, a full redraw's.
    took, before, during, ms = release_drag(start * 1.3)
    held = shot("drag_held")
    pending_after_shot = info().get("pending")
    settle()
    committed = shot("drag_committed")
    full = redrawn("drag_redrawn")
    say("async commits: drag pictures held vs committed %.4f, committed vs redrawn %.4f (pending through the playblast: %s)"
        % (differ(held, committed), differ(committed, full), pending_after_shot))
    check("async commits: the frame held while a drag's commit evaluates is the commit's picture, and once it lands "
          "the commit draws as a full redraw (%.4f, %.4f)" % (differ(held, committed), differ(committed, full)),
          during.get("pending") and differ(held, committed) < 0.01 and differ(committed, full) < 0.001)
    # A node edit while pending: waited for, then the edit evaluates (in compute).
    release_drag(start * 1.1)
    groomlab.set_param(groom, noise, "frequency", 3.0)
    cmds.refresh(currentView=True, force=True)
    edited = info()
    check("async commits: an edit while a commit is pending waits for it and evaluates (%s)" % edited,
          not edited.get("pending") and edited.get("settledBy") == "compute" and not groomlab.errors(groom))
    yield

    # Brush strokes on the GPU: the release commits off the UI thread.
    cmds.select(cmds.listRelatives(shape, parent=True, fullPath=True)[0])
    ctx = groomlab.brush_tool("comb", radius=150, strength=1.0)
    yield
    x0, y0 = to_view((0.4, 1.0, -0.2))
    strokes = [0]

    def stroke():
        strokes[0] += 1
        dx = 10 * (strokes[0] % 5)
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="draw " + " ".join(
            "%d %d" % (x0 - 40 + 8 * k + dx, y0 + 2 * k - dx) for k in range(10)))
        return json.loads(cmds.groomLabBrushCtx(ctx, query=True, debugStrokeDraws=True) or "[]")

    def evaluated_afresh():
        # The groom's points as a copy of it evaluates them in full (kept off the undo queue).
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            copy = cmds.duplicate(cmds.listRelatives(shape, parent=True, fullPath=True)[0], inputConnections=True)[0]
            copy_shape = cmds.listRelatives(copy, shapes=True, fullPath=True, type="groomLabShape")[0]
            points = cmds.groomLabGraphStats(copy_shape, points=True)
            cmds.delete(copy)
            cmds.select(cmds.listRelatives(shape, parent=True, fullPath=True)[0])
        finally:
            cmds.undoInfo(stateWithoutFlush=True)
        return points

    settle_undo()
    data0 = cmds.getAttr(shape + ".guideData")
    runs0 = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("stroke", {}).get("runs", 0)
    draws = stroke()
    data1 = cmds.getAttr(shape + ".guideData")
    first = info()
    runs = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("stroke", {}).get("runs", 0) - runs0
    # Another stroke at once: it chains on the first commit (groom_shape.h chainStroke), waiting for
    # nothing (its press and dabs on the GPU, from the session as the first stroke left it), and its
    # own commit goes on from the first's once that has run; only the last lands.
    runs1 = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("stroke", {}).get("runs", 0)
    draws2 = stroke()
    data2 = cmds.getAttr(shape + ".guideData")
    second = info()
    runs2 = json.loads(cmds.groomLabGpuPreview(shape, stats=True)).get("stroke", {}).get("runs", 0) - runs1
    say("async commits: stroke releases %s ms (GPU dabs %d), first %s, second %s (its press %.1f ms, GPU dabs %d)"
        % ([round(d.get("computeMs", 0), 1) for d in draws[-1:]], runs, first, second,
           draws2[0].get("cpuMs", 0) if draws2 else -1, runs2))
    check("async commits: a GPU stroke's release commits off the UI thread (%d dabs on the GPU)" % runs,
          runs >= 5 and data1 != data0 and draws and draws[-1].get("pending") and first.get("pending"))
    check("async commits: a second stroke right after the release chains on its commit, waiting for nothing, its "
          "dabs on the GPU, then commits its own after it (%s)" % second,
          data2 != data1 and second.get("pending") and second.get("chained") == first.get("chained", 0) + 1
          and second.get("commits") == first.get("commits", 0) and runs2 >= 5 and draws2)
    settle()
    # What the chained commits landed is, to the bit, what the groom evaluated afresh makes (a copy
    # of it, evaluated in full).
    landed_points = cmds.groomLabGraphStats(shape, points=True)
    fresh_points = evaluated_afresh()
    check("async commits: chained commits land what a fresh evaluation makes, to the bit (%d values)" % len(landed_points or []),
          landed_points and landed_points == fresh_points)
    cmds.undo()
    one = cmds.getAttr(shape + ".guideData")
    cmds.undo()
    two = cmds.getAttr(shape + ".guideData")
    cmds.redo()
    cmds.redo()
    check("async commits: each stroke undoes and redoes as one step", one == data1 and two == data0
          and cmds.getAttr(shape + ".guideData") == data2)
    # Two strokes chained, the second undone while both commits are pending: one step back, and the
    # groom then is the first stroke's, to the bit.
    stroke()
    data3 = cmds.getAttr(shape + ".guideData")
    stroke()
    chained = info()
    cmds.undo()
    cmds.refresh(currentView=True, force=True)
    undone = info()
    undone_points = cmds.groomLabGraphStats(shape, points=True)
    fresh_points = evaluated_afresh()
    check("async commits: a chained stroke undone while the commits are pending goes one step back, as a fresh "
          "evaluation (%s, %s)" % (chained, undone),
          chained.get("pending") and cmds.getAttr(shape + ".guideData") == data3 and not undone.get("pending")
          and undone_points == fresh_points)
    cmds.redo()
    settle()
    yield
    # Pictures after a stroke: held (the GPU's last dab), then landed (the CPU's), as a full redraw.
    stroke()
    held = shot("stroke_held")
    settle()
    committed = shot("stroke_committed")
    full = redrawn("stroke_redrawn")
    say("async commits: stroke pictures held vs committed %.4f, committed vs redrawn %.4f"
        % (differ(held, committed), differ(committed, full)))
    check("async commits: a stroke's commit, landed, draws as a full redraw, and the frame held meanwhile is its "
          "look (%.4f, %.4f)" % (differ(held, committed), differ(committed, full)),
          differ(committed, full) < 0.001 and differ(held, committed) < 0.01)
    if api == "DirectX11" and not os.environ.get("GROOMLAB_GPU_PREVIEW_COPY"):
        done = json.loads(cmds.groomLabGpuPreview(shape, stats=True))
        check("async commits: the preview's own buffers go once the commit lands", done.get("displayMB") == 0)
    # Maya's idle lands a stroke's commit too.
    stroke()
    for _ in range(40):
        yield
        if not info().get("pending"):
            break
    check("async commits: a stroke's commit lands on Maya's idle (%s)" % info(), info().get("settledBy") == "idle")
    # Saved while pending: the file holds the commit (the stored texts), and opens to it.
    stroke()
    pending = info().get("pending")
    data = cmds.getAttr(shape + ".guideData")
    path = os.path.join(folder, "pending.ma")
    cmds.file(rename=path)
    cmds.file(save=True, type="mayaAscii", force=True)
    saved_pending = info().get("pending")
    settle()
    yield
    # Playback (a time change) while pending: waited for, the new frame evaluated.
    stroke()
    cmds.currentTime(5)
    cmds.refresh(currentView=True, force=True)
    played = info()
    check("async commits: a time change while a commit is pending waits for it and draws the new frame (%s)" % played,
          not played.get("pending") and abs(played.get("seconds", 0) - 5.0 / 24.0) < 1e-6 and not groomlab.errors(groom))
    cmds.currentTime(1)
    cmds.setToolTo("selectSuperContext")
    cmds.deleteUI(ctx, toolContext=True)
    cmds.file(path, open=True, force=True)
    shapes = cmds.ls(type="groomLabShape", long=True)
    reopened = cmds.getAttr(shapes[0] + ".guideData") if shapes else None
    check("async commits: saved while a commit was pending, the file opens to the commit (pending at the save: %s, "
          "after it: %s)" % (pending, saved_pending), pending and reopened == data)
    yield
    # A new scene while a commit is pending: the groom goes (its worker waited for), nothing left behind.
    shape = cmds.ls(type="groomLabShape", long=True)[0]
    cmds.select(cmds.listRelatives(shape, parent=True, fullPath=True)[0])
    took = cmds.groomLabGpuPreview(shape, node=noise, param="amplitude", value=start * 1.2)
    cmds.refresh(currentView=True, force=True)
    cmds.groomLabGpuPreview(shape, endDrag=True)
    groomlab.set_param(shape, noise, "amplitude", start * 1.2)
    cmds.refresh(currentView=True, force=True)
    was = json.loads(cmds.groomLabGraphStats(shape, frameInfo=True)).get("pending")
    cmds.file(new=True, force=True)
    yield
    check("async commits: a new scene while a commit is pending (%s) leaves nothing behind" % was,
          not cmds.ls(type="groomLabShape"))


def overlay_snapshots(folder):
    """GROOMLAB_VIEWPORT_SNAPSHOTS: pictures of what the strand looks above leave out (guides by
    level, theme, layer and group, tubes, selection, parts, flow, the maps on the scalp,
    instances, cards, a Scatter's roots, a stroke's dimmed strands), to compare a change of the
    draw code against: the same build draws them the same, pixel for pixel."""
    import groomlab

    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, CLASSIC_GRAPH)  # these cases name and wire that chain's nodes
    cmds.setAttr(groom + ".previewDensity", 0.3)
    curves = []
    for k, angle in enumerate((0.3, 0.9, 1.4)):
        n = (math.sin(angle), math.cos(angle), 0.25 * k)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.4 * c for c in n), tuple(1.7 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.generate_children(groom, "primary", name="children", count=3)
    primaries = groomlab.guide_ids(groom, "primary")
    cmds.select(scalp)
    cmds.viewFit("persp", fitFactor=0.9)
    cmds.select(clear=True)

    def shot(name):
        cmds.refresh(force=True)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=os.path.join(folder, "overlay_" + name + ".png"))

    shot("guides")
    cmds.setAttr(groom + ".displayTubes", True)
    shot("tubes")
    cmds.setAttr(groom + ".displayTubes", False)
    cmds.setAttr(groom + ".isolateGroup", "children", type="string")
    shot("guide_level")
    cmds.setAttr(groom + ".isolateGroup", "", type="string")
    cmds.setAttr(groom + ".guidePoints", 1)
    cmds.setAttr(groom + ".guideSmoothing", 1)
    shot("guide_groups")  # roots and tips on straight curves, each group in its own color
    cmds.setAttr(groom + ".guidePoints", 2)
    cmds.setAttr(groom + ".guideSmoothing", 4)
    groomlab.set_guide_attribute(groom, "len", 0.8, guides=primaries[:1])
    groomlab.freeze_guides(groom, primaries[1:2])
    cmds.setAttr(groom + ".guideColorMode", 1)
    cmds.setAttr(groom + ".guideColorLayer", "len", type="string")
    shot("guide_layer")
    cmds.setAttr(groom + ".guideColorMode", 0)
    groomlab.select_guides(groom, guides=primaries[:1])
    groomlab.set_soft_selection(groom, 1.0)
    shot("selection")
    groomlab.clear_guide_selection(groom)
    groomlab.brush_stroke(groom, "part", [(0.1, 1.0, -0.3), (0.2, 0.95, 0.0), (0.3, 0.9, 0.3)])
    groomlab.flow_stroke(groom, [(-0.6, 0.8, -0.2), (-0.5, 0.8, 0.1), (-0.4, 0.8, 0.4)])
    groomlab.flow_sink(groom, (0.0, 0.3, 0.95), "whorl", radius=0.4)
    shot("parts_flow")
    cmds.setAttr(groom + ".displayOverlaysOnTop", False)
    shot("parts_flow_under")
    cmds.setAttr(groom + ".displayOverlaysOnTop", True)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr(groom + ".displayFlow", False)
    groomlab.add_map(groom, "fur", value=0.2, kind="vertices", color=(0.2, 0.5, 0.9, 1.0))
    groomlab.brush_stroke(groom, "paint", [(0.0, 1.0, 0.0), (0.3, 0.9, 0.3)], radius=0.4, attribute="fur",
                          paint_kind="vertices", paint_color=(1.0, 0.3, 0.1, 1.0))
    groomlab.show_map_on_scalp(groom, "fur", "rgb")
    shot("map_vertices")
    groomlab.show_map_on_scalp(groom, "fur", "g")
    shot("map_vertices_g")
    groomlab.show_map_on_scalp(groom, "len", "rgb")
    shot("map_layer")
    groomlab.show_map_on_scalp(groom, None)
    cube = cmds.polyCube(width=1, height=2, depth=1)[0]
    cmds.setAttr(cube + ".visibility", False)
    groomlab.add_instances(groom, cube, probability=0.3, scale=0.05)
    shot("instances")
    cmds.setAttr(groom + ".displayInstances", False)
    groomlab.create_cards(groom, max_cards=12)
    shot("cards")
    cmds.setAttr(groom + ".displayCards", False)
    cmds.setAttr(groom + ".viewNode", "scatter", type="string")
    shot("roots")
    cmds.setAttr(groom + ".viewNode", "", type="string")
    cmds.setAttr(groom + ".displayGuides", True)
    cmds.setAttr(groom + ".strandColorMode", 0)
    cmds.setAttr(groom + ".displayLighting", False)
    shot("flat")
    say("overlay snapshots in", folder)
