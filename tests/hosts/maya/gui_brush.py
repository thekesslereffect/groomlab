# SPDX-License-Identifier: Apache-2.0
"""GUI test for the interactive brush (needs a Maya UI session, not mayapy).

Maya's viewport ignores synthetic mouse input, so strokes go through the
context's -debugStroke hook, which runs the same press/drag/release code from
view coordinates. Covers: comb stroke, one undo step per stroke, redo, the
Place brush, clicks off the scalp, and the Tool Settings panel.

A suite of the GUI session (gui_runner.py, ctest maya.gui; harness: gui_support.py).

Each step is its own deferred call: one undo chunk per step, and idle work
(like building Tool Settings) happens between steps, as with a user.
"""

import json
import math
import os
import tempfile

import maya.OpenMayaUI as omui
import maya.mel as mel
from maya import cmds

from gui_support import check, finish, later, persp_panel, say, to_view

state = {}


def red_pixels(name):
    """The active view redrawn and read back (its color buffer: what is on screen, not a
    playblast): how many pixels are the symmetry line's red. The picture is kept as
    groomlab_brush_<name>.png (in GROOMLAB_BRUSH_OUT, else the temp folder)."""
    return view_pixels(name, "red", lambda r, g, b: r > 170 and r > 2 * max(g, b))


def cyan_pixels(name):
    """The same for the Shape brush's tube wires (light cyan, 0.45 0.85 1)."""
    return view_pixels(name, "cyan", lambda r, g, b: b > 200 and g > 160 and 70 < r < 0.8 * g)


def warm_pixels(name):
    """The same for the selected guides' tube wires (warm orange, 1 0.7 0.15)."""
    return view_pixels(name, "warm", lambda r, g, b: r > 180 and 100 < g < r and b < 0.6 * g)


def view_pixels(name, what, test):
    """The active view redrawn and read back: how many pixels pass test(r, g, b)."""
    from maya.api import OpenMaya as om
    from maya.api import OpenMayaUI as omui2
    from PySide2 import QtGui
    cmds.refresh(force=True)
    view = omui2.M3dView.active3dView()
    view.refresh(False, True)
    image = om.MImage()
    view.readColorBuffer(image, True)
    folder = os.environ.get("GROOMLAB_BRUSH_OUT") or tempfile.gettempdir()
    path = os.path.join(folder, "groomlab_brush_%s.png" % name)
    image.writeToFile(path, "png")
    picture = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
    data = bytes(picture.constBits())[:picture.byteCount()]  # b, g, r, a
    count = sum(1 for i in range(0, len(data), 4) if test(data[i + 2], data[i + 1], data[i]))
    say("%s: %d %s pixels (%s)" % (name, count, what, path))
    return count


def start():
    state.clear()
    later(step_setup)


def step_setup():
    # The session loaded the plugin and sourced its MEL (the AE template before any groom is selected).
    import groomlab
    state["groomlab"] = groomlab

    cmds.file(new=True, force=True)
    cmds.undoInfo(state=True, infinity=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    curves = []
    for i in range(8):
        a = 2 * math.pi * i / 8
        x, z = 0.5 * math.cos(a), 0.5 * math.sin(a)
        y = math.sqrt(1 - 0.25)
        curves.append(cmds.curve(degree=2, point=[(x, y, z), (1.2 * x, y + 0.5, 1.2 * z), (1.3 * x, y + 1.0, 1.3 * z)]))
    groom = groomlab.create_groom(scalp)
    # Dimmed strands while sculpting (not the default since 0.34): what the stroke checks below
    # watch the dabs write.
    cmds.setAttr(groom + ".sculptDisplay", 1)
    cmds.setAttr(groom + ".sculptDensity", 0.35)
    groomlab.guides_from_curves(groom, curves, tube_radius=0.12)
    cmds.delete(curves)
    cmds.setAttr("persp.translate", 0.0, 1.4, 5.0)
    cmds.setAttr("persp.rotate", -8, 0, 0)
    cmds.select(cmds.listRelatives(groom, parent=True)[0])  # the transform, as a user would
    state["groom"] = groom
    state["ctx"] = groomlab.brush_tool("comb", radius=260, strength=1.0)
    later(step_partial_upload)


def step_partial_upload():
    # A stroke drawn as a drag writes only the strands each dab changed into the viewport's
    # buffers (the dimmed strands while it is live, the lit ones on release; under DirectX 11
    # into the buffers' system-memory copy, sent once per stream), and draws what writing every
    # strand again draws.
    groom, ctx = state["groom"], state["ctx"]
    groomlab = state["groomlab"]
    # The groom the brush tool is on: its stroke document was evaluated ahead, in idle time.
    check("the brush tool's groom has its stroke document ready before a stroke",
          json.loads(cmds.groomLabGraphStats(groom))["strokeDocument"])
    # The CPU's dabs (the GPU preview, on by default, draws a stroke's dabs itself).
    gpu = groomlab.gpu_preview()
    groomlab.gpu_preview(False)
    # Strands that keep their point counts (an even Resample), every one shown: a few thousand.
    graph = groomlab.get_graph(groom, expand_default=True)
    density = cmds.getAttr(groom + ".previewDensity")
    edited = json.loads(json.dumps(graph))
    for node in edited["nodes"]:
        if node["type"] == "Resample":
            node.setdefault("params", {})["mode"] = "even"
    groomlab.set_graph(groom, edited)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    folder = tempfile.mkdtemp(prefix="groomlab_brush_")

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, name + ".png")
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(480, 360), completeFilename=path)
        return path

    def rewrite():
        # Flat, then lit again: every strand written anew.
        cmds.setAttr(groom + ".displayLighting", False)
        cmds.refresh(currentView=True, force=True)
        cmds.setAttr(groom + ".displayLighting", True)
        cmds.refresh(currentView=True, force=True)

    # The pictures show the strands alone: their roots lie on the scalp, where a strand's depth
    # and the scalp's tie, and which of the two such a pixel shows is the order Viewport 2.0 draws
    # the items in. Under DirectX 11 that order can change when the strands' shader is made again
    # (the flat-then-lit rewrite) and stays changed for the session: a few root pixels then differ
    # with the buffers bit-identical (read back from the device).
    panel = persp_panel()
    meshes = cmds.modelEditor(panel, query=True, polymeshes=True)
    cmds.modelEditor(panel, edit=True, polymeshes=False)
    # Both pictures after a rewrite: under DirectX 11 a pixel or two of the first one drawn after
    # the item's first writing differ from any later one, stroke or not.
    rewrite()
    x0, y0 = to_view((0.0, 0.85, 0.5))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="draw " + " ".join("%d %d" % (x0 + 20 * k, y0 - 15) for k in range(8)))
    steps = json.loads(cmds.groomLabBrushCtx(ctx, query=True, debugStrokeDraws=True))
    dabs = [s["draw"]["items"].get("groomLabStrandsDim", {}).get("partialStrands", 0) for s in steps[1:-1]]
    release = steps[-1]["draw"]["items"].get("groomLabStrands", {}).get("partialStrands", 0)
    check("a drawn stroke writes the strands its dabs and its release changed alone (dabs %s, release %d)"
          % (dabs, release), dabs and min(dabs) > 0 and release > 0)
    stroked = shot("stroked")
    rewrite()
    whole = shot("whole")
    from PySide2 import QtGui
    a, b = QtGui.QImage(stroked), QtGui.QImage(whole)
    check("the strands written after the stroke draw as the strands written whole", not a.isNull() and a == b)
    cmds.modelEditor(panel, edit=True, polymeshes=meshes)
    groomlab.set_graph(groom, graph)
    cmds.setAttr(groom + ".previewDensity", density)
    groomlab.gpu_preview(gpu)
    # The brush put away: the stroke document goes (and the viewport's dimmed strands); taken up
    # again, it is made again.
    cmds.setToolTo("selectSuperContext")
    cmds.refresh(currentView=True, force=True)
    check("the brush put away, the groom lets its stroke document go",
          not json.loads(cmds.groomLabGraphStats(groom))["strokeDocument"])
    cmds.setToolTo(ctx)
    later(step_shape_wires)


