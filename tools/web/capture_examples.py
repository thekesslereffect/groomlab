# SPDX-License-Identifier: Apache-2.0
"""The website's example images (web/public/examples) from a groom scene, in one Maya GUI session.

Run by maya.exe -script on a mel that execs this file, with PROBE_SCENE (the scene, a copy: it is edited
in memory, never saved), PROBE_OUT (where the pictures go), SHOTS (steps: guides, wind, ui, cards,
closeup, beauty), GROOMLAB_PLUGIN and GROOMLAB_SCRIPTS set; MAYA_VP2_DEVICE_OVERRIDE=VirtualDeviceDx11
for DirectX 11. Made for the wolf groom (its node and mesh names); another scene needs its names here.
"""

import os
import sys
import traceback

import maya.mel
import maya.utils
from maya import cmds

OUT = os.environ["PROBE_OUT"]
LOG = os.path.join(OUT, "shots.log")
GROOM = "groomLab1Shape"


def say(msg):
    with open(LOG, "a") as f:
        f.write(str(msg) + "\n")


def _load():
    scripts = os.environ["GROOMLAB_SCRIPTS"]
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    try:
        cmds.loadPlugin(os.environ["GROOMLAB_PLUGIN"])
        for proc in ("groomLabBrushProperties", "groomLabBrushValues", "AEgroomLabShapeTemplate"):
            maya.mel.eval('source "%s/%s.mel"' % (scripts.replace("\\", "/"), proc))
        import groomlab  # noqa: F401
    except Exception:
        say(traceback.format_exc())
        cmds.quit(force=True, exitCode=1)
        return
    cmds.evalDeferred(_run, lowestPriority=True)


def _run():
    try:
        run()
    except Exception:
        say(traceback.format_exc())
    if not os.environ.get("SHOTS_KEEP"):
        cmds.quit(force=True, exitCode=0)


def qt():
    from groomlab.ui.qt import QtWidgets, QtGui, QtCore
    return QtWidgets, QtGui, QtCore


def pump(n=6):
    QtWidgets, _, _ = qt()
    for _ in range(n):
        QtWidgets.QApplication.processEvents()
        cmds.refresh(force=True)


def graph_edit(fn):
    from groomlab.params import get_graph, set_graph
    g = get_graph(GROOM)
    fn(g)
    set_graph(GROOM, g)


def node(g, name):
    return next(n for n in g["nodes"] if n["name"] == name)


def fix_graph(g):
    # The scene predates centersFrom and Length's trim range: what it meant then.
    for name in ("mediumClump", "bigClump"):
        p = node(g, name).setdefault("params", {})
        p.pop("shapeFrom", None)
        p["centersFrom"] = "inputCurves"
    p = node(g, "length").setdefault("params", {})
    r = p.pop("randomness", 0.0)
    p.update({"fraction": 1.0, "trimMin": 0.0, "trimMax": r})


def setup_view(panel):
    cmds.displayPref(displayGradient=True)
    cmds.displayRGBColor("backgroundTop", 0.155, 0.165, 0.19)
    cmds.displayRGBColor("backgroundBottom", 0.085, 0.09, 0.105)
    cmds.displayRGBColor("background", 0.12, 0.125, 0.145)
    cmds.modelEditor(panel, edit=True, grid=False, headsUpDisplay=False, displayAppearance="smoothShaded",
                     displayTextures=True, displayLights="all", shadows=False, manipulators=False,
                     selectionHiliteDisplay=False, locators=False, cameras=False, lights=False)
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", True)
    cmds.setAttr("hardwareRenderingGlobals.multiSampleCount", 16)
    cmds.setAttr("hardwareRenderingGlobals.ssaoEnable", True)
    for l in cmds.ls(type="light"):
        cmds.delete(cmds.listRelatives(l, parent=True))
    def light(name, intensity, rot, color=(1, 1, 1)):
        shape = cmds.directionalLight(name=name, intensity=intensity, rgb=color)
        xf = cmds.listRelatives(shape, parent=True)[0]
        cmds.setAttr(xf + ".rotate", *rot)
    light("key", 1.25, (-35, 40, 0), (1.0, 0.97, 0.92))
    light("rim", 0.9, (-20, 200, 0), (0.85, 0.9, 1.0))
    light("fill", 0.35, (-10, -70, 0), (0.8, 0.85, 1.0))
    cmds.setAttr(GROOM + ".displayHairShading", 1)
    cmds.setAttr(GROOM + ".displayGuides", False)
    if cmds.objExists("pPlane1"):
        cmds.setAttr("pPlane1.visibility", False)
    cmds.select(clear=True)


