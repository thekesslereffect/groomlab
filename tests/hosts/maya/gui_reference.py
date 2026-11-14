# SPDX-License-Identifier: Apache-2.0
"""GUI test for reference photos (Groom Tools > Photos; needs a Maya UI session).

Looks through a reference camera: a playblast checks that the photo is drawn where its pixels
map to (the camera model the solve and the Photos window use); pairs made through a known camera
solve back to that camera, and a locked one does not tumble. Through a locked one the wheel and
Alt+drags zoom and pan the view in 2D (ui/photo_view.py: the camera stays, a Place click lands
under the cursor), F shows all of it again, and unlocked the events go on to Maya. (The Photos window's own clicks are
checked in the editor tests: editor_cases.py's ReferenceWindowTest.)

A suite of the GUI session (gui_runner.py, ctest maya.gui; harness: gui_support.py).
Each step is a deferred call.
"""

import math
import os
import tempfile

import maya.OpenMaya as om
import maya.OpenMayaUI as omui
from maya import cmds

from gui_support import check, finish, persp_panel, say, soon, to_view

state = {}
SPOT = (240, 60, 16)  # the photo's white square: x, y (pixels from the top-left), size
TRUTH = {"translate": (16.0, 9.0, 44.0), "rotate": (-10.0, 20.0, 2.0), "focal": 50.0}


def start():
    state.clear()
    soon(setup)


def view_camera():
    path = om.MDagPath()
    omui.M3dView.active3dView().getCamera(path)
    path.pop()
    return path.fullPathName()


