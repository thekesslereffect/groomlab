# SPDX-License-Identifier: Apache-2.0
"""Synthetic XGen test scenes, made with XGen's own API in mayapy (nothing copied from elsewhere).

    igs_scene(folder, textures)    an Interactive Groom description on a round head: Scale, Clump,
                                   Noise, Cut, a painted density map (a file texture under
                                   ${GROOMLAB_XGEN_TEST_TEXTURES}) and a mask whose file sits under
                                   ${GROOMLAB_XGEN_MISSING_ROOT}, a variable nothing sets
    core_scene(folder)             an XGen Core description: guides, density with a Ptex mask
                                   (${DESC}/paintmaps/dens/), clumping with its maps, noise, coil,
                                   cut and a length expression
    strand_stats(strands)          numbers to compare XGen's strands with GroomLab's

Each scene is saved as an .ma (a Core one with its .xgen collection and data folder beside it).
"""

import math
import os
import random
import shutil

from maya import cmds, mel

DENSITY_TEXTURE = "density.png"
MISSING_VARIABLE = "GROOMLAB_XGEN_MISSING_ROOT"
TEXTURES_VARIABLE = "GROOMLAB_XGEN_TEST_TEXTURES"


def head(name="head"):
    """A round head of quads (a smoothed cube, 10 units across) with UVs."""
    cube = cmds.polyCube(width=10, height=10, depth=10, subdivisionsX=4, subdivisionsY=4, subdivisionsZ=4, name=name)[0]
    cmds.polySmooth(cube, divisions=1)
    cmds.delete(cube, constructionHistory=True)
    return cube


def write_gradient_png(path, size=64):
    """A gray PNG, white at the top of the image (V = 1) to black at the bottom, written by Qt."""
    from groomlab.ui.qt import QtGui
    image = QtGui.QImage(size, size, QtGui.QImage.Format_RGB32)
    for y in range(size):
        v = int(round(255 * (1.0 - y / float(size - 1))))
        for x in range(size):
            image.setPixel(x, y, QtGui.qRgb(v, v, v))
    if not image.save(path, "PNG"):
        raise RuntimeError("could not write %s" % path)
    return path


def _file_texture(path, name):
    node = cmds.shadingNode("file", asTexture=True, name=name)
    place = cmds.shadingNode("place2dTexture", asUtility=True)
    cmds.connectAttr(place + ".outUV", node + ".uvCoord")
    cmds.connectAttr(place + ".outUvFilterSize", node + ".uvFilterSize")
    cmds.setAttr(node + ".fileTextureName", path, type="string")
    return node


def _insert_modifier(shape, node_type, name):
    """A modifier added at the end of an IGS description's chain, as XGen's editor adds one."""
    node = cmds.createNode(node_type, name=name)
    in_plug = node + ".inSplineData" + ("[0]" if cmds.attributeQuery("inSplineData", node=node, multi=True) else "")
    out_plug = node + ".outSplineData" + ("[0]" if cmds.attributeQuery("outSplineData", node=node, multi=True) else "")
    upstream = cmds.listConnections(shape + ".inSplineData", source=True, destination=False, plugs=True)[0]
    cmds.disconnectAttr(upstream, shape + ".inSplineData")
    cmds.connectAttr(upstream, in_plug)
    cmds.connectAttr(out_plug, shape + ".inSplineData")
    if node_type == "xgmModifierClump":
        base = [n for n in cmds.ls(type="xgmSplineBase")][0]
        cmds.connectAttr(base + ".outMeshData", node + ".inMeshData")
    return node


