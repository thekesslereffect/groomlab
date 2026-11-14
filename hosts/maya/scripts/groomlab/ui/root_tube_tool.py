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
shared with a neighbour, sealed, move together; Ctrl+click an edge adds a CV there, on every
outline sharing the edge). The root last touched shows green. Finish gives each changed tube its
new root and updates the regions (groomlab.root_tubes.edit_root_tube).

Select root: every root shows its outline and its middle; clicking a middle selects that root
(green), dragging it carries the whole root over the scalp (with Snap, its CVs sealed onto a
neighbour stay put, the others slide along), the tube and the hierarchy under it moving with it on
release (one undo step, the regions updated). Duplicate: the selected root follows the cursor over
the scalp (no button held) and a click sets a copy of it, its tube and its hierarchy down there
(groomlab.root_tubes.duplicate_root_tube); Delete removes the selected root, its tube and its
hierarchy.

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

# Placing: the outline amber, closed (drawing its path) green. A finished root neutral; the one
# being edited, selected or carried green.
COLORS = {"outline": (1.0, 0.75, 0.15), "closing": (0.3, 1.0, 0.4), "active": (0.3, 1.0, 0.4),
          "stroke": (0.3, 0.85, 1.0), "root": (0.85, 0.85, 0.9), "neighbour": (0.55, 0.55, 0.6)}