def step_shape_wires():
    # The Shape brush: while it is on, the groom draws its guides' tubes as wires (every guide's,
    # none being selected), on OpenGL and DirectX 11 alike; a stroke paints the tubes near it, one
    # undo step; another brush and the wires go.
    groomlab, groom, ctx = state["groomlab"], state["groom"], state["ctx"]
    before = cyan_pixels("shape_off")
    groomlab.brush_tool("shape", radius=120, strength=1.0, tube_channel="width")
    on = cyan_pixels("shape_on")
    check("the Shape brush draws the tubes as wires (%d cyan pixels, %d before)" % (on, before), on > before + 40)
    data0 = cmds.getAttr(groom + ".guideData")
    x0, y0 = to_view((0.0, 0.85, 0.5))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (x0 + 10 * k, y0) for k in range(10)))
    painted = [g for g in cmds.groomLabGuides(groom, listGuides="primary")
               if len(groomlab.get_tube(groom, g)["width"]) > 2]
    check("a Shape stroke paints the tubes near it, a section per guide point (%d guides)" % len(painted), bool(painted))
    check("... with the wires still drawn after it", cyan_pixels("shape_stroked") > before + 40)
    cmds.undo()
    check("the Shape stroke is one undo step", cmds.getAttr(groom + ".guideData") == data0)
    # Ctrl shrinks the tubes instead (Ctrl is every brush's opposite).
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="ctrl " + " ".join("%d %d" % (x0 + 10 * k, y0) for k in range(10)))
    shrunk = [g for g in cmds.groomLabGuides(groom, listGuides="primary")
              if min(groomlab.get_tube(groom, g)["width"]) < 0.1 - 1e-4]
    check("Ctrl+Shape shrinks the tubes near it (%d guides), selecting none" % len(shrunk),
          bool(shrunk) and not groomlab.selected_guides(groom))
    cmds.undo()
    check("... one undo step too", cmds.getAttr(groom + ".guideData") == data0)
    # Every guide's tube draws while shaping; a selected guide's stands out (warm), the rest stay.
    guides = cmds.groomLabGuides(groom, listGuides="primary")
    warm0 = warm_pixels("shape_none_selected")
    groomlab.select_guides(groom, guides=[guides[0]])
    warm1 = warm_pixels("shape_one_selected")
    check("a selected guide's tube draws brighter (%d warm pixels, %d before)" % (warm1, warm0), warm1 > warm0 + 20)
    check("... and the other guides' tubes still draw", cyan_pixels("shape_one_selected_rest") > before + 40)
    groomlab.clear_guide_selection(groom)
    # Shift+drag smooths the channel it paints (width) along the guides under it, the guides kept.
    groomlab.set_tube(groom, group="primary", width=[0.1, 0.25, 0.1, 0.25, 0.1, 0.25, 0.1])
    points0 = [cmds.groomLabGuides(groom, guidePoints=g) for g in guides]

    def wiggle():  # how much the widths go up and down along the guides, all told
        total = 0.0
        for g in guides:
            w = groomlab.get_tube(groom, g)["width"]
            total += sum(abs(w[i + 1] - w[i]) for i in range(len(w) - 1))
        return total
    jagged = wiggle()
    # Shift with the Shape brush smooths the channel it paints (it selects with the other guide brushes).
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="shift " + " ".join("%d %d" % (x0 + 10 * k, y0) for k in range(10)))
    check("Shift+drag with the Shape brush smooths the tubes' width (%.3f, %.3f before), selecting none" % (
        wiggle(), jagged), wiggle() < 0.9 * jagged and not groomlab.selected_guides(groom))
    check("... and leaves the guides' shape", [cmds.groomLabGuides(groom, guidePoints=g) for g in guides] == points0)
    cmds.undo()
    cmds.undo()
    groomlab.brush_tool("comb", radius=260, strength=1.0)
    off = cyan_pixels("shape_put_away")
    check("another brush: the tube wires go (%d cyan pixels)" % off, off < before + 10)
    later(step_stroke)


def step_stroke():
    groom, ctx = state["groom"], state["ctx"]
    x0, y0 = to_view((0.0, 0.85, 0.5))  # front-top of the head
    state["data0"] = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (x0 + 20 * k, y0) for k in range(16)))
    state["data1"] = cmds.getAttr(groom + ".guideData")
    check("comb stroke changes the guides", state["data1"] != state["data0"])
    later(step_undo)


def step_undo():
    cmds.undo()
    check("one undo restores the guides from before the stroke", cmds.getAttr(state["groom"] + ".guideData") == state["data0"])
    later(step_redo)


