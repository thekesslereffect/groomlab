# SPDX-License-Identifier: Apache-2.0
"""The Tube tool's root-first mode in Maya's viewport (issue 61): a dragger context.

Draw root: click the root's outline on the scalp one CV at a time (a press places a CV, dragging
moves it; near a neighbouring root tube a CV snaps onto its CVs or edges, so tubes meet with no
gap). Clicking the first CV again closes the outline. With "Draw before extrude" the root's middle
then shows as a CV and a stroke drawn from it out sets the tube's path (as the Draw tool draws a
guide); on release the tube is made and the next root can be drawn. Without it the tube goes
straight off the scalp at once (Length, Segments). Undoing the tube just made (Ctrl+Z) brings its
outline back: drawing its path again, or (without Draw before extrude) the outline open; redoing
it puts the tube back and the outline away.

Edit roots: the root tubes (the selected guides' or, with none selected, every root tube) show as
their root outlines with their CVs, the guides hidden; dragging a CV moves it on the scalp (CVs
shared with a neighbour, sealed, move together; with Snap a CV snaps onto another root's CVs and
edges, never its own root's; Ctrl+click an edge adds a CV there, on every outline sharing the
edge; Ctrl+click a CV takes it out of the root edged white only, not out of a neighbour sealed to
it). The roots last touched are edged white. Undo CV takes back the last CV move. Finish gives
each changed tube its new root and updates the regions (groomlab.root_tubes.edit_root_tube);
leaving the tool any other way (another tool, Draw root, Select root) finishes the edit too, so
no edit is lost. Maya's Undo while editing undoes the scene, not a CV move: the roots it changed
come back as the scene has them.

Select root: every root shows its outline and its middle; clicking a middle selects that root
(green; Shift+click adds a root to the selection or takes it out), dragging a selected one carries
every selected root over the scalp as one (with Snap, their CVs sealed onto a root not selected
stay put, the others slide along), their tubes and the hierarchies under them moving with them on
release (one undo step, the regions updated). Duplicate: the selected roots follow the cursor over
the scalp (no button held) and a click sets a copy of them, their tubes and hierarchies down there
(groomlab.root_tubes.duplicate_root_tube); Delete removes the selected roots, their tubes and
hierarchies.

Every root shows in its region's color (as the region map paints it), filled, with its CVs as dots
over its edges. The outlines and strokes are drawn by the plugin's groomLabRootOutlines under
PREVIEW (its attributes set outside the undo queue; removed when the tool is left).
"""

import math

from maya import cmds

import groomlab
from groomlab import root_tubes

CONTEXT = "groomLabRootTubeCtx"
PREVIEW = "glRootTubePreview"
PICK_PIXELS = 12

# Placing: the outline amber, closed (drawing its path) green. A finished root in its region's
# color (root_tubes.root_colors), filled; the one being edited, selected or carried edged white,
# its CVs shown. "root": a root the region map has no color for yet.
COLORS = {"outline": (1.0, 0.75, 0.15), "closing": (0.3, 1.0, 0.4), "active": (0.3, 1.0, 0.4),
          "stroke": (0.3, 0.85, 1.0), "root": (0.85, 0.85, 0.9), "neighbour": (0.55, 0.55, 0.6)}

MODES = ("place", "draw", "edit", "select", "duplicate")