MODES = ("place", "draw", "edit", "select", "duplicate")


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
        self.settings = {"draw": True, "length": 1.0, "segments": 12, "taper": 0.5, "snap": True, "group": None}
        self.neighbours = []    # snap targets: [(guide, cvs)]
        self.edits = {}         # edit mode: {guide: [cvs]}
        self.original = {}      # ... as they were
        self.drag = []          # edit mode: the CVs being dragged [(guide, index)]
        self.touched = set()    # edit mode: the roots last touched (green)
        self.roots = {}         # select mode: {guide: [cvs]}
        self.middles = {}       # ... {guide: (middle, normal)}
        self.selected = None    # ... the selected root's guide
        self.move = None        # ... a root being carried: {guide, start, base, held, outline}
        self.ghost = None       # duplicate mode: the copy's outline where it would go
        self.guides_shown = None
        self.listeners = []     # called with a message after each change (the Groom Tools card)
        self.last_made = None   # the tube just made: {guide, cvs, center, normal, drawn} (undoing it brings it back)
        self.restored = None    # ... the one an undo brought back (a redo puts it away again)
        self.callbacks = []     # Maya's Undo and Redo events, while the tool is up
        self.hover_filter = None

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
        self.reset()
        self.groom = groom
        self.mode = mode
        self.last_made = self.restored = None
        self.listen(True)
        if cmds.currentCtx() != CONTEXT:
            cmds.setToolTo(CONTEXT)
        if mode == "edit":
            self.begin_edit()
        elif mode == "select":
            self.begin_select()
        else:
            self.neighbours = root_tubes.snap_targets(groom) if self.settings["snap"] else []
            self.say("Click the root's outline on the scalp, one CV at a time; click the first CV to close it.")
        self.redraw()

    def reset(self):
        self.cvs, self.stroke, self.drag = [], [], []
        self.center = self.normal = None
        self.edits, self.original, self.touched = {}, {}, set()
        self.roots, self.middles, self.move, self.ghost = {}, {}, None, None
        self.hover(False)
        self.show_guides(True)
        self.clear_preview()

    def leave(self):
        """The tool put away: its preview removed, the guides shown again (edits not finished are
        dropped)."""
        self.reset()
        self.mode = "place"
        self.selected = None
        self.last_made = self.restored = None
        self.listen(False)

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
        last = self.last_made
        if not last or self.mode != "place" or self.cvs:
            return
        if last["guide"] in root_tubes.root_tubes(self.groom):
            return
        self.last_made = None
        self.restored = last
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
        back = self.restored
        if not back or self.mode not in ("place", "draw"):
            return
        if back["guide"] not in root_tubes.root_tubes(self.groom):
            return
        self.restored = None
        self.last_made = back
        self.mode = "place"
        self.cvs, self.stroke, self.center = [], [], None
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        self.redraw()
        self.say("Tube redone. Draw the next root.")

    def active(self):
        return cmds.currentCtx() == CONTEXT

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
                                              segments=s["segments"], taper=s["taper"], group=s["group"])
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
        self.neighbours = root_tubes.snap_targets(self.groom) if s["snap"] else []
        self.redraw()
        self.say("Made root tube %s (its own region on %s). Draw the next root." % (guide, root_tubes.REGION_MAP))
        return guide

    def undo_cv(self):
        """The last CV taken back (the path dropped first in draw mode)."""
        if self.mode == "draw":
            self.mode, self.stroke, self.center = "place", [], None
        elif self.mode == "place" and self.cvs:
            self.cvs.pop()
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
        self.touched = set()
        self.show_guides(False)
        self.say("Editing %d root%s: drag their CVs on the scalp (Ctrl+click an edge to add one), then Finish edit." %
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

    def press_edit(self, x, y):
        if "ctrl" in self.modifier():
            return self.insert_cv(x, y)
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
        tol = self._tolerance()
        # A CV shared with a neighbour (sealed) moves with it.
        self.drag = [(g, i) for g, cvs in self.edits.items() for i, p in enumerate(cvs)
                     if root_tubes.tm._dist(p, picked) <= tol]
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
        self.edits, self.original, self.drag, self.touched = {}, {}, [], set()
        self.show_guides(True)
        self.mode = "place"
        self.neighbours = root_tubes.snap_targets(self.groom) if self.settings["snap"] else []
        self.redraw()
        self.say("Finished: %d root%s changed, the regions updated." % (len(changed), "" if len(changed) == 1 else "s"))
        return len(changed)

    # -- Selecting, carrying, duplicating and deleting roots ------------------------------

    def begin_select(self):
        self.reload_roots()
        if not self.roots:
            self.say("The groom has no root tubes: draw one first.")
            return
        self.say("Click a root's middle to select it; drag it to move the root over the scalp.")

    def reload_roots(self):
        """The roots as they are (their outlines and middles), the selection kept while its root is."""
        self.roots, self.middles = {}, {}
        for g in root_tubes.root_tubes(self.groom):
            try:
                cvs = [tuple(p) for p in root_tubes.root_outline(self.groom, g)]
                self.roots[g] = cvs
                self.middles[g] = root_tubes._scalp_surface(self.groom)(root_tubes._centroid(cvs))
            except (RuntimeError, ValueError):
                continue
        if self.selected not in self.roots:
            self.selected = None
            if self.mode == "duplicate":
                self.mode = "select"
                self.hover(False)
        self.redraw()

    def press_select(self, x, y):
        best = None
        for g, (middle, _n) in self.middles.items():
            sx, sy = self.to_screen(middle)
            d = math.hypot(sx - x, sy - y)
            if d <= PICK_PIXELS and (best is None or d < best[0]):
                best = (d, g)
        self.move = None
        if best is None:
            self.selected = None
            self.redraw()
            self.say("Nothing selected: click a root's middle.")
            return
        g = best[1]
        self.selected = g
        hit = self.hit(x, y)
        base = self.roots[g]
        held = []
        if self.settings["snap"]:
            others = [(h, cvs) for h, cvs in self.roots.items() if h != g]
            held = root_tubes.sealed(self.groom, g, base, others)
        self.move = {"guide": g, "press": hit[0] if hit else self.middles[g][0], "base": list(base), "held": held,
                     "outline": list(base)}
        self.redraw()
        self.say("Root %s selected%s: drag to move it; Duplicate or Delete it." %
                 (g, " (%d CV%s sealed to a neighbour stay put)" % (len(held), "" if len(held) == 1 else "s")
                  if held else ""))

    def drag_select(self, x, y):
        if not self.move:
            return
        hit = self.hit(x, y)
        if hit is None:
            return
        tm = root_tubes.tm
        m = self.move
        middle = self.middles[m["guide"]][0]
        to = tm._add(middle, tm._sub(hit[0], m["press"]))
        m["outline"] = root_tubes.carried(self.groom, m["base"], middle, to, m["held"])
        self.roots[m["guide"]] = m["outline"]
        self.redraw()

    def release_select(self):
        m, self.move = self.move, None
        if not m or m["outline"] == m["base"]:
            return
        try:
            root_tubes.edit_root_tube(self.groom, m["guide"], m["outline"])
        except (RuntimeError, ValueError) as e:
            self.say("The root wasn't moved: %s" % str(e).strip())
        self.reload_roots()
        self.say("Root %s moved, the regions updated." % m["guide"])

    def begin_duplicate(self):
        """The selected root follows the cursor over the scalp; a click sets a copy down there."""
        if self.mode not in ("select", "duplicate") or self.selected not in self.roots:
            self.say("Select a root first (Select root, then click its middle).")
            return False
        self.mode = "duplicate"
        self.ghost = None
        self.hover(True)
        self.redraw()
        self.say("Move the cursor over the scalp and click to set the copy of root %s down." % self.selected)
        return True

    def end_duplicate(self):
        if self.mode == "duplicate":
            self.mode = "select"
            self.ghost = None
            self.hover(False)
            self.redraw()

    def follow(self, x, y):
        """Duplicate mode: the copy's outline under view point (x, y)."""
        if self.selected not in self.roots:
            return
        hit = self.hit(x, y)
        if hit is None:
            return
        self.ghost = (hit[0], root_tubes.carried(self.groom, self.roots[self.selected],
                                                 self.middles[self.selected][0], hit[0]))
        self.redraw()

    def place_copy(self):
        if not self.ghost:
            self.say("Click on the scalp to set the copy down.")
            return None
        to, _outline = self.ghost
        try:
            new = root_tubes.duplicate_root_tube(self.groom, self.selected, to)
        except (RuntimeError, ValueError) as e:
            self.say("The root wasn't duplicated: %s" % str(e).strip())
            return None
        source = self.selected
        self.selected = int(new)
        self.end_duplicate()
        self.reload_roots()
        self.say("Duplicated root %s as %s (its tube and hierarchy with it); the copy is selected." % (source, new))
        return new

    def delete_selected(self):
        if self.mode not in ("select", "duplicate") or self.selected not in self.roots:
            self.say("Select a root first (Select root, then click its middle).")
            return 0
        g = self.selected
        self.end_duplicate()
        root_tubes.delete_root_tubes(self.groom, [g])
        self.selected = None
        self.reload_roots()
        self.say("Deleted root %s, its tube and the hierarchy under it." % g)
        return 1

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

    def _marker(self, center, normal, radius, color, name="glRootCenter"):
        tm = root_tubes.tm
        a = tm._unit(tm._cross(normal, (1.0, 0.0, 0.0) if abs(normal[0]) < 0.9 else (0.0, 1.0, 0.0)))
        b = tm._cross(normal, a)
        ring = [tm._add(center, tm._add(tm._mul(a, radius * math.cos(k * math.pi / 6.0)),
                                        tm._mul(b, radius * math.sin(k * math.pi / 6.0)))) for k in range(12)]
        self._curve(name + "Ring", ring, color, closed=True, cvs=False)
        self._curve(name, [center, tm._add(center, tm._mul(normal, radius * 0.01))], color, cvs=True, width=4.0)

    def redraw(self):
        tm = root_tubes.tm
        with _NoUndo():
            self.clear_preview()
            if not self.groom:
                return
            if self.mode == "edit":
                for g, cvs in self.edits.items():
                    self._curve("glRootEdit_%s" % g, cvs, COLORS["active" if g in self.touched else "root"], closed=True)
            elif self.mode in ("select", "duplicate"):
                for g, cvs in self.roots.items():
                    picked = g == self.selected
                    self._curve("glRootSelect_%s" % g, cvs, COLORS["active" if picked else "root"], closed=True,
                                cvs=picked, width=3.0 if picked else 1.5)
                    middle, normal = self.middles[g]
                    reach = max(tm._dist(middle, p) for p in cvs) if cvs else 0.1
                    self._marker(middle, normal, 0.12 * reach, COLORS["active" if picked else "root"],
                                 "glRootMiddle_%s" % g)
                if self.mode == "duplicate" and self.ghost:
                    self._curve("glRootCopy", self.ghost[1], COLORS["active"], closed=True, width=3.0)
            else:
                for _g, cvs in self.neighbours:
                    self._curve("glRootNeighbour", cvs, COLORS["neighbour"], closed=True, cvs=False, width=1.0)
                if self.cvs:
                    closed = self.mode == "draw"
                    self._curve("glRootOutline", self.cvs, COLORS["closing" if closed else "outline"], closed=closed)
                if self.mode == "draw" and self.center is not None:
                    reach = max(tm._dist(self.center, p) for p in self.cvs) if self.cvs else 0.1
                    self._marker(self.center, self.normal, 0.15 * reach, COLORS["closing"])
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
    taper, snap, group."""
    TOOL.settings.update(settings)
    TOOL.start(groom, mode)
    return TOOL