def step_redo():
    cmds.redo()
    check("redo reapplies the stroke", cmds.getAttr(state["groom"] + ".guideData") == state["data1"])
    groom, ctx = state["groom"], state["ctx"]

    # A stroke pressed on guide tips sticking out past the head (the ray misses the scalp).
    state["groomlab"].brush_tool("comb", radius=60, strength=1.0)
    tips = []
    for gid in cmds.groomLabGuides(groom, listGuides="primary"):
        xyz = cmds.groomLabGuides(groom, guidePoints=gid)
        tips.append(tuple(xyz[-3:]))
    view = omui.M3dView.active3dView()
    top, right = view.portHeight() - 40, view.portWidth() - 40
    on_screen = [p for p in tips if 40 < to_view(p)[0] < right and 40 < to_view(p)[1] < top]
    tip = max(on_screen, key=lambda p: to_view(p)[1])  # the highest in view: past the head's outline
    tx, ty = to_view(tip)
    data = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (tx + 8 * k, ty) for k in range(10)))
    check("a stroke starting on hair off the scalp combs it", cmds.getAttr(groom + ".guideData") != data)

    # Pressing near (not on) a guide and barely moving must not make it jump.
    first = cmds.groomLabGuides(groom, listGuides="primary")[0]
    before = cmds.groomLabGuides(groom, guidePoints=first)
    near_x, near_y = to_view(before[-3:])
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d %d %d" % (near_x + 35, near_y, near_x + 36, near_y))
    after = cmds.groomLabGuides(groom, guidePoints=first)
    jump = max(abs(a - b) for a, b in zip(before, after))
    check("a 1-pixel drag started beside a guide barely moves it (%.3f)" % jump, jump < 0.03)

    # A stroke pressed over empty space starts once it reaches hair.
    data = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (tx - 200 + 20 * k, ty) for k in range(14)))
    check("a stroke from empty space starts when it reaches hair", cmds.getAttr(groom + ".guideData") != data)

    # Shift with a brush: the selection painted with the ring (each guide it passes over selected),
    # the rest frozen, one undo step; a Shift click adds the guides in the ring.
    gl = state["groomlab"]
    gl.clear_guide_selection(groom)
    data = cmds.getAttr(groom + ".guideData")
    selected = cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select " + " ".join("%d %d" % (tx - 200 + 20 * k, ty) for k in range(14)))
    painted = gl.selected_guides(groom)
    check("a Shift drag paints guides into the selection (%d)" % len(painted), len(painted) > 0 and selected == len(painted))
    check("... and freezes the rest (the guides change)", cmds.getAttr(groom + ".guideData") != data)
    cmds.undo()
    check("one undo takes the painted selection and the freeze back", not gl.selected_guides(groom) and
          cmds.getAttr(groom + ".guideData") == data)
    cmds.redo()
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select 5 5")
    import json as jsonlib  # (this step imports json later: the name is its local)
    still = [g for g in jsonlib.loads(cmds.getAttr(groom + ".guideData"))["guides"] if g.get("attributes", {}).get("freeze", 0.0) > 0.5]
    check("a Shift click on nothing deselects and unfreezes every guide (%d selected, %d frozen)"
          % (len(gl.selected_guides(groom)), len(still)), not gl.selected_guides(groom) and not still)
    gl.clear_guide_selection(groom)
    from groomlab.painting import unfreeze_all
    unfreeze_all(groom)  # (the strokes after it brush every guide)

    before = len(cmds.groomLabGuides(groom, listGuides="primary"))
    state["groomlab"].brush_tool("place", length=0.7)
    px, py = to_view((0.3, 0.3, 0.9))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d" % (px, py))
    check("place adds one guide", len(cmds.groomLabGuides(groom, listGuides="primary")) == before + 1)

    data = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="5 5 40 5")
    check("a stroke off the scalp changes nothing", cmds.getAttr(groom + ".guideData") == data)

    # Place Many: dragging keeps placing, spaced.
    before = len(cmds.groomLabGuides(groom, listGuides="primary"))
    state["groomlab"].brush_tool("place", spacing=0.12)
    ax, ay = to_view((-0.6, 0.55, 0.6))
    bx, by = to_view((0.6, 0.55, 0.6))
    drag = " ".join("%d %d" % (ax + (bx - ax) * k // 30, ay + (by - ay) * k // 30) for k in range(31))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=drag)
    added = len(cmds.groomLabGuides(groom, listGuides="primary")) - before
    check("place many adds several spaced guides (%d)" % added, 4 <= added <= 12)
    cmds.undo()
    check("place many is one undo step", len(cmds.groomLabGuides(groom, listGuides="primary")) == before)

    # Draw: press on the scalp and draw the guide upward (view y grows upward).
    state["groomlab"].brush_tool("draw", segments=8)
    rx, ry = to_view((0.0, 0.6, 0.8))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (rx + 3 * k, ry + 6 * k) for k in range(20)))
    ids = cmds.groomLabGuides(groom, listGuides="primary")
    check("draw adds one guide", len(ids) == before + 1)
    pts = cmds.groomLabGuides(groom, guidePoints=ids[-1])
    check("the drawn guide has the segment count", len(pts) == 27)
    check("the drawn guide rises from the scalp", pts[-2] > pts[1] + 0.1)

    # Comb with stretch: pulling a tip outward makes the guide longer.
    def total_length():
        total = 0.0
        for g in cmds.groomLabGuides(groom, listGuides="primary"):
            p = cmds.groomLabGuides(groom, guidePoints=g)
            pts = [p[i:i + 3] for i in range(0, len(p), 3)]
            total += sum(math.sqrt(sum((u - v) ** 2 for u, v in zip(a, b))) for a, b in zip(pts, pts[1:]))
        return total
    before = total_length()
    state["groomlab"].brush_tool("comb", radius=60, stretch=True)
    tip = cmds.groomLabGuides(groom, guidePoints=cmds.groomLabGuides(groom, listGuides="primary")[0])
    tx, ty = to_view(tip[-3:])  # a guide's tip, pulled on along the guide
    px, py = to_view(tip[-6:-3])
    dx, dy = tx - px, ty - py
    norm = max(math.hypot(dx, dy), 1e-6)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join(
        "%d %d" % (tx + 8 * k * dx / norm, ty + 8 * k * dy / norm) for k in range(12)))
    check("comb with stretch lengthens", total_length() > before + 0.05)
    cmds.undo()
    state["groomlab"].brush_tool("comb", radius=200, stretch=False)

    # Flow: a drag lays arrows on the scalp; the field draws.
    state["groomlab"].brush_tool("flow", radius=120)
    fx, fy = to_view((-0.5, 0.85, 0.5))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (fx + 12 * k, fy) for k in range(16)))
    check("flow lays arrows", len(state["groomlab"].flow_arrows(groom)) > 2)
    import json
    pins = json.loads(cmds.getAttr(groom + ".flowData"))["pins"]
    check("a flow stroke lays one line, in order",
          len({p.get("line") for p in pins}) == 1 and [p["seq"] for p in pins] == list(range(len(pins))))
    cmds.refresh(force=True)
    check("flow arrows draw", state["groomlab"].errors(groom) == [])
    cmds.undo()
    check("a flow stroke is one undo step", state["groomlab"].flow_arrows(groom) == [])

    # Slide: roots under the cursor move along the scalp.
    state["groomlab"].brush_tool("slide", radius=200, level=0)
    roots = [cmds.groomLabGuides(groom, guidePoints=g)[:3] for g in cmds.groomLabGuides(groom, listGuides="primary")]
    gx, gy = to_view((0.0, 0.85, 0.5))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (gx + 8 * k, gy) for k in range(12)))
    moved = [cmds.groomLabGuides(groom, guidePoints=g)[:3] for g in cmds.groomLabGuides(groom, listGuides="primary")]
    check("slide moves roots", moved != roots)
    cmds.undo()

    # Part: a freehand stroke across the scalp becomes a part on release, one undo step.
    groomlab = state["groomlab"]
    groomlab.draw_part_tool(groom, name="crown")
    ax, ay = to_view((-0.4, 0.85, 0.35))
    bx, by = to_view((0.4, 0.85, 0.35))
    stroke = " ".join("%d %d" % (ax + (bx - ax) * k // 20, ay + (by - ay) * k // 20 + (k % 3)) for k in range(21))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=stroke)
    check("a freehand stroke adds a part", groomlab.list_parts(groom) == ["crown"])
    check("the part tool adds a Part node", any(n["type"] == "Part" for n in groomlab.get_graph(groom)["nodes"]))
    cmds.undo()
    check("the part is one undo step", groomlab.list_parts(groom) == [])

    # Erasing: Part with Ctrl erases parts; the Erase brush takes parts and flow arrows too.
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=stroke)
    mx, my = (ax + bx) // 2, (ay + by) // 2
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="ctrl %d %d %d %d" % (mx, my - 30, mx, my + 30))
    check("part with Ctrl erases the part", groomlab.list_parts(groom) == [])
    cmds.undo()
    check("the part erase undoes", groomlab.list_parts(groom) == ["crown"])
    groomlab.brush_tool("flow", radius=120)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (fx + 12 * k, fy) for k in range(16)))
    arrows = len(groomlab.flow_arrows(groom))
    guides = len(cmds.groomLabGuides(groom, listGuides="primary"))
    groomlab.brush_tool("erase", radius=60, erase_guides=False)
    wipe = " ".join("%d %d" % (mx + (fx + 90 - mx) * k // 12, my + (fy - my) * k // 12) for k in range(13))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d %d %d " % (mx, my - 40, mx, my + 40) + wipe)
    check("erase takes the part", groomlab.list_parts(groom) == [])
    check("erase takes flow arrows", len(groomlab.flow_arrows(groom)) < arrows)
    check("erase with Guides off keeps guides", len(cmds.groomLabGuides(groom, listGuides="primary")) == guides)
    cmds.undo()
    check("one erase is one undo step",
          groomlab.list_parts(groom) == ["crown"] and len(groomlab.flow_arrows(groom)) == arrows)
    cmds.undo()
    cmds.undo()

    # Pinned groom: the brush keeps working on it with something else selected.
    groomlab.pin_groom(groom)
    cmds.select(cmds.polyCube(name="notTheGroom")[0])
    groomlab.brush_tool("comb", radius=200)
    data = cmds.getAttr(groom + ".guideData")
    gx, gy = to_view((0.0, 0.85, 0.5))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (gx + 10 * k, gy) for k in range(10)))
    check("a pinned groom is brushed whatever is selected", cmds.getAttr(groom + ".guideData") != data)
    check("active_groom follows the pin", groomlab.active_groom() == groom)
    # Another groom selected since: it is the current groom, and the brush works on it (not the
    # picked one) as the Groom Tools and the Graph Editor show it.
    other = groomlab.create_groom(cmds.polySphere(name="otherScalp", radius=0.2)[0], name="otherGroom")
    cmds.select(other)
    data = cmds.getAttr(groom + ".guideData")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (gx + 10 * k, gy) for k in range(10)))
    check("a groom selected after the pick is brushed instead", cmds.getAttr(groom + ".guideData") == data)
    check("active_groom follows the selected groom", groomlab.active_groom() == other)
    groomlab.pin_groom(None)
    cmds.delete(cmds.listRelatives(other, parent=True), "otherScalp")
    cmds.delete("notTheGroom")
    cmds.select(cmds.listRelatives(groom, parent=True)[0])

    # Display: isolate a group, color by layer, frozen guides, sculpt display (drawn without errors).
    groomlab.isolate_group(groom, "primary")
    groomlab.add_map(groom, "fur", 0.5)
    groomlab.color_guides_by_map(groom, "fur")
    groomlab.set_sculpt_display(groom, "guidesOnly", 0.2)
    groomlab.freeze_guides(groom, guides=cmds.groomLabGuides(groom, listGuides="primary")[:2])
    cmds.refresh(force=True)
    check("display options draw", cmds.getAttr(groom + ".isolateGroup") == "primary")
    frozen = cmds.groomLabGuides(groom, listGuides="primary")[0]
    before = cmds.groomLabGuides(groom, guidePoints=frozen)
    groomlab.brush_tool("comb", radius=2000, level=0)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (gx + 15 * k, gy) for k in range(12)))
    check("frozen guides are left alone", cmds.groomLabGuides(groom, guidePoints=frozen) == before)
    groomlab.unfreeze_all(groom)
    groomlab.isolate_group(groom, None)
    groomlab.color_guides_by_map(groom, None)

    # Scalp maps: the Paint brush on a vertex map and on a texture map, painted where the cursor
    # hits the scalp and shown there, one undo step per stroke (the texture's file included).
    import time
    groomlab.add_map(groom, "crown", 0.0, kind="vertices")
    groomlab.paint_map(groom, "crown", value=1.0, strength=1.0, radius=50)
    check("painting a scalp map shows it on the scalp", cmds.getAttr(groom + ".scalpMapDisplay") == "crown")
    sx, sy = to_view((0.0, 0.85, 0.53))  # on the scalp, up and toward the camera
    cmds.refresh(force=True)  # the map item draws (per-vertex gray)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    values = cmds.groomLabScalpMaps(groom, values="crown")
    check("a stroke paints the vertices under the cursor", max(values) > 0.5 and min(values) == 0.0)
    cmds.undo()
    check("a scalp map stroke undoes", max(cmds.groomLabScalpMaps(groom, values="crown")) == 0.0)
    cmds.redo()
    check("and redoes", max(cmds.groomLabScalpMaps(groom, values="crown")) > 0.5)
    painted = cmds.groomLabScalpMaps(groom, values="crown")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="shift " + " ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    smoothed = cmds.groomLabScalpMaps(groom, values="crown")
    say("smoothing: max %.3f -> %.3f, sum %.2f -> %.2f, same %s" % (max(painted), max(smoothed), sum(painted), sum(smoothed), smoothed == painted))
    check("Shift+drag smooths the map", smoothed != painted and max(smoothed) < max(painted)
          and sum(smoothed) < 1.25 * sum(painted))  # a blur: the peak spreads out, nothing is painted
    check("the Paint brush is back after smoothing", cmds.groomLabBrushCtx(ctx, query=True, mode=True) == "paint")
    # An alpha: the same stroke on a fresh map paints the Spots alpha's blobs, stamps spaced
    # along it, far fewer vertices than the round brush.
    speckle = next(a["path"] for a in groomlab.brush_alphas() if a["name"] == "Spots")
    groomlab.add_map(groom, "speckled", 0.0, kind="vertices")
    groomlab.paint_map(groom, "speckled", value=1.0, strength=1.0, radius=50)
    cmds.groomLabBrushCtx(ctx, edit=True, alpha=speckle, alphaSpacing=0.5, hardness=1.0)
    check("the tool takes the alpha", os.path.normcase(cmds.groomLabBrushCtx(ctx, query=True, alpha=True)) == os.path.normcase(speckle))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    speckled = cmds.groomLabScalpMaps(groom, values="speckled")
    dotted = sum(1 for v in speckled if v > 0.01)
    cmds.groomLabBrushCtx(ctx, edit=True, alpha="")
    groomlab.add_map(groom, "round", 0.0, kind="vertices")
    groomlab.paint_map(groom, "round", value=1.0, strength=1.0, radius=50)
    cmds.groomLabBrushCtx(ctx, edit=True, hardness=1.0)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    round_count = sum(1 for v in cmds.groomLabScalpMaps(groom, values="round") if v > 0.01)
    say("alpha stroke: %d vertices painted, round brush %d" % (dotted, round_count))
    check("an alpha stroke paints its pattern", 0 < dotted < 0.8 * round_count)
    groomlab.add_map(groom, "detail", 0.0, kind="texture", resolution=128)
    path = groomlab.map_texture(groom, "detail")
    size_before = os.path.getsize(path)
    time.sleep(0.05)
    groomlab.paint_map(groom, "detail", value=1.0, strength=1.0, radius=50)
    cmds.refresh(force=True)  # the texture drawn on the scalp through the card effect
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    check("a stroke rewrites the texture map's file", os.path.exists(path) and cmds.getAttr(groom + ".brushTick") > 0)
    from maya.api import OpenMaya as om2
    image = om2.MImage()
    image.readFromFile(path)
    pixels = bytes(image.pixels()) if not isinstance(image.pixels(), int) else b""
    check("the file holds the stroke", (max(pixels) if pixels else 255) > 128)
    # Shift+drag smooths the texture map too, and the Paint brush stays on.
    with open(path, "rb") as f:
        stroked = f.read()
    tool = cmds.currentCtx()
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="shift " + " ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    with open(path, "rb") as f:
        smoothed_file = f.read()
    say("texture smoothing: tool %s -> %s, mode %s, file changed %s, errors %s" % (
        tool, cmds.currentCtx(), cmds.groomLabBrushCtx(ctx, query=True, mode=True), smoothed_file != stroked,
        groomlab.errors(groom)))
    check("Shift+drag smooths a texture map", smoothed_file != stroked)
    check("the Paint brush stays on after smoothing a texture",
          cmds.currentCtx() == tool and cmds.groomLabBrushCtx(ctx, query=True, mode=True) == "paint")
    cmds.undo()  # the smoothing
    cmds.undo()
    # What a texture stroke costs with the scalp masking the brush and without (reported only).
    groomlab.add_map(groom, "bigTex", 0.5, kind="texture", resolution=1024)
    groomlab.paint_map(groom, "bigTex", value=1.0, strength=1.0, radius=150)
    sweep = "shift " + " ".join("%d %d" % (sx - 60 + 6 * k, sy + (k % 5) * 6) for k in range(24))
    timings = {}
    for masked in (True, False):
        cmds.groomLabBrushCtx(ctx, edit=True, maskByScalp=masked)
        start = time.time()
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=sweep)
        timings[masked] = time.time() - start
        cmds.undo()
    cmds.groomLabBrushCtx(ctx, edit=True, maskByScalp=True)
    say("[ budget ] texture smoothing, 24 dabs at radius 150 on 1024x1024: masked by the scalp %.0f ms, "
        "unmasked %.0f ms (reported only)" % (timings[True] * 1000, timings[False] * 1000))
    groomlab.remove_map(groom, "bigTex")
    import hashlib

    def digest(p):
        with open(p, "rb") as f:
            return hashlib.md5(f.read()).hexdigest()

    # What a texture stroke's press and release cost on a big color map (its file's bytes kept for
    # undo), and a dab on a vertex map (reported only).
    groomlab.add_map(groom, "hugeTex", kind="texture", resolution=2048, color=(0.0, 0.0, 0.0, 1.0))
    huge_path = groomlab.map_texture(groom, "hugeTex")
    huge_before = digest(huge_path)
    groomlab.paint_map(groom, "hugeTex", strength=1.0, radius=40, color=(1.0, 0.5, 0.0, 1.0))
    click = "%d %d %d %d" % (sx, sy, sx + 2, sy)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=click)  # the first opens the map (its table over the scalp)
    huge = []
    held = cmds.groomLabBrushCtx(ctx, query=True, heldUndoBytes=True)
    for k in range(3):
        start = time.time()
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d %d %d" % (sx, sy + 6 * k, sx + 2, sy + 6 * k))
        huge.append(time.time() - start)
    held_2048 = (cmds.groomLabBrushCtx(ctx, query=True, heldUndoBytes=True) - held) / 3.0
    file_2048 = os.path.getsize(huge_path)
    for _ in range(4):
        cmds.undo()
    check("2048x2048 color texture strokes undo (the file as it was)", digest(huge_path) == huge_before)
    groomlab.remove_map(groom, "hugeTex")
    groomlab.paint_map(groom, "crown", value=1.0, strength=1.0, radius=50)
    dabs = 24
    start = time.time()
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx - 60 + 5 * k, sy) for k in range(dabs)))
    vertex_stroke = time.time() - start
    cmds.undo()
    say("[ budget ] texture stroke press+release on a 2048x2048 color map: %s ms, undo %.2f MB a stroke (the file "
        "is %.1f MB); vertex map dab: %.1f ms (%d dabs in %.0f ms, reported only)" % (
            ", ".join("%.0f" % (t * 1000) for t in huge), held_2048 / 1e6, file_2048 / 1e6,
            vertex_stroke * 1000 / dabs, dabs, vertex_stroke * 1000))

    # Undo of texture strokes whose bytes before come from the stroke before (not read at the
    # press), and from the file when it changed in between (written outside the brush).
    groomlab.add_map(groom, "lazyTex", 0.0, kind="texture", resolution=128)
    lazy_path = groomlab.map_texture(groom, "lazyTex")
    lazy = [digest(lazy_path)]
    groomlab.paint_map(groom, "lazyTex", value=1.0, strength=1.0, radius=50)
    for k in range(2):
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * j, sy + 15 * k) for j in range(6)))
        lazy.append(digest(lazy_path))
    check("two texture strokes each change the file", len(set(lazy)) == 3)
    cmds.undo()
    check("undoing the second stroke gives the first stroke's file", digest(lazy_path) == lazy[1])
    cmds.undo()
    check("undoing the first gives the file before both", digest(lazy_path) == lazy[0])
    cmds.redo()
    cmds.redo()
    check("redoing both gives the second stroke's file", digest(lazy_path) == lazy[2])
    from PySide2 import QtGui as _qg
    outside = _qg.QImage(128, 128, _qg.QImage.Format_Grayscale8)
    outside.fill(77)
    time.sleep(0.05)
    check("the file is written outside the brush", outside.save(lazy_path, "PNG"))
    written = digest(lazy_path)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * j, sy + 30) for j in range(6)))
    check("a stroke after the file changed outside paints over the new file",
          digest(lazy_path) != written and _qg.QImage(lazy_path).pixelColor(0, 0).red() in (0, 77))
    cmds.undo()
    check("undone, the file is as written outside", digest(lazy_path) == written)
    groomlab.remove_map(groom, "lazyTex")

    # Undo and redo by tiles: strokes after strokes, undone and redone in turn; a stroke after an
    # undo; a scripted stroke between interactive ones. Each step gives the file it had.
    groomlab.add_map(groom, "chainTex", kind="texture", resolution=256, color=(0.0, 0.0, 0.0, 1.0))
    chain_path = groomlab.map_texture(groom, "chainTex")
    groomlab.paint_map(groom, "chainTex", strength=0.6, radius=40, color=(1.0, 0.5, 0.25, 1.0))
    files = [digest(chain_path)]
    for k in range(4):
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx - 20 + 6 * j, sy + 9 * k) for j in range(7)))
        files.append(digest(chain_path))
    check("four texture strokes each change the file", len(set(files)) == 5)
    undone = []
    for k in range(4):
        cmds.undo()
        undone.append(digest(chain_path) == files[3 - k])
    check("undoing them one by one gives each file before", all(undone))
    redone = []
    for k in range(4):
        cmds.redo()
        redone.append(digest(chain_path) == files[k + 1])
    check("redoing them gives each file after", all(redone))
    cmds.undo()
    cmds.undo()
    check("two undone", digest(chain_path) == files[2])
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 10 + 5 * j, sy - 12) for j in range(7)))
    after_undo = digest(chain_path)
    check("a stroke after an undo paints on the file as it was", after_undo not in files)
    point = groomlab.scalp_mesh(groom)
    cmds.groomLabBrushStroke(groom, mode="paint", attribute="chainTex", paintKind="texture", radius=0.15, strength=1.0,
                             paintColor=(0.0, 1.0, 0.0, 1.0), point=[(0.1, 0.85, 0.5), (0.2, 0.8, 0.55)])
    scripted = digest(chain_path)
    check("a scripted stroke between them changes the file", scripted != after_undo)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx - 30 + 5 * j, sy + 20) for j in range(7)))
    last = digest(chain_path)
    steps = []
    for want in (scripted, after_undo, files[2], files[1], files[0]):
        cmds.undo()
        steps.append(digest(chain_path) == want)
    check("undoing through a scripted stroke and a stroke after an undo gives each file (%s)" % steps, all(steps))
    steps = []
    for want in (files[1], files[2], after_undo, scripted, last):
        cmds.redo()
        steps.append(digest(chain_path) == want)
    check("and redoing them gives each file again (%s)" % steps, all(steps))
    groomlab.remove_map(groom, "chainTex")

    groomlab.paint_map(groom, "detail", value=1.0, strength=1.0, radius=50)
    check("a texture stroke undoes (the file as it was)", os.path.getsize(path) == size_before)
    cmds.refresh(force=True)
    # Another groom made active while the tool still names this groom's file (-paintTexture):
    # a duplicate, sharing the file. Its strokes go to its own copy, never into this groom's file.
    shared = digest(path)
    copy = cmds.duplicate(cmds.listRelatives(groom, parent=True, fullPath=True)[0])[0]
    copy = cmds.listRelatives(copy, shapes=True, fullPath=True, type=groomlab.GROOM_TYPE)[0]
    cmds.connectAttr(groomlab.scalp_mesh(groom) + ".worldMesh[0]", copy + ".inMesh")
    cmds.connectAttr("time1.outTime", copy + ".time")
    check("the duplicate shares the texture map's file", groomlab.map_texture(copy, "detail") == path)
    groomlab.pin_groom(copy)
    check("the tool still names the first groom's file",
          cmds.groomLabBrushCtx(ctx, query=True, paintTexture=True) ==
          next(m["texture"] for m in groomlab.maps(groom) if m["name"] == "detail"))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    forked = groomlab.map_texture(copy, "detail")
    check("a stroke on the active duplicate forks the shared file", forked != path and os.path.exists(forked))
    check("the first groom's file is untouched", digest(path) == shared)
    before_fork = digest(forked)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy + 20) for k in range(8)))
    check("the next stroke paints the active groom's own file", digest(forked) != before_fork and digest(path) == shared)
    cmds.undo()
    cmds.undo()
    check("undone, the duplicate reads the shared file again", groomlab.map_texture(copy, "detail") == path)
    groomlab.pin_groom(None)
    cmds.delete(cmds.listRelatives(copy, parent=True, fullPath=True))
    cmds.select(cmds.listRelatives(groom, parent=True)[0])
    # Color maps: the interactive brush paints the color into the ticked channels only (the
    # vertex map previewed through evaluation, the texture as an RGBA image), Ctrl toward the
    # inverse color there; the scalp draws them in color or one channel.
    groomlab.add_map(groom, "tint", kind="vertices", color=(0.0, 0.25, 0.0, 1.0))
    groomlab.paint_map(groom, "tint", strength=1.0, radius=50, color=(1.0, 0.0, 0.0, 1.0), channels="r")
    cmds.refresh(force=True)  # the map item draws per-vertex color
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    tint = cmds.groomLabScalpMaps(groom, values="tint")
    check("a stroke paints the color into the ticked channel",
          max(tint[0::4]) > 0.5 and set(round(v, 3) for v in tint[1::4]) == {0.25})
    cmds.refresh(force=True)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="ctrl " + " ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    inverted = cmds.groomLabScalpMaps(groom, values="tint")
    check("Ctrl paints toward the inverse color in the ticked channel",
          max(inverted[0::4]) < max(tint[0::4]) and set(round(v, 3) for v in inverted[1::4]) == {0.25})
    cmds.undo()
    groomlab.show_map_on_scalp(groom, "tint", channel="g")
    cmds.refresh(force=True)
    groomlab.show_map_on_scalp(groom, "tint", channel="rgb")
    groomlab.add_map(groom, "tintTex", kind="texture", resolution=128, color=(0.0, 0.0, 0.0, 1.0))
    tint_path = groomlab.map_texture(groom, "tintTex")
    tint_before = digest(tint_path)
    time.sleep(0.05)
    groomlab.paint_map(groom, "tintTex", strength=1.0, radius=50, color=(0.0, 0.0, 1.0, 1.0), channels="b")
    cmds.refresh(force=True)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 5 * k, sy) for k in range(8)))
    cmds.refresh(force=True)  # the rewritten RGBA file drawn on the scalp
    from PySide2 import QtGui
    picture = QtGui.QImage(tint_path)
    reds, blues = [], []
    for y in range(0, picture.height(), 2):
        for x in range(0, picture.width(), 2):
            c = picture.pixelColor(x, y)
            reds.append(c.red())
            blues.append(c.blue())
    say("color texture: %dx%d, max red %d, max blue %d" % (picture.width(), picture.height(), max(reds or [0]),
                                                           max(blues or [0])))
    check("a color texture stroke paints blue, nothing red", bool(blues) and max(blues) > 128 and max(reds) == 0)
    # The scalp showed the rewritten file from the texture cache the stroke primed, the same
    # image as reading the file (checked against MImage the first time).
    source = cmds.groomLabBrushCtx(ctx, query=True, fileImage=True)
    check("the scalp shows a painted file from the texture cache (%s)" % source, source == "cache")
    cmds.undo()
    check("a color texture stroke undoes", digest(tint_path) == tint_before)
    groomlab.show_map_on_scalp(groom, None)
    cmds.refresh(force=True)

    # Hotkeys: runtime commands, the marking menu, the hotkey set.
    from groomlab.ui import hotkeys
    hotkeys.install()
    check("runtime commands exist", cmds.runTimeCommand("GroomLabComb", exists=True))
    check("the GroomLab hotkey set is current", cmds.hotkeySet(query=True, current=True) == "GroomLab")
    hotkeys.marking_menu_press()
    check("the marking menu builds", cmds.popupMenu(hotkeys.MARKING_MENU, exists=True))
    hotkeys.marking_menu_release()
    hotkeys.tool("cut")
    check("a hotkey starts a tool", cmds.groomLabBrushCtx(ctx, query=True, mode=True) == "cut")
    # B / M held: a left drag sets the brush radius / strength (Maya's own B elsewhere).
    check("B is bound to the brush radius drag",
          cmds.hotkey("b", query=True, name=True) == "GroomLabBrushRadiusPressNameCommand")
    # Symmetry on Alt+Shift+X (free in Maya's own set); Alt+X stays Maya's Walk Tool.
    check("Alt+Shift+X is bound to the symmetry toggle",
          cmds.hotkey("X", altModifier=True, query=True, name=True) == "GroomLabToggleSymmetryNameCommand")
    check("Alt+X is not GroomLab's (Maya's Walk Tool)",
          cmds.hotkey("x", altModifier=True, query=True, name=True) != "GroomLabToggleSymmetryNameCommand")
    check("Alt+Shift+G is bound to every groom's guides on or off",
          cmds.hotkey("G", altModifier=True, query=True, name=True) == "GroomLabToggleAllGuidesNameCommand")
    shown = groomlab.guides_visible(groom)
    hotkeys.toggle_all_guides()
    check("Alt+Shift+G hides every groom's guides", not groomlab.guides_visible(groom))
    hotkeys.toggle_all_guides()
    check("Alt+Shift+G again shows them", groomlab.guides_visible(groom))
    groomlab.set_guides_visible(groom, shown)
    hotkeys.drag_slider("strength", "ModifyDisplacementPress")
    check("M held: the brush drags its strength", cmds.groomLabBrushCtx(ctx, query=True, dragSlider=True) == "strength")
    hotkeys.drag_slider(None, "ModifyDisplacementRelease")
    check("M released", cmds.groomLabBrushCtx(ctx, query=True, dragSlider=True) == "none")

    # Picking a level (debugPick): the level of the guide under the cursor.
    tip = cmds.groomLabGuides(groom, guidePoints=cmds.groomLabGuides(groom, listGuides="primary")[-1])[-3:]
    cmds.groomLabBrushCtx(ctx, edit=True, level=3)
    px, py = to_view(tuple(tip))
    check("a pick on a primary picks level 0",
          cmds.groomLabBrushCtx(ctx, edit=True, debugPick="%d %d" % (px, py)) == 0
          and cmds.groomLabBrushCtx(ctx, query=True, level=True) == 0)
    cmds.groomLabBrushCtx(ctx, edit=True, level=2)
    check("a pick on nothing keeps the level",
          cmds.groomLabBrushCtx(ctx, edit=True, debugPick="2 2") == -2
          and cmds.groomLabBrushCtx(ctx, query=True, level=True) == 2)

    # Shot sculpting: a stroke becomes a corrective on this frame; the authored guides stay.
    from groomlab import shots
    groomlab.brush_tool("comb", radius=200, level=0)
    cmds.currentTime(10)
    layer = shots.shot_sculpt(groom, all_frames=False)
    authored = cmds.getAttr(groom + ".guideData")
    primaries = cmds.groomLabGuides(groom, listGuides="primary")

    def shown(frame):
        cmds.currentTime(frame)
        return [cmds.groomLabGuides(groom, guidePoints=g, evaluated=True) for g in primaries]

    rest = [cmds.groomLabGuides(groom, guidePoints=g) for g in primaries]
    sx, sy = to_view((0.0, 0.85, 0.5))
    cmds.currentTime(10)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 12 * k, sy) for k in range(12)))
    check("a shot stroke leaves the authored guides alone", cmds.getAttr(groom + ".guideData") == authored)
    recorded = [l for l in shots.layers(groom) if l["name"] == layer]
    check("a shot stroke records a corrective layer", bool(recorded) and recorded[0]["guides"] > 0)
    cmds.undo()
    check("the shot stroke is one undo step", all(l["guides"] == 0 for l in shots.layers(groom)))
    cmds.redo()
    check("the corrected groom shows on its frame", shown(10) != rest)
    check("the correction fades out away from its frame", shown(40) == rest)

    # Sculpt layers on every frame: a second look on top, switched on and off.
    sculpt = shots.add_layer(groom, all_frames=True)
    cmds.currentTime(40)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx - 12 * k, sy) for k in range(12)))
    look = shown(40)
    check("an all-frames layer shows on any frame", look != rest and shown(70) == look)
    shots.set_layer(groom, sculpt, enabled=False)
    check("switching a layer off hides its sculpt", shown(40) == rest)
    shots.solo_layer(groom, sculpt)
    check("solo shows only that layer", shown(40) == look and shown(10) == look)
    shots.shot_sculpt(groom, False)
    cmds.setAttr(groom + ".correctiveData", "", type="string")

    # Erase: press over guides.
    state["groomlab"].brush_tool("erase", radius=120)
    before = len(cmds.groomLabGuides(groom, listGuides="primary"))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d" % to_view((0.0, 0.6, 0.8)))
    check("erase removes guides", len(cmds.groomLabGuides(groom, listGuides="primary")) < before)

    state["groomlab"].brush_tool("smooth", radius=123, strength=0.4, level=1)
    mel.eval("toolPropertyWindow")
    later(step_settings)


