# SPDX-License-Identifier: Apache-2.0
"""Grooms on scalps deformed by Maya's Proximity Wrap (issue #76: the hair stopped following them).

A suite of the GUI session (gui_runner.py; opt in: GROOMLAB_GUI_SUITES=deformers; harness:
gui_support.py). Scalps wrapped (proximityWrap) to a body whose corrective blend shape is keyed:
each its own wrap and groom, one wrap for both, scalps skinned first and wrapped after their
grooms were made, and one groom on both scalps (extraScalps). Each scene is stepped through frames
with the dependency graph alone (the reference), then with the Evaluation Manager in parallel, with
GPU Override on (proximityWrap is a deformer it runs on the GPU; Wrap is not), and with cached
playback (played through once, then stepped): each frame's strands (pulled) and the viewport's
picture must be the reference's.
"""

import hashlib
import math
import os
import tempfile

from maya import cmds

from gui_support import check, finish, say, soon

FIRST, LAST = 1, 10
# Dense enough for GPU Override to take the wraps (it leaves meshes under its minimum vertex
# count, a couple of thousand, on the CPU; a real scalp is denser still).
SUBDIVISIONS = {"subdivisionsX": 80, "subdivisionsY": 40}
FRAMES = (FIRST, 5, LAST, 3)


def start():
    soon(run)


def grow(groom, angles):
    import groomlab
    cmds.setAttr(groom + ".previewDensity", 1.0)
    curves = []
    for a in angles:
        n = (math.sin(a), math.cos(a), 0.2)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.2 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.set_param(groom, "scatter", "density", 300.0)


def wrap(meshes, driver):
    from maya.internal.nodes.proximitywrap import node_interface
    node = cmds.deformer(meshes, type="proximityWrap")[0]
    face = node_interface.NodeInterface(node)
    if hasattr(face, "addDrivers"):  # Maya 2023+
        face.addDrivers([driver])
    else:  # Maya 2022
        face.addDriver(driver)
    return node


def scene(kind):
    """(grooms, scalps) of a scene: separate, shared, skinned or extra."""
    import groomlab
    cmds.file(new=True, force=True)
    cmds.playbackOptions(minTime=FIRST, maxTime=LAST, animationStartTime=FIRST, animationEndTime=LAST,
                         loop="once", playbackSpeed=0, maxPlaybackSpeed=0)
    body = cmds.polySphere(radius=1, name="body", **SUBDIVISIONS)[0]
    target = cmds.duplicate(body, name="corrective")[0]
    cmds.select(target + ".vtx[0:1200]")
    cmds.move(0.5, 0, 0, relative=True)
    blend = cmds.blendShape(target, body)[0]
    cmds.delete(target)
    cmds.setKeyframe(blend, attribute="w[0]", time=FIRST, value=0)
    cmds.setKeyframe(blend, attribute="w[0]", time=LAST, value=1)
    driver = cmds.listRelatives(body, shapes=True)[0]
    scalps = [cmds.polySphere(radius=1.01, name=n, **SUBDIVISIONS)[0] for n in ("scalpA", "scalpB")]
    angles = ((0.3, 0.7), (1.2, 1.6))
    grooms = []
    if kind == "extra":
        wrap(scalps, driver)
        groom = groomlab.create_groom(scalps[0])
        groomlab.add_scalp(groom, scalps[1])
        grow(groom, angles[0] + angles[1])
        grooms.append(groom)
    elif kind in ("rig", "rig_shared"):
        # Issue #76's setup: the scalps groomed first, then wrapped to the character, whose rig
        # (a skinned body) moves them; rig_shared: one wrap for every scalp (as the user rigs it:
        # one Proximity Wrap is lighter than a Wrap per scalp).
        cmds.select(clear=True)
        joint = cmds.joint(position=(0, -1, 0))
        cmds.skinCluster(joint, body, toSelectedBones=True)
        cmds.setKeyframe(joint, attribute="rotateZ", time=FIRST, value=0)
        cmds.setKeyframe(joint, attribute="rotateZ", time=LAST, value=40)
        for s, a in zip(scalps, angles):
            groom = groomlab.create_groom(s)
            grow(groom, a)
            grooms.append(groom)
        if kind == "rig_shared":
            wrap(scalps, driver)
        else:
            for s in scalps:
                wrap([s], driver)
    elif kind == "skinned":
        cmds.select(clear=True)
        joint = cmds.joint(position=(0, 0, 0))
        for s, a in zip(scalps, angles):
            groom = groomlab.create_groom(s)
            grow(groom, a)
            grooms.append(groom)
        for s in scalps:
            cmds.skinCluster(joint, s, toSelectedBones=True)
        cmds.setKeyframe(joint, attribute="rotateY", time=FIRST, value=0)
        cmds.setKeyframe(joint, attribute="rotateY", time=LAST, value=30)
        for s in scalps:
            wrap([s], driver)
    else:
        if kind == "shared":
            wrap(scalps, driver)
        for s, a in zip(scalps, angles):
            if kind == "separate":
                wrap([s], driver)
            groom = groomlab.create_groom(s)
            grow(groom, a)
            grooms.append(groom)
    cmds.hide(body)
    cmds.select(clear=True)
    cmds.currentTime(FIRST)
    cmds.viewFit("persp", allObjects=True)
    return grooms, scalps


