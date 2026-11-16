# SPDX-License-Identifier: Apache-2.0
"""GUI test for the Tube tool's root-first mode in Maya (needs a Maya UI session): the core's root
tool (root_tube_tool.h) run by GroomLab's brush context, its clicks through -debugStroke (the same
code as the mouse, from view coordinates). Its behaviour itself is pinned by the core's tests
(tests/engine/core/test_root_tube_tool.cpp); this checks Maya's wiring: the clicks reach the tool,
its edits are Maya undo steps, the card's buttons and state, the guides hidden while editing.

A suite of the GUI session (gui_runner.py, ctest maya.gui; harness: gui_support.py).
"""

import json
import math

from maya import cmds

from gui_support import check, finish, persp_panel, settle_undo, soon, to_view

state = {}


def start():
    state.clear()
    soon(setup)


def _ring(center, radius, count=5):
    n = math.sqrt(sum(c * c for c in center))
    up = tuple(c / n for c in center)
    a = (up[1], -up[0], 0.0) if abs(up[2]) < 0.9 else (1.0, 0.0, 0.0)
    la = math.sqrt(sum(c * c for c in a))
    a = tuple(c / la for c in a)
    b = (up[1] * a[2] - up[2] * a[1], up[2] * a[0] - up[0] * a[2], up[0] * a[1] - up[1] * a[0])
    out = []
    for k in range(count):
        t = 2.0 * math.pi * k / count
        p = tuple(up[i] + radius * (a[i] * math.cos(t) + b[i] * math.sin(t)) for i in range(3))
        lp = math.sqrt(sum(c * c for c in p))
        out.append(tuple(c / lp for c in p))
    return out


def _click(point, modifier=""):
    import groomlab
    x, y = to_view(point)
    words = ("%s %d %d" % (modifier, x, y)).strip()
    cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, debugStroke=words)


def _roots():
    from groomlab import root_tubes
    return root_tubes.root_tubes(state["groom"])


def setup():
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=40)[0]
    groom = groomlab.create_groom(scalp)
    cmds.select(cmds.listRelatives(groom, parent=True))
    cmds.setFocus(persp_panel())
    cmds.setAttr("persp.translate", 0, 4, 0.01)  # looking down onto the top of the head
    cmds.setAttr("persp.rotate", -89.9, 0, 0)
    cmds.refresh(force=True)
    state["groom"] = groom
    soon(draw_a_root)


def draw_a_root():
    from groomlab.ui import root_tube_tool
    tool = root_tube_tool.start(state["groom"], "place", draw=False, length=0.6, snap=True)
    state["tool"] = tool
    settle_undo()
    cvs = _ring((0.0, 1.0, 0.0), 0.15)
    for p in cvs:
        _click(p)
    check("the root's CVs placed by clicks (%s)" % tool.poll().get("cvs"), tool.poll().get("cvs") == 5)
    _click(cvs[0])  # the first CV again: closed, the tube made straight off the scalp
    check("a click on the first CV makes the root tube (%s)" % tool.poll().get("message"), len(_roots()) == 1)
    check("the tool back to placing", tool.mode == "place")
    cmds.undo()
    tool.poll()
    check("Maya's Undo takes the tube back and its outline comes back to close again",
          not _roots() and tool.state.get("cvs") == 5)
    cmds.redo()
    tool.poll()
    check("Redo puts it back, the outline away", len(_roots()) == 1 and tool.state.get("cvs") == 0)
    soon(select_carry_duplicate)


def select_carry_duplicate():
    from groomlab import root_tubes
    from groomlab.ui import root_tube_tool
    tool = root_tube_tool.start(state["groom"], "select")
    guide = _roots()[0]
    middle, _n = root_tubes.root_center(state["groom"], guide)
    _click(middle)
    check("a click on a root's middle selects it", tool.poll().get("selected") == [guide])
    x, y = to_view(middle)
    import groomlab
    cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, debugStroke="%d %d %d %d %d %d" % (x, y, x + 30, y, x + 60, y))
    moved, _n = root_tubes.root_center(state["groom"], guide)
    check("a drag carries it over the scalp", math.sqrt(sum((a - b) ** 2 for a, b in zip(moved, middle))) > 0.02)
    tool.begin_duplicate()
    check("Duplicate waits for a click", tool.mode == "duplicate")
    _click(_ring((-0.45, 0.85, 0.2), 0.0, 1)[0])
    check("a click sets the copy down (%s)" % tool.state.get("message"), len(_roots()) == 2)
    tool.delete_selected()
    check("Delete takes the copy away", len(_roots()) == 1)
    soon(edit_and_leave)


def edit_and_leave():
    import groomlab
    from groomlab import root_tubes
    from groomlab.ui import root_tube_tool
    shown = groomlab.guides_visible(state["groom"])
    tool = root_tube_tool.start(state["groom"], "edit")
    check("Edit roots hides the guides", tool.state.get("hidesGuides") and not groomlab.guides_visible(state["groom"]))
    guide = _roots()[0]
    cvs = root_tubes.root_outline(state["groom"], guide)
    x, y = to_view(cvs[0])
    cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, debugStroke="%d %d %d %d" % (x, y, x + 25, y + 10))
    tool.finish_edit()
    after = root_tubes.root_outline(state["groom"], guide)
    check("Finish edit gives the root its moved CV", math.sqrt(sum((a - b) ** 2 for a, b in zip(after[0], cvs[0]))) > 0.01)
    check("and shows the guides again", groomlab.guides_visible(state["groom"]) == shown)
    tool.leave()
    check("leaving puts the tool away", not tool.poll().get("active"))
    finish()