def step_settings():
    state["tries"] = state.get("tries", 0) + 1
    if not cmds.control("groomLabBrushOpenTools", exists=True) and state["tries"] < 20:
        later(step_settings)  # the panel builds on idle
        return
    check("tool settings point to the Groom Tools", cmds.control("groomLabBrushOpenTools", exists=True))
    cmds.select(state["groom"])
    mel.eval("openAEWindow")
    later(step_attribute_editor)


def step_attribute_editor():
    state["ae_tries"] = state.get("ae_tries", 0) + 1
    if not cmds.layout("groomLabAEButtons", exists=True) and state["ae_tries"] < 30:
        later(step_attribute_editor)  # the Attribute Editor builds on idle
        return
    check("the Attribute Editor uses the GroomLab template", cmds.layout("groomLabAEButtons", exists=True))
    later(step_mask_by_scalp)


def step_mask_by_scalp():
    # A guide on the front of a head and one straight behind it on the back: a comb stroke over
    # the face reaches the front guide only; with the mask off it reaches through the head.
    groomlab = state["groomlab"]
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    front = cmds.curve(degree=1, point=[(0.0, 0.1, 1.0), (0.0, 0.1, 1.4), (0.0, 0.1, 1.8)])
    back = cmds.curve(degree=1, point=[(0.0, 0.1, -1.0), (0.0, 0.1, -1.4), (0.0, 0.1, -1.8)])
    groom = groomlab.create_groom(scalp)
    ids = groomlab.guides_from_curves(groom, [front, back], tube_radius=0.1)
    cmds.delete(front, back)
    cmds.setAttr("persp.translate", 0.0, 0.1, 6.0)
    cmds.setAttr("persp.rotate", 0, 0, 0)
    cmds.select(cmds.listRelatives(groom, parent=True)[0])
    ctx = groomlab.brush_tool("comb", radius=200, strength=1.0)
    cx, cy = to_view((0.0, 0.1, 1.0))
    stroke = " ".join("%d %d" % (cx - 90 + 15 * k, cy) for k in range(13))
    points = lambda gid: cmds.groomLabGuides(groom, guidePoints=str(gid))
    before = {gid: points(gid) for gid in ids}
    check("mask by scalp is on by default", cmds.groomLabBrushCtx(ctx, query=True, maskByScalp=True))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=stroke)
    check("the stroke combs the guide in front", points(ids[0]) != before[ids[0]])
    check("the guide behind the head is left alone", points(ids[1]) == before[ids[1]])
    cmds.undo()
    cmds.groomLabBrushCtx(ctx, edit=True, maskByScalp=False)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=stroke)
    check("unmasked, the stroke reaches through the head", points(ids[1]) != before[ids[1]])
    later(step_paint_across_seam)