def strands(groom):
    return tuple(cmds.groomLabGraphStats(groom, points=True) or [])


def apart(a, b):
    """How far two strand point lists are apart: the largest coordinate difference (inf: other counts)."""
    return max((abs(x - y) for x, y in zip(a, b)), default=0.0) if len(a) == len(b) else float("inf")


def shot(folder, name, frame):
    path = os.path.join(folder, "%s_%d.png" % (name, frame))
    cmds.playblast(frame=[frame], format="image", compression="png", viewer=False, showOrnaments=False,
                   percent=100, widthHeight=(320, 240), completeFilename=path)
    return path


def differ(a, b):
    from groomlab.ui.qt import QtGui
    ia, ib = QtGui.QImage(a), QtGui.QImage(b)
    if ia.size() != ib.size() or ia.isNull():
        return 1.0
    ia = ia.convertToFormat(QtGui.QImage.Format_RGB32)
    ib = ib.convertToFormat(QtGui.QImage.Format_RGB32)
    off = sum(1 for y in range(ia.height()) for x in range(ia.width()) if ia.pixel(x, y) != ib.pixel(x, y))
    return off / float(ia.width() * ia.height())


def set_mode(mode):
    import gui_cached_playback as cached
    cached.cache_manager().enabled = False
    cmds.evaluationManager(mode="off" if mode == "dg" else "parallel")
    cmds.evaluator(name="deformer", enable=mode == "gpu")
    if mode == "cached":
        cached.cache_on()
        cmds.currentTime(FIRST)
        cmds.play(forward=True, wait=True)
        safe = cached.safe_mode()
        check("cached: never in safe mode (%s)" % safe, not safe)


def step(label, grooms, scalps, folder):
    rows = []
    for f in FRAMES:
        cmds.currentTime(f)
        cmds.refresh(force=True)
        picture = shot(folder, label, f)
        rows.append((f, [round(cmds.pointPosition(s + ".vtx[100]", world=True)[0], 4) for s in scalps],
                     [strands(g) for g in grooms], picture))
    return rows


def run_scene(kind, folder):
    grooms, scalps = scene(kind)
    set_mode("dg")
    reference = step(kind + "_dg", grooms, scalps, folder)
    for mode in ("parallel", "gpu", "cached"):
        set_mode(mode)
        if mode == "gpu":
            if kind == kinds_run()[0]:
                try:
                    say("GPU Override's fewest vertices: %s" % cmds.deformerEvaluator(query=True, minimumVerts=True))
                except Exception as e:  # noqa: BLE001
                    say("GPU Override's fewest vertices: (%s)" % e)
        rows = step("%s_%s" % (kind, mode), grooms, scalps, folder)
        for (f, sx, st, pic), (_, rsx, rst, rpic) in zip(rows, reference):
            name = "%s %s frame %d" % (kind, mode, f)
            far = [apart(a, b) for a, b in zip(st, rst)]
            if sx != rsx or max(far) > 0:
                say("%s: scalp x %s (dg %s), strands apart from the DG's by %s" % (name, sx, rsx, ["%.2g" % d for d in far]))
            check(name + ": the scalps deform as with the DG", sx == rsx)
            # (GPU Override's deformers round otherwise than the CPU's: a hair's breadth allowed)
            check(name + ": the strands follow the scalps as with the DG (%.2g apart)" % max(far), max(far) < 1e-3)
            share = differ(pic, rpic)
            check("%s: the viewport draws what the DG draws (%.4f differ)" % (name, share), share < 0.002)
        for g in range(len(grooms)):
            moved = len(set(r[2][g] for r in rows)) == len(FRAMES)
            check("%s %s: groom %d's hair moves with the scalp from frame to frame" % (kind, mode, g), moved)
    moved = len(set(r[2][0] for r in reference)) == len(FRAMES)
    check("%s dg: the hair moves with the scalp from frame to frame" % kind, moved)
    pictures = set(hashlib.md5(open(r[3], "rb").read()).hexdigest() for r in reference)
    check("%s dg: the viewport's picture changes from frame to frame" % kind, len(pictures) == len(FRAMES))


def kinds_run():
    return [k.strip() for k in os.environ.get("GROOMLAB_GUI_CASES", "").split(",") if k.strip()] or list(KINDS)


KINDS = ("rig", "rig_shared", "separate", "shared", "skinned", "extra")


def run():
    import gui_cached_playback as cached
    folder = tempfile.mkdtemp(prefix="groomlab_deformers_")
    was = {"mode": cmds.evaluationManager(query=True, mode=True)[0],
           "gpu": cmds.evaluator(name="deformer", query=True, enable=True),
           "cache": cached.cache_manager().enabled}
    try:
        for kind in kinds_run():
            run_scene(kind, folder)
    finally:
        cached.cache_manager().enabled = was["cache"]
        cmds.evaluationManager(mode=was["mode"])
        cmds.evaluator(name="deformer", enable=bool(was["gpu"]))
        say("pictures in %s" % folder)
    finish()