def frame_on(points, fit, rot, aspect=1.6, offset=(0.0, 0.0)):
    """The persp camera turned to `rot`, its bounding sphere of `points` filling `fit` of the
    narrower field of view at a picture of `aspect`; `offset` shifts the subject (shares of the
    picture, + right / up)."""
    import math
    from maya.api import OpenMaya as om
    cmds.setAttr("persp.rotate", *rot)
    cmds.setAttr("perspShape.filmFit", 1)  # horizontal
    lo = [min(p[k] for p in points) for k in range(3)]
    hi = [max(p[k] for p in points) for k in range(3)]
    c = om.MVector(*[(lo[k] + hi[k]) / 2 for k in range(3)])
    r = 0.5 * (om.MVector(*hi) - om.MVector(*lo)).length()
    m = om.MMatrix(cmds.xform("persp", query=True, worldSpace=True, matrix=True))
    right, up, back = om.MVector(m[0], m[1], m[2]), om.MVector(m[4], m[5], m[6]), om.MVector(m[8], m[9], m[10])
    focal = cmds.getAttr("perspShape.focalLength")
    hap = cmds.getAttr("perspShape.horizontalFilmAperture") * 25.4
    hfov = 2.0 * math.atan(hap / (2.0 * focal))
    vfov = 2.0 * math.atan(math.tan(hfov / 2.0) / aspect)
    dist = r / math.sin(min(hfov, vfov) / 2.0) * fit
    half_w = dist * math.tan(hfov / 2.0)
    half_h = dist * math.tan(vfov / 2.0)
    eye = c + back * dist - right * (offset[0] * 2.0 * half_w) - up * (offset[1] * 2.0 * half_h)
    cmds.setAttr("persp.translate", eye.x, eye.y, eye.z)
    cmds.setAttr("perspShape.centerOfInterest", dist)
    cmds.camera("persp", edit=True, nearClipPlane=max(0.01, dist * 0.01), farClipPlane=dist * 50)


def materials():
    """Simple shading for the wolf's own meshes (a copy of the scene): dark skin, a black nose and
    glossy eyes, so the fur reads."""
    def shader(name, kind, color, **attrs):
        node = cmds.shadingNode(kind, asShader=True, name=name)
        cmds.setAttr(node + ".color", *color, type="double3")
        for k, v in attrs.items():
            cmds.setAttr(node + "." + k, v)
        sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=name + "SG")
        cmds.connectAttr(node + ".outColor", sg + ".surfaceShader")
        return sg
    skin = shader("wSkin", "lambert", (0.16, 0.13, 0.115))
    eye = shader("wEye", "blinn", (0.02, 0.018, 0.016), specularRollOff=0.6, eccentricity=0.15)
    mouth = shader("wMouth", "lambert", (0.25, 0.08, 0.08))
    nails = shader("wNails", "lambert", (0.05, 0.045, 0.04))
    for mesh, sg in (("wolf_body", skin), ("wolf_eye_l", eye), ("wolf_eye_r", eye), ("wolf_mouthbag", mouth),
                     ("wolf_nails", nails)):
        if cmds.objExists(mesh):
            cmds.sets(mesh, edit=True, forceElement=sg)


def body_points(top_share=1.0):
    pts = cmds.xform("wolf_body.vtx[*]", query=True, worldSpace=True, translation=True)
    pts = [(pts[i], pts[i + 1], pts[i + 2]) for i in range(0, len(pts), 3)]
    ys = [p[1] for p in pts]
    low = max(ys) - top_share * (max(ys) - min(ys))
    return [p for p in pts if p[1] >= low]


def shot(name, size):
    pump(3)
    path = os.path.join(OUT, name + ".png")
    cmds.playblast(frame=[cmds.currentTime(query=True)], format="image", compression="png", viewer=False,
                   showOrnaments=False, percent=100, widthHeight=size, completeFilename=path)
    say("shot " + path)
    return path