def step_paint_across_seam():
    # A texture map on a polySphere, whose UVs are cut along a meridian (u = 0 | 1): white on the
    # u < 0.5 half, black on the other, so the map steps at the seam and at u = 0.5 (no seam
    # there). Shift+drag smoothing across each blurs both alike: the seam is no line on the
    # surface. Then a paint stroke across the seam paints both sides.
    import tempfile
    from maya.api import OpenMaya as om2
    from PySide2 import QtGui
    groomlab = state["groomlab"]
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    groom = groomlab.create_groom(scalp)
    size = 256
    folder = tempfile.mkdtemp(prefix="groomlab_seam_").replace("\\", "/")
    groomlab.add_map(groom, "seamTex", kind="texture", resolution=size, texture=folder + "/seam.png")
    path = groomlab.map_texture(groom, "seamTex")
    image = QtGui.QImage(size, size, QtGui.QImage.Format_Grayscale8)
    for x in range(size):
        shade = QtGui.QColor(255, 255, 255) if x < size // 2 else QtGui.QColor(0, 0, 0)
        for y in range(size):
            image.setPixelColor(x, y, shade)
    check("the seam test's map file is written", image.save(path, "PNG"))  # the next stroke reads it (its stamp changed)
    mesh = om2.MFnMesh(om2.MGlobal.getSelectionListByName(cmds.listRelatives(scalp, shapes=True)[0]).getDagPath(0))

    def meridian_point(u_wanted, y=0.3):
        # The point at height y where the map's u is u_wanted (0: the seam), by the angle round y.
        best = None
        for k in range(720):
            a = 2 * math.pi * k / 720
            r = math.sqrt(1 - y * y)
            p = om2.MPoint(r * math.cos(a), y, r * math.sin(a))
            closest = mesh.getClosestPoint(p, om2.MSpace.kWorld)[0]
            u = mesh.getUVAtPoint(closest, om2.MSpace.kWorld)[0]
            d = min(abs(u - u_wanted), 1 - abs(u - u_wanted))
            if best is None or d < best[0]:
                best = (d, a)
        return best[1]

    def value_at(angle, y=0.3):
        r = math.sqrt(1 - y * y)
        p = mesh.getClosestPoint(om2.MPoint(r * math.cos(angle), y, r * math.sin(angle)), om2.MSpace.kWorld)[0]
        u, v = mesh.getUVAtPoint(p, om2.MSpace.kWorld)[:2]
        picture = QtGui.QImage(path)
        x = min(max(int(u * picture.width()), 0), picture.width() - 1)
        row = min(max(int((1 - v) * picture.height()), 0), picture.height() - 1)
        return picture.pixelColor(x, row).red()

    def look_at(angle, y=0.3):
        r = math.sqrt(1 - y * y)
        target = (r * math.cos(angle), y, r * math.sin(angle))
        cmds.viewPlace("perspShape", eye=(4 * target[0], 4 * target[1], 4 * target[2]), lookAt=target, up=(0, 1, 0))
        cmds.refresh(force=True)
        return to_view(target)

    seam, middle = meridian_point(0.0), meridian_point(0.5)
    step = 2 * math.pi / 40 * 0.35  # about a third of a polygon either side
    say("seam at %.1f degrees, u = 0.5 at %.1f degrees" % (math.degrees(seam), math.degrees(middle)))
    before = [value_at(seam - step), value_at(seam + step), value_at(middle - step), value_at(middle + step)]
    check("the map steps at the seam and at u = 0.5 (%s)" % before,
          abs(before[0] - before[1]) > 200 and abs(before[2] - before[3]) > 200)
    ctx = groomlab.paint_map(groom, "seamTex", value=1.0, strength=1.0, radius=60, smooth_strength=6.0)
    for angle in (seam, middle):
        sx, sy = look_at(angle)
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="shift " + " ".join("%d %d" % (sx + 4 * k - 16, sy) for k in range(9)))
    after = [value_at(seam - step), value_at(seam + step), value_at(middle - step), value_at(middle + step)]
    across_seam, across_middle = abs(after[0] - after[1]), abs(after[2] - after[3])
    say("smoothed across the seam: %d -> %d; across u = 0.5: %d -> %d (%s)" % (
        abs(before[0] - before[1]), across_seam, abs(before[2] - before[3]), across_middle, after))
    check("smoothing blurs the step across the seam", across_seam < 160)
    check("smoothing blurs across the seam as across no seam", abs(across_seam - across_middle) < 60)
    check("no error from seam smoothing", not groomlab.errors(groom))
    groomlab.fill_map(groom, "seamTex", value=0.0)
    state["seam"] = (groom, seam, step, look_at, value_at)
    later(step_stroke_across_seam)