# The preview's node (the plugin's, root_outlines.h) and its styles' bits.
OUTLINES_NODE = "groomLabRootOutlines"
CLOSED, FILLED, DOTS, HIGHLIGHT, MARKER = 1, 2, 4, 8, 16
DOT_PIXELS = 9.0  # the CV dots' size across


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
        self.mode = "place"     # MODES: "place" (CVs), "draw" (the path), "edit" (CVs), "select", "duplicate"
        self.cvs = []           # the outline being placed
        self.stroke = []        # the path being drawn
        self.center = None      # the closed outline's middle (draw mode), and the scalp's normal there
        self.normal = None
        self.settings = {"draw": True, "length": 1.0, "segments": 12, "taper": 0.5, "snap": True, "group": None,
                         "density": 1.0}
        self.neighbours = []    # snap targets: [(guide, cvs)]
        self.edits = {}         # edit mode: {guide: [cvs]}
        self.original = {}      # ... as they were
        self.drag = []          # edit mode: the CVs being dragged [(guide, index)]
        self.touched = set()    # edit mode: the roots last touched (edged white)
        self.current = None     # ... the root last clicked (Ctrl+click on a sealed CV takes it out of this one)
        self.history = []       # ... the edits before each CV move (Undo CV)
        self.scope = False      # ... only the selected guides' roots
        self.roots = {}         # select mode: {guide: [cvs]}
        self.middles = {}       # ... {guide: (middle, normal)}
        self.picked = []        # ... the selected roots' guides (the last picked last)
        self.move = None        # ... roots being carried: {press, middle, base: {guide: cvs}, held: {guide: [i]}}
        self.ghost = None       # duplicate mode: (where, [the copies' outlines]) where they would go
        self.guides_shown = None
        self.listeners = []     # called with a message after each change (the Groom Tools card)
        self.last_made = None   # the tube just made: {guide, cvs, center, normal, drawn} (undoing it brings it back)
        self.restored = None    # ... the one an undo brought back (a redo puts it away again)
        self.callbacks = []     # Maya's Undo and Redo events, while the tool is up
        self.hover_filter = None
        self.colors = {}        # {guide: its region's color} (recolor)
        self.facing = {}        # {guide: the scalp's normal under its middle}

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

    def modifier(self):
        try:
            return cmds.draggerContext(CONTEXT, query=True, modifier=True) or ""
        except RuntimeError:
            return ""

    # -- Starting and leaving ------------------------------------------------------------

    def start(self, groom, mode="place"):
        """Make the tool current on `groom` in "place" (draw roots), "edit" (edit roots' CVs) or
        "select" (pick, carry, duplicate and delete roots)."""
        if not groom:
            raise ValueError("select a groom first")
        if not cmds.draggerContext(CONTEXT, exists=True):
            cmds.draggerContext(CONTEXT, space="screen", projection="viewPlane", cursor="crossHair",
                                undoMode="step", name=CONTEXT)
        cmds.draggerContext(CONTEXT, edit=True, pressCommand=_press, dragCommand=_drag, releaseCommand=_release,
                            finalize=_finalize)
        self.finish_pending()
        self.reset()
        self.groom = groom
        self.mode = mode
        self.last_made = self.restored = None
        self.recolor()
        self.listen(True)
        if cmds.currentCtx() != CONTEXT:
            cmds.setToolTo(CONTEXT)
        if mode == "edit":
            self.begin_edit()
        elif mode == "select":
            self.picked = []  # (a new selection)
            self.begin_select()
        else:
            self.neighbours = root_tubes.snap_targets(groom) if self.settings["snap"] else []
            self.say("Click the root's outline on the scalp, one CV at a time; click the first CV to close it.")
        self.redraw()

    def reset(self):
        self.cvs, self.stroke, self.drag = [], [], []
        self.center = self.normal = None
        self.edits, self.original, self.touched = {}, {}, set()
        self.current, self.history = None, []
        self.roots, self.middles, self.move, self.ghost = {}, {}, None, None
        self.hover(False)
        self.show_guides(True)
        self.clear_preview()

    def leave(self):
        """The tool put away: edits not finished given to their roots (finish_edit), its preview
        removed, the guides shown again."""
        self.finish_pending()
        self.reset()
        self.mode = "place"
        self.picked = []
        self.last_made = self.restored = None
        self.listen(False)
        try:  # (the Groom Tools card's buttons, once Maya has its new tool)
            cmds.evalDeferred(lambda: _guarded(lambda: self.say("")), lowestPriority=True)
        except (RuntimeError, TypeError):
            pass

    def finish_pending(self):
        """Edits not finished (the tool left or its mode changed while editing): finished."""
        if self.mode == "edit" and self.edits and self.groom and cmds.objExists(self.groom):
            if any(cvs != self.original.get(g) for g, cvs in self.edits.items()):
                self.finish_edit()

    def listen(self, on):
        om, _omui = _om()
        for cb in self.callbacks:
            try:
                om.MMessage.removeCallback(cb)
            except RuntimeError:
                pass
        self.callbacks = ([om.MEventMessage.addEventCallback("Undo", _undone),
                           om.MEventMessage.addEventCallback("Redo", _redone)] if on else [])

    def undone(self):
        """After an undo: when it took back the tube just made, its outline back (drawing its path
        again with Draw before extrude, else the outline open, to close it again). In select
        mode, the roots as they now are."""
        if not self.groom or not cmds.objExists(self.groom):
            return
        if self.mode in ("select", "duplicate"):
            return self.reload_roots()
        if self.mode == "edit":
            return self.reconcile_edits()
        last = self.last_made
        if not last or self.mode != "place" or self.cvs:
            return
        if last["guide"] in root_tubes.root_tubes(self.groom):
            return
        self.last_made = None
        self.restored = last
        self.recolor()
        self.cvs = list(last["cvs"])
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        if last["drawn"]:
            self.mode = "draw"
            self.center, self.normal = last["center"], last["normal"]
            self.stroke = [self.center]
            self.say("Tube undone: drag from the root out to draw its path again.")
        else:
            self.say("Tube undone: its outline is back; click its first CV to close it again.")
        self.redraw()

    def redone(self):
        """After a redo: the tube an undo took back is back, so its outline (brought back to draw
        again) goes. In select mode, the roots as they now are."""
        if not self.groom or not cmds.objExists(self.groom):
            return
        if self.mode in ("select", "duplicate"):
            return self.reload_roots()
        if self.mode == "edit":
            return self.reconcile_edits()
        back = self.restored
        if not back or self.mode not in ("place", "draw"):
            return
        if back["guide"] not in root_tubes.root_tubes(self.groom):
            return
        self.restored = None
        self.last_made = back
        self.recolor()
        self.mode = "place"
        self.cvs, self.stroke, self.center = [], [], None
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        self.redraw()
        self.say("Tube redone. Draw the next root.")

    def active(self):
        return cmds.currentCtx() == CONTEXT

    @property
    def selected(self):
        """The last root selected (None: none)."""
        return self.picked[-1] if self.picked else None

    # -- Events ------------------------------------------------------------------------

    def press(self):
        if not self.groom or not cmds.objExists(self.groom):
            self.say("Select a groom first.")
            return
        x, y = self.press_point()
        if self.mode == "edit":
            return self.press_edit(x, y)
        if self.mode == "select":
            return self.press_select(x, y)
        if self.mode == "duplicate":
            return self.follow(x, y)
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
        self.last_made = self.restored = None  # (a new outline begun: undo and redo no longer touch the last one)
        self.cvs.append(point)
        self.redraw()
        if how:
            self.say("CV %d snapped to a neighbouring root's %s." % (len(self.cvs), "CV" if how == "cv" else "edge"))

    def drag_event(self):
        x, y = self.view_point()
        if self.mode == "edit":
            return self.drag_edit(x, y)
        if self.mode == "select":
            return self.drag_select(x, y)
        if self.mode == "duplicate":
            return self.follow(x, y)
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
            if self.history and self.history[-1] == self.edits:  # (a click that moved nothing)
                self.history.pop()
        elif self.mode == "select":
            self.release_select()
        elif self.mode == "duplicate":
            self.place_copy()

    # -- Placing the root's CVs ----------------------------------------------------------

    def snapped(self, point):
        if not self.settings["snap"] or not self.neighbours:
            return point, None
        return root_tubes.snap_point(point, self.neighbours, self.to_screen)

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
                                              segments=s["segments"], taper=s["taper"], group=s["group"],
                                              density=s.get("density", 1.0))
        except (RuntimeError, ValueError) as e:
            self.say("The tube wasn't made: %s" % str(e).strip())
            self.mode = "place"
            self.cvs, self.stroke, self.center = [], [], None
            self.redraw()
            return None
        self.last_made = {"guide": int(guide), "cvs": list(self.cvs), "center": self.center, "normal": self.normal,
                          "drawn": path is not None}
        self.restored = None
        self.mode = "place"
        self.cvs, self.stroke, self.center = [], [], None
        self.recolor()
        self.neighbours = root_tubes.snap_targets(self.groom) if s["snap"] else []
        self.redraw()
        self.say("Made root tube %s (its own region on %s). Draw the next root." % (guide, root_tubes.REGION_MAP))
        return guide

    def undo_cv(self):
        """The last CV taken back (the path dropped first in draw mode; in edit mode the last CV
        moved, added or taken out put back)."""
        if self.mode == "edit":
            if not self.history:
                self.say("No CV move to take back.")
                return
            before = self.history.pop()
            self.touched = set(g for g in before if before[g] != self.edits.get(g))
            self.edits.update(dict((g, list(c)) for g, c in before.items() if g in self.edits))
            self.drag = []
            self.redraw()
            self.say("CV move taken back.")
            return
        if self.mode == "draw":
            self.mode, self.stroke, self.center = "place", [], None
        elif self.mode == "place" and self.cvs:
            self.cvs.pop()
        self.redraw()

    # -- Editing roots -----------------------------------------------------------------

    def begin_edit(self):
        selected = groomlab.selected_guides(self.groom)
        tubes = root_tubes.root_tubes(self.groom, selected) if selected else []
        self.scope = bool(tubes)  # (only the selected guides' roots: Undo brings no others in)
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
        self.touched = set()
        self.show_guides(False)
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        self.say("Editing %d root%s: drag their CVs on the scalp (Ctrl+click an edge to add one, a CV to take it out), "
                 "then Finish edit." %
                 (len(self.edits), "" if len(self.edits) == 1 else "s"))

    def _tolerance(self):
        scale = max(root_tubes.tm._dist(a, b) for cvs in self.edits.values() for a, b in zip(cvs, cvs[1:] + cvs[:1]))
        return 1e-4 * max(scale, 1e-6)

    def insert_cv(self, x, y):
        """A new CV on the outline edge nearest view point (x, y) (within PICK_PIXELS), on the
        scalp; put into every outline being edited that shares the edge, so sealed neighbours stay
        sealed. Then dragged as a picked CV is."""
        tm = root_tubes.tm
        best = None
        for g, cvs in self.edits.items():
            n = len(cvs)
            for i in range(n):
                a, b = cvs[i], cvs[(i + 1) % n]
                ax, ay = self.to_screen(a)
                bx, by = self.to_screen(b)
                dx, dy = bx - ax, by - ay
                span = dx * dx + dy * dy
                f = min(max(((x - ax) * dx + (y - ay) * dy) / span, 0.0), 1.0) if span > 1e-9 else 0.0
                d = math.hypot(ax + f * dx - x, ay + f * dy - y)
                if d <= PICK_PIXELS and (best is None or d < best[0]):
                    best = (d, g, i, f)
        self.drag = []
        if best is None:
            self.say("Ctrl+click on a root's edge to add a CV there.")
            return
        _d, g, i, f = best
        self.remember()
        self.current = g
        cvs = self.edits[g]
        a, b = cvs[i], cvs[(i + 1) % len(cvs)]
        point = tuple(root_tubes._project(self.groom, [tm.lerp(a, b, f)])[0])
        tol = self._tolerance()
        full = 0
        for h, other in self.edits.items():
            n = len(other)
            for j in range(n):
                p, q = other[j], other[(j + 1) % n]
                if ((tm._dist(p, a) <= tol and tm._dist(q, b) <= tol) or
                        (tm._dist(p, b) <= tol and tm._dist(q, a) <= tol)):
                    if n >= tm.MAX_SHAPE_POINTS:
                        full += 1
                        break
                    other.insert(j + 1, point)
                    self.drag.append((h, j + 1))
                    break
        self.touched = set(h for h, _j in self.drag) or {g}
        self.redraw()
        if full and not self.drag:
            self.say("A root takes at most %d CVs." % tm.MAX_SHAPE_POINTS)
        else:
            self.say("Added a CV%s: drag it, then Finish edit." %
                     (" on %d sealed outlines" % len(self.drag) if len(self.drag) > 1 else ""))

    def remember(self):
        """The edits as they are, for Undo CV."""
        self.history.append(dict((g, list(c)) for g, c in self.edits.items()))
        del self.history[:-100]

    def _cv_under(self, x, y):
        """The CV nearest view point (x, y) within PICK_PIXELS: (point, [(guide, index)] of every
        root's CV there, sealed ones too), or None."""
        best = None
        for g, cvs in self.edits.items():
            for i, p in enumerate(cvs):
                sx, sy = self.to_screen(p)
                d = math.hypot(sx - x, sy - y)
                if d <= PICK_PIXELS and (best is None or d < best[0]):
                    best = (d, g, i)
        if best is None:
            return None
        picked = self.edits[best[1]][best[2]]
        tol = self._tolerance()
        owners = []
        for g, cvs in self.edits.items():
            near = [i for i, p in enumerate(cvs) if root_tubes.tm._dist(p, picked) <= tol]
            if near:
                owners.append((g, near[0]))
        return picked, owners

    def _meant(self, owners, x, y):
        """Of the roots sharing a CV, the one meant: the one edged white (last touched), else the
        one last clicked, else the one whose middle is nearest the click on the screen."""
        roots = [g for g, _i in owners]
        touched = [g for g in roots if g in self.touched]
        if len(touched) == 1:
            return touched[0]
        if self.current in roots:
            return self.current

        def away(g):
            mx, my = self.to_screen(root_tubes._centroid(self.edits[g]))
            return math.hypot(mx - x, my - y)
        return min(roots, key=away)

    def remove_cv(self, x, y):
        """Ctrl+click on a CV: taken out of the root meant (_meant) only; a neighbour sealed to it
        keeps its CV. False when no CV is there."""
        under = self._cv_under(x, y)
        if under is None:
            return False
        _point, owners = under
        g = self._meant(owners, x, y)
        i = dict(owners)[g]
        self.drag = []
        if len(self.edits[g]) <= 3:
            self.say("A root needs at least 3 CVs.")
            return True
        self.remember()
        del self.edits[g][i]
        self.current = g
        self.touched = {g}
        self.redraw()
        self.say("Took a CV out of root %s%s." % (g, " (its neighbour keeps its own)" if len(owners) > 1 else ""))
        return True

    def press_edit(self, x, y):
        if "ctrl" in self.modifier():
            if self.remove_cv(x, y):
                return
            return self.insert_cv(x, y)
        under = self._cv_under(x, y)
        if under is None:
            self.drag = []
            return
        _point, owners = under
        self.remember()
        # A CV shared with a neighbour (sealed) moves with it.
        self.drag = owners
        self.current = self._meant(owners, x, y)
        self.touched = set(g for g, _i in self.drag)
        self.redraw()

    def drag_edit(self, x, y):
        if not self.drag:
            return
        hit = self.hit(x, y)
        if hit is None:
            return
        point = hit[0]
        if self.settings["snap"]:
            # Onto another root's CVs and edges only: never its own (two of its CVs would become one).
            mine = set(g for g, _i in self.drag)
            others = [(g, cvs) for g, cvs in self.edits.items() if g not in mine and len(cvs) >= 2]
            others += [(g, cvs) for g, cvs in self.neighbours if g not in self.edits and g not in mine]
            point = root_tubes.snap_point(point, others, self.to_screen)[0]
        for g, i in self.drag:
            self.edits[g][i] = tuple(point)
        self.redraw()

    def reconcile_edits(self):
        """After Maya's Undo or Redo while editing: the roots as the scene has them where it changed
        them (a root it took away dropped, one it brought back added), the others' edits kept."""
        if not self.groom or not cmds.objExists(self.groom):
            return
        now = set(root_tubes.root_tubes(self.groom))
        changed = []
        for g in list(self.edits):
            if g not in now:
                del self.edits[g]
                self.original.pop(g, None)
                changed.append(g)
                continue
            try:
                stored = [tuple(p) for p in root_tubes.root_outline(self.groom, g)]
            except (RuntimeError, ValueError):
                continue
            old = self.original.get(g, [])
            if len(stored) != len(old) or any(root_tubes.tm._dist(a, b) > 1e-6 for a, b in zip(stored, old)):
                self.edits[g] = list(stored)
                self.original[g] = list(stored)
                changed.append(g)
        if not self.scope:
            for g in sorted(now - set(self.edits)):
                try:
                    self.edits[g] = [tuple(p) for p in root_tubes.root_outline(self.groom, g)]
                    self.original[g] = list(self.edits[g])
                    changed.append(g)
                except (RuntimeError, ValueError):
                    continue
        self.drag, self.history = [], []
        self.touched &= set(self.edits)
        self.recolor()
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        self.redraw()
        if changed:
            self.say("Undo changed %d root%s in the scene: %s as the scene has %s now (Undo CV takes back a "
                     "CV move)." % (len(changed), "" if len(changed) == 1 else "s",
                                    "it is" if len(changed) == 1 else "they are", "it" if len(changed) == 1 else "them"))

    def _cleaned(self, cvs):
        """An outline with no CV twice in a row (a CV dragged onto its neighbour)."""
        tol = self._tolerance()
        out = []
        for p in cvs:
            if not out or root_tubes.tm._dist(p, out[-1]) > tol:
                out.append(tuple(p))
        while len(out) > 1 and root_tubes.tm._dist(out[0], out[-1]) <= tol:
            out.pop()
        return out

    def finish_edit(self):
        """Every changed root given its new outline (one undo step), the regions updated; edit
        mode left whatever happens (a root that can't take its outline keeps its old one, and
        says why)."""
        if self.mode != "edit":
            return 0
        now = set(root_tubes.root_tubes(self.groom)) if self.groom and cmds.objExists(self.groom) else set()
        changed = [g for g, cvs in self.edits.items() if g in now and cvs != self.original.get(g)]
        done, failed = [], []
        try:
            with groomlab.undo_chunk("edit root tubes"):
                for g in changed:
                    cvs = self._cleaned(self.edits[g])
                    if len(cvs) < 3:
                        failed.append((g, "fewer than 3 CVs"))
                        continue
                    try:
                        root_tubes.edit_root_tube(self.groom, g, cvs, regions=False)
                        done.append(g)
                    except Exception as e:  # noqa: BLE001 (one root's trouble never strands the edit)
                        failed.append((g, str(e).strip() or type(e).__name__))
                        try:  # (back as it was, should it have half changed)
                            root_tubes.edit_root_tube(self.groom, g, self.original[g], regions=False)
                        except Exception:  # noqa: BLE001
                            pass
                if done:
                    root_tubes.update_regions(self.groom)
        except Exception as e:  # noqa: BLE001
            failed.append((None, str(e).strip() or type(e).__name__))
        self.edits, self.original, self.drag, self.touched = {}, {}, [], set()
        self.current, self.history = None, []
        self.show_guides(True)
        self.mode = "place"
        self.recolor()
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        self.redraw()
        text = "Finished: %d root%s changed, the regions updated." % (len(done), "" if len(done) == 1 else "s")
        if failed:
            text += " Kept as %s: %s." % ("it was" if len(failed) == 1 else "they were",
                                         "; ".join("root %s (%s)" % (g, why) if g is not None else why
                                                   for g, why in failed))
        self.say(text)
        return len(done)

    # -- Selecting, carrying, duplicating and deleting roots ------------------------------

    def begin_select(self):
        self.reload_roots()
        if not self.roots:
            self.say("The groom has no root tubes: draw one first.")
            return
        self.say("Click a root's middle to select it; drag it to move the root over the scalp.")

    def reload_roots(self):
        """The roots as they are (their outlines and middles), the selection kept as far as its
        roots are."""
        self.roots, self.middles = {}, {}
        self.recolor()
        for g in root_tubes.root_tubes(self.groom):
            try:
                cvs = [tuple(p) for p in root_tubes.root_outline(self.groom, g)]
                self.roots[g] = cvs
                self.middles[g] = root_tubes._scalp_surface(self.groom)(root_tubes._centroid(cvs))
            except (RuntimeError, ValueError):
                continue
        self.picked = [g for g in self.picked if g in self.roots]
        if not self.picked and self.mode == "duplicate":
            self.mode = "select"
            self.hover(False)
        self.redraw()

    def group_middle(self):
        """The selected roots' middle as one: the mean of theirs, on the scalp."""
        tm = root_tubes.tm
        mids = [self.middles[g][0] for g in self.picked]
        mean = tm._mul(tuple(sum(c) for c in zip(*mids)), 1.0 / len(mids))
        return tuple(root_tubes._scalp_surface(self.groom)(mean)[0])

    def press_select(self, x, y):
        best = None
        for g, (middle, _n) in self.middles.items():
            sx, sy = self.to_screen(middle)
            d = math.hypot(sx - x, sy - y)
            if d <= PICK_PIXELS and (best is None or d < best[0]):
                best = (d, g)
        shift = "shift" in self.modifier()
        self.move = None
        if best is None:
            if not shift:
                self.picked = []
            self.redraw()
            self.say("Nothing selected: click a root's middle (Shift+click: add one)." if not self.picked else
                     "%d root%s selected." % (len(self.picked), "" if len(self.picked) == 1 else "s"))
            return
        g = best[1]
        if shift and g in self.picked:  # (Shift+click a selected one: out of the selection)
            self.picked.remove(g)
            self.redraw()
            self.say("%d root%s selected." % (len(self.picked), "" if len(self.picked) == 1 else "s"))
            return
        if shift:
            self.picked.append(g)
        elif g not in self.picked:
            self.picked = [g]
        else:  # (a selected one: carried with the others, last picked)
            self.picked.remove(g)
            self.picked.append(g)
        hit = self.hit(x, y)
        held = {}
        if self.settings["snap"]:
            others = [(h, cvs) for h, cvs in self.roots.items() if h not in self.picked]
            for h in self.picked:
                held[h] = root_tubes.sealed(self.groom, h, self.roots[h], others)
        middle = self.group_middle()
        self.move = {"press": hit[0] if hit else self.middles[g][0], "middle": middle,
                     "base": dict((h, list(self.roots[h])) for h in self.picked), "held": held, "moved": False}
        self.redraw()
        sealed = sum(len(v) for v in held.values())
        self.say("%d root%s selected%s: drag to move %s; Duplicate or Delete %s." %
                 (len(self.picked), "" if len(self.picked) == 1 else "s",
                  " (%d CV%s sealed to a neighbour stay put)" % (sealed, "" if sealed == 1 else "s") if sealed else "",
                  "it" if len(self.picked) == 1 else "them", "it" if len(self.picked) == 1 else "them"))

    def drag_select(self, x, y):
        if not self.move:
            return
        hit = self.hit(x, y)
        if hit is None:
            return
        tm = root_tubes.tm
        m = self.move
        to = tm._add(m["middle"], tm._sub(hit[0], m["press"]))
        for h, base in m["base"].items():
            self.roots[h] = root_tubes.carried(self.groom, base, m["middle"], to, m["held"].get(h, ()))
        m["moved"] = True
        self.redraw()

    def release_select(self):
        m, self.move = self.move, None
        if not m or not m["moved"]:
            return
        changed = [h for h in m["base"] if self.roots[h] != m["base"][h]]
        try:
            with groomlab.undo_chunk("move root tubes"):
                for h in changed:
                    root_tubes.edit_root_tube(self.groom, h, self.roots[h], regions=False)
                if changed:
                    root_tubes.update_regions(self.groom)
        except (RuntimeError, ValueError) as e:
            self.say("The roots weren't moved: %s" % str(e).strip())
        self.reload_roots()
        self.say("%d root%s moved, the regions updated." % (len(changed), "" if len(changed) == 1 else "s"))

    def begin_duplicate(self):
        """The selected roots follow the cursor over the scalp; a click sets a copy down there."""
        if self.mode not in ("select", "duplicate") or not self.picked:
            self.say("Select a root first (Select root, then click its middle).")
            return False
        self.mode = "duplicate"
        self.ghost = None
        self.hover(True)
        self.redraw()
        self.say("Move the cursor over the scalp and click to set the cop%s down." %
                 ("y" if len(self.picked) == 1 else "ies"))
        return True

    def end_duplicate(self):
        if self.mode == "duplicate":
            self.mode = "select"
            self.ghost = None
            self.hover(False)
            self.redraw()

    def follow(self, x, y):
        """Duplicate mode: the copies' outlines under view point (x, y)."""
        if not self.picked:
            return
        hit = self.hit(x, y)
        if hit is None:
            return
        middle = self.group_middle()
        self.ghost = (hit[0], [root_tubes.carried(self.groom, self.roots[g], middle, hit[0]) for g in self.picked])
        self.redraw()

    def place_copy(self):
        if not self.ghost:
            self.say("Click on the scalp to set the copy down.")
            return None
        to, _outlines = self.ghost
        middle = self.group_middle()
        made = []
        try:
            with groomlab.undo_chunk("duplicate root tubes"):
                for g in self.picked:
                    made.append(int(root_tubes.duplicate_root_tube(self.groom, g, to, regions=False, start=middle)))
                root_tubes.update_regions(self.groom)
        except (RuntimeError, ValueError) as e:
            self.say("The roots weren't duplicated: %s" % str(e).strip())
            return None
        sources = list(self.picked)
        self.picked = made
        self.end_duplicate()
        self.reload_roots()
        self.say("Duplicated root%s %s as %s (tubes and hierarchies with them); the copies are selected." %
                 ("" if len(sources) == 1 else "s", ", ".join(map(str, sources)), ", ".join(map(str, made))))
        return made[0] if len(made) == 1 else made

    def delete_selected(self):
        if self.mode not in ("select", "duplicate") or not self.picked:
            self.say("Select a root first (Select root, then click its middle).")
            return 0
        gone = list(self.picked)
        self.end_duplicate()
        root_tubes.delete_root_tubes(self.groom, gone)
        self.picked = []
        self.reload_roots()
        self.say("Deleted root%s %s, with %s tube%s and hierarch%s." %
                 ("" if len(gone) == 1 else "s", ", ".join(map(str, gone)), "its" if len(gone) == 1 else "their",
                  "" if len(gone) == 1 else "s", "y" if len(gone) == 1 else "ies"))
        return len(gone)

    def hover(self, on):
        """Duplicate mode: the viewport's mouse moves (no button held) followed (follow)."""
        from groomlab.ui.qt import QtCore, QtWidgets, wrap_instance
        if self.hover_filter is not None:
            widget, watcher, tracking = self.hover_filter
            try:
                widget.removeEventFilter(watcher)
                widget.setMouseTracking(tracking)
            except RuntimeError:
                pass
            self.hover_filter = None
        if not on:
            return
        try:
            _om_, omui = _om()
            view = omui.M3dView.active3dView()
            widget = wrap_instance(view.widget(), QtWidgets.QWidget)
        except Exception:  # noqa: BLE001 (no viewport: batch; a press then a drag still place it)
            return
        tool = self

        class Watcher(QtCore.QObject):
            def eventFilter(self, obj, event):
                if event.type() == QtCore.QEvent.MouseMove and not event.buttons():
                    try:
                        pos = event.position() if hasattr(event, "position") else event.pos()
                        sx = view.portWidth() / float(max(obj.width(), 1))
                        sy = view.portHeight() / float(max(obj.height(), 1))
                        _guarded(lambda: tool.follow(pos.x() * sx, view.portHeight() - pos.y() * sy))
                    except RuntimeError:
                        pass
                return False

        watcher = Watcher(widget)
        tracking = widget.hasMouseTracking()
        widget.setMouseTracking(True)
        widget.installEventFilter(watcher)
        self.hover_filter = (widget, watcher, tracking)

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

    def _node(self):
        """The groomLabRootOutlines shape the preview is drawn by (made when missing)."""
        shape = PREVIEW + "Shape"
        if not cmds.objExists(shape):
            if not cmds.objExists(PREVIEW):
                cmds.createNode("transform", name=PREVIEW, skipSelect=True)
                cmds.setAttr(PREVIEW + ".hiddenInOutliner", True)
            cmds.createNode(OUTLINES_NODE, name=shape, parent=PREVIEW, skipSelect=True)
        return shape

    def color_of(self, guide):
        """A root's color: its region's on the region map (root_tubes.root_colors)."""
        return self.colors.get(guide, COLORS["root"])

    def recolor(self):
        """The roots' colors as the region map has them now, and where each faces."""
        try:
            self.colors = root_tubes.root_colors(self.groom) if self.groom and cmds.objExists(self.groom) else {}
        except (RuntimeError, ValueError):
            self.colors = {}
        self.facing = {}

    def facing_of(self, guide, cvs):
        """The scalp's normal under a root's middle (the outline drawn faint when it faces away)."""
        if guide not in self.facing:
            try:
                self.facing[guide] = tuple(root_tubes._scalp_surface(self.groom)(root_tubes._centroid(cvs))[1])
            except (RuntimeError, ValueError, ZeroDivisionError):
                self.facing[guide] = (0.0, 0.0, 0.0)
        return self.facing[guide]

    def outlines(self):
        """What the preview draws now: [(points, color, normal, style)] (style: root_outlines.h's
        bits), and the fills' opacity."""
        tm = root_tubes.tm
        out = []
        fill = 1.0
        if self.mode == "edit":
            for g, cvs in self.neighbours:  # (editing the selected guides' roots: the others, unfilled)
                if g not in self.edits:
                    out.append((cvs, self.color_of(g), self.facing_of(g, cvs), CLOSED))
            for g, cvs in self.edits.items():
                style = CLOSED | FILLED | DOTS | (HIGHLIGHT if g in self.touched else 0)
                out.append((cvs, self.color_of(g), self.facing_of(g, self.original.get(g, cvs)), style))
        elif self.mode in ("select", "duplicate"):
            fill = 0.6
            for g, cvs in self.roots.items():
                picked = g in self.picked
                middle, normal = self.middles[g]
                out.append((cvs, self.color_of(g), normal, CLOSED | FILLED | (HIGHLIGHT | DOTS if picked else 0)))
                out.append(([middle], self.color_of(g), normal, MARKER | (HIGHLIGHT if picked else 0)))
            if self.mode == "duplicate" and self.ghost:
                for g, cvs in zip(self.picked, self.ghost[1]):
                    out.append((cvs, self.color_of(g), (0.0, 0.0, 0.0), CLOSED | FILLED | HIGHLIGHT | DOTS))
        else:
            for g, cvs in self.neighbours:
                out.append((cvs, self.color_of(g), self.facing_of(g, cvs), CLOSED))
            if self.cvs:
                closed = self.mode == "draw"
                out.append((self.cvs, COLORS["closing" if closed else "outline"], (0.0, 0.0, 0.0),
                            DOTS | (CLOSED | HIGHLIGHT if closed else 0)))
            if self.mode == "draw" and self.center is not None:
                out.append(([self.center], COLORS["closing"], self.normal or (0.0, 0.0, 0.0), MARKER | HIGHLIGHT))
                if len(self.stroke) >= 2:
                    out.append((self.stroke, COLORS["stroke"], (0.0, 0.0, 0.0), 0))
        return out, fill

    def redraw(self):
        from maya.api import OpenMaya as om
        with _NoUndo():
            if not self.groom:
                self.clear_preview()
                return
            items, fill = self.outlines()
            node = om.MFnDependencyNode(om.MSelectionList().add(self._node()).getDependNode(0))
            points = [om.MVector(*p) for cvs, _c, _n, _s in items for p in cvs]
            node.findPlug("points", False).setMObject(om.MFnVectorArrayData().create(om.MVectorArray(points)))
            node.findPlug("counts", False).setMObject(
                om.MFnIntArrayData().create(om.MIntArray([len(cvs) for cvs, _c, _n, _s in items])))
            node.findPlug("colors", False).setMObject(
                om.MFnVectorArrayData().create(om.MVectorArray([om.MVector(*c) for _p, c, _n, _s in items])))
            node.findPlug("normals", False).setMObject(
                om.MFnVectorArrayData().create(om.MVectorArray([om.MVector(*n) for _p, _c, n, _s in items])))
            node.findPlug("styles", False).setMObject(
                om.MFnIntArrayData().create(om.MIntArray([int(st) for _p, _c, _n, st in items])))
            node.findPlug("fillOpacity", False).setFloat(fill)
            node.findPlug("dotSize", False).setFloat(DOT_PIXELS * _pixel_scale())
        try:
            cmds.refresh(currentView=True, force=True)
        except RuntimeError:  # (no viewport: batch)
            pass