def igs_scene(folder, textures):
    """The IGS scene; returns {"scene", "description" (its transform), "shape"}."""
    cmds.loadPlugin("xgenToolkit", quiet=True)
    cmds.file(new=True, force=True)
    os.makedirs(folder, exist_ok=True)
    cmds.workspace(folder, openWorkspace=True)
    scalp = head()
    cmds.select(scalp)
    shape = cmds.xgmCreateSplineDescription(createDefaultHair=True, name="igsHair", density=12.0, length=3.0,
                                            widthScale=0.03, cvCount=16, generatorSeed=3)
    shape = cmds.ls(shape, long=True)[0]
    cmds.setAttr(shape + ".widthTaper", 0.6)
    clump = _insert_modifier(shape, "xgmModifierClump", "clumpA")
    cmds.setAttr(clump + ".clump", 0.8)
    cmds.setAttr(clump + ".pointDensity", 0.15)
    noise = _insert_modifier(shape, "xgmModifierNoise", "noiseA")
    cmds.setAttr(noise + ".magnitude", 0.6)
    cmds.setAttr(noise + ".frequency", 2.0)
    cut = _insert_modifier(shape, "xgmModifierCut", "cutA")
    cmds.setAttr(cut + ".cutMode", 0)
    cmds.setAttr(cut + ".amount", 0.5)
    # A painted density map, from a folder an environment variable names (as studios set them).
    base = cmds.ls(type="xgmSplineBase")[0]
    density = _file_texture("${%s}/%s" % (TEXTURES_VARIABLE, DENSITY_TEXTURE), "densityPaint")
    cmds.connectAttr(density + ".outAlpha", base + ".densityMask", force=True)
    write_gradient_png(os.path.join(textures, DENSITY_TEXTURE))
    # A mask whose file sits under a variable nothing sets: the import reports it missing.
    mask = _file_texture("${%s}/noise_mask.png" % MISSING_VARIABLE, "noiseMaskPaint")
    cmds.connectAttr(mask + ".outAlpha", noise + ".mask", force=True)
    scene = os.path.join(folder, "igs_scene.ma").replace("\\", "/")
    cmds.file(rename=scene)
    cmds.file(save=True, type="mayaAscii")
    return {"scene": scene, "description": cmds.listRelatives(shape, parent=True)[0], "shape": shape}


class _DescriptionEditor(object):
    """XGen's clump map tools ask the Description Editor (a window) for the current description:
    this stands in for it in mayapy."""

    def __init__(self, palette, description):
        self.palette, self.description = palette, description

    def currentDescription(self):
        return self.description

    def currentPalette(self):
        return self.palette

    def __getattr__(self, name):
        return lambda *args, **kwargs: None