def step_stroke_across_seam():
    # A stroke across the seam paints both sides of it, and undoes on both.
    groom, seam, step, look_at, value_at = state.pop("seam")
    sx, sy = look_at(seam)
    ctx = state["groomlab"].paint_map(groom, "seamTex", value=1.0, strength=1.0, radius=40)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx + 6 * k - 24, sy) for k in range(9)))
    painted = [value_at(seam - step), value_at(seam + step)]
    check("a stroke across the seam paints both sides (%s)" % painted, min(painted) > 128)
    cmds.undo()
    undone = [value_at(seam - step), value_at(seam + step)]
    check("and undoes on both sides (%s)" % undone, max(undone) < 8)
    later(step_brush_pack)


def step_brush_pack():
    # The brush pack: + New Map > For a setting > Density (the Paint tab) makes a white map, links
    # it to Scatter's density and turns the Paint brush on it, one undo step; Ctrl paints hair
    # away; Direction turns guides; the curves and topological symmetry reach the context; the
    # Groom Tools show the new tiles (and no Width or Density tile).
    groomlab = state["groomlab"]
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=20)[0]
    curves = []
    for i in range(8):
        a = 2 * math.pi * i / 8
        x, z = 0.5 * math.cos(a), 0.5 * math.sin(a)
        y = math.sqrt(1 - 0.25)
        curves.append(cmds.curve(degree=2, point=[(x, y, z), (1.4 * x, y + 0.4, 1.4 * z), (1.6 * x, y + 0.8, 1.6 * z)]))
    groom = groomlab.create_groom(scalp)
    groomlab.guides_from_curves(groom, curves, tube_radius=0.12)
    cmds.delete(curves)
    cmds.setAttr("persp.translate", 0.0, 1.4, 5.0)
    cmds.setAttr("persp.rotate", -8, 0, 0)
    cmds.select(cmds.listRelatives(groom, parent=True)[0])

    # Direction: guides turn toward the stroke, one undo step.
    data0 = cmds.getAttr(groom + ".guideData")
    ctx = groomlab.brush_tool("direction", radius=200, strength=1.0, strength_profile=[[0, 0.5], [1, 1]],
                              falloff_profile=[[0, 1], [1, 0]], symmetry="topology")
    check("the curves reach the context",
          json.loads(cmds.groomLabBrushCtx(ctx, query=True, strengthProfile=True)) == [[0, 0.5], [1, 1]]
          and json.loads(cmds.groomLabBrushCtx(ctx, query=True, falloffProfile=True)) == [[0, 1], [1, 0]])
    check("topological symmetry reaches the context", cmds.groomLabBrushCtx(ctx, query=True, symmetry=True) == "topology")
    x0, y0 = to_view((0.3, 0.85, 0.4))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (x0 + 15 * k, y0) for k in range(10)))
    data1 = cmds.getAttr(groom + ".guideData")
    check("the Direction brush turns the guides", data1 != data0)
    cmds.undo()
    check("one undo restores them", cmds.getAttr(groom + ".guideData") == data0)

    # Solo: Place with Solo new guides freezes every other guide in its step; Shift+click a guide
    # with a brush selects it (the rest frozen); the Select tool freezes the guides not selected, a pick of
    # nothing unfreezes them; Unfreeze All frees them all.
    def frozen():
        guides = json.loads(cmds.getAttr(groom + ".guideData"))["guides"]
        return sum(1 for g in guides if g.get("attributes", {}).get("freeze", 0.0) > 0.5), len(guides)

    ctx = groomlab.brush_tool("place", solo_new_guides=True, match_neighbors=False)
    px, py = to_view((-0.3, 0.9, 0.3))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d" % (px, py))
    n, total = frozen()
    check("Place with Solo new guides freezes every other guide (%d of %d)" % (n, total), total == 9 and n == total - 1)
    cmds.undo()
    check("one undo takes the guide and the freeze back", frozen() == (0, 8))
    ctx = groomlab.brush_tool("comb", radius=6, level=0, group="")  # a ring round one guide
    gid = cmds.groomLabGuides(groom, listGuides="primary")[0]
    xyz = cmds.groomLabGuides(groom, guidePoints=gid)
    mid = len(xyz) // 6 * 3
    mx, my = to_view(tuple(xyz[mid:mid + 3]))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (mx, my))
    check("Shift+click a guide selects it and freezes the rest (%r)" % (frozen(),), frozen() == (7, 8))
    check("... the brush's group as it was", cmds.groomLabBrushCtx(ctx, query=True, group=True) == "")
    sx, sy = to_view((0.0, -0.3, 0.95))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (sx, sy))
    check("Shift+click the bare scalp deselects and unfreezes every guide",
          frozen() == (0, 8) and not groomlab.selected_guides(groom))
    cmds.undo()
    check("each Shift click is an undo step", frozen() == (7, 8))
    # A click takes every guide in the ring, not only one under the cursor.
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (sx, sy))
    cmds.groomLabBrushCtx(ctx, edit=True, radius=2000)
    taken = cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (mx, my))
    check("a Shift+click with a wide ring selects the guides in it (%d)" % taken, taken > 1 and frozen()[0] == 8 - taken)
    # "shift x y" with a guide brush is the same Shift selection as the mouse's.
    cmds.groomLabBrushCtx(ctx, edit=True, radius=6)
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (sx, sy))
    shifted = cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="shift %d %d" % (mx, my))
    check("a Shift+click (the brush's Shift) selects the guide, the rest frozen (%d, %r)" % (shifted, frozen()),
          shifted == 1 and frozen() == (7, 8))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (sx, sy))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (mx, my))
    # With symmetry, the guide's mirror guide stays live too (the ring's guides pair up across x).
    no_line = red_pixels("symmetry_off")
    groomlab.brush_tool("comb", radius=6, level=0, group="", symmetry="x")  # (a ring round one guide)
    # The scalp's line of symmetry shows in red as soon as the tool is on with symmetry, the
    # cursor never over the view (the groom draws it on every redraw, not the tool on a hover).
    line = red_pixels("symmetry_x")
    check("symmetry on: the red line of symmetry shows without a hover (%d red pixels, %d before)" % (line, no_line),
          line > no_line + 40)
    cmds.groomLabBrushCtx(ctx, edit=True, symmetry="none")
    gone = red_pixels("symmetry_none")
    check("symmetry off: the line goes (%d red pixels)" % gone, gone < no_line + 10)
    cmds.groomLabBrushCtx(ctx, edit=True, symmetry="x")
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="select %d %d" % (mx, my))
    check("Shift+click with symmetry selects the guide's mirror too (%r)" % (frozen(),), frozen() == (6, 8))
    groomlab.select_tool(symmetry="x")
    cmds.groomLabSelectCtx(groomlab.SELECT_CONTEXT, edit=True, debugLasso="%d %d" % (mx, my))
    check("the Select tool with symmetry picks the mirror guide too",
          len(groomlab.selected_guides(groom)) == 2 and frozen() == (6, 8))
    cmds.groomLabSelectCtx(groomlab.SELECT_CONTEXT, edit=True, debugLasso="5 5")
    check("a pick of nothing still unfreezes every guide", frozen() == (0, 8))
    cmds.undo()
    cmds.undo()
    cmds.undo()
    check("back to the solo before the symmetric ones", frozen() == (7, 8))
    groomlab.brush_tool("comb", radius=60, level=0, group="")
    groomlab.select_tool()
    cmds.groomLabSelectCtx(groomlab.SELECT_CONTEXT, edit=True, debugLasso="5 5")
    check("a Select pick of nothing unfreezes every guide", frozen() == (0, 8))
    cmds.groomLabSelectCtx(groomlab.SELECT_CONTEXT, edit=True, debugLasso="%d %d" % (mx, my))
    check("selecting a guide freezes the rest (%r)" % (frozen(),), frozen() == (7, 8))
    cmds.undo()
    check("one undo per selection change", frozen() == (0, 8))
    cmds.redo()
    groomlab.unfreeze_all()
    check("Unfreeze All frees them all", frozen() == (0, 8))
    cmds.select(cmds.listRelatives(groom, parent=True)[0])

    # + New Map > For a setting > Density in the Paint tab (the Density brush is gone): made, linked,
    # painted with Ctrl (hair away), one undo step for the map and its link.
    graph0 = cmds.getAttr(groom + ".graph")
    try:
        groomlab.brush_tool("density")
        check("the Density brush is gone", False)
    except ValueError as e:
        check("the Density brush is gone and says where it went", "+ New Map > for a setting" in str(e))
    from groomlab.ui import palette
    tools = palette.show(tab="paint")
    tools.poll()
    tools.radius.setValue(90)
    name = tools.paint_tab.map_for_setting("scatter", "density", kind="vertices")
    ctx = groomlab.BRUSH_CONTEXT
    check("For a setting > Density made the map scatter_density (%r: %s)" % (name, tools.status.text()),
          name == "scatter_density" and "scatter_density" in groomlab.list_maps(groom))
    check("... a map on the scalp's vertices", groomlab.map_kind(groom, "scatter_density") == "vertices")
    check("... linked to Scatter's density", ("scatter", "density") in groomlab.map_users(groom, "scatter_density"))
    check("... picked, the Paint brush on it toward 1",
          cmds.currentCtx() == ctx and cmds.groomLabBrushCtx(ctx, query=True, mode=True) == "paint"
          and cmds.groomLabBrushCtx(ctx, query=True, attribute=True) == "scatter_density"
          and abs(cmds.groomLabBrushCtx(ctx, query=True, value=True) - 1.0) < 1e-6)
    cmds.groomLabBrushCtx(ctx, edit=True, strength=1.0)
    x0, y0 = to_view((0.0, 0.6, 0.8))
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="ctrl " + " ".join("%d %d" % (x0 - 60 + 15 * k, y0) for k in range(9)))
    values = json.loads(cmds.getAttr(groom + ".scalpMaps"))["scatter_density"]["values"]
    check("Ctrl painted hair away under the stroke, the rest left white (min %.2f)" % min(values),
          min(values) < 0.6 and max(values) == 1.0)
    check("no error from painting the density map", not groomlab.errors(groom))
    cmds.undo()
    values = json.loads(cmds.getAttr(groom + ".scalpMaps"))["scatter_density"]["values"]
    check("one undo takes the stroke back (the map white again)", min(values) == 1.0)
    cmds.undo()
    check("a second undo takes the map and its link back",
          "scatter_density" not in groomlab.list_maps(groom) and cmds.getAttr(groom + ".graph") == graph0)
    cmds.redo()
    cmds.redo()
    check("redo links and paints again", "scatter_density" in groomlab.list_maps(groom))
    tools.stop_tool()

    # The Groom Tools: the new tiles (no Width or Density), and the Paint card with its curves.
    tools = palette.show()
    check("the Groom Tools have the new brushes' tiles, not Width or Density",
          all(t in tools.buttons for t in ("direction", "noise")) and not any(t in tools.buttons for t in ("width", "density")))
    check("the new tiles have icons", all(not tools.buttons[t].icon().isNull() for t in ("direction", "noise")))
    check("Unfreeze All sits beside the Freeze tile", "unfreeze" in tools.buttons and not tools.buttons["unfreeze"].isCheckable())
    check("the hints name the Shift selection and Ctrl's opposite",
          "Shift+drag: paint the selection" in tools.gestures.text() and "Ctrl: the opposite" in tools.gestures.text()
          and "lasso" not in tools.gestures.text())
    tools.activate("paint")
    check("Paint picked in the Groom Tools is the context's mode",
          cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, mode=True) == "paint")
    check("the Paint card shows the falloff curve, not the root-to-tip curve",
          tools.falloff_curve_box.isVisibleTo(tools) and not tools.strength_curve_box.isVisibleTo(tools))
    tools.activate("comb")
    check("Comb's card shows the root-to-tip curve", tools.strength_curve_box.isVisibleTo(tools))
    tools.strength_curve.check.setChecked(True)
    check("ticking Root to tip sends the curve to the tool",
          cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, strengthProfile=True) != "")
    tools.strength_curve.check.setChecked(False)
    tools.symmetry.setChecked(True)
    tools.symmetry_mode.setCurrentIndex(tools.symmetry_mode.findData("topology"))
    check("the symmetry box sends topology",
          cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, symmetry=True) == "topology")
    tools.symmetry.setChecked(False)
    tools.stop_tool()
    later(step_budgets)