def write_photo(path, width, height, spot):
    from groomlab.ui.qt import QtGui
    image = QtGui.QImage(width, height, QtGui.QImage.Format_RGB32)
    image.fill(QtGui.QColor(20, 20, 20))
    sx, sy, size = spot
    for i in range(sx - size // 2, sx + size // 2):
        for j in range(sy - size // 2, sy + size // 2):
            image.setPixelColor(i, j, QtGui.QColor(255, 255, 255))
    assert image.save(path)
    return path.replace("\\", "/")


def setup():
    import groomlab
    from groomlab import reference
    cmds.file(new=True, force=True)
    folder = tempfile.mkdtemp(prefix="groomlab_gui_ref_")
    cmds.workspace(folder, openWorkspace=True)
    scalp = cmds.polySphere(radius=10, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    cam = reference.import_reference(write_photo(os.path.join(folder, "front.png"), 320, 240, SPOT), groom=groom)
    reference.set_reference_opacity(cam, 1.0)
    panel = persp_panel()
    cmds.setFocus(panel)
    reference.look_through(cam, panel)
    check("the viewport looks through the reference", reference.current_reference(panel) == cam)
    cmds.refresh(force=True)
    state.update(folder=folder, cam=cam, panel=panel, scalp=scalp, groom=groom)
    soon(the_photo_is_drawn_where_its_pixels_are)


def point_on_photo(cam, u, v):
    """The world point on the reference's image plane showing photo pixel (u, v)."""
    from groomlab import reference
    shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
    width, height = cmds.getAttr(cam + ".glRefWidth"), cmds.getAttr(cam + ".glRefHeight")
    depth = cmds.getAttr(reference.image_plane(cam) + ".depth")
    k = depth / (cmds.getAttr(shape + ".focalLength") / 25.4)
    local = om.MPoint((u / width - 0.5) * cmds.getAttr(shape + ".horizontalFilmAperture") * k,
                      (0.5 - v / height) * cmds.getAttr(shape + ".verticalFilmAperture") * k, -depth)
    path = om.MDagPath()
    sel = om.MSelectionList()
    sel.add(cam)
    sel.getDagPath(0, path)
    world = local * path.inclusiveMatrix()
    return world.x, world.y, world.z


def the_photo_is_drawn_where_its_pixels_are():
    cam = state["cam"]
    check("the active view is the reference camera (%s)" % view_camera(),
          view_camera() == cmds.ls(cam, long=True)[0])
    # Find the photo's white square in a playblast: where the camera model puts its pixels.
    view = omui.M3dView.active3dView()
    width, height = view.portWidth(), view.portHeight()
    shot = os.path.join(state["folder"], "through.png")
    cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False, percent=100,
                   widthHeight=(width, height), completeFilename=shot, offScreen=True)
    from groomlab.ui.qt import QtGui
    image = QtGui.QImage(shot)
    xs, ys = [], []
    for j in range(0, image.height()):
        for i in range(0, image.width()):
            c = image.pixelColor(i, j)
            if c.red() > 200 and c.green() > 200 and c.blue() > 200:
                xs.append(i)
                ys.append(j)
    check("the playblast shows the photo's square (%d pixels, %dx%d)" % (len(xs), image.width(), image.height()),
          len(xs) > 4)
    if xs:
        cx = (sum(xs) / float(len(xs)) + 0.5) * width / float(image.width())
        cy = height - (sum(ys) / float(len(ys)) + 0.5) * height / float(image.height())
        x, y = to_view(point_on_photo(cam, SPOT[0], SPOT[1]))
        say("square at", (cx, cy), "its pixels at", (x, y))
        check("the square is where the photo's pixels map to (%.1f, %.1f vs %d, %d)" % (cx, cy, x, y),
              abs(cx - x) < 3.0 and abs(cy - y) < 3.0)
    soon(clicks_solve_back_to_the_camera)


def clicks_solve_back_to_the_camera():
    """Pairs made through a known camera (the photo points as it sees the mesh points) give that
    camera back."""
    from groomlab import reference
    cam, panel = state["cam"], state["panel"]
    shape = cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0]
    cmds.xform(cam, worldSpace=True, translation=TRUTH["translate"], rotation=TRUTH["rotate"])
    cmds.setAttr(shape + ".focalLength", TRUTH["focal"])
    xyz = cmds.xform(state["scalp"] + ".vtx[*]", query=True, worldSpace=True, translation=True)
    points = [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]
    eye = TRUTH["translate"]
    facing = [p for p in points if sum(a * (e - a) for a, e in zip(p, eye)) > 0.1 * 10 * math.sqrt(
        sum((e - a) ** 2 for a, e in zip(p, eye)))]  # the side facing the camera, toward its rim
    # Spread from the middle to the rim: points at many depths pin the focal length (a few close
    # to the middle let a nearer, wider camera fit them as well, within a pixel's rounding).
    chosen = facing[::max(1, len(facing) // 14)][:14]
    path = om.MDagPath()
    sel = om.MSelectionList()
    sel.add(cam)
    sel.getDagPath(0, path)
    inverse = path.inclusiveMatrixInverse()
    width, height = cmds.getAttr(cam + ".glRefWidth"), cmds.getAttr(cam + ".glRefHeight")
    k = TRUTH["focal"] / 25.4
    for i, p in enumerate(chosen):  # the photo point: the mesh point as the camera sees it
        c = om.MPoint(*p) * inverse
        u = (k * c.x / -c.z / cmds.getAttr(shape + ".horizontalFilmAperture") + 0.5) * width
        v = (0.5 - k * c.y / -c.z / cmds.getAttr(shape + ".verticalFilmAperture")) * height
        reference.set_pair(cam, i, image=(u, v), mesh=p)
    complete = reference.complete_pairs(cam)
    check("%d pairs" % len(complete), len(complete) == len(chosen))
    cmds.xform(cam, worldSpace=True, translation=(0, 0, 70), rotation=(0, 0, 0))
    cmds.setAttr(shape + ".focalLength", 35.0)
    result = reference.solve(cam)
    now = cmds.xform(cam, query=True, worldSpace=True, translation=True)
    off = math.sqrt(sum((a - b) ** 2 for a, b in zip(now, TRUTH["translate"])))
    say("solve:", result, "position off by", off)
    check("the clicks solve back to the camera (rms %.2f px, %.3f off, focal %.2f)" % (
        result["rms"], off, result["focal"]),
        result["rms"] < 2.0 and off < 0.05 * 49.0 and abs(result["focal"] / TRUTH["focal"] - 1.0) < 0.05)
    reference.lock_reference(cam)
    before = cmds.xform(cam, query=True, worldSpace=True, matrix=True)
    reference.look_through(cam, panel)
    cmds.tumble(cam, rotationAngles=(20, 30))
    check("a locked reference does not tumble", cmds.xform(cam, query=True, worldSpace=True, matrix=True) == before)
    soon(the_2d_view_zooms_without_moving_the_camera)


def _on_head(offset):
    """A point of the head (radius 10 at the origin) facing the solved camera, moved by `offset`."""
    d = [e + o for e, o in zip(TRUTH["translate"], offset)]
    n = math.sqrt(sum(c * c for c in d))
    return tuple(10.0 * c / n for c in d)


def the_2d_view_zooms_without_moving_the_camera():
    from groomlab import reference
    from groomlab.ui import photo_view
    from groomlab.ui.qt import QtCore, QtGui, QtWidgets, shiboken
    from gui_support import process_events
    cam, panel = state["cam"], state["panel"]
    view = omui.M3dView.active3dView()
    widget = shiboken.wrapInstance(int(view.widget()), QtWidgets.QWidget)
    scale = view.portWidth() / float(max(widget.width(), 1))

    def at_widget(xy):  # view pixels (y up) as the widget's point
        return QtCore.QPoint(int(round(xy[0] / scale)), int(round((view.portHeight() - xy[1]) / scale)))

    def wheel(pos, notches):
        QtWidgets.QApplication.sendEvent(widget, QtGui.QWheelEvent(
            QtCore.QPointF(pos), QtCore.QPointF(widget.mapToGlobal(pos)), QtCore.QPoint(), QtCore.QPoint(0, 120 * notches),
            QtCore.Qt.NoButton, QtCore.Qt.NoModifier, QtCore.Qt.NoScrollPhase, False))

    def drag(button, start, end):
        alt = QtCore.Qt.AltModifier
        for kind, pos, buttons in ((QtCore.QEvent.MouseButtonPress, start, button), (QtCore.QEvent.MouseMove, end, button),
                                   (QtCore.QEvent.MouseButtonRelease, end, QtCore.Qt.NoButton)):
            QtWidgets.QApplication.sendEvent(widget, QtGui.QMouseEvent(kind, QtCore.QPointF(pos), QtCore.QPointF(widget.mapToGlobal(pos)),
                                                                       button, buttons, alt))

    def shown(p):
        cmds.refresh(force=True)
        return to_view(p)

    check("the view's 2D gestures are on (%s)" % panel, panel in photo_view._filters)
    matrix = cmds.xform(cam, query=True, worldSpace=True, matrix=True)
    spot = _on_head((3.0, 2.0, 0.0))
    before = shown(spot)
    cursor = at_widget(before)
    wheel(cursor, 3)
    zoom = reference.view_2d(cam)["zoom"]
    after = shown(spot)
    check("the wheel zooms in 2D (%.3f)" % zoom, abs(zoom - (1.0 / 0.9) ** 3) < 1e-3)
    check("... the camera stays", cmds.xform(cam, query=True, worldSpace=True, matrix=True) == matrix)
    check("... about the cursor: the head's point under it stays (%s, %s)" % (before, after),
          abs(after[0] - before[0]) <= 2 and abs(after[1] - before[1]) <= 2)

    drag(QtCore.Qt.MiddleButton, cursor, cursor + QtCore.QPoint(40, -20))
    moved = shown(spot)
    check("Alt+middle pans the picture with the cursor (%s to %s)" % (after, moved),
          abs(moved[0] - after[0] - 40 * scale) <= 2 and abs(moved[1] - after[1] - 20 * scale) <= 2)
    drag(QtCore.Qt.RightButton, cursor, cursor + QtCore.QPoint(60, 0))
    check("Alt+right zooms in further (%.2f)" % reference.view_2d(cam)["zoom"], reference.view_2d(cam)["zoom"] > zoom * 1.2)
    check("... the camera still where it was", cmds.xform(cam, query=True, worldSpace=True, matrix=True) == matrix)

    # A brush through the zoomed view: Place at the head's point lands its root under the click.
    groom = state["groom"]
    target = _on_head((-2.0, 1.0, 0.0))
    px, py = shown(target)
    before_ids = set(_guide_ids(groom))
    ctx = groomlab_brush("place")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d" % (px, py))
    added = [g for g in _guide_ids(groom) if g not in before_ids]
    check("Place through the zoomed view adds a guide", len(added) == 1)
    if added:
        root = cmds.groomLabGuides(groom, guidePoints=str(added[0]))[:3]
        rx, ry = shown(tuple(root))
        check("... its root under the click (%d, %d vs %d, %d)" % (rx, ry, px, py), abs(rx - px) <= 3 and abs(ry - py) <= 3)
    cmds.setToolTo("selectSuperContext")

    # F: the whole view again.
    for kind in (QtCore.QEvent.ShortcutOverride, QtCore.QEvent.KeyPress):
        QtWidgets.QApplication.sendEvent(widget, QtGui.QKeyEvent(kind, QtCore.Qt.Key_F, QtCore.Qt.NoModifier))
    check("F shows the whole view again", reference.view_2d(cam)["zoom"] == 1.0 and not cmds.getAttr(
        cmds.listRelatives(cam, shapes=True, type="camera", fullPath=True)[0] + ".panZoomEnabled"))
    # Unlocked: the 2D view goes and the wheel is Maya's again (it lines the camera up by eye).
    reference.zoom_view_2d(cam, 2.0)
    reference.lock_reference(cam, False)
    check("unlocking shows the whole view", reference.view_2d(cam)["zoom"] == 1.0)
    wheel(cursor, 1)
    check("... and the wheel no longer zooms in 2D", reference.view_2d(cam)["zoom"] == 1.0)
    reference.lock_reference(cam)
    reference.look_back(panel)
    finish()


def _guide_ids(groom):
    import groomlab
    return [i for g in groomlab.list_guide_groups(groom) for i in (cmds.groomLabGuides(groom, listGuides=g["name"]) or [])]


def groomlab_brush(mode):
    import groomlab
    return groomlab.brush_tool(mode, length=3.0)
