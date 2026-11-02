# SPDX-License-Identifier: Apache-2.0
"""Render > Maya Hardware 2.0 (groomlab.hardware2_render): Nika's groom rendered small by Maya's own
Hardware 2.0, in this session (it draws through Viewport 2.0, so it needs the GUI's device: run on
OpenGL in maya.gui and again on DirectX 11 in maya.gui_dx11).

A suite of the GUI session (gui_runner.py; harness: gui_support.py). Checks:
  - the Studio Portrait renders: an image that is neither white nor black, the hair in the middle of
    it (the render without the groom differs from it there), the studio's Arnold lights stood in
    for by Maya lights;
  - the hair alone (head hidden, nothing behind it: the PNG's alpha is the coverage) is anti-aliased
    (many pixels of partial coverage) and drawn at the render density: more coverage at
    renderDensity 1 than at 0.25 with the viewport's previewDensity at 0.25 both times;
  - everything is put back after: the render globals, the color management's output, the groom's
    attributes, the studio's materials and lights, no node left over, the undo queue and the
    scene's modified flag as they were;
  - the Render dialog's progress window drives a render frame by frame (Stop ends it between frames).
GROOMLAB_HW2_OUT=<folder> keeps the pictures there (else a temporary folder).
"""

import os
import time

from maya import cmds

from gui_support import check, finish, process_events, say, soon

SIZE = (320, 180)


def start():
    soon(run)


def run():
    try:
        if os.environ.get("GROOMLAB_HW2_COMPARE"):
            compare(os.environ.get("GROOMLAB_HW2_OUT") or ".")
        else:
            render_check()
    finally:
        cmds.file(new=True, force=True)
    finish()