def step_budgets():
    # What hovering and combing cost over about 4k guides, and texture strokes on 4096x4096 maps
    # (reported only).
    import time
    groomlab = state["groomlab"]
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    curves = []
    for i in range(64):
        a = 2 * math.pi * (i % 16) / 16
        ring = 0.3 + 0.15 * (i // 16)
        y = math.sqrt(1 - ring * ring)
        x, z = ring * math.cos(a), ring * math.sin(a)
        curves.append(cmds.curve(degree=2, point=[(x, y, z), (1.2 * x, y + 0.4, 1.2 * z), (1.3 * x, y + 0.8, 1.3 * z)]))
    groom = groomlab.create_groom(scalp)
    groomlab.guides_from_curves(groom, curves, tube_radius=0.15)
    cmds.delete(curves)
    children = groomlab.generate_children(groom, "primary", count=62)
    guides = 64 + children
    cmds.setAttr("persp.translate", 0.0, 1.4, 5.0)
    cmds.setAttr("persp.rotate", -8, 0, 0)
    cmds.select(cmds.listRelatives(groom, parent=True)[0])
    cmds.refresh(force=True)
    ctx = groomlab.brush_tool("comb", radius=50, strength=1.0, level=1)
    hx, hy = to_view((0.0, 0.95, 0.3))
    moves = 20
    cmds.groomLabBrushCtx(ctx, edit=True, debugHover="%d %d" % (hx, hy))  # the first builds what hovering keeps
    start = time.time()
    lit = 0
    for k in range(moves):
        lit = cmds.groomLabBrushCtx(ctx, edit=True, debugHover="%d %d" % (hx + 3 * k, hy))
    hover = (time.time() - start) * 1000 / moves
    check("hovering lights up guides under the brush (%d)" % lit, lit > 0)
    dabs = 20
    data = cmds.getAttr(groom + ".guideData")

    def timed_stroke(count, prefix=""):
        begin = time.time()
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=prefix + " ".join("%d %d" % (hx + 6 * k, hy) for k in range(count)))
        took = (time.time() - begin) * 1000
        cmds.undo()
        return took

    start = time.time()
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (hx + 6 * k, hy) for k in range(dabs)))
    comb = (time.time() - start) * 1000 / dabs
    check("the comb stroke over %d guides changes them" % guides, cmds.getAttr(groom + ".guideData") != data)
    cmds.undo()
    # A dab's own cost: a long stroke's time less a short one's (the press and the commit apart).
    comb_dab = (timed_stroke(dabs + 10) - timed_stroke(10)) / dabs
    comb_drawn = (timed_stroke(dabs + 10, "draw ") - timed_stroke(10, "draw ")) / dabs
    say("[ budget ] %d guides: hover %.1f ms per mouse move (%d lit); comb stroke of %d dabs %.1f ms a dab, a dab "
        "itself %.1f ms, drawn as a drag %.1f ms (reported only)" % (guides, hover, lit, dabs, comb, comb_dab, comb_drawn))

    sx, sy = to_view((0.0, 0.85, 0.53))
    groomlab.add_map(groom, "hugeTex", kind="texture", resolution=4096, color=(0.0, 0.0, 0.0, 1.0))
    path = groomlab.map_texture(groom, "hugeTex")
    with open(path, "rb") as f:
        original = f.read()
    groomlab.paint_map(groom, "hugeTex", strength=1.0, radius=40, color=(1.0, 0.5, 0.0, 1.0))
    start = time.time()
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d %d %d" % (sx, sy, sx + 2, sy))  # opens the map
    first = time.time() - start
    clicks = []
    held_before = cmds.groomLabBrushCtx(ctx, query=True, heldUndoBytes=True)
    for k in range(3):
        start = time.time()
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="%d %d %d %d" % (sx, sy + 6 * k, sx + 2, sy + 6 * k))
        clicks.append(time.time() - start)
    held_clicks = (cmds.groomLabBrushCtx(ctx, query=True, heldUndoBytes=True) - held_before) / 3.0
    start = time.time()
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=" ".join("%d %d" % (sx - 60 + 5 * k, sy) for k in range(dabs)))
    stroke = time.time() - start
    # The same stroke redrawing the viewport after each dab, as a drag does (the brush's image
    # shown on the scalp, its changed texels uploaded).
    start = time.time()
    cmds.groomLabBrushCtx(ctx, edit=True, debugStroke="draw " + " ".join("%d %d" % (sx - 60 + 5 * k, sy + 12) for k in range(dabs)))
    drawn = time.time() - start
    upload = cmds.groomLabBrushCtx(ctx, query=True, previewUpload=True)
    check("the brush's image goes to the viewport (%s)" % upload, upload in ("region", "regionInImage", "whole"))
    size = os.path.getsize(path)
    for _ in range(6):
        cmds.undo()
    with open(path, "rb") as f:
        check("4096x4096 color texture strokes undo (the file as it was)", f.read() == original)
    say("[ budget ] 4096x4096 color texture: first stroke %.0f ms (opens the map), press+release %s ms (undo %.2f MB "
        "a stroke; the file is %.1f MB), a stroke of %d dabs %.0f ms (%.1f ms a dab), drawn as a drag %.0f ms "
        "(%.1f ms a dab with its preview; upload: %s; reported only)" % (
            first * 1000, ", ".join("%.0f" % (t * 1000) for t in clicks), held_clicks / 1e6, size / 1e6, dabs,
            stroke * 1000, stroke * 1000 / dabs, drawn * 1000, drawn * 1000 / dabs, upload))
    groomlab.remove_map(groom, "hugeTex")
    finish()
