# SPDX-License-Identifier: Apache-2.0
"""GUI test for guide selection and interactive symmetric paint (needs a Maya UI session).

The viewport ignores synthetic Qt mouse events, so most lassos and the stroke go
through the tools' -debugLasso / -debugStroke hooks, which run the same code
as the mouse from view coordinates; mouse_marquee posts Windows mouse messages
instead (the real input path, the cursor left where it is).

A suite of the GUI session (gui_runner.py, ctest maya.gui; harness: gui_support.py).
Each step is a deferred call, as with a user.
"""

import math

import maya.OpenMayaUI as omui
from maya import cmds

from gui_support import check, finish, guarded, persp_panel, settle_undo, soon, to_view

state = {}


def start():
    state.clear()
    soon(setup)


def setup():
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    curves = []
    for angle in (0.35, 0.8):
        n = (math.sin(angle), math.cos(angle), 0.2)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.1 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    groomlab.mirror_group(groom, "primary", "x")
    groomlab.generate_children(groom, "primary", name="children", count=3)
    cmds.select(cmds.listRelatives(groom, parent=True))
    cmds.setFocus(persp_panel())
    cmds.setAttr("persp.translate", 0, 2, 9)
    cmds.setAttr("persp.rotate", -8, 0, 0)
    cmds.refresh(force=True)
    state["groom"] = groom
    soon(lasso_all)


def lasso_all():
    import groomlab
    ctx = groomlab.select_tool(level=0)
    view = omui.M3dView.active3dView()
    w, h = view.portWidth(), view.portHeight()
    settle_undo()  # Maya's idle work done: the clicks below are the steps on top of the queue
    count = cmds.groomLabSelectCtx(ctx, edit=True, debugLasso="0 0 %d 0 %d %d 0 %d" % (w, w, h, h))
    primaries = groomlab.guide_ids(state["groom"], "primary")
    check("lasso over the view selects the %d primaries (got %s)" % (len(primaries), count), count == len(primaries))
    check("selection stored", sorted(groomlab.selected_guides(state["groom"])) == sorted(primaries))

    # A click at a guide's tip picks that guide alone.
    target = primaries[0]
    xyz = cmds.groomLabGuides(state["groom"], guidePoints=target)
    x, y = to_view(xyz[-3:])
    cmds.groomLabSelectCtx(ctx, edit=True, debugLasso="%d %d" % (x, y))
    check("click picks one guide", groomlab.selected_guides(state["groom"]) == [target])
    frozen = [str(g["id"]) for g in groomlab.get_hierarchy(state["groom"])["guides"]
              if g.get("attributes", {}).get("freeze", 0.0) > 0.5 and str(g["id"]) in set(map(str, primaries))]
    check("the Select tool freezes the guides not selected (%d frozen)" % len(frozen),
          str(target) not in frozen and len(frozen) == len(primaries) - 1)  # the primaries but the one
    cmds.groomLabSelectCtx(ctx, edit=True, debugLasso="%d %d" % (x, y), debugMode="toggle")
    check("shift-click toggles it off", groomlab.selected_guides(state["groom"]) == [])
    state["target"] = target
    soon(undo_selection)


def undo_selection():
    import groomlab
    cmds.undo()
    check("undo restores the clicked selection", groomlab.selected_guides(state["groom"]) == [state["target"]])
    soon(mouse_marquee)


def draw_serial():
    """The count of viewport updates that drew new geometry (any groom's)."""
    import json
    return json.loads(cmds.groomLabGraphStats(state["groom"], drawTimes=True))["draw"].get("serial")