def compare(folder, width=960, height=540):
    """A picture to set beside Arnold's (GROOMLAB_HW2_COMPARE=1): Nika's groom in the Studio
    Portrait rendered by Hardware 2.0 and by Arnold (kick, the scene exported as .ass), both in
    linear sRGB shown un-tone-mapped (kick's PNG holds the linear values: encoded as sRGB here),
    side by side in <folder>/side_by_side_<api>.png."""
    import subprocess
    import groomlab
    from groomlab import studio
    from groomlab.ui.qt import QtGui
    api = cmds.groomLabGpuPreview(drawApi=True)
    if not os.path.isdir(folder):
        os.makedirs(folder)
    was = (cmds.colorManagementPrefs(query=True, renderingSpaceName=True), cmds.colorManagementPrefs(query=True, viewTransformName=True))
    cmds.colorManagementPrefs(edit=True, renderingSpaceName="scene-linear Rec.709-sRGB")
    cmds.colorManagementPrefs(edit=True, viewTransformName="Un-tone-mapped (sRGB)")
    # The groom's Arnold translator (the build's MtoA extension), found before MtoA loads.
    plugin_dir = os.path.dirname(os.environ.get("GROOMLAB_PLUGIN", ""))
    extensions = os.path.join(os.path.dirname(plugin_dir), "extensions", os.path.basename(plugin_dir))
    if os.path.isdir(extensions) and not cmds.pluginInfo("mtoa", query=True, loaded=True):
        os.environ["MTOA_EXTENSIONS_PATH"] = os.pathsep.join(p for p in [extensions, os.environ.get("MTOA_EXTENSIONS_PATH")] if p)
    try:
        cmds.loadPlugin("mtoa", quiet=True)
        cmds.file(new=True, force=True)
        groom = groomlab.create_groom(groomlab.add_sample("nika"), fit=True)
        studio.studio(groom, preset="portrait")
        started = time.time()
        job = groomlab.render_hardware2(width=width, height=height, quality="final", show=False,
                                        folder=os.path.join(folder, "compare_hw2_%s" % api))
        seconds = time.time() - started
        cmds.setAttr("defaultArnoldRenderOptions.AASamples", 4)
        cmds.setAttr("defaultArnoldDriver.ai_translator", "png", type="string")
        scene, arnold = os.path.join(folder, "compare_arnold.ass"), os.path.join(folder, "compare_arnold.png")
        for path in (scene, arnold):
            if os.path.exists(path):
                os.remove(path)
        cmds.arnoldExportAss(filename=scene, asciiAss=True, cam=studio.CAMERA)
        mtoa_dir = os.path.dirname(os.path.dirname(cmds.pluginInfo("mtoa", query=True, path=True)))
        started = time.time()
        subprocess.run([os.path.join(mtoa_dir, "bin", "kick"), "-i", scene, "-l", os.path.join(mtoa_dir, "shaders"), "-o", arnold,
                        "-r", str(width), str(height), "-dw", "-dp", "-v", "0"], stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
        kick_seconds = time.time() - started
        with open(scene) as f:
            if "curves" not in f.read():
                say("compare: the .ass has no curves: MtoA loaded before GroomLab's translator was found "
                    "(MTOA_EXTENSIONS_PATH); Arnold's side shows no groom")
    finally:
        cmds.colorManagementPrefs(edit=True, renderingSpaceName=was[0])
        cmds.colorManagementPrefs(edit=True, viewTransformName=was[1])
    lut = [int(round(255 * (v / 255.0 * 12.92 if v / 255.0 <= 0.0031308 else 1.055 * (v / 255.0) ** (1 / 2.4) - 0.055)))
           for v in range(256)]
    right = QtGui.QImage(arnold).convertToFormat(QtGui.QImage.Format_ARGB32)
    for y in range(right.height()):
        for x in range(right.width()):
            p = right.pixel(x, y)
            right.setPixel(x, y, (p & 0xff000000) | (lut[p >> 16 & 255] << 16) | (lut[p >> 8 & 255] << 8) | lut[p & 255])
    out = QtGui.QImage(2 * width + 8, height + 28, QtGui.QImage.Format_RGB32)
    out.fill(QtGui.QColor(30, 30, 30))
    painter = QtGui.QPainter(out)
    painter.drawImage(0, 28, QtGui.QImage(job.images()[0]))
    painter.drawImage(width + 8, 28, right)
    painter.setPen(QtGui.QColor(230, 230, 230))
    painter.drawText(8, 19, "Maya Hardware 2.0 (%s, 16 samples): %.1f s" % (api, seconds))
    painter.drawText(width + 16, 19, "Arnold (kick, AA 4): %.1f s" % kick_seconds)
    painter.end()
    side = os.path.join(folder, "side_by_side_%s.png" % api)
    out.save(side)
    say("compare: %s (Hardware 2.0 %.1f s, kick %.1f s)" % (side, seconds, kick_seconds))


def luma(p):
    return 0.2126 * ((p >> 16) & 255) + 0.7152 * ((p >> 8) & 255) + 0.0722 * (p & 255)


def image(path):
    from groomlab.ui.qt import QtGui
    return QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_ARGB32)


def state(groom):
    """What a render must leave as it found it."""
    from groomlab import hardware2_render, studio
    plugs = ["hardwareRenderingGlobals." + a for a in ("multiSampleEnable", "multiSampleCount", "lightingMode", "renderMode")]
    plugs += ["defaultRenderGlobals." + a for a in ("imageFormat", "imageFilePrefix", "animation", "outFormatControl")]
    plugs += [groom + "." + a for a, _ in hardware2_render.GROOM_DISPLAY] + [groom + ".previewDensity", groom + ".visibility"]
    plugs += [s + ".visibility" for s in cmds.ls(type="aiAreaLight", long=True) or []]
    # (An empty string attribute never set reads None, and "" once set to it: the same to Maya.)
    values = {p: "" if cmds.getAttr(p) is None else cmds.getAttr(p) for p in plugs if cmds.objExists(p)}
    values["color"] = [cmds.colorManagementPrefs(query=True, **{f: True}) for f in ("outputTransformEnabled", "outputUseViewTransform")]
    values["nodes"] = sorted(cmds.ls(long=True))
    values["materials"] = [cmds.listConnections(m + "SG.surfaceShader", source=True, destination=False)
                           for m in (studio.BACKDROP_MATERIAL, studio.MANNEQUIN_MATERIAL) if cmds.objExists(m + "SG")]
    values["time"] = cmds.currentTime(query=True)
    values["undo"] = cmds.undoInfo(query=True, undoName=True)
    values["modified"] = cmds.file(query=True, modified=True)
    values["strands"] = cmds.getAttr(groom + ".outStrandCount")
    values["renderView"] = cmds.getAttr(groom + ".renderView")
    return values


