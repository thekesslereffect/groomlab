# SPDX-License-Identifier: Apache-2.0
"""Viewport pictures (opt-in GUI suite "pictures": a look-over tool for display changes, few checks).

Run alone in a GUI session: GROOMLAB_GUI_SUITES=pictures, GROOMLAB_PICTURES_OUT=<folder>. Playblasts
the strands in each display mode after a CPU-evaluated tweak (lit lines with width as coverage and
without, unlit, colored, ribbons lit, unlit and colored, shadows, the strands' own colors, selected;
ribbons again where it matters), through a GPU drag (lines, and ribbons)
and after it, a brush stroke's release (written in part) and a deforming scalp's playback frames, then
the same followed (Follow Scalp: the strands at rest carried by the GPU, lines and ribbons);
each lit case again with the GPU frames off (groomLabGpuPreview -frames off: the CPU writes them),
named cpu_<case>.png beside <case>.png. Each case's drawn buffers against the CPU's
(groomLabGpuPreview -checkFrames) and its upload go to the log. With GROOMLAB_PICTURES_BEFORE (a
folder of an earlier run: another build) every picture is compared with the one of the same name
there, and each case with its CPU twin: the pixels that differ, and by how much.
"""

import json
import math
import os

from maya import cmds

from gui_support import finish, persp_panel, say, soon

SIZE = (640, 480)


def start():
    soon(run)


def out_dir():
    folder = os.environ.get("GROOMLAB_PICTURES_OUT")
    if not folder:
        import tempfile
        folder = tempfile.mkdtemp(prefix="groomlab_pictures_")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    return folder


def shot(name, groom=None):
    cmds.refresh(currentView=True, force=True)
    path = os.path.join(out_dir(), name + ".png")
    cmds.playblast(frame=[cmds.currentTime(query=True)], format="image", compression="png", viewer=False,
                   showOrnaments=False, percent=100, widthHeight=SIZE, completeFilename=path)
    if groom:
        draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["items"]
        try:
            frames = json.loads(cmds.groomLabGpuPreview(groom, checkFrames=True) or "{}")
        except Exception:
            frames = {}
        say("%s: drawn %s; buffers against the CPU's %s" % (
            name, dict((k, (round(v.get("megabytes", 0), 2), v.get("gpuFrames"), v.get("partialStrands"))) for k, v in draw.items()),
            frames))
    return path


def pixels(path):
    from groomlab.ui.qt import QtGui
    image = QtGui.QImage(path)
    if image.isNull():
        return None, None
    image = image.convertToFormat(QtGui.QImage.Format_RGB32)
    return image.size(), bytes(image.constBits())[:image.sizeInBytes()]


def compare(a, b):
    """(pixels differing at all, differing by more than 1 in a channel, the largest difference, pixels)."""
    sa, da = pixels(a)
    sb, db = pixels(b)
    if da is None or db is None or sa != sb:
        return None
    count = len(da) // 4
    any_diff = big = worst = 0
    if da == db:
        return 0, 0, 0, count
    for i in range(0, len(da), 4):
        if da[i:i + 3] != db[i:i + 3]:
            any_diff += 1
            d = max(abs(da[i + c] - db[i + c]) for c in range(3))
            worst = max(worst, d)
            if d > 1:
                big += 1
    return any_diff, big, worst, count


def gpu_frames(on):
    """GPU frames on or off (a build without them: nothing to switch)."""
    try:
        cmds.groomLabGpuPreview(frames=on)
    except Exception:
        pass


def tweak(groom, i):
    import groomlab
    groomlab.set_param(groom, "noise", "amplitude", 0.03 + 0.005 * (i % 4))


def frame_view():
    """The camera at an angle, the whole groom in view."""
    cmds.setAttr("persp.rotate", -24.0, 45.0, 0.0)
    cmds.setAttr("perspShape.focalLength", 35.0)
    cmds.viewFit("persp", allObjects=True)
    cmds.refresh(force=True)


def both(name, groom, step=None):
    """The case after `step` (a tweak: the same point counts, other points) with the GPU frames, then
    the same strands written again with the CPU's."""
    if step:
        step()
    shot(name, groom)
    gpu_frames(False)
    shot("cpu_" + name, groom)
    gpu_frames(True)


