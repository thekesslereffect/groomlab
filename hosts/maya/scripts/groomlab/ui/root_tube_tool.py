# SPDX-License-Identifier: Apache-2.0
"""The Tube tool's root-first mode in Maya's viewport (issue 61): a dragger context.

Draw root: click the root's outline on the scalp one CV at a time (a press places a CV, dragging
moves it; near a neighbouring root tube a CV snaps onto its CVs or edges, so tubes meet with no
gap). Clicking the first CV again closes the outline. With "Draw before extrude" the root's middle
then shows as a CV and a stroke drawn from it out sets the tube's path (as the Draw tool draws a
guide); on release the tube is made and the next root can be drawn. Without it the tube goes
straight off the scalp at once (Length, Segments).

Edit roots: the root tubes (the selected guides' or, with none selected, every root tube) show as
their root outlines with their CVs, the guides hidden; dragging a CV moves it on the scalp (CVs
shared with a neighbour, sealed, move together). Finish gives each changed tube its new root and
updates the regions (groomlab.root_tubes.edit_root_tube).

The outlines and strokes are drawn with temporary curves under PREVIEW (not in the undo queue,
removed when the tool is left).
"""

import math

from maya import cmds

import groomlab
from groomlab import root_tubes

CONTEXT = "groomLabRootTubeCtx"
PREVIEW = "glRootTubePreview"
PICK_PIXELS = 12

COLORS = {"outline": (1.0, 0.75, 0.15), "closing": (0.3, 1.0, 0.4), "center": (0.3, 0.85, 1.0),
          "stroke": (0.3, 0.85, 1.0), "edit": (1.0, 0.55, 0.15), "neighbour": (0.55, 0.55, 0.6)}


class _NoUndo(object):
    """Scene edits kept out of the undo queue (the tool's preview curves)."""

    def __enter__(self):
        self.state = cmds.undoInfo(query=True, stateWithoutFlush=True)
        cmds.undoInfo(stateWithoutFlush=False)

    def __exit__(self, *_):
        cmds.undoInfo(stateWithoutFlush=self.state)


def _om():
    from maya.api import OpenMaya as om
    from maya.api import OpenMayaUI as omui
    return om, omui