def post_mouse(points, done, shift=False):
    """A press at points[0], moves through the rest and a release at the last, as Windows mouse
    messages to the viewport's window (the path a real mouse takes through Qt to the tool, which
    synthetic Qt events skip), one per timer tick (Qt merges queued moves). The cursor stays put.
    `shift`: Shift held through it (Maya's thread's key state, as SetKeyboardState sets it for
    this thread only, and the messages' MK_SHIFT), let go after the release."""
    import ctypes
    from groomlab.ui.qt import QtCore, QtWidgets
    from groomlab.ui.qt import shiboken
    wrapInstance = shiboken.wrapInstance
    widget = wrapInstance(int(omui.M3dView.active3dView().widget()), QtWidgets.QWidget)
    native = widget if widget.testAttribute(QtCore.Qt.WA_NativeWindow) else (widget.nativeParentWidget() or widget.window())
    hwnd, ratio = int(native.winId()), widget.devicePixelRatioF()

    def lparam(p):
        q = widget.mapTo(native, QtCore.QPoint(*p))
        return (int(q.y() * ratio) << 16) | (int(q.x() * ratio) & 0xFFFF)
    held = 0x4 if shift else 0  # MK_SHIFT
    messages = [(0x200, held, points[0]), (0x201, 1 | held, points[0])]  # WM_MOUSEMOVE, WM_LBUTTONDOWN (MK_LBUTTON)
    for a, b in zip(points, points[1:]):
        messages += [(0x200, 1 | held, (a[0] + (b[0] - a[0]) * i // 6, a[1] + (b[1] - a[1]) * i // 6)) for i in range(1, 7)]
    messages.append((0x202, held, points[-1]))  # WM_LBUTTONUP

    def hold_shift(down):
        keys = (ctypes.c_ubyte * 256)()
        ctypes.windll.user32.GetKeyboardState(keys)
        for vk in (0x10, 0xA0):  # VK_SHIFT, VK_LSHIFT
            keys[vk] = 0x80 if down else 0
        ctypes.windll.user32.SetKeyboardState(keys)
    if shift:
        hold_shift(True)

    def step():
        if not messages:
            if shift:
                QtCore.QTimer.singleShot(300, lambda: hold_shift(False))
            QtCore.QTimer.singleShot(400, guarded(done))  # the release's work and redraw
            return
        message, keys, p = messages.pop(0)
        ctypes.windll.user32.PostMessageW(hwnd, message, keys, lparam(p))
        QtCore.QTimer.singleShot(15, step)
    step()


def mouse_marquee():
    """The Select tool driven by the mouse: a marquee, then a click on nothing, each drawn at once
    (testers saw the selection change only once the camera moved)."""
    import sys
    import groomlab
    from groomlab.ui.qt import QtCore, QtWidgets
    from groomlab.ui.qt import shiboken
    wrapInstance = shiboken.wrapInstance
    if sys.platform != "win32":  # (Windows mouse messages)
        soon(symmetric_paint)
        return
    groom = state["groom"]
    groomlab.clear_guide_selection(groom)
    groomlab.unfreeze_all(groom)
    groomlab.select_tool(level=0)
    cmds.refresh(force=True)
    widget = wrapInstance(int(omui.M3dView.active3dView().widget()), QtWidgets.QWidget)
    w, h = widget.width(), widget.height()
    state["serial"] = draw_serial()
    primaries = groomlab.guide_ids(groom, "primary")

    def marqueed():
        check("a mouse marquee selects the %d primaries" % len(primaries),
              sorted(groomlab.selected_guides(groom)) == sorted(primaries))
        drawn = draw_serial()
        check("the viewport draws the marquee's selection without a camera move (draws %s -> %s)" % (state["serial"], drawn),
              drawn != state["serial"])
        state["serial"] = drawn
        post_mouse([(6, 6)], clicked)

    def clicked():
        check("a click on nothing deselects every guide", groomlab.selected_guides(groom) == [])
        drawn = draw_serial()
        check("the viewport draws the deselection without a camera move (draws %s -> %s)" % (state["serial"], drawn),
              drawn != state["serial"])
        state["serial"] = drawn
        # Pick A, then B, then A again (a tester's steps: the second pick of A drew only once the
        # camera moved; Maya sends no dirty message for a plug set again while still dirty).
        a_, b_ = primaries[0], primaries[1]
        state["picks"] = [(a_, False, [a_]), (b_, False, [b_]), (a_, False, [a_]),
                          # Shift toggles: B added, A off, A on again (multi-select drawn each time)
                          (b_, True, sorted([a_, b_])), (a_, True, [b_]), (a_, True, sorted([a_, b_]))]
        pick_next()

    def tip(guide):
        """A guide's tip in the widget's coordinates (view coordinates are pixels from the bottom)."""
        view = omui.M3dView.active3dView()
        x, y = to_view(cmds.groomLabGuides(groom, guidePoints=guide)[-3:])
        ratio = widget.devicePixelRatioF()
        return int(x / ratio), int((view.portHeight() - y) / ratio)

    def pick_next():
        if not state["picks"]:
            soon(symmetric_paint)
            return
        guide, shift, wanted = state["picks"].pop(0)
        what = "a Shift-click on guide %s" % guide if shift else "a click on guide %s" % guide

        def picked():
            got = sorted(groomlab.selected_guides(groom))
            check("%s selects %s (got %s)" % (what, wanted, got), got == wanted)
            drawn = draw_serial()
            check("the viewport draws %s without a camera move (draws %s -> %s)" % (what, state["serial"], drawn),
                  drawn != state["serial"])
            state["serial"] = drawn
            # Past the double-click interval: a second press on the same spot within it is Qt's
            # double click, which Maya's tools take as one (a person's clicks are farther apart).
            QtCore.QTimer.singleShot(QtWidgets.QApplication.doubleClickInterval() + 100, guarded(pick_next))
        post_mouse([tip(guide)], picked, shift=shift)
    post_mouse([(10, 10), (w - 10, 10), (w - 10, h - 10), (10, h - 10), (10, 12)], marqueed)


def symmetric_paint():
    import groomlab
    groom = state["groom"]
    groomlab.clear_guide_selection(groom)
    groomlab.unfreeze_all(groom)  # the Select tool soloed its selection: every guide brushable again
    groomlab.set_guide_attribute(groom, "clump", 0.0, level=0)
    ctx = groomlab.brush_tool("clump", radius=150, strength=1.0, level=0, symmetry="x", selected_only=False)
    xyz = cmds.groomLabGuides(groom, guidePoints=state["target"])
    x, y = to_view(xyz[:3])  # press on the scalp, at the guide's root
    changed = cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d %d %d" % (x, y, x + 2, y))
    stored = {str(g["id"]): g.get("attributes", {}).get("clump", 0.0) for g in groomlab.get_hierarchy(groom)["guides"]}
    groomlab.select_guides(groom, guides=[state["target"]])
    (mirror,) = groomlab.mirror_guide_selection(groom, "x")
    check("stroke painted (changed=%s)" % changed, changed and stored[state["target"]] > 0.2)
    # The mirror guide is the nearest guide to the target's mirror image, not an exact mirror, so its
    # paint depends on where that image falls in the brush circle (how many pixels the offset spans).
    check("symmetry painted the mirror guide too (%.3f vs %.3f)" % (stored[state["target"]], stored[mirror]),
          stored[mirror] > 0.6 * stored[state["target"]])

    # Selected only: with just the target selected, the mirror stays put.
    groomlab.set_guide_attribute(groom, "clump", 0.0, level=0)
    groomlab.select_guides(groom, guides=[state["target"]])
    cmds.groomLabBrushCtx(ctx, edit=True, symmetry="none", selectedOnly=True)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d %d %d" % (x, y, x + 2, y))
    stored = {str(g["id"]): g.get("attributes", {}).get("clump", 0.0) for g in groomlab.get_hierarchy(groom)["guides"]}
    others = [v for k, v in stored.items() if k != state["target"]]
    check("selected-only paints the selection alone", stored[state["target"]] > 0.2 and max(others) == 0.0)
    finish()