def run():
    import groomlab
    gpu_frames(True)
    folder = out_dir()
    say("pictures into", folder, "on", cmds.groomLabGpuPreview(drawApi=True))
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    groom = groomlab.create_groom(scalp)
    graph = groomlab.get_graph(groom, expand_default=True)
    if not any(n["type"] == "Noise" for n in graph["nodes"]):
        graph["nodes"].insert(-1, {"name": "noise", "type": "Noise", "params": {"amplitude": 0.03}})
        into = next(c for c in graph["connections"] if c["to"] == "output")
        graph["connections"] = [c for c in graph["connections"] if c is not into]
        graph["connections"] += [{"from": into["from"], "to": "noise", "input": "strands"},
                                 {"from": "noise", "to": "output", "input": "strands"}]
        groomlab.set_graph(groom, graph)
    groomlab.set_param(groom, "scatter", "density", 5000.0)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.select(clear=True)
    frame_view()
    say("strands:", cmds.getAttr(groom + ".outStrandCount"))
    counter = [0]

    def tweaked():
        counter[0] += 1
        tweak(groom, counter[0])

    modes = (
        ("lit_coverage", {}),
        ("lit_opaque", {"displayWidthCoverage": False}),
        ("unlit_coverage", {"displayLighting": False}),
        ("colored_random", {"strandColorMode": 3}),
        ("colored_clump", {"strandColorMode": 4}),
        ("ribbons_lit", {"displayRibbons": True}),
        ("ribbons_wide", {"displayRibbons": True, "ribbonWidthScale": 4.0}),
        ("ribbons_unlit", {"displayRibbons": True, "displayLighting": False, "ribbonWidthScale": 4.0}),
        ("ribbons_colored", {"displayRibbons": True, "strandColorMode": 3, "ribbonWidthScale": 4.0}),
    )
    defaults = {"displayLighting": True, "displayWidthCoverage": True, "strandColorMode": 0, "displayRibbons": False,
                "displayShadows": 0, "ribbonWidthScale": 1.0}
    for name, attrs in modes:
        for attr, value in defaults.items():
            cmds.setAttr(groom + "." + attr, value)
        for attr, value in attrs.items():
            cmds.setAttr(groom + "." + attr, value)
        cmds.refresh(force=True)
        both(name, groom, tweaked)
    for attr, value in defaults.items():
        cmds.setAttr(groom + "." + attr, value)
    # Selected (highlighted).
    cmds.select(groom)
    both("selected", groom, tweaked)
    cmds.setAttr(groom + ".displayRibbons", True)
    cmds.setAttr(groom + ".ribbonWidthScale", 4.0)
    both("ribbons_selected", groom, tweaked)
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr(groom + ".ribbonWidthScale", 1.0)
    cmds.select(clear=True)
    # A shadowing light, shadows on the strands and ambient occlusion.
    light = cmds.directionalLight(intensity=1.2, rotation=(-60.0, 30.0, 0.0))
    cmds.setAttr(light + ".useDepthMapShadows", True)
    panel = persp_panel()
    cmds.modelEditor(panel, edit=True, displayLights="all", shadows=True)
    cmds.setAttr(groom + ".displayShadows", 2)
    cmds.setAttr("hardwareRenderingGlobals.ssaoEnable", True)
    both("lit_shadows", groom, tweaked)
    cmds.setAttr(groom + ".displayRibbons", True)
    cmds.setAttr(groom + ".ribbonWidthScale", 4.0)
    both("ribbons_shadows", groom, tweaked)
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr(groom + ".ribbonWidthScale", 1.0)
    cmds.setAttr("hardwareRenderingGlobals.ssaoEnable", False)
    cmds.setAttr(groom + ".displayShadows", 0)
    cmds.modelEditor(panel, edit=True, displayLights="default", shadows=False)
    cmds.delete(cmds.listRelatives(light, parent=True) or light)
    # A GPU drag (Noise), and after it (the item's own buffers again, written by the CPU's commit).
    cmds.refresh(force=True)
    taken = cmds.groomLabGpuPreview(groom, node="noise", param="amplitude", value=0.06)
    shot("gpu_drag", groom)
    cmds.groomLabGpuPreview(groom, endDrag=True)
    groomlab.set_param(groom, "noise", "amplitude", 0.06)
    shot("gpu_drag_committed", groom)
    say("GPU drag taken:", taken)
    # The same with ribbons: the GPU expands the drag's lines into them.
    cmds.setAttr(groom + ".displayRibbons", True)
    cmds.setAttr(groom + ".ribbonWidthScale", 4.0)
    cmds.refresh(force=True)
    taken = cmds.groomLabGpuPreview(groom, node="noise", param="amplitude", value=0.075)
    drag = shot("gpu_drag_ribbons", groom)
    say("GPU drag with ribbons: taken %s, %s" % (taken, cmds.groomLabGpuPreview(groom, stats=True)))
    cmds.groomLabGpuPreview(groom, endDrag=True)
    groomlab.set_param(groom, "noise", "amplitude", 0.075)
    committed = shot("gpu_drag_ribbons_committed", groom)
    say("GPU drag with ribbons against its commit:", compare(drag, committed))
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr(groom + ".ribbonWidthScale", 1.0)
    # The strands' own colors (a HairColor varying the melanin per strand: lit, colors stream).
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "haircolor", "type": "HairColor", "params": {"melanin": 0.4, "melaninRandom": 0.6}})
    into = next(c for c in graph["connections"] if c["to"] == "output")
    graph["connections"] = [c for c in graph["connections"] if c is not into]
    graph["connections"] += [{"from": into["from"], "to": "haircolor", "input": "strands"},
                             {"from": "haircolor", "to": "output", "input": "strands"}]
    groomlab.set_graph(groom, graph)
    say("own colors: errors %s, %s strands" % (groomlab.errors(groom), cmds.getAttr(groom + ".outStrandCount")))
    both("own_colors", groom, tweaked)
    cmds.setAttr(groom + ".displayRibbons", True)
    cmds.setAttr(groom + ".ribbonWidthScale", 4.0)
    both("ribbons_own_colors", groom, tweaked)
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr(groom + ".ribbonWidthScale", 1.0)
    # A brush stroke's release: the changed strands written alone.
    curves = []
    for k, angle in enumerate((0.3, 0.9, 1.4, 2.0)):
        n = (math.sin(angle), math.cos(angle), 0.25 * k - 0.3)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.4 * c for c in n), tuple(1.7 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.generate_children(groom, "primary", name="children", count=4)
    cmds.setAttr(groom + ".displayGuides", False)
    shot("guides", groom)
    groomlab.brush_stroke(groom, "comb", [(0.3, 1.0, -0.3), (0.5, 0.95, -0.3)], radius=0.3, level=-1)
    stroke = shot("stroke", groom)
    cmds.setAttr(groom + ".displayLighting", False)  # every strand written again
    cmds.refresh(currentView=True, force=True)
    cmds.setAttr(groom + ".displayLighting", True)
    whole = shot("stroke_whole", groom)
    # (Reported only: under DirectX 11 this scene's strands written whole after their lighting went
    # off and on draw a little darker at the pole than the stroke's, before the GPU frames as after.)
    say("stroke written in part against whole:", compare(stroke, whole))
    # Playback on a deforming scalp (a bend keyed): every frame new points and root normals.
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_param(groom, "scatter", "density", 5000.0)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    bend = cmds.nonLinear(scalp, type="bend")[0]
    for frame, curvature in ((1, 0.0), (5, 60.0)):
        cmds.setKeyframe(bend, attribute="curvature", time=frame, value=curvature)
    cmds.select(clear=True)
    frame_view()
    for frame in (1, 3, 5):
        cmds.currentTime(frame)
        shot("bend_%d" % frame, groom)
    gpu_frames(False)
    for frame in (1, 3, 5):
        cmds.currentTime(frame)
        shot("cpu_bend_%d" % frame, groom)
    gpu_frames(True)
    # The same playback following the scalp (a rest mesh, Follow Scalp on): the groom evaluated at
    # rest once, each frame's strands carried by their roots on the GPU (lines and ribbons); the CPU
    # twins carry them on the CPU (the GPU frames off).
    cmds.currentTime(1)
    groomlab.set_rest_mesh(groom)
    groomlab.set_follow_scalp(groom, True)
    for look, attrs in (("follow", {}), ("follow_ribbons", {"displayRibbons": True, "ribbonWidthScale": 4.0})):
        for attr, value in attrs.items():
            cmds.setAttr(groom + "." + attr, value)
        for on, prefix in ((True, ""), (False, "cpu_")):
            gpu_frames(on)
            for frame in (1, 3, 5):
                cmds.currentTime(frame)
                shot("%s%s_bend_%d" % (prefix, look, frame), groom)
        gpu_frames(True)
        for attr in attrs:
            cmds.setAttr(groom + "." + attr, defaults[attr])
    # Comparisons: each case with its CPU twin, and with an earlier run's.
    names = sorted(f[:-4] for f in os.listdir(folder) if f.endswith(".png"))
    for name in names:
        if name.startswith("cpu_") and name[4:] in names:
            say("GPU frames against the CPU's, %s: %s" % (name[4:], compare(os.path.join(folder, name[4:] + ".png"),
                                                                           os.path.join(folder, name + ".png"))))
    before = os.environ.get("GROOMLAB_PICTURES_BEFORE")
    if before:
        for name in names:
            other = os.path.join(before, name + ".png")
            if os.path.exists(other):
                say("against the earlier run, %s: %s" % (name, compare(other, os.path.join(folder, name + ".png"))))
    cmds.file(new=True, force=True)
    finish()