def core_scene(folder):
    """The Core scene; returns {"scene", "palette", "description", "ptex"}."""
    cmds.loadPlugin("xgenToolkit", quiet=True)
    import xgenm as xg
    import xgenm.xgGlobal as xgg
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    cmds.file(new=True, force=True)
    cmds.workspace(folder, openWorkspace=True)
    xg.setProjectPath(folder.replace("\\", "/") + "/")
    scene = os.path.join(folder, "core_scene.ma").replace("\\", "/")
    cmds.file(rename=scene)
    cmds.file(save=True, type="mayaAscii")
    scalp = head()
    palette, desc = "hairCollection", "coreHair"
    cmds.select(scalp)
    xg.createDescription(palette, desc, "Spline", "Random", "Renderman", "Guides")
    cmds.select(scalp + ".f[*]")
    xg.modifyFaceBinding(palette, desc, "Append")
    # Guides on the upper half, bent back and growing longer toward the back.
    rng = random.Random(7)
    positions = cmds.xform(scalp + ".vtx[*]", query=True, worldSpace=True, translation=True)
    curves = []
    for i in range(0, len(positions), 3):
        root = positions[i:i + 3]
        if root[1] < 1.5:
            continue
        r = math.sqrt(sum(c * c for c in root))
        n = [c / r for c in root]
        length = 2.5 + 0.12 * (5.0 - root[2]) + rng.uniform(-0.1, 0.1)
        pts = []
        for k in range(6):
            t = k / 5.0
            pts.append((root[0] + n[0] * length * t,
                        root[1] + n[1] * length * t - 0.4 * t * t * length,
                        root[2] + n[2] * length * t - 0.5 * t * t * length))
        curves.append(cmds.curve(point=pts, degree=3))
    cmds.select(curves)
    cmds.xgmCurveToGuide(description=desc, tipSnapPower=1.0, tipSnapAmount=0.0, deleteCurve=True)
    xg.setAttr("density", "12.0", palette, desc, "RandomGenerator")
    xg.setAttr("fxCVCount", "24", palette, desc, "SplinePrimitive")  # enough CVs for the coil and noise
    xg.setAttr("length", "$a=1.1;#0.05,5.0\\n$a", palette, desc, "SplinePrimitive")
    xg.setAttr("width", "$a=0.02;#0.005,0.5\\n$a", palette, desc, "SplinePrimitive")
    xg.setAttr("taper", "$a=0.6;#-1.0,1.0\\n$a", palette, desc, "SplinePrimitive")
    # A Ptex density mask XGen bakes itself from vertex colors: dense on the crown, sparse at the back.
    for i in range(0, len(positions), 3):
        x, y, z = positions[i:i + 3]
        v = max(0.0, min(1.0, 0.5 + 0.1 * z))
        cmds.polyColorPerVertex("%s.vtx[%d]" % (scalp, i // 3), rgb=(v, v, v), colorDisplayOption=False)
    color_set = cmds.polyColorSet(scalp, query=True, currentColorSet=True)[0]
    desc_path = xg.descriptionPath(palette, desc).replace("\\", "/").rstrip("/")
    paint = desc_path + "/paintmaps"
    os.makedirs(paint, exist_ok=True)
    mel.eval('ptexBake -inMesh "%s" -outPtex "%s/%s" -bakeColorSet "%s";' % (scalp, paint, "dens", color_set))
    ptex = paint + "/dens/%s.ptx" % scalp
    xg.setAttr("mask", "$a=map('${DESC}/paintmaps/dens/');#3dpaint,10.0\\n$a", palette, desc, "RandomGenerator")
    # Clumping with its maps (clump points at the guides), as XGen's Setup Maps makes them.
    clump = xg.addFXModule(palette, desc, "ClumpingFXModule")
    xg.setAttr("clump", "0.85", palette, desc, clump)
    saved = xgg.DescriptionEditor
    xgg.DescriptionEditor = _DescriptionEditor(palette, desc)
    try:
        viewer_t = cmds.createNode("transform", name="xgmPointsViewer1")
        cmds.createNode("xgmPointsViewer", name=viewer_t + "Shape", parent=viewer_t)
        point_dir = xg.getAttr("pointDir", palette, desc, clump)
        cmds.xgmPoints(guidePoints=True)
        cmds.xgmPoints(savePoints=point_dir, module=clump)
        cmds.delete(viewer_t)
        xg.setAttr("mapInitialized", "True", palette, desc, clump)
        cmds.xgmClumpMap(fxmodule=clump, mapDir=xg.getAttr("mapDir", palette, desc, clump), pointDir=point_dir,
                         texelsPerUnit=10.0, radiusVariance="0.5", description=desc)
    finally:
        xgg.DescriptionEditor = saved
    noise = xg.addFXModule(palette, desc, "NoiseFXModule")
    xg.setAttr("magnitude", "0.4", palette, desc, noise)
    xg.setAttr("frequency", "2.0", palette, desc, noise)
    coil = xg.addFXModule(palette, desc, "CoilFXModule")
    xg.setAttr("count", "1.5", palette, desc, coil)
    xg.setAttr("radius", "0.15", palette, desc, coil)
    cut = xg.addFXModule(palette, desc, "CutFXModule")
    xg.setAttr("amount", "0.3", palette, desc, cut)
    cmds.file(save=True, type="mayaAscii")
    return {"scene": scene, "palette": palette, "description": desc, "ptex": ptex}


def strand_picture(path, panels, size=420, view="front", max_strands=4000):
    """A PNG of strand sets side by side, drawn flat (orthographic, `view` "front": x right, y up;
    "side": z right, y up; "top": x right, z down), all at one scale: panels [(title, StrandSet)].
    Each strand a thin line darkening toward its tip."""
    from groomlab.ui.qt import QtCore, QtGui
    app = QtGui.QGuiApplication.instance() or QtGui.QGuiApplication(["strand_picture", "-platform", "offscreen"])
    axes = {"front": (0, 1, 1.0), "side": (2, 1, 1.0), "top": (0, 2, -1.0)}[view]
    lo, hi = [1e30, 1e30], [-1e30, -1e30]
    for _, s in panels:
        for p in s.positions:
            c = ((p.x, p.y, p.z)[axes[0]], axes[2] * (p.x, p.y, p.z)[axes[1]])
            for k in range(2):
                lo[k], hi[k] = min(lo[k], c[k]), max(hi[k], c[k])
    span = max(hi[0] - lo[0], hi[1] - lo[1], 1e-6) * 1.06
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    title_h = 26
    image = QtGui.QImage(size * len(panels), size + title_h, QtGui.QImage.Format_RGB32)
    image.fill(QtGui.QColor(246, 244, 240))
    painter = QtGui.QPainter(image)
    painter.setRenderHint(QtGui.QPainter.Antialiasing)
    for i, (title, s) in enumerate(panels):
        ox = i * size
        painter.setPen(QtGui.QColor(40, 40, 40))
        painter.drawText(QtCore.QRectF(ox, 4, size, title_h - 6), QtCore.Qt.AlignCenter, title)
        step = max(1, s.strand_count // max_strands)
        for k in range(0, s.strand_count, step):
            pts = s.strand_points(k)
            for j, (a, b) in enumerate(zip(pts, pts[1:])):
                shade = int(150 - 110 * j / max(1, len(pts) - 2))
                painter.setPen(QtGui.QPen(QtGui.QColor(shade, int(shade * 0.75), int(shade * 0.5), 110), 0.8))
                pa = ((a.x, a.y, a.z)[axes[0]], axes[2] * (a.x, a.y, a.z)[axes[1]])
                pb = ((b.x, b.y, b.z)[axes[0]], axes[2] * (b.x, b.y, b.z)[axes[1]])
                painter.drawLine(QtCore.QPointF(ox + (pa[0] - cx) / span * size + size / 2,
                                                title_h + size / 2 - (pa[1] - cy) / span * size),
                                 QtCore.QPointF(ox + (pb[0] - cx) / span * size + size / 2,
                                                title_h + size / 2 - (pb[1] - cy) / span * size))
    painter.end()
    image.save(path, "PNG")
    del app
    return path


def strand_stats(strands, sample=300, seed=1):
    """{"count", "length" (p10, median, p90), "straightness" (mean root-to-tip over length),
    "bbox" (min, max), "tip_spread" (median nearest-tip distance over median nearest-root distance:
    below 1, the tips gather: clumps)} of a groomlab_core StrandSet."""
    n = strands.strand_count
    if n == 0:
        return {"count": 0}
    lengths, straight, roots, tips = [], [], [], []
    lo, hi = [1e30] * 3, [-1e30] * 3
    for i in range(n):
        pts = strands.strand_points(i)
        L = 0.0
        for a, b in zip(pts, pts[1:]):
            L += math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2)
        lengths.append(L)
        r, t = pts[0], pts[-1]
        d = math.sqrt((r.x - t.x) ** 2 + (r.y - t.y) ** 2 + (r.z - t.z) ** 2)
        straight.append(d / L if L > 0 else 1.0)
        roots.append((r.x, r.y, r.z))
        tips.append((t.x, t.y, t.z))
        for p in pts:
            for k, c in enumerate((p.x, p.y, p.z)):
                lo[k] = min(lo[k], c)
                hi[k] = max(hi[k], c)
    lengths.sort()
    rng = random.Random(seed)
    picks = rng.sample(range(n), min(sample, n))

    def nearest(points, i):
        p = points[i]
        best = 1e30
        for j, q in enumerate(points):
            if j != i:
                d = (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2
                if d < best:
                    best = d
        return math.sqrt(best)

    root_nn = sorted(nearest(roots, i) for i in picks)
    tip_nn = sorted(nearest(tips, i) for i in picks)
    return {"count": n,
            "length": (lengths[n // 10], lengths[n // 2], lengths[(9 * n) // 10]),
            "straightness": sum(straight) / n,
            "bbox": (tuple(lo), tuple(hi)),
            "tip_spread": tip_nn[len(tip_nn) // 2] / max(1e-9, root_nn[len(root_nn) // 2])}