def _pixel_scale():
    """The screen's pixels per point (a high-DPI screen's dots as big to the eye)."""
    try:
        from groomlab.ui import qt
        app = qt.QtWidgets.QApplication.instance()
        return max(1.0, float(app.devicePixelRatio())) if app else 1.0
    except Exception:  # noqa: BLE001 (no Qt: batch)
        return 1.0


TOOL = RootTubeTool()


def _press():
    _guarded(TOOL.press)


def _drag():
    _guarded(TOOL.drag_event)


def _release():
    _guarded(TOOL.release)


def _finalize():
    _guarded(TOOL.leave)


def _undone(*_):
    _guarded(TOOL.undone)


def _redone(*_):
    _guarded(TOOL.redone)


def _guarded(fn):
    try:
        fn()
    except Exception as e:  # noqa: BLE001 (an event never ends in a bare traceback)
        import traceback
        traceback.print_exc()
        TOOL.say("Error: %s" % str(e).strip())


def start(groom, mode="place", **settings):
    """The root-first tube tool on `groom` ("place": draw roots; "edit": edit the root tubes' CVs;
    "select": select, move, duplicate and delete roots); `settings`: draw, length, segments,
    taper, snap, group, density."""
    TOOL.settings.update(settings)
    TOOL.start(groom, mode)
    return TOOL
