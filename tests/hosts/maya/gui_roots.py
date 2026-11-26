# SPDX-License-Identifier: Apache-2.0
"""GUI test for the Roots brush (root-first tubes) in Maya (needs a Maya UI session): the core's root
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
    # (Edit roots leaves the guides and tubes to the Visibility switches: #85)
    check("Edit roots leaves the guides as the switches have them",
          not tool.state.get("hidesGuides") and groomlab.guides_visible(state["groom"]) == shown)
    guide = _roots()[0]
    cvs = root_tubes.root_outline(state["groom"], guide)
    x, y = to_view(cvs[0])
    cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, debugStroke="%d %d %d %d" % (x, y, x + 25, y + 10))
    tool.finish_edit()
    after = root_tubes.root_outline(state["groom"], guide)
    check("Finish edit gives the root its moved CV", math.sqrt(sum((a - b) ** 2 for a, b in zip(after[0], cvs[0]))) > 0.01)
    check("and the guides as they were", groomlab.guides_visible(state["groom"]) == shown)
    tool.leave()
    check("leaving puts the tool away", not tool.poll().get("active"))
    soon(roots_brush)


def roots_brush():
    # The Roots brush (its own tile, Create): picking it starts the root tool drawing a root (no
    # brush stroke on the Tube brush's "shape" mode); its card the levels and the root block, no
    # brush settings; another brush leaves root mode. Tube keeps its id and edits tubes only.
    import groomlab
    from groomlab.ui import palette, root_tube_tool
    window = palette.show()
    try:
        window.groom_picked(window.grooms.findData(state["groom"]))
        window.buttons["roots"].click()
        tool = root_tube_tool.TOOL
        check("Roots: the root tool, drawing a root",
              tool.active() and tool.mode == "place" and window.tool == "roots"
              and cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True) == "roots")
        check("Roots' card: the levels and the root block, no brush settings",
              window.root_card.root_block.isVisibleTo(window) and window.root_card.hierarchy_block.isVisibleTo(window)
              and not window.brush_block.isVisibleTo(window) and not window.tube_panel.profile_block.isVisibleTo(window))
        # The root tubes drawn as the guides display says: picking Roots sends its look, and a
        # switch flipped there reaches the tool (and the groom) at once.
        ctx = groomlab.BRUSH_CONTEXT
        look = lambda: tuple(cmds.groomLabBrushCtx(ctx, query=True, **{k: True}) for k in  # noqa: E731
                             ("tubeColors", "tubeLevelColors", "tubeShaded", "tubeWireframe"))
        check("Roots takes the guides display's tube look",
              look() == (window.tube_colors.isChecked(), window.tube_level_colors.isChecked(),
                         window.tube_shaded.isChecked(), window.tube_wireframe.isChecked()))
        window.tube_level_colors.setChecked(not window.tube_level_colors.isChecked())
        check("Colors per level reaches the tool", look()[1] == window.tube_level_colors.isChecked())
        window.tube_level_colors.setChecked(not window.tube_level_colors.isChecked())
        window.root_card.select_root.click()
        check("Select root from the card", tool.mode == "select" and window.tool == "roots")
        window.root_card.select_root.click()
        check("Select root again: back to Draw", tool.mode == "place" and window.tool == "roots")
        window.poll()
        check("the poll keeps the Roots tile on", window.tool == "roots" and window.buttons["roots"].isChecked())
        window.buttons["shape"].click()
        check("another brush leaves root mode", not tool.poll().get("active") and window.tool == "shape"
              and cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True) == "shape")
        check("Tube's card: no root block", not window.root_card.root_block.isVisibleTo(window)
              and window.root_card.hierarchy_block.isVisibleTo(window))
        window.buttons["comb"].click()  # another tool up: the tubes' look still reaches the tool
        window.tube_colors.setChecked(not window.tube_colors.isChecked())
        check("Colors with Comb up reaches the tool", look()[0] == window.tube_colors.isChecked())
        window.tube_colors.setChecked(not window.tube_colors.isChecked())
    finally:
        window.stop_tool()
        window.close()
    soon(tube_level_pin)


def tube_level_pin():
    # A tube hierarchy's level kept in view while another level's guides are combed: the tubes'
    # Level (the guides display), a level row's tube in Guide groups and the Roots card's Lock view
    # are one setting, the groom's tubeViewGroup; picking a level shows its guides alone, the pin
    # stays; the Tubes switch (displayTubes) is the one way the tubes show with every tool.
    from groomlab import root_tubes
    from groomlab.ui import palette
    groom = state["groom"]
    info = root_tubes.add_tube_level(groom)
    base, deeper = info["base"], info["levels"][1]
    window = palette.show()
    try:
        window.groom_picked(window.grooms.findData(groom))
        window.buttons["comb"].click()
        window.guides_tab.refresh()  # (the new level among the groups to pick)
        window.pick_group(deeper)
        from groomlab.ui.qt import QtWidgets
        tube = window.findChild(QtWidgets.QToolButton, "groupTube_" + deeper)
        check("a level row has its tube", tube is not None and window.findChild(QtWidgets.QToolButton, "groupTube_" + base))
        window.set_tube_view(deeper)
        check("level 2's tubes kept in view, shown with Comb up",
              cmds.getAttr(groom + ".tubeViewGroup") == deeper and cmds.getAttr(groom + ".displayTubes")
              and window.tube_show.isChecked())
        check("the guides display's Level says so", window.tube_level.currentData() == deeper)
        check("so does the Roots card's Lock view", window.root_card.lock_view.isChecked())
        window.pick_group(base)
        hidden = set(cmds.getAttr(groom + ".hiddenGuideGroups").split()) if cmds.getAttr(groom + ".hiddenGuideGroups") else set()
        check("picking level 1 shows its guides alone (hidden: %s)" % sorted(hidden), base not in hidden and deeper in hidden)
        check("the pin stays while level 1 is combed", cmds.getAttr(groom + ".tubeViewGroup") == deeper)
        check("the card says what shows", "tubes: Level 2" in window.card_editing.text())
        window.tube_show.setChecked(False)
        check("Show tubes off: the groom's switch", not cmds.getAttr(groom + ".displayTubes"))
        window.set_tube_view("")
        check("Follow guides: no pin, no Lock view", cmds.getAttr(groom + ".tubeViewGroup") == ""
              and not window.root_card.lock_view.isChecked() and window.tube_level.currentData() == "")
    finally:
        window.stop_tool()
        window.close()
    finish()