class RootTubeTool(object):
    """The tool's state between events (one per session: TOOL)."""

    def __init__(self):
        self.groom = None
        self.mode = "place"     # "place" (CVs), "draw" (the path from the root), "edit" (roots' CVs)
        self.cvs = []           # the outline being placed
        self.stroke = []        # the path being drawn
        self.center = None      # the closed outline's middle (draw mode), and the scalp's normal there
        self.normal = None
        self.settings = {"draw": True, "length": 1.0, "segments": 12, "taper": 0.5, "smooth": False, "snap": True,
                         "group": None}
        self.neighbours = []    # snap targets: [(guide, cvs)]
        self.edits = {}         # edit mode: {guide: [cvs]}
        self.original = {}      # ... as they were
        self.drag = []          # edit mode: the CVs being dragged [(guide, index)]
        self.guides_shown = None
        self.listeners = []     # called with a message after each change (the Groom Tools card)

    # -- Plumbing ---------------------------------------------------------------------

    def say(self, text):
        for fn in list(self.listeners):
            try:
                fn(text)
            except Exception:  # noqa: BLE001 (a closed panel)
                pass
        if text:
            print("GroomLab Root tubes: " + text)

    def scalps(self):
        from groomlab import utils
        return [utils._om_mesh(s) for s in sorted(utils._groom_meshes(self.groom))]

    def view_point(self):
        point = cmds.draggerContext(CONTEXT, query=True, dragPoint=True)
        return point[0], point[1]

    def press_point(self):
        point = cmds.draggerContext(CONTEXT, query=True, anchorPoint=True)
        return point[0], point[1]

    def ray(self, x, y):
        om, omui = _om()
        view = omui.M3dView.active3dView()
        p, d = om.MPoint(), om.MVector()
        view.viewToWorld(int(x), int(y), p, d)
        return p, d

    def hit(self, x, y):
        """The scalp under view point (x, y): (point, normal) or None."""
        om, _omui = _om()
        p, d = self.ray(x, y)
        best = None
        for fn in self.scalps():
            try:
                found = fn.closestIntersection(om.MFloatPoint(p), om.MFloatVector(d), om.MSpace.kWorld, 1e9, False)
            except RuntimeError:
                continue
            if not found or found[2] < 0:
                continue
            q = found[0]
            dist = om.MPoint(q).distanceTo(p)
            if best is None or dist < best[0]:
                n = fn.getClosestNormal(om.MPoint(q), om.MSpace.kWorld)[0]
                n.normalize()
                best = (dist, (q.x, q.y, q.z), (n.x, n.y, n.z))
        return (best[1], best[2]) if best else None

    def to_screen(self, point):
        om, omui = _om()
        out = omui.M3dView.active3dView().worldToView(om.MPoint(*point))
        return float(out[0]), float(out[1])

    def on_plane(self, x, y, origin):
        """View point (x, y) on the plane through `origin` facing the camera."""
        om, _omui = _om()
        p, d = self.ray(x, y)
        n = om.MVector(d).normal()
        o = om.MPoint(*origin)
        denom = n * n
        t = ((o - p) * n) / denom
        q = p + d * t
        return q.x, q.y, q.z

    # -- Starting and leaving ------------------------------------------------------------

    def start(self, groom, mode="place"):
        """Make the tool current on `groom` in "place" (draw roots) or "edit" (edit roots)."""
        if not groom:
            raise ValueError("select a groom first")
        if not cmds.draggerContext(CONTEXT, exists=True):
            cmds.draggerContext(CONTEXT, space="screen", projection="viewPlane", cursor="crossHair",
                                undoMode="step", name=CONTEXT)
        cmds.draggerContext(CONTEXT, edit=True, pressCommand=_press, dragCommand=_drag, releaseCommand=_release,
                            finalize=_finalize)
        self.reset()
        self.groom = groom
        self.mode = mode
        if cmds.currentCtx() != CONTEXT:
            cmds.setToolTo(CONTEXT)
        if mode == "edit":
            self.begin_edit()
        else:
            self.neighbours = root_tubes.snap_targets(groom) if self.settings["snap"] else []
            self.say("Click the root's outline on the scalp, one CV at a time; click the first CV to close it.")
        self.redraw()

    def reset(self):
        self.cvs, self.stroke, self.drag = [], [], []
        self.center = self.normal = None
        self.edits, self.original = {}, {}
        self.show_guides(True)
        self.clear_preview()

    def leave(self):
        """The tool put away: its preview removed, the guides shown again (edits not finished are
        dropped)."""
        self.reset()
        self.mode = "place"

    def active(self):
        return cmds.currentCtx() == CONTEXT

    # -- Placing the root's CVs ----------------------------------------------------------

    def snapped(self, point):
        if not self.settings["snap"] or not self.neighbours:
            return point, None
        return root_tubes.snap_point(point, self.neighbours, self.to_screen)

    def press(self):
        if not self.groom or not cmds.objExists(self.groom):
            self.say("Select a groom first.")
            return
        x, y = self.press_point()
        if self.mode == "edit":
            return self.press_edit(x, y)
        if self.mode == "draw":
            self.stroke = [self.center]
            return
        if len(self.cvs) >= 3:
            fx, fy = self.to_screen(self.cvs[0])
            if math.hypot(fx - x, fy - y) <= PICK_PIXELS:
                return self.close()
        hit = self.hit(x, y)
        if hit is None:
            self.say("Click on the scalp.")
            return
        point, how = self.snapped(hit[0])
        self.cvs.append(point)
        self.redraw()
        if how:
            self.say("CV %d snapped to a neighbouring root's %s." % (len(self.cvs), "CV" if how == "cv" else "edge"))

    def drag_event(self):
        x, y = self.view_point()
        if self.mode == "edit":
            return self.drag_edit(x, y)
        if self.mode == "draw":
            if self.center is None:
                return
            p = self.on_plane(x, y, self.stroke[-1] if self.stroke else self.center)
            if not self.stroke or root_tubes.tm._dist(p, self.stroke[-1]) > 1e-6:
                self.stroke.append(p)
            self.redraw()
            return
        if not self.cvs:
            return
        hit = self.hit(x, y)
        if hit is not None:
            self.cvs[-1] = self.snapped(hit[0])[0]
            self.redraw()

    def release(self):
        if self.mode == "draw" and self.center is not None:
            if len(self.stroke) < 3 or sum(root_tubes.tm._dist(a, b) for a, b in zip(self.stroke, self.stroke[1:])) < 1e-4:
                self.say("Drag out from the root to draw the tube's path.")
                self.stroke = [self.center]
                self.redraw()
                return
            self.make(self.stroke)
        elif self.mode == "edit":
            self.drag = []

    def close(self):
        """The outline closed on its first CV: on to drawing the path, or the tube made now."""
        surface = root_tubes._scalp_surface(self.groom)
        self.center, self.normal = surface(root_tubes._centroid(self.cvs))
        if self.settings["draw"]:
            self.mode = "draw"
            self.stroke = [self.center]
            self.redraw()
            self.say("Outline closed: drag from the root out to draw the tube's path.")
        else:
            self.make(None)

    def make(self, path):
        s = self.settings
        try:
            guide = root_tubes.make_root_tube(self.groom, self.cvs, path=path, length=s["length"],
                                              segments=s["segments"], taper=s["taper"], smooth=s["smooth"],
                                              group=s["group"])
        except (RuntimeError, ValueError) as e:
            self.say("The tube wasn't made: %s" % str(e).strip())
            self.mode = "place"
            self.cvs, self.stroke, self.center = [], [], None
            self.redraw()
            return None
        self.mode = "place"
        self.cvs, self.stroke, self.center = [], [], None
        self.neighbours = root_tubes.snap_targets(self.groom) if s["snap"] else []
        self.redraw()
        self.say("Made root tube %s (its own region on %s). Draw the next root." % (guide, root_tubes.REGION_MAP))
        return guide

    def undo_cv(self):
        """The last CV taken back (the path dropped first in draw mode)."""
        if self.mode == "draw":
            self.mode, self.stroke, self.center = "place", [], None
        elif self.cvs:
            self.cvs.pop()
        self.redraw()

    def cancel(self):
        """The outline being drawn dropped; edit mode left without changes."""
        if self.mode == "edit":
            self.edits = {}
            self.show_guides(True)
            self.mode = "place"
            self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
            self.say("Edit cancelled: the roots are as they were.")
        self.cvs, self.stroke, self.center = [], [], None
        if self.mode == "draw":
            self.mode = "place"
        self.redraw()

    # -- Editing roots -----------------------------------------------------------------

    def begin_edit(self):
        selected = groomlab.selected_guides(self.groom)
        tubes = root_tubes.root_tubes(self.groom, selected) if selected else []
        if not tubes:
            tubes = root_tubes.root_tubes(self.groom)
        if not tubes:
            self.mode = "place"
            self.say("The groom has no root tubes to edit: draw one first.")
            return
        self.edits = {}
        for g in tubes:
            try:
                self.edits[g] = [tuple(p) for p in root_tubes.root_outline(self.groom, g)]
            except (RuntimeError, ValueError):
                continue
        self.original = dict((g, list(c)) for g, c in self.edits.items())
        self.show_guides(False)
        self.say("Editing %d root%s: drag their CVs on the scalp, then Finish edit." %
                 (len(self.edits), "" if len(self.edits) == 1 else "s"))

    def press_edit(self, x, y):
        best = None
        for g, cvs in self.edits.items():
            for i, p in enumerate(cvs):
                sx, sy = self.to_screen(p)
                d = math.hypot(sx - x, sy - y)
                if d <= PICK_PIXELS and (best is None or d < best[0]):
                    best = (d, g, i)
        if best is None:
            self.drag = []
            return
        picked = self.edits[best[1]][best[2]]
        scale = max(root_tubes.tm._dist(a, b) for cvs in self.edits.values() for a, b in zip(cvs, cvs[1:] + cvs[:1]))
        tol = 1e-4 * max(scale, 1e-6)
        # A CV shared with a neighbour (sealed) moves with it.
        self.drag = [(g, i) for g, cvs in self.edits.items() for i, p in enumerate(cvs)
                     if root_tubes.tm._dist(p, picked) <= tol]

    def drag_edit(self, x, y):
        if not self.drag:
            return
        hit = self.hit(x, y)
        if hit is None:
            return
        point = hit[0]
        if self.settings["snap"]:
            moving = set(self.drag)
            others = [(g, [p for i, p in enumerate(cvs) if (g, i) not in moving]) for g, cvs in self.edits.items()]
            others = [(g, cvs) for g, cvs in others if len(cvs) >= 2]
            point = root_tubes.snap_point(point, others, self.to_screen)[0]
        for g, i in self.drag:
            self.edits[g][i] = tuple(point)
        self.redraw()

    def finish_edit(self):
        """Every changed root given its new outline (one undo step), the regions updated."""
        if self.mode != "edit":
            return 0
        changed = [g for g, cvs in self.edits.items() if cvs != self.original.get(g)]
        try:
            with groomlab.undo_chunk("edit root tubes"):
                for g in changed:
                    root_tubes.edit_root_tube(self.groom, g, self.edits[g], regions=False)
                if changed:
                    root_tubes.update_regions(self.groom)
        except (RuntimeError, ValueError) as e:
            self.say("The roots weren't changed: %s" % str(e).strip())
            return 0
        self.edits, self.original, self.drag = {}, {}, []
        self.show_guides(True)
        self.mode = "place"
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        self.redraw()
        self.say("Finished: %d root%s changed, the regions updated." % (len(changed), "" if len(changed) == 1 else "s"))
        return len(changed)

    def show_guides(self, on):
        """Edit mode hides the groom's guides (and their tubes), showing only the roots' outlines."""
        if not self.groom or not cmds.objExists(self.groom):
            self.guides_shown = None
            return
        with _NoUndo():
            if not on:
                if self.guides_shown is None:
                    self.guides_shown = groomlab.guides_visible(self.groom)
                groomlab.set_guides_visible(self.groom, False)
            elif self.guides_shown is not None:
                groomlab.set_guides_visible(self.groom, self.guides_shown)
                self.guides_shown = None

    # -- The preview -------------------------------------------------------------------

    def clear_preview(self):
        if cmds.objExists(PREVIEW):
            with _NoUndo():
                cmds.delete(PREVIEW)

    def _root(self):
        if not cmds.objExists(PREVIEW):
            cmds.createNode("transform", name=PREVIEW, skipSelect=True)
            cmds.setAttr(PREVIEW + ".hiddenInOutliner", True)
        return PREVIEW

    def _curve(self, name, points, color, closed=False, cvs=True, width=2.0):
        pts = [tuple(p) for p in points]
        if closed and len(pts) >= 3:
            pts = pts + [pts[0]]
        if len(pts) < 2:
            pts = pts * 2 if pts else []
        if not pts:
            return None
        curve = cmds.curve(name=name, point=pts, degree=1)
        curve = cmds.ls(cmds.parent(curve, self._root(), relative=True)[0], long=True)[0]
        shape = cmds.listRelatives(curve, shapes=True, fullPath=True)[0]
        cmds.setAttr(shape + ".overrideEnabled", True)
        cmds.setAttr(shape + ".overrideRGBColors", True)
        cmds.setAttr(shape + ".overrideColorRGB", *color)
        cmds.setAttr(shape + ".dispCV", bool(cvs))
        for attr, value in (("lineWidth", width), ("alwaysDrawOnTop", True)):
            if cmds.attributeQuery(attr, node=shape, exists=True):
                cmds.setAttr(shape + "." + attr, value)
        return curve

    def _marker(self, center, normal, radius, color):
        tm = root_tubes.tm
        a = tm._unit(tm._cross(normal, (1.0, 0.0, 0.0) if abs(normal[0]) < 0.9 else (0.0, 1.0, 0.0)))
        b = tm._cross(normal, a)
        ring = [tm._add(center, tm._add(tm._mul(a, radius * math.cos(k * math.pi / 6.0)),
                                        tm._mul(b, radius * math.sin(k * math.pi / 6.0)))) for k in range(12)]
        self._curve("glRootCenterRing", ring, color, closed=True, cvs=False)
        self._curve("glRootCenter", [center, tm._add(center, tm._mul(normal, radius * 0.01))], color, cvs=True,
                    width=4.0)

    def redraw(self):
        with _NoUndo():
            self.clear_preview()
            if not self.groom:
                return
            if self.mode == "edit":
                for g, cvs in self.edits.items():
                    self._curve("glRootEdit_%s" % g, cvs, COLORS["edit"], closed=True)
            else:
                for _g, cvs in self.neighbours:
                    self._curve("glRootNeighbour", cvs, COLORS["neighbour"], closed=True, cvs=False, width=1.0)
                if self.cvs:
                    closed = self.mode == "draw"
                    self._curve("glRootOutline", self.cvs, COLORS["closing" if closed else "outline"], closed=closed)
                if self.mode == "draw" and self.center is not None:
                    reach = max(root_tubes.tm._dist(self.center, p) for p in self.cvs) if self.cvs else 0.1
                    self._marker(self.center, self.normal, 0.15 * reach, COLORS["center"])
                    if len(self.stroke) >= 2:
                        self._curve("glRootStroke", self.stroke, COLORS["stroke"], cvs=False, width=3.0)
        try:
            cmds.refresh(currentView=True, force=True)
        except RuntimeError:  # (no viewport: batch)
            pass


TOOL = RootTubeTool()


def _press():
    _guarded(TOOL.press)


def _drag():
    _guarded(TOOL.drag_event)


def _release():
    _guarded(TOOL.release)


def _finalize():
    _guarded(TOOL.leave)


def _guarded(fn):
    try:
        fn()
    except Exception as e:  # noqa: BLE001 (an event never ends in a bare traceback)
        import traceback
        traceback.print_exc()
        TOOL.say("Error: %s" % str(e).strip())


def start(groom, mode="place", **settings):
    """The root-first tube tool on `groom` ("place": draw roots; "edit": edit the root tubes' CVs);
    `settings`: draw, length, segments, taper, smooth, snap, group."""
    TOOL.settings.update(settings)
    TOOL.start(groom, mode)
    return TOOL