def render_check():
    import tempfile
    import groomlab
    from groomlab import studio
    from groomlab.ui import menu
    folder = os.environ.get("GROOMLAB_HW2_OUT") or tempfile.mkdtemp(prefix="groomlab_hw2_")
    api = cmds.groomLabGpuPreview(drawApi=True)
    cmds.file(new=True, force=True)
    groom = groomlab.create_groom(groomlab.add_sample("nika"), fit=True)
    cmds.setAttr(groom + ".previewDensity", 0.25)  # the viewport thinned: the render draws them all
    preview_strands = cmds.getAttr(groom + ".outStrandCount")
    check("Render > Maya Hardware 2.0 is in the menus (render.hardware2) with its dialog's settings",
          "render.hardware2" in menu.ACTIONS and menu._standalone_renderers()["hw2"]["exe"] is None)

    # -- The hair alone: coverage and density (no studio, the head hidden, alpha = coverage) --
    heads = [cmds.listRelatives(m, parent=True, fullPath=True)[0] for m in studio.head_meshes(groom)]
    cmds.hide(heads)
    camera = cmds.camera(name="hw2CheckCam")[0]
    cmds.setAttr(camera + ".rotate", -5.0, 25.0, 0.0)
    cmds.viewFit(camera, [groom], fitFactor=0.95)
    light = cmds.directionalLight(intensity=1.0)
    cmds.setAttr(cmds.listRelatives(light, parent=True)[0] + ".rotate", -35.0, 30.0, 0.0)

    def coverage(name, render_density, quality="final"):
        cmds.setAttr(groom + ".renderDensity", render_density)
        job = groomlab.render_hardware2(camera=camera, width=SIZE[0], height=SIZE[1], quality=quality, show=False,
                                        folder=os.path.join(folder, "%s_%s" % (name, api)))
        if not job.ok():
            return job, 0.0, 0, 0
        picture = image(job.images()[0])
        total, partial, covered = 0.0, 0, 0
        for y in range(picture.height()):
            for x in range(picture.width()):
                a = picture.pixel(x, y) >> 24 & 255
                if a:
                    covered += 1
                    total += a / 255.0
                    if a < 250:
                        partial += 1
        return job, total, covered, partial

    before = state(groom)
    full, full_cover, full_pixels, full_partial = coverage("hair_render_density_1", 1.0)
    after = state(groom)
    say("hair alone at render density 1 on %s: %s, coverage %.0f over %d pixels (%d partial), %.2f s (%s)"
        % (api, full.ok(), full_cover, full_pixels, full_partial, sum(full.seconds), full.errors()))
    thin, thin_cover, thin_pixels, _ = coverage("hair_render_density_025", 0.25)
    cmds.setAttr(groom + ".renderDensity", 1.0)
    check("Hardware 2.0 on %s renders the hair anti-aliased: %d of its %d pixels partly covered (alpha)"
          % (api, full_partial, full_pixels), full.ok() and full_pixels > 500 and full_partial > 0.3 * full_pixels)
    # The strands drawn (the evaluation the draw override draws), and the pixels they touch: the
    # coverage summed is about the same at any density (displayCoverageDensity: a thinned groom's
    # strands each cover what the strands they stand for would), the strands far more.
    drawn, thin_drawn = sum(full.strands.values()), sum(thin.strands.values())
    check("Hardware 2.0 on %s draws the groom at its render density, not the viewport's 0.25: %d strands at "
          "renderDensity 1 (%d pixels touched, coverage %.0f), %d at 0.25 (%d pixels, %.0f), %d in the viewport"
          % (api, drawn, full_pixels, full_cover, thin_drawn, thin_pixels, thin_cover, preview_strands),
          thin.ok() and drawn > 3.0 * preview_strands and thin_drawn < 1.5 * preview_strands and full_pixels > thin_pixels)
    changed = sorted(k for k in before if k not in ("undo",) and before[k] != after[k])
    check("Hardware 2.0 on %s puts everything back (%s differ; %d strands before, %d after, %d in the viewport at "
          "preview density)" % (api, changed or "nothing", before["strands"], after["strands"], preview_strands),
          not changed and before["undo"] == after["undo"] and not cmds.ls("groomlab_hw2*"))
    cmds.delete(camera, cmds.listRelatives(light, parent=True))
    cmds.showHidden(heads)

    # -- The Studio Portrait --
    if not cmds.pluginInfo("mtoa", query=True, loaded=True):
        try:
            cmds.loadPlugin("mtoa")
        except RuntimeError:
            say("no Arnold (mtoa): the studio's render is not checked")
            return
    studio.studio(groom, preset="portrait")
    cmds.file(modified=False)
    width, height = (960, 540) if os.environ.get("GROOMLAB_HW2_OUT") else SIZE
    before = state(groom)
    started = time.time()
    job = groomlab.render_hardware2(width=width, height=height, quality="final", show=False,
                                    folder=os.path.join(folder, "studio_portrait_%s" % api))
    seconds = time.time() - started
    after = state(groom)
    say("studio portrait on %s: %s in %.2f s (%s); %s" % (api, job.images(), seconds, job.errors(), open(job.log).read()))
    cmds.setAttr(groom + ".visibility", False)
    cmds.file(modified=False)
    bald = groomlab.render_hardware2(width=width, height=height, quality="final", show=False,
                                     folder=os.path.join(folder, "studio_portrait_bald_%s" % api))
    cmds.setAttr(groom + ".visibility", True)
    ok = job.ok() and bald.ok()
    mean, hair, cx, cy = 0.0, 0, 0.0, 0.0
    if ok:
        a, b = image(job.images()[0]), image(bald.images()[0])
        w, h = a.width(), a.height()
        n = 0
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                pa, pb = a.pixel(x, y), b.pixel(x, y)
                mean += luma(pa)
                n += 1
                if abs(luma(pa) - luma(pb)) > 6:
                    hair += 1
                    cx += x / float(w)
                    cy += y / float(h)
        mean /= max(n, 1)
        cx, cy = cx / max(hair, 1), cy / max(hair, 1)
    check("Hardware 2.0 on %s renders the studio portrait: not white or black (mean luma %.1f), the hair in the "
          "middle (%d pixels differ without it, centered at %.2f, %.2f), lit by %d stand-ins for the studio's lights"
          % (api, mean, hair, cx, cy, len(job.lights)),
          ok and 25.0 < mean < 220.0 and hair > 200 and 0.3 < cx < 0.7 and 0.2 < cy < 0.8 and len(job.lights) == 5)
    changed = sorted(k for k in before if k not in ("undo",) and before[k] != after[k])
    check("Hardware 2.0 on %s puts the studio back (%s differ; the scene %s; the undo queue's last %r)"
          % (api, changed or "nothing", "modified" if after["modified"] else "unmodified", after["undo"]),
          not changed and before["undo"] == after["undo"] and not after["modified"])

    # -- The dialog's progress window: frame by frame, then stopped between frames --
    spec = menu._standalone_renderers()["hw2"]
    cmds.playbackOptions(minTime=1, maxTime=3)
    job = spec["render"](start=1, end=3, width=SIZE[0], height=SIZE[1], quality="draft",
                         folder=os.path.join(folder, "progress_%s" % api))
    menu._watch_render(job, spec)
    until = time.time() + 120
    while not job.finished() and time.time() < until:
        process_events(50)
    process_events(100)
    check("Hardware 2.0's progress window on %s renders the frames one by one (%d of 3 images, %s)"
          % (api, len(job.images()), [os.path.basename(p) for p in job.images()]),
          job.ok() and len(job.images()) == 3 and not menu._RENDER_JOBS)
    before = state(groom)
    job = spec["render"](start=1, end=3, width=SIZE[0], height=SIZE[1], quality="draft",
                         folder=os.path.join(folder, "stopped_%s" % api))
    job.poll()
    job.cancel()
    after = state(groom)
    changed = sorted(k for k in before if k not in ("undo",) and before[k] != after[k])
    check("Hardware 2.0 on %s stops between frames (%d of 3 images, cancelled %s) and puts everything back (%s differ)"
          % (api, len(job.images()), job.cancelled, changed or "nothing"),
          job.cancelled and len(job.images()) == 1 and job.finished() and not changed)
    say("Hardware 2.0 pictures: %s" % folder)