def compose(path, size, parts, labels=(), background=(30, 33, 40)):
    """A picture of `size` from images placed at (x, y, w, h) (scaled to fit, centered), with text labels
    [(text, x, y, px, rgb)]."""
    QtWidgets, QtGui, QtCore = qt()
    out = QtGui.QImage(size[0], size[1], QtGui.QImage.Format_RGB32)
    out.fill(QtGui.QColor(*background))
    painter = QtGui.QPainter(out)
    painter.setRenderHint(QtGui.QPainter.SmoothPixmapTransform, True)
    painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
    for image, (x, y, w, h) in parts:
        img = image if isinstance(image, QtGui.QImage) else QtGui.QImage(image)
        scaled = img.scaled(w, h, QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation)
        painter.drawImage(x + (w - scaled.width()) // 2, y + (h - scaled.height()) // 2, scaled)
    for text, x, y, px, rgb in labels:
        font = QtGui.QFont("Segoe UI", 1)
        font.setPixelSize(px)
        painter.setFont(font)
        painter.setPen(QtGui.QColor(*rgb))
        painter.drawText(QtCore.QRect(x - 400, y, 800, px * 2), QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop, text)
    painter.end()
    out.save(path)
    say("composed " + path)


def grab(widget, path=None):
    pump(8)
    image = widget.grab().toImage()
    if path:
        image.save(path)
    return image


def add_wind(g):
    g["nodes"].append({"name": "wind", "type": "Wind", "params": {
        "direction": [0.0, 0.35, -1.0], "strength": 0.75, "gust": 1.0, "frequency": 0.012, "speed": 90.0,
        "turbulence": 0.35, "loop": True, "loopStart": 1, "loopEnd": 48}})
    conns = g["connections"]
    for c in conns:
        if c["from"] == "width" and c["to"] == "resample":
            c["from"] = "wind"
    conns.append({"from": "width", "to": "wind", "input": "strands"})



def remove_wind(g):
    g["nodes"] = [n for n in g["nodes"] if n["name"] != "wind"]
    g["connections"] = [c for c in g["connections"] if c["to"] != "wind"]
    for c in g["connections"]:
        if c["from"] == "wind":
            c["from"] = "width"

def run():
    cmds.file(os.environ["PROBE_SCENE"], open=True, force=True, prompt=False)
    graph_edit(fix_graph)
    say("errors %s" % cmds.groomLabGraphStats(GROOM)[:200] if False else "strands %d" % cmds.getAttr(GROOM + ".outStrandCount"))
    panel = [p for p in cmds.getPanel(type="modelPanel") if cmds.modelPanel(p, query=True, camera=True) == "persp"][0]
    cmds.setFocus(panel)
    setup_view(panel)
    materials()
    shot("_warmup", (320, 200))
    cmds.setAttr("perspShape.focalLength", 50)
    steps = os.environ.get("SHOTS", "beauty").split(",")
    whole = body_points(1.0)
    head = body_points(0.22)
    if "guides" in steps:
        cmds.setAttr(GROOM + ".displayGuides", True)
        frame_on(head, 0.85, (-12, 25, 0))
        shot("viewport-guides", (1600, 1000))
        cmds.setAttr(GROOM + ".displayGuides", False)
    if "wind" in steps:
        graph_edit(add_wind)
        frame_on(whole, 0.92, (-4, 85, 0), aspect=0.95, offset=(-0.05, 0.03))
        frames = (1, 13, 25, 37)
        paths = []
        for f in frames:
            cmds.currentTime(f)
            paths.append(shot("_wind_%d" % f, (760, 800)))
        parts = [(paths[i], (i * 600, 10, 600, 640)) for i in range(4)]
        labels = [("frame %d" % f, i * 600 + 300, 668, 26, (215, 220, 230)) for i, f in enumerate(frames)]
        compose(os.path.join(OUT, "motion-strip.png"), (2400, 750), parts, labels)
        cmds.currentTime(1)
        graph_edit(remove_wind)
    if "ui" in steps:
        import groomlab
        from groomlab.ui import graph_editor, palette
        QtWidgets, QtGui, QtCore = qt()
        groomlab.pin_groom(GROOM)
        # The Graph Editor as its own window, the clump node's panel open.
        editor = graph_editor.GraphEditor(groom=GROOM)
        editor.setWindowFlags(QtCore.Qt.Window)
        editor.resize(1600, 1000)
        editor.show()
        pump(10)
        for sp in editor.findChildren(QtWidgets.QSplitter):
            if sp.count() == 2:
                sp.setSizes([1040, 560])
        pump(4)
        for item in editor.scene.selectedItems():
            item.setSelected(False)
        names = ["scatter", "interpolate", "mediumClump", "bigClump", "noise", "frizzNoise", "flyaway"]
        rect = None
        for n in names:
            if n in editor.items:
                r = editor.items[n].sceneBoundingRect()
                rect = r if rect is None else rect.united(r)
        if rect is not None:
            editor.view.frame_rect(rect.adjusted(-60, -160, 60, 220))
        if "mediumClump" in editor.items:
            editor.items["mediumClump"].setSelected(True)
        pump(12)
        grab(editor, os.path.join(OUT, "graph-editor.png"))
        editor.close()
        # Groom Tools, as its own window: the Tools tab.
        cmds.select(GROOM)
        pump(4)
        tools = palette.ToolPalette()
        tools.setWindowFlags(QtCore.Qt.Window)
        tools.resize(570, 1500)
        tools.show()
        tools.show_tab("tools")
        pump(8)
        for b in tools.findChildren(QtWidgets.QAbstractButton):
            if b.text().strip() == "Comb" and b.isVisible():
                b.click()
                break
        pump(12)
        grab(tools, os.path.join(OUT, "groom-tools.png"))
        # Paint: the density map picked, the viewport beside the panel.
        tools.show_tab("paint")
        cmds.select(GROOM)
        pump(6)
        tools.paint_tab.refresh(force=True)
        pump(4)
        lst = tools.paint_tab.list
        for i in range(lst.count()):
            if lst.item(i).text().startswith("density_bigclumps"):
                lst.setCurrentItem(lst.item(i))
        say("map picked %s" % (lst.currentItem().text() if lst.currentItem() else None))
        pump(4)
        tools.paint_tab.view_strands.setChecked(False)
        tools.paint_tab.view_scalp.setChecked(True)
        cmds.setAttr(GROOM + ".previewDensity", 0.12)
        pump(8)
        say("display %s" % [(a, cmds.getAttr(GROOM + "." + a)) for a in cmds.listAttr(GROOM) or [] if "map" in a.lower() and "display" in a.lower()][:6])
        say("strands view %s enabled %s" % (tools.paint_tab.view_strands.isChecked(), tools.paint_tab.view_strands.isEnabled()))
        tools.resize(500, 1000)
        pump(14)
        panel_img = grab(tools)
        frame_on(whole, 0.8, (-6, 30, 0), aspect=1.1)
        vp = shot("_paint_viewport", (1100, 1000))
        compose(os.path.join(OUT, "paint-maps.png"), (1600, 1000), [(vp, (0, 0, 1100, 1000)), (panel_img, (1100, 0, 500, 1000))])
        cmds.setAttr(GROOM + ".previewDensity", 1.0)
        tools.paint_tab.view_scalp.setChecked(False)
        tools.close()
    if "cards" in steps:
        from groomlab import cards as gc
        upper = body_points(0.5)
        frame_on(upper, 0.8, (-6, 40, 0), aspect=0.66)
        strands_img = shot("_cards_strands", (660, 1000))
        gc.create_cards(GROOM, lod="hero")
        pump(6)
        bake = os.path.join(OUT, "cards_bake").replace("\\", "/")
        say("bake %s" % gc.bake_cards(GROOM, directory=bake, name="wolf"))
        stats = gc.card_stats(GROOM)
        say("card stats %s" % stats)
        cmds.setAttr(GROOM + ".displayCards", True)
        gc.card_display(GROOM, textures=True)
        pump(6)
        cards_img = shot("_cards_baked", (660, 1000))
        gc.card_display(GROOM, textures=False)
        pump(6)
        geo_img = shot("_cards_geometry", (660, 1000))
        gc.card_display(GROOM, textures=True)
        cmds.setAttr(GROOM + ".displayCards", False)
        count = stats.get("cards", 0)
        tris = stats.get("triangles", stats.get("cardTriangles", 0))
        parts = [(strands_img, (0, 30, 533, 830)), (cards_img, (533, 30, 534, 830)), (geo_img, (1067, 30, 533, 830))]
        labels = [("Strands", 266, 880, 26, (235, 238, 245)), ("the groom", 266, 916, 19, (150, 156, 170)),
                  ("Cards", 800, 880, 26, (235, 238, 245)), ("baked maps, alpha-tested", 800, 916, 19, (150, 156, 170)),
                  ("Card geometry", 1333, 880, 26, (235, 238, 245)),
                  ("%s cards, %sk triangles" % (count, int(round(tris / 1000.0))) if tris else "%s cards" % count, 1333, 916, 19, (150, 156, 170))]
        compose(os.path.join(OUT, "game-cards.png"), (1600, 1000), parts, labels)
    if "closeup" in steps:
        frame_on(head, 0.8, (-4, 62, 0), offset=(-0.05, 0.0))
        shot("hairstyle", (1600, 1000))

    if "beauty" in steps:
        shot("_warmup_full", (1600, 1000))
        frame_on(whole, 0.78, (-6, 30, 0))
        shot("fur-creature", (1600, 1000))
        frame_on(whole, 0.85, (-4, 50, 0), aspect=2.0, offset=(0.0, 0.0))
        shot("hero", (2400, 1200))

maya.utils.executeDeferred(_load)
