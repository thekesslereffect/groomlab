# SPDX-License-Identifier: GPL-3.0-or-later
"""The Groom Tools' operators: picking a tool (a tile, the pie menu, a key), the tool itself in
the 3D viewport (a modal operator: the brush ring under the cursor, strokes with the pen's
pressure, Ctrl the opposite action, Shift selecting guides with a guide brush (a drag paints them
in, a click adds the guides in the ring; painting, Shift smooths), lasso, box and
click selection, B / M held + drag for the radius / strength, Page Up / Down for the level, S for
Select, Y for the pie menu, Ctrl+Z / Ctrl+Shift+Z the helper's undo and redo, Esc to leave),
guide group and map edits, and the pie menu standing in for Maya's marking menu (the same radial
layout, the rest of the catalog below it).

The modal operator only translates Blender's events: ToolSession does the work, over plain
values, so the tests drive it with synthetic events and a synthetic view."""

import os

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty,
                       StringProperty)
from bpy_extras.io_utils import ImportHelper

from . import actions, display, groom, icons, process, props, tools
from . import definitions as defs
from .helper_client import HelperError, sentence

# The marking menu (engine/ui/marking_menu.json, Maya's hotkeys.py): the brushes reached for most
# around it, the rest of the catalog after them, then its tail.
RADIAL = [(r["position"], r["tool"]) for r in defs.get("markingMenu/radial")]
# Blender lays a pie's items out in this order of directions.
PIE_ORDER = ("W", "E", "S", "N", "NW", "NE", "SW", "SE")
PIE_KEY = "Y"
# The keys every host shares (the core's shortcuts.h, the helper's "shortcuts"): Blender's rows, kept
# here for when the helper isn't running (Preferences); a test checks them against the helper's.
SHORTCUTS = (
    ("Ctrl+Z / Ctrl+Shift+Z", "Undo / redo the groom's edits"),
    ("B + drag", "The brush radius"),
    ("M + drag", "The brush strength"),
    ("Ctrl (stroke)", "The opposite: Length shortens, Lift lays down, Shape shrinks; painting, toward 0 (unfreeze, erase)"),
    ("Shift (painting, Shape)", "Painting maps or guide attributes, or the Shape brush: smooth instead"),
    ("S", "Select guides (Shift: toggle, Ctrl: deselect)"),
    ("Shift + drag", "A guide brush: paint guides into the selection, the rest frozen (Shift + click: add the guides in the ring; on none: deselect and unfreeze all)"),
    ("Page Up / Page Down", "The brush level (parents / children)"),
    ("Y", "The pie menu: every brush"),
    ("Esc / right-click", "Cancel the stroke; Esc again leaves the tool"),
    ("Tab", "Add a node at the cursor"),
    ("B", "Bypass the selected nodes (again: turn them back on)"),
    ("V", "View the groom up to the node"),
)


def shortcuts():
    """[(keys, what)]: the helper's rows of the shared table when it runs, else SHORTCUTS."""
    from . import process
    if process.running():
        try:
            return [(r["keys"], r["action"]) for r in process.call("shortcuts")["shortcuts"]]
        except Exception:
            pass
    return list(SHORTCUTS)


# (keymap, operator, (property, value) or None): the add-on's keys the preferences show.
KEYMAP_ITEMS = (("Object Mode", "wm.call_menu_pie", ("name", "GROOMLAB_MT_pie")),)
NAV_PASS = {"MIDDLEMOUSE", "WHEELUPMOUSE", "WHEELDOWNMOUSE", "WHEELINMOUSE", "WHEELOUTMOUSE", "TRACKPADPAN",
            "TRACKPADZOOM", "MOUSEROTATE", "MOUSESMARTZOOM", "NDOF_MOTION"}
SLIDER_KEYS = {"B": "radius", "M": "strength", "F": "radius"}


def _fail(op, e):
    op.report({"ERROR"}, sentence(e.message if isinstance(e, HelperError) else str(e)))
    return {"CANCELLED"}


def active(context):
    return tools.active(context)


# ---- Where the mouse is: a 3D view's region (patched by the tests) ------------------------------

def _inside(region, x, y):
    return region.x <= x < region.x + region.width and region.y <= y < region.y + region.height


def locate(context, event):
    """(region, region_3d, x, y) of the 3D view under the mouse (region pixels, origin bottom left),
    or None over anything else (another editor, a header, the toolbar or sidebar)."""
    window = context.window
    if window is None:
        return None
    mx, my = event.mouse_x, event.mouse_y
    for area in window.screen.areas:
        if area.type != "VIEW_3D" or not (area.x <= mx < area.x + area.width and area.y <= my < area.y + area.height):
            continue
        for region in area.regions:
            if region.type != "WINDOW" and region.width > 1 and region.height > 1 and _inside(region, mx, my):
                return None  # a header, toolbar, sidebar or the HUD over the view
        space = area.spaces.active
        quads = list(getattr(space, "region_quadviews", None) or [])
        windows = [r for r in area.regions if r.type == "WINDOW"]
        for i, region in enumerate(windows):
            if _inside(region, mx, my):
                rv3d = quads[i] if len(windows) == 4 and len(quads) == 4 else space.region_3d
                return region, rv3d, float(mx - region.x), float(my - region.y)
    return None


def blender_tool(context):
    """The id of Blender's own active tool in the 3D view (builtin.select_box...), None without one."""
    try:
        return context.workspace.tools.from_space_view3d_mode(context.mode, create=False).idname
    except Exception:  # noqa: BLE001 - no workspace (background), or no tool for the mode
        return None


def over_toolbar(context, event):
    """Whether the mouse is over a 3D view's toolbar (Blender's tools)."""
    window = context.window
    if window is None:
        return False
    for area in window.screen.areas:
        if area.type == "VIEW_3D":
            for region in area.regions:
                if region.type == "TOOLS" and region.width > 1 and _inside(region, event.mouse_x, event.mouse_y):
                    return True
    return False


# ---- The tool, event by event --------------------------------------------------------------------

class ToolSession(object):
    """The GroomLab tool in the viewport: Blender's events into the helper's hover, strokes,
    selection, undo and the tool's settings."""

    def __init__(self):
        self.stroking = None      # the scalp while a stroke is live
        self.points = []          # mouse samples not sent yet
        self.lasso = None         # (scalp, view, points, mode) while selecting ("brush": a guide brush's Shift painting)
        self.painted_at = None    # where the painting last picked
        self.slider = None        # (setting, x at the press, value at the press, (view, x, y) of the ring)
        self.held = set()         # B / M / F held
        self.hovering = None      # the scalp hovered last
        self.view = None
        self.cursor = False       # the crosshair set over a viewport
        self.blender_tool = None  # Blender's active tool when ours started

    def tool(self, context):
        return context.scene.groomlab_tools.tool

    def scalp(self, context):
        s = active(context)
        return s if s is not None and tools.is_hierarchy(s) else None

    # -- Events --

    def event(self, context, event, locate_fn=None):
        """One event: {'RUNNING_MODAL'} (ours), {'PASS_THROUGH'} (Blender's) or {'FINISHED'} (the tool left)."""
        locate_fn = locate_fn or locate
        if not self.tool(context):
            self.leave(context)
            return {"FINISHED"}
        scalp = self.scalp(context)
        if scalp is None or context.mode != "OBJECT":  # no groom, or another mode: Blender's tools again
            self.leave(context)
            return {"FINISHED"}
        kind, value = event.type, event.value
        # Blender's tools again: one picked (the toolbar, W, Shift+Space...) leaves ours, the event
        # Blender's (a click on the tool already active included).
        current = blender_tool(context)
        if self.blender_tool is None:
            self.blender_tool = current
        idle = self.stroking is None and self.lasso is None and self.slider is None
        if current != self.blender_tool or (idle and value == "PRESS" and (
                (kind == "LEFTMOUSE" and over_toolbar(context, event)) or
                (kind == "W" and not (event.ctrl or event.alt or event.shift or event.oskey)
                 and locate_fn(context, event) is not None))):
            self.leave(context, clear_tool=True)
            return {"FINISHED", "PASS_THROUGH"}
        if kind in NAV_PASS or kind.startswith("NDOF") or kind.startswith("NUMPAD"):
            return {"PASS_THROUGH"}
        if kind in ("MOUSEMOVE", "INBETWEEN_MOUSEMOVE"):
            return self.move(context, scalp, event, locate_fn, kind == "MOUSEMOVE")
        if kind in ("LEFTMOUSE", "PEN"):
            if value == "PRESS":
                return self.press(context, scalp, event, locate_fn)
            if value == "RELEASE":
                return self.release(context, scalp, event, locate_fn)
            return {"RUNNING_MODAL"} if (self.stroking or self.lasso or self.slider) else {"PASS_THROUGH"}
        busy = self.stroking is not None or self.lasso is not None or self.slider is not None
        lengths = self.tool(context) == "lengths"
        if lengths and kind == "RIGHTMOUSE":
            # The Length Paths tool: right-drag sets a key's length.
            if value == "PRESS":
                return self.press(context, scalp, event, locate_fn, adjust=True)
            if value == "RELEASE":
                return self.release(context, scalp, event, locate_fn)
            return {"RUNNING_MODAL"} if busy else {"PASS_THROUGH"}
        if lengths and not busy and value == "PRESS" and kind in ("RET", "NUMPAD_ENTER", "ESC"):
            # Enter applies the preview; Esc drops it (with none, Esc leaves the tool as ever).
            state = tools.length_state(scalp)
            if kind != "ESC" or state.get("edited"):
                try:
                    tools.length_edit(scalp, "cancel" if kind == "ESC" else "apply")
                except HelperError as e:
                    tools.status(context.scene, e.message)
                return {"RUNNING_MODAL"}
        if kind in ("ESC", "RIGHTMOUSE") and value == "PRESS":
            if busy:
                self.cancel(context)
                return {"RUNNING_MODAL"}
            if kind == "ESC":
                self.leave(context, clear_tool=True)
                return {"FINISHED"}
            return {"PASS_THROUGH"}
        over = locate_fn(context, event) is not None
        if kind in SLIDER_KEYS and not (event.ctrl or event.alt or event.oskey):
            if value == "PRESS" and over:
                self.held.add(kind if kind != "F" or not event.shift else "M")
                return {"RUNNING_MODAL"}
            if value == "RELEASE":
                was = bool(self.held)
                self.held.clear()
                return {"RUNNING_MODAL"} if was else {"PASS_THROUGH"}
            return {"RUNNING_MODAL"} if over else {"PASS_THROUGH"}
        if kind == "Z" and value == "PRESS" and (event.ctrl or event.oskey) and not busy and over:
            self.undo(context, scalp, redo=event.shift)  # over the viewport: the groom's own undo
            return {"RUNNING_MODAL"}
        if not over or value != "PRESS":
            return {"PASS_THROUGH"}
        if kind == pie_key() and not (event.ctrl or event.alt or event.shift):
            if not bpy.app.background:
                bpy.ops.wm.call_menu_pie("INVOKE_DEFAULT", name=GROOMLAB_MT_pie.bl_idname)
            return {"RUNNING_MODAL"}
        if kind == "S" and not (event.ctrl or event.alt or event.oskey or busy):
            self.pick(context, "select")
            return {"RUNNING_MODAL"}
        if kind in ("PAGE_UP", "PAGE_DOWN") and not busy:
            step_level(context, -1 if kind == "PAGE_UP" else 1)
            return {"RUNNING_MODAL"}
        return {"PASS_THROUGH"}

    def located(self, context, scalp, event, locate_fn):
        where = locate_fn(context, event)
        if where is None:
            return None
        region, rv3d, x, y = where
        return tools.view_of(region, rv3d, scalp), x, y

    def move(self, context, scalp, event, locate_fn, flush):
        if self.slider is not None:
            self.drag_slider(context, scalp, event)
            return {"RUNNING_MODAL"}
        where = self.located(context, scalp, event, locate_fn)
        pressure = float(getattr(event, "pressure", 1.0) or 1.0)
        if self.stroking is not None:
            if where is not None:
                self.points.append([where[1], where[2], pressure])
            if flush:
                self.flush(context)
            return {"RUNNING_MODAL"}
        if self.lasso is not None:
            if where is not None:
                points = self.lasso[2]
                points.append([where[1], where[2]])
                if self.lasso[3] == "brush":
                    self.paint_selection(context, scalp, where)
            return {"RUNNING_MODAL"}
        self.set_cursor(context, where is not None)
        if where is None:
            if self.hovering is not None:
                tools.hover_clear(self.hovering)
                self.hovering = None
                display.redraw()
            return {"PASS_THROUGH"}
        if flush and self.tool(context) != "select":
            try:
                tools.hover(scalp, where[0], where[1], where[2])
                self.hovering = scalp
            except HelperError as e:
                tools.status(context.scene, e.message)
            display.redraw(viewport_only=True)
        return {"PASS_THROUGH"}

    def press(self, context, scalp, event, locate_fn, adjust=False):
        where = self.located(context, scalp, event, locate_fn)
        if where is None:
            return {"PASS_THROUGH"}
        view, x, y = where
        t = context.scene.groomlab_tools
        if self.held:
            setting = SLIDER_KEYS.get(sorted(self.held)[0], "radius") if "M" not in self.held else "strength"
            self.slider = (setting, float(event.mouse_x), float(t.radius if setting == "radius" else t.strength),
                           (view, x, y))
            return {"RUNNING_MODAL"}
        if self.tool(context) == "select":
            mode = "toggle" if event.shift else "deselect" if event.ctrl else "replace"
            self.lasso = (scalp, view, [[x, y]], mode)
            return {"RUNNING_MODAL"}
        info = tools.tool_info(self.tool(context))
        if event.shift and info is not None and info.get("shiftSelects"):
            # Shift on a guide brush: selecting guides without leaving it (a drag paints them in with
            # the ring, a click adds the guides in it: release).
            self.lasso = (scalp, view, [[x, y]], "brush")
            self.painted_at = None
            return {"RUNNING_MODAL"}
        pressure = float(getattr(event, "pressure", 1.0) or 1.0)
        try:
            tools.stroke_begin(scalp, view, x, y, pressure=pressure, ctrl=event.ctrl, shift=event.shift, adjust=adjust)
        except HelperError as e:
            tools.status(context.scene, e.message)
            return {"RUNNING_MODAL"}
        self.stroking = scalp
        self.points = []
        display.apply_strand_visibility(scalp, stroking=True)
        return {"RUNNING_MODAL"}

    def flush(self, context):
        if self.stroking is None or not self.points:
            return
        points, self.points = self.points, []
        try:
            tools.stroke_drag(self.stroking, points)
        except HelperError as e:
            tools.status(context.scene, e.message)
        display.redraw(viewport_only=True)

    def release(self, context, scalp, event, locate_fn):
        if self.slider is not None:
            self.slider = None
            return {"RUNNING_MODAL"}
        if self.stroking is not None:
            where = self.located(context, scalp, event, locate_fn)
            if where is not None:
                self.points.append([where[1], where[2], float(getattr(event, "pressure", 1.0) or 1.0)])
            self.flush(context)
            stroked, self.stroking = self.stroking, None
            try:
                tools.stroke_end(stroked)
                if self.tool(context) == "lengths":
                    tools.length_state(stroked)  # the Paths box follows the key placed or dragged
                    tools.evaluate_live(stroked)  # the guides at their previewed lengths
            except HelperError as e:
                tools.status(context.scene, e.message)
            display.apply_strand_visibility(stroked)
            return {"RUNNING_MODAL"}
        if self.lasso is not None:
            lasso_scalp, view, points, mode = self.lasso
            self.lasso = None
            shape = context.scene.groomlab_tools.select_shape
            span = max(abs(p[0] - points[0][0]) + abs(p[1] - points[0][1]) for p in points)
            if mode == "brush":                          # a guide brush's Shift
                try:
                    if span < 4.0:                       # a click: the guides in the ring added, the rest frozen
                        ring = self.ring(context, points[0])
                        if not tools.select(lasso_scalp, view, ring, "add").get("picked"):
                            tools.select(lasso_scalp, view, ring, "replace")  # on none: all deselected, unfrozen
                    else:                                # painted: the rest frozen, one undo step
                        tools.selection_set(lasso_scalp, mode="add", solo=True)
                except HelperError as e:
                    tools.status(context.scene, e.message)
                display.redraw(viewport_only=True)
                return {"RUNNING_MODAL"}
            if span < 4.0:
                points = points[:1]                      # a click
            elif shape == "BOX":
                points = [points[0], points[-1]]         # a box's corners
            elif len(points) < 3:
                points = [points[0], points[-1]]
            try:
                r = tools.select(lasso_scalp, view, points, mode)
                tools.status(context.scene, "%d guides selected" % r.get("count", 0))
            except HelperError as e:
                tools.status(context.scene, e.message)
            return {"RUNNING_MODAL"}
        return {"PASS_THROUGH"}

    @staticmethod
    def ring(context, at):
        """The brush ring around `at` (view pixels) as a 24-point lasso."""
        import math
        r = max(1.0, float(context.scene.groomlab_tools.radius))
        return [[at[0] + r * math.cos(2 * math.pi * k / 24), at[1] + r * math.sin(2 * math.pi * k / 24)] for k in range(24)]

    def paint_selection(self, context, scalp, where):
        """A brush's Ctrl drag: once it has moved, the guides under the ring join the selection
        (no undo step: the release makes the one), a dab every quarter of the radius."""
        view, x, y = where
        points = self.lasso[2]
        t = context.scene.groomlab_tools
        x0, y0 = points[0]
        if self.painted_at is None and abs(x - x0) + abs(y - y0) < 4.0:
            return  # a click so far
        at = self.painted_at or (x0, y0)
        if self.painted_at is not None and abs(x - at[0]) + abs(y - at[1]) < 0.25 * t.radius:
            return
        self.painted_at = (x, y)
        ring = self.ring(context, (x, y))
        try:
            tools.select(scalp, view, ring, "add", solo=False)
            tools.hover(scalp, view, x, y)  # the ring where the cursor is
        except HelperError as e:
            tools.status(context.scene, e.message)
        display.redraw(viewport_only=True)

    def drag_slider(self, context, scalp, event):
        """B / M held + drag: the radius (a pixel a pixel) or the strength (200 px for 1), the ring
        redrawn where the drag started, the value in the status line."""
        setting, x0, v0, (view, rx, ry) = self.slider
        t = context.scene.groomlab_tools
        dx = float(event.mouse_x) - x0
        if setting == "radius":
            t.radius = max(1, int(round(v0 + dx)))
            tools.status(context.scene, "Radius %d px" % t.radius)
        else:
            info = tools.tool_info(t.tool)
            lo = -1.0 if info and info.get("signed") else 0.0
            t.strength = min(1.0, max(lo, v0 + dx / 200.0))
            tools.status(context.scene, "Strength %.2f" % t.strength)
        if t.tool != "select":
            try:
                tools.hover(scalp, view, rx, ry)
            except HelperError:
                pass
        display.redraw()

    def cancel(self, context):
        if self.stroking is not None:
            stroked, self.stroking = self.stroking, None
            self.points = []
            try:
                tools.stroke_cancel(stroked)
            except HelperError as e:
                tools.status(context.scene, e.message)
            display.apply_strand_visibility(stroked)
        self.lasso = None
        if self.slider is not None:
            setting, _x0, v0, _ring = self.slider
            t = context.scene.groomlab_tools
            if setting == "radius":
                t.radius = int(v0)
            else:
                t.strength = v0
            self.slider = None
        display.redraw()

    def undo(self, context, scalp, redo=False):
        try:
            r = tools.undo(scalp, redo=redo)
        except HelperError as e:
            tools.status(context.scene, e.message)
            return None
        tools.status(context.scene, ("Redo" if redo else "Undo") + (" done" if r.get("done") else ": nothing to do"))
        return r

    def pick(self, context, tool):
        try:
            tools.pick(context, tool)
        except HelperError as e:
            tools.status(context.scene, e.message)

    def set_cursor(self, context, over):
        """A crosshair over the viewport (the ring is the brush), Blender's cursor elsewhere."""
        window = getattr(context, "window", None)
        if window is None or over == self.cursor:
            return
        try:
            if over:
                window.cursor_modal_set("CROSSHAIR")
            else:
                window.cursor_modal_restore()
        except Exception:
            pass
        self.cursor = over

    def leave(self, context, clear_tool=False):
        self.set_cursor(context, False)
        if self.stroking is not None:
            self.cancel(context)
        if self.hovering is not None:
            tools.hover_clear(self.hovering)
            self.hovering = None
        if clear_tool:
            with props.quiet():
                context.scene.groomlab_tools.tool = ""
            display.mark(active(context))  # the views without the tool (no paint view, no tube wires)
        display.redraw()


def step_level(context, delta):
    """Page Up / Page Down: the Group choice one level up or down (Maya's step_level)."""
    t = context.scene.groomlab_tools
    level, _group = tools.group_target(context.scene)
    groups = list(t.groups)
    levels = sorted({g.level for g in groups})
    if not levels:
        return
    target = min(levels, key=lambda lv: abs(lv - (max(level, 0) + delta)))
    for g in groups:
        if g.level == target:
            t.group = g.name
            break


def pie_key():
    """The key the pie menu is bound to (the add-on keymap's, as the user set it)."""
    wm = bpy.context.window_manager
    for kc in (wm.keyconfigs.user, wm.keyconfigs.addon):
        if kc is None:
            continue
        km = kc.keymaps.get("Object Mode")
        if km is None:
            continue
        for kmi in km.keymap_items:
            if kmi.idname == "wm.call_menu_pie" and getattr(kmi.properties, "name", "") == "GROOMLAB_MT_pie" and kmi.active:
                return kmi.type
    return PIE_KEY


_session = [None]   # the running tool's ToolSession (one at a time)


def running():
    return _session[0] is not None


def stroking():
    return _session[0] is not None and _session[0].stroking is not None


class GROOMLAB_OT_tool_modal(bpy.types.Operator):
    """The active GroomLab tool in the 3D viewport (a tile or the pie menu starts it)"""
    bl_idname = "groomlab.tool_modal"
    bl_label = "GroomLab Tool"

    def invoke(self, context, event):
        if _session[0] is not None:
            return {"CANCELLED"}
        if context.window is None:
            return {"CANCELLED"}
        _session[0] = ToolSession()
        context.window_manager.modal_handler_add(self)
        self._status(context)
        return {"RUNNING_MODAL"}

    def _status(self, context):
        tool = context.scene.groomlab_tools.tool
        info = tools.tool_info(tool)
        label = info["label"] if info else tool
        text = ("GroomLab %s: drag over the groom · Shift smooth · Ctrl+click selects the guides in the ring (the rest frozen) · "
                "Ctrl+drag paint selection · Ctrl+Shift+drag lasso · B/M + drag radius/strength · %s pie · S select · Ctrl+Z undo · "
                "Esc or W leave" % (label, pie_key()))
        try:
            context.workspace.status_text_set(text)
        except Exception:
            pass
        self._shown = tool

    def modal(self, context, event):
        session = _session[0]
        if session is None:
            return self._end(context)
        try:
            result = session.event(context, event)
        except Exception as e:  # noqa: BLE001 - a modal operator must never leave Blender stuck
            session.leave(context)
            print("GroomLab tool: %r" % e)
            return self._end(context)
        if "FINISHED" in result:
            self._end(context)
            return result  # (with PASS_THROUGH: the event is Blender's too)
        if getattr(self, "_shown", None) != context.scene.groomlab_tools.tool:
            self._status(context)
        return result

    def _end(self, context):
        _session[0] = None
        try:
            context.workspace.status_text_set(None)
        except Exception:
            pass
        display.redraw()
        return {"FINISHED"}

    def cancel(self, context):
        if _session[0] is not None:
            _session[0].leave(context)
        _session[0] = None


def stop_tool(context):
    """Leaves the GroomLab tool (Maya's palette stop_tool): no tool, the viewport's tool ended, and
    the views as they are without it (the paint view and the Shape brush's tube wires gone)."""
    with props.quiet():
        context.scene.groomlab_tools.tool = ""
    if _session[0] is not None:
        _session[0].leave(context)
    display.mark(active(context))
    display.redraw()


def start_modal(context):
    """The tool in the viewport (from a tile, the pie or the menu), once."""
    if running() or bpy.app.background or context.window is None:
        return False
    try:
        return "RUNNING_MODAL" in bpy.ops.groomlab.tool_modal("INVOKE_DEFAULT")
    except RuntimeError:
        return False


class GroomToolOperator(bpy.types.Operator):
    @classmethod
    def poll(cls, context):
        s = active(context)
        return s is not None and tools.is_hierarchy(s)


class GROOMLAB_OT_tool(bpy.types.Operator):
    """Use this GroomLab tool: drag over the groom in the viewport"""
    bl_idname = "groomlab.tool"
    bl_label = "GroomLab Tool"
    bl_options = {"REGISTER"}

    tool: StringProperty(name="Tool")

    @classmethod
    def description(cls, context, properties):
        info = tools.tool_info(properties.tool)
        return "%s: %s" % (info["label"], info["hint"]) if info else "Use this GroomLab tool"

    def execute(self, context):
        try:
            tools.catalog()
        except HelperError as e:
            return _fail(self, e)
        s = active(context)
        if s is not None and not tools.is_hierarchy(s):
            self.report({"WARNING"}, "This groom's guides are Blender curves: sculpt them with Sculpt Guides. "
                                     "GroomLab's tools work on grooms made with Create Groom from Selection")
            return {"CANCELLED"}
        try:
            tools.pick(context, self.tool)
        except HelperError as e:
            return _fail(self, e)
        if s is not None:
            start_modal(context)
        return {"FINISHED"}


class GROOMLAB_OT_tool_exit(bpy.types.Operator):
    """Leave the GroomLab tool (Blender's own tools again)"""
    bl_idname = "groomlab.tool_exit"
    bl_label = "Leave Tool"

    def execute(self, context):
        stop_tool(context)
        return {"FINISHED"}


class GROOMLAB_OT_tool_undo(GroomToolOperator):
    """Undo the groom's last edit (a stroke, a guide edit, a map): GroomLab's own undo"""
    bl_idname = "groomlab.tool_undo"
    bl_label = "Undo"

    redo: BoolProperty(name="Redo", default=False, options={"SKIP_SAVE"})

    def execute(self, context):
        try:
            r = tools.undo(active(context), redo=self.redo)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"} if r.get("done") else {"CANCELLED"}


class GROOMLAB_OT_step_level(GroomToolOperator):
    """Brush one guide level up (the parents) or down (the children)"""
    bl_idname = "groomlab.step_level"
    bl_label = "Step Level"

    delta: IntProperty(default=1)

    def execute(self, context):
        step_level(context, self.delta)
        return {"FINISHED"}


# ---- Guide groups and edits (Maya's "Guide groups & edits") --------------------------------------

class GROOMLAB_OT_group_new(GroomToolOperator):
    """New group (top-level, or a child group of another): Place and Draw add to it"""
    bl_idname = "groomlab.group_new"
    bl_label = "New Guide Group"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(name="Name", description="e.g. bangs, beard, flyaways")
    parent: StringProperty(name="Parent", description="Empty: a top-level group; a group's name: a child group of it")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        if not self.name.strip():
            self.report({"ERROR"}, "No group name: type a name for the group")
            return {"CANCELLED"}
        args = {"name": self.name.strip()}
        if self.parent.strip():
            args["parent"] = self.parent.strip()
        try:
            r = tools.guides_edit(active(context), "add_group", **args)
        except HelperError as e:
            return _fail(self, e)
        made = r.get("group") or self.name.strip()
        context.scene.groomlab_tools.group = made
        tools.status(context.scene, "New guides go into %s." % made)
        return {"FINISHED"}


GROUP_ACTIONS = [("RENAME", defs.label("group.rename"), defs.tip("group.rename")),
                 ("SELECT", defs.label("group.select"), defs.tip("group.select")),
                 ("RESET_SEGMENTS", defs.label("group.resetSegments"), defs.tip("group.resetSegments")),
                 ("DELETE", defs.label("group.delete"), defs.tip("group.delete")),
                 ("DEFAULT_COLOR", defs.label("group.defaultColor"), defs.tip("group.defaultColor")),
                 ("HIDE", "Show / Hide", "Show or hide the group in the viewport"),
                 ("PROMOTE", "Promote", "Move the group one level up"),
                 ("DEMOTE", "Demote", "Move the group one level down, under a sibling group")]


def _description(cls, properties, fallback=""):
    """An operator's tooltip: the definitions' tip of the action the call stands for."""
    return actions.tip_for(cls.bl_idname, properties, fallback)


class GROOMLAB_OT_group_edit(GroomToolOperator):
    """Edit the picked guide group"""
    bl_idname = "groomlab.group_edit"
    bl_label = "Edit Group"
    bl_options = {"REGISTER", "UNDO"}

    action: EnumProperty(items=GROUP_ACTIONS)
    group: StringProperty(name="Group", options={"SKIP_SAVE"})
    name: StringProperty(name="New Name", options={"SKIP_SAVE"})
    parent: StringProperty(name="New Parent", options={"SKIP_SAVE"})

    @classmethod
    def description(cls, context, properties):
        return _description(cls, properties, next((tip for k, _l, tip in GROUP_ACTIONS if k == properties.action), ""))

    def invoke(self, context, event):
        if not self.group:
            self.group = context.scene.groomlab_tools.group
        if self.action == "RENAME":
            self.name = self.group
            return context.window_manager.invoke_props_dialog(self)
        if self.action == "DEMOTE":
            return context.window_manager.invoke_props_dialog(self)
        if self.action == "DELETE":
            return context.window_manager.invoke_confirm(self, event)
        return self.execute(context)

    def execute(self, context):
        scalp = active(context)
        group = self.group or context.scene.groomlab_tools.group
        if not group or group == tools.ALL_GROUPS:
            self.report({"ERROR"}, "No group picked: pick a group first")
            return {"CANCELLED"}
        try:
            if self.action == "RENAME":
                tools.guides_edit(scalp, "rename_group", group=group, name=self.name.strip())
                context.scene.groomlab_tools.group = self.name.strip()
            elif self.action == "SELECT":
                tools.selection_set(scalp, group=group, mode="replace")
            elif self.action == "DELETE":
                tools.guides_edit(scalp, "remove_group", group=group)
            elif self.action == "DEFAULT_COLOR":
                tools.set_group_color(scalp, group, None)
            elif self.action == "RESET_SEGMENTS":
                segments = tools.reset_group_segments(scalp, group)
                tools.refresh_segments(scalp, context.scene)
                tools.status(context.scene, "Every guide of %s at %d segments (frozen ones kept)." % (group, segments))
            elif self.action == "HIDE":
                hidden = set(g.name for g in context.scene.groomlab_tools.groups if g.hidden)
                hidden ^= {group}
                tools.set_hidden_groups(scalp, sorted(hidden))
            elif self.action == "PROMOTE":
                tools.guides_edit(scalp, "promote_group", group=group)
            elif self.action == "DEMOTE":
                tools.guides_edit(scalp, "demote_group", group=group, parent=self.parent.strip())
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# Edit Guides' entries this operator runs (lists.json's groups/edits, by action id), then Blender's own.
GUIDE_EDIT_IDS = (("CHILDREN", "guides.children"), ("SYNC_POS", "guides.syncPos"), ("SYNC_NEG", "guides.syncNeg"),
                  ("MIRROR_SELECTION", "guides.mirrorSelection"), ("CLEAR", "guides.clearSelection"))
GUIDE_EXTRAS = [
    ("LOCK", "Lock", "Lock the selected guides: their children stop following them"),
    ("UNLOCK", "Unlock", "Unlock the selected guides"),
    ("FREEZE", "Freeze", "Freeze the selected guides: brushes leave them alone (they draw blue)"),
    ("UNFREEZE", "Unfreeze", "Unfreeze the selected guides"),
    ("UNFREEZE_ALL", "Unfreeze All", "Clear the freeze mask: every guide can be brushed again"),
    ("DELETE", "Delete Selected", "Delete the selected guides (and their children)"),
    ("SELECT_ALL", "Select All", "Select every guide of the picked group"),
]
GUIDE_EDITS = [(k, defs.label(a), defs.tip(a)) for k, a in GUIDE_EDIT_IDS] + GUIDE_EXTRAS


class GROOMLAB_OT_guides_edit(GroomToolOperator):
    """Edits on many guides (Maya's Edit Guides menu): one undo step each"""
    bl_idname = "groomlab.guides_edit"
    bl_label = "Edit Guides"
    bl_options = {"REGISTER", "UNDO"}

    action: EnumProperty(items=GUIDE_EDITS)

    @classmethod
    def description(cls, context, properties):
        return _description(cls, properties, next((tip for k, _l, tip in GUIDE_EDITS if k == properties.action), ""))

    def execute(self, context):
        scalp = active(context)
        t = context.scene.groomlab_tools
        level, group = tools.group_target(context.scene)
        a = self.action
        try:
            if a == "CHILDREN":
                args = {"count": int(t.children_count)}
                if t.selected_count:
                    args["selected"] = True
                elif group:
                    args["group"] = group
                tools.guides_edit(scalp, "generate_children", **args)
            elif a in ("SYNC_POS", "SYNC_NEG"):
                tools.guides_edit(scalp, "mirror_all", axis="x", center=0.0, fromPositive=a == "SYNC_POS")
            elif a == "MIRROR_SELECTION":
                if not t.selected_count:
                    self.report({"ERROR"}, "No guides selected: select some first (the Select tool, or a group's Select)")
                    return {"CANCELLED"}
                tools.mirror_selection(scalp)
            elif a in ("LOCK", "UNLOCK", "FREEZE", "UNFREEZE", "DELETE"):
                if not t.selected_count:
                    self.report({"ERROR"}, "No guides selected: select some first (the Select tool, or a group's Select)")
                    return {"CANCELLED"}
                op = {"LOCK": "lock", "UNLOCK": "unlock", "FREEZE": "freeze", "UNFREEZE": "unfreeze",
                      "DELETE": "delete_guides"}[a]
                tools.guides_edit(scalp, op)
            elif a == "UNFREEZE_ALL":
                tools.guides_edit(scalp, "unfreeze_all")
            elif a == "SELECT_ALL":
                if group:
                    tools.selection_set(scalp, group=group, mode="replace")
                else:
                    tools.selection_set(scalp, all=True, mode="replace")
            elif a == "CLEAR":
                tools.selection_set(scalp, ids=[], mode="replace")
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# ---- Maps (Maya's Paint tab) ---------------------------------------------------------------------

_TARGET_ITEMS = []  # Blender keeps no reference to a dynamic enum's items: these are held here
_OTHER_ITEMS = []
FOR_TIP = defs.get("lists/maps/forTip")


def _target_items(self, context):
    """New Map's For: nothing (a blank map), the common settings the graph has a node for, Other."""
    _TARGET_ITEMS[:] = [("NONE", "Nothing yet (a blank map)", "A blank map, named as you like")]
    scalp = active(context) if context else None
    if scalp is not None:
        try:
            settings, others = tools.setting_targets(scalp)
        except Exception:  # noqa: BLE001 - no helper yet: a blank map only
            settings, others = [], []
        for s in settings:
            if s["node"]:
                _TARGET_ITEMS.append(("%s.%s" % (s["node"], s["param"]), "Setting: %s" % s["label"], s["hint"]))
        if others:
            _TARGET_ITEMS.append(("OTHER", "Other setting...", "Any mappable param of the graph"))
    return _TARGET_ITEMS


def _other_items(self, context):
    _OTHER_ITEMS[:] = []
    scalp = active(context) if context else None
    if scalp is not None:
        try:
            _OTHER_ITEMS.extend((o["id"], o["label"], o["hint"][:200]) for o in tools.setting_targets(scalp)[1])
        except Exception:  # noqa: BLE001
            pass
    return _OTHER_ITEMS or [("NONE", "(no mappable params)", "")]


class GROOMLAB_OT_map_new(GroomToolOperator):
    """A new map: on the guides, on the scalp's vertices, or a texture; blank, or for a setting (Density, Width,
    Length...: made, linked and painted at once; Ctrl on Density removes hair)"""
    bl_idname = "groomlab.map_new"
    bl_label = "New Map"
    bl_options = {"REGISTER", "UNDO"}

    target: EnumProperty(name="For", items=_target_items, description=FOR_TIP)
    other: EnumProperty(name="Setting", items=_other_items, description="Every mappable param of the graph")
    name: StringProperty(name="Name", default="mask", description="Letters, digits and _ (not starting with a digit)")
    kind: EnumProperty(name=defs.get("choices/mapKind")["label"], items=defs.enum_items("mapKind"), default="vertices")
    color: BoolProperty(name=defs.choice_label("mapValues", "color"), default=False,
                        description=defs.choice("mapValues", "color")["tip"])
    value: FloatProperty(name="Fill", default=0.0, min=0.0, max=1.0, subtype="FACTOR")
    resolution: EnumProperty(name=defs.get("choices/mapResolution")["label"], items=defs.enum_items("mapResolution"),
                             default=defs.enum_default("mapResolution"))

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def chosen(self):
        """(node, param) the map is for, or None (a blank map)."""
        key = self.other if self.target == "OTHER" else self.target
        if not key or key == "NONE" or "." not in key:
            return None
        node, param = key.split(".", 1)
        return node, param

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "target")
        if self.target == "OTHER":
            layout.prop(self, "other")
        setting = self.chosen() is not None
        if not setting:
            layout.prop(self, "name")
        layout.prop(self, "kind")
        if not setting:
            layout.prop(self, "color")
            layout.prop(self, "value")
        if self.kind == "texture":
            layout.prop(self, "resolution")
        if setting:
            node, param = self.chosen()
            layout.label(text="Named %s_%s, white; Ctrl paints toward 0" % (node, param))

    def execute(self, context):
        scalp = active(context)
        target = self.chosen()
        try:
            if target:
                # A setting's map lives on the scalp (vertices or a texture).
                kind = self.kind if self.kind in ("vertices", "texture") else "vertices"
                r = tools.map_for_param(scalp, target[0], target[1], kind=kind, resolution=int(self.resolution))
                tools.pick(context, "paint")
                tools.status(context.scene, "%s %s, which %s.%s reads: Ctrl paints it toward 0%s, strokes back toward the "
                                            "value" % ("Made" if r.get("made") else "Painting", r["name"], target[0],
                                                       target[1], " (no hair there)" if target[1] == "density" else ""))
                return {"FINISHED"}
            tools.map_create(scalp, self.name.strip(), kind=self.kind, color=self.color and self.kind != "guides",
                             value=self.value, resolution=int(self.resolution))
        except HelperError as e:
            return _fail(self, e)
        tools.push_safely(context)
        return {"FINISHED"}


class GROOMLAB_OT_paint_param(GroomToolOperator):
    """Paint the map this param reads (one made white and linked, on the scalp's vertices, when it reads none):
    the Paint tool on it, Ctrl toward 0"""
    bl_idname = "groomlab.paint_param"
    bl_label = "Paint Param"
    bl_options = {"REGISTER", "UNDO"}

    node: StringProperty(name="Node")
    param: StringProperty(name="Param")

    def execute(self, context):
        try:
            tools.map_for_param(active(context), self.node, self.param)
            tools.pick(context, "paint")
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class MapOperator(GroomToolOperator):
    """An edit of the picked map (the Paint tab's list)."""

    @classmethod
    def poll(cls, context):
        return GroomToolOperator.poll(context) and tools.current_map(context.scene) is not None

    @classmethod
    def description(cls, context, properties):
        return _description(cls, properties, cls.__doc__ or "")


class GROOMLAB_OT_map_remove(MapOperator):
    """Delete the map (a texture map's file too, unless Delete Its File is off in the list's right-click menu)"""
    bl_idname = "groomlab.map_remove"
    bl_label = "Delete Map"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        t = context.scene.groomlab_tools
        name = t.paint_map
        if not name:
            return {"CANCELLED"}
        try:
            r = tools.map_remove(active(context), name, delete_file=t.map_delete_files)
        except HelperError as e:
            return _fail(self, e)
        if r.get("fileDeleted"):
            tools.status(context.scene, "Deleted %s and its file." % name)
        return {"FINISHED"}


class GROOMLAB_OT_map_fill(MapOperator):
    """Set the map to the value (a color map: to the color, in the channels ticked): a guide map on the selected
    guides when some are, else everywhere"""
    bl_idname = "groomlab.map_fill"
    bl_label = "Fill"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        t = context.scene.groomlab_tools
        m = tools.current_map(context.scene)
        try:
            if m.channels > 1:
                channels = "".join(c for c, on in zip("rgba", (t.channel_r, t.channel_g, t.channel_b, t.channel_a)) if on)
                tools.map_fill(active(context), m.name, color=list(t.paint_color), channels=channels or "rgba",
                               mode=t.paint_mode)
            else:
                tools.map_fill(active(context), m.name, value=float(t.value),
                               selected=m.kind == "guides" and t.selected_count > 0, mode=t.paint_mode)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


IMAGE_GLOB = "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.psd;*.gif;*.hdr;*.tif;*.tiff;*.exr"


class GROOMLAB_OT_map_import(GroomToolOperator, ImportHelper):
    """A new texture map from an image (named after the file; a JPEG, TGA, PSD... is copied into the masks folder
    as a PNG; color or gray as the image is)"""
    bl_idname = "groomlab.map_import"
    bl_label = "Import Image as Map"
    bl_options = {"REGISTER", "UNDO"}
    filter_glob: StringProperty(default=IMAGE_GLOB, options={"HIDDEN"})

    def execute(self, context):
        try:
            r = tools.map_import(active(context), self.filepath)
        except HelperError as e:
            return _fail(self, e)
        with props.quiet():
            context.scene.groomlab_tools.paint_map = r["name"]
        tools.refresh_info(active(context), context.scene)
        return {"FINISHED"}


class GROOMLAB_OT_map_relink(MapOperator, ImportHelper):
    """Point the map at another image: a PNG is used in place (painting edits it); a JPEG, TGA, PSD... is copied
    into the masks folder as a PNG"""
    bl_idname = "groomlab.map_relink"
    bl_label = "Relink Image"
    bl_options = {"REGISTER", "UNDO"}
    filter_glob: StringProperty(default=IMAGE_GLOB, options={"HIDDEN"})

    def execute(self, context):
        try:
            tools.map_relink(active(context), context.scene.groomlab_tools.paint_map, self.filepath)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_map_rename(MapOperator):
    """Rename the map; Map nodes and params reading it follow"""
    bl_idname = "groomlab.map_rename"
    bl_label = "Rename Map"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(name="New Name", options={"SKIP_SAVE"},
                         description="Letters, digits and _ (not starting with a digit)")

    def invoke(self, context, event):
        self.name = context.scene.groomlab_tools.paint_map
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        old = context.scene.groomlab_tools.paint_map
        new = self.name.strip()
        if not new or new == old:
            return {"CANCELLED"}
        try:
            tools.map_rename(active(context), old, new)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_map_convert(MapOperator):
    """Convert to another kind: its name, and what reads it, stay the same"""
    bl_idname = "groomlab.map_convert"
    bl_label = "Convert Map"
    bl_options = {"REGISTER", "UNDO"}

    kind: EnumProperty(name=defs.get("choices/mapKind")["label"], items=defs.enum_items("mapKind", field="convert"),
                       default="vertices")
    resolution: EnumProperty(name=defs.get("choices/mapResolution")["label"], items=defs.enum_items("mapResolution"),
                             default=defs.enum_default("mapResolution"))

    @classmethod
    def description(cls, context, properties):
        c = defs.choice("mapKind", properties.kind)
        return c["tip"] if c else cls.__doc__

    def invoke(self, context, event):
        if self.kind == "texture":  # "A texture at the scalp's UVs...": its resolution asked
            return context.window_manager.invoke_props_dialog(self)
        return self.execute(context)

    def draw(self, context):
        self.layout.prop(self, "resolution")

    def execute(self, context):
        m = tools.current_map(context.scene)
        if m.kind == self.kind:
            self.report({"INFO"}, "%s is %s already" % (m.name, defs.choice_label("mapKind", m.kind).lower()))
            return {"CANCELLED"}
        try:
            r = tools.map_convert(active(context), m.name, self.kind, resolution=int(self.resolution))
        except HelperError as e:
            return _fail(self, e)
        for w in r.get("warnings") or []:
            self.report({"WARNING"}, sentence(w))
        return {"FINISHED"}


class GROOMLAB_OT_map_set_channels(MapOperator):
    """Make the map gray (its luminance) or color (r g b a)"""
    bl_idname = "groomlab.map_set_channels"
    bl_label = "Map Channels"
    bl_options = {"REGISTER", "UNDO"}

    channels: IntProperty(name="Channels", default=4, min=1, max=4)

    def execute(self, context):
        try:
            tools.map_set_channels(active(context), context.scene.groomlab_tools.paint_map, self.channels)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_map_reload(MapOperator):
    """Read the image again after editing it outside Blender"""
    bl_idname = "groomlab.map_reload"
    bl_label = "Reload Image"

    def execute(self, context):
        try:
            tools.map_reload(active(context), context.scene.groomlab_tools.paint_map)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_map_show_file(MapOperator):
    """Open the image's folder in the file browser"""
    bl_idname = "groomlab.map_show_file"
    bl_label = "Show in Explorer"

    def execute(self, context):
        m = tools.current_map(context.scene)
        folder = os.path.dirname(m.texture) if m.texture else ""
        if not folder or not os.path.isdir(folder):
            self.report({"ERROR"}, "%s has no image on disk" % m.name)
            return {"CANCELLED"}
        bpy.ops.wm.path_open(filepath=folder)
        return {"FINISHED"}


# ---- Brush alphas -------------------------------------------------------------------------------------

class GROOMLAB_OT_alpha_add(bpy.types.Operator, ImportHelper):
    """Copy images (PNG, JPEG, TGA, PSD...) into your alpha folder (Documents/GroomLab/alphas, every host lists
    them) and paint with the last one"""
    bl_idname = "groomlab.alpha_add"
    bl_label = "Add Alpha"
    filter_glob: StringProperty(default=IMAGE_GLOB, options={"HIDDEN"})
    files: CollectionProperty(type=bpy.types.OperatorFileListElement, options={"HIDDEN", "SKIP_SAVE"})
    directory: StringProperty(subtype="DIR_PATH", options={"HIDDEN"})

    def execute(self, context):
        from . import alphas
        names = [f.name for f in self.files if f.name] or [os.path.basename(self.filepath)]
        folder = self.directory or os.path.dirname(self.filepath)
        added = ""
        for name in names:
            try:
                added = process.call("add_brush_alpha", file=os.path.join(folder, name))["path"]
            except HelperError as e:
                self.report({"ERROR"}, str(e))
        if not added:
            return {"CANCELLED"}
        alphas.library(refresh=True)
        context.scene.groomlab_tools.alpha = added
        return {"FINISHED"}


class GROOMLAB_OT_alpha_folder(bpy.types.Operator):
    """Open your alpha folder (Documents/GroomLab/alphas): images dropped there are listed too"""
    bl_idname = "groomlab.alpha_folder"
    bl_label = "Open Alpha Folder"

    def execute(self, context):
        from . import alphas
        folder = alphas.user_dir()
        if not folder:
            self.report({"ERROR"}, "no alpha folder (set GROOMLAB_USER_ALPHAS)")
            return {"CANCELLED"}
        os.makedirs(folder, exist_ok=True)
        bpy.ops.wm.path_open(filepath=folder)
        return {"FINISHED"}


class GROOMLAB_OT_alpha_clear(bpy.types.Operator):
    """Paint without an alpha: the brush's falloff alone"""
    bl_idname = "groomlab.alpha_clear"
    bl_label = "Clear Alpha"

    def execute(self, context):
        context.scene.groomlab_tools.alpha = ""
        return {"FINISHED"}


# ---- Guide groups: colors, promote / demote, mirroring, reparenting ----------------------------------

class GROOMLAB_OT_group_color(GroomToolOperator):
    """The group's color in the viewport"""
    bl_idname = "groomlab.group_color"
    bl_label = "Group Color"
    bl_options = {"REGISTER", "UNDO"}

    group: StringProperty(name="Group", options={"SKIP_SAVE"})
    color: FloatVectorProperty(name="Color", size=3, subtype="COLOR_GAMMA", min=0.0, max=1.0, default=(1.0, 0.5, 0.2))

    def invoke(self, context, event):
        t = context.scene.groomlab_tools
        self.group = self.group or t.group
        g = next((g for g in t.groups if g.name == self.group), None)
        if g is not None and g.has_color:
            self.color = g.color
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        group = self.group or context.scene.groomlab_tools.group
        if not group or group == tools.ALL_GROUPS:
            self.report({"ERROR"}, "No group picked: pick a group first")
            return {"CANCELLED"}
        try:
            tools.set_group_color(active(context), group, tuple(self.color))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_group_promote_demote(GroomToolOperator):
    """Move a group one level up, or down under a sibling group"""
    bl_idname = "groomlab.group_promote_demote"
    bl_label = "Promote / Demote Group"
    bl_options = {"REGISTER", "UNDO"}

    action: EnumProperty(name="Move", items=[("PROMOTE", "Promote", "One level up"),
                                             ("DEMOTE", "Demote", "One level down, under a sibling group")])
    group: StringProperty(name="Group", options={"SKIP_SAVE"})
    parent: StringProperty(name="Under", options={"SKIP_SAVE"}, description="Demote: the sibling group to move it under")

    def invoke(self, context, event):
        t = context.scene.groomlab_tools
        if not self.group and t.group != tools.ALL_GROUPS:
            self.group = t.group
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        t = context.scene.groomlab_tools
        layout = self.layout
        layout.row().prop(self, "action", expand=True)
        layout.prop_search(self, "group", t, "groups")
        if self.action == "DEMOTE":
            layout.prop_search(self, "parent", t, "groups")

    def execute(self, context):
        if not self.group:
            self.report({"ERROR"}, "No group: pick the group to move")
            return {"CANCELLED"}
        try:
            if self.action == "PROMOTE":
                tools.guides_edit(active(context), "promote_group", group=self.group)
            else:
                tools.guides_edit(active(context), "demote_group", group=self.group, parent=self.parent.strip())
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_guides_mirror_group(GroomToolOperator):
    """Copy a root group from one side to the other"""
    bl_idname = "groomlab.guides_mirror_group"
    bl_label = "Mirror Group"
    bl_options = {"REGISTER", "UNDO"}

    group: StringProperty(name="Root Group", options={"SKIP_SAVE"})
    direction: EnumProperty(name="From", items=[("POS", "+X → -X", "Copy the +X side over the -X side"),
                                                ("NEG", "-X → +X", "Copy the -X side over the +X side")], default="POS")

    def invoke(self, context, event):
        t = context.scene.groomlab_tools
        if not self.group:
            picked = next((g for g in t.groups if g.name == t.group), None)
            root = picked
            while root is not None and root.parent:
                root = next((g for g in t.groups if g.name == root.parent), None)
            self.group = root.name if root is not None else (t.groups[0].name if len(t.groups) else "")
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        self.layout.prop_search(self, "group", context.scene.groomlab_tools, "groups")
        self.layout.row().prop(self, "direction", expand=True)

    def execute(self, context):
        if not self.group:
            self.report({"ERROR"}, "No group: pick a root group to mirror")
            return {"CANCELLED"}
        try:
            tools.guides_edit(active(context), "mirror_group", group=self.group, axis="x", center=0.0,
                              fromPositive=self.direction == "POS")
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_guides_reparent(GroomToolOperator):
    """Move the selected children under another parent"""
    bl_idname = "groomlab.guides_reparent"
    bl_label = "Reparent Selected Guides"
    bl_options = {"REGISTER", "UNDO"}

    parent: IntProperty(name="New Parent (Guide ID)", default=0, min=0, description="The ID of the new parent guide")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scalp = active(context)
        ids = tools.selected_guides(scalp)
        if not ids:
            self.report({"ERROR"}, "No guides selected: select the children to move first")
            return {"CANCELLED"}
        try:
            tools.guides_edit(scalp, "reparent", ids=ids, parent=int(self.parent))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# ---- Parts and flow (the Part and Flow tools' cards) -------------------------------------------------

class GROOMLAB_OT_part_remove(GroomToolOperator):
    """Remove the part picked above"""
    bl_idname = "groomlab.part_remove"
    bl_label = "Remove Part"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(name="Part", options={"SKIP_SAVE"})

    def execute(self, context):
        t = context.scene.groomlab_tools
        name = self.name or (t.parts[t.part_index].name if 0 <= t.part_index < len(t.parts) else "")
        if not name:
            self.report({"ERROR"}, "No part picked: pick a part in the list")
            return {"CANCELLED"}
        try:
            tools.part_remove(active(context), name)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_parts_clear(GroomToolOperator):
    """Remove every part"""
    bl_idname = "groomlab.parts_clear"
    bl_label = "Clear All Parts"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            tools.parts_clear(active(context))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_flow_clear(GroomToolOperator):
    """Remove every flow arrow and sink"""
    bl_idname = "groomlab.flow_clear"
    bl_label = "Clear All Flow"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            tools.flow_clear(active(context))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_length_edit(GroomToolOperator):
    """An edit of the Length Paths tool's paths (the Paths box): pick a path, New Path, Delete Key,
    Delete Path, Cancel or Apply"""
    bl_idname = "groomlab.length_edit"
    bl_label = "Length Paths"
    bl_options = {"REGISTER"}

    edit: bpy.props.StringProperty(options={"SKIP_SAVE"})
    path: bpy.props.IntProperty(options={"SKIP_SAVE"})

    @classmethod
    def description(cls, context, properties):
        return {"select": "Edit this path", "newPath": "The next click on the scalp starts a new path (or Shift+click)",
                "deleteKey": "Delete the selected key (or Ctrl+click it in the viewport)",
                "deletePath": "Delete the selected path and its keys",
                "cancel": "Drop the changes since the last Apply (Esc)",
                "apply": "Set the guides to the lengths shown (one undo step; Enter); the paths stay with the groom to "
                         "edit and apply again"}.get(properties.edit, cls.__doc__)

    def execute(self, context):
        try:
            if self.edit == "select":
                tools.length_edit(active(context), "select", path=self.path, key=0)
            else:
                tools.length_edit(active(context), self.edit)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# ---- Tubes (the Tubes section) --------------------------------------------------------------------

class TubeOperator(GroomToolOperator):
    bl_options = {"REGISTER", "UNDO"}


def _library_pick(context):
    t = context.scene.groomlab_tools
    return t.tube_shapes[t.tube_shape_index] if 0 <= t.tube_shape_index < len(t.tube_shapes) else None


class GROOMLAB_OT_tube_apply(TubeOperator):
    """Give the targeted guides this shape"""
    bl_idname = "groomlab.tube_apply"
    bl_label = "Apply to Selected"

    def execute(self, context):
        scalp = active(context)
        guides = tools.groom_tools(scalp).tube_guides
        if not guides:
            self.report({"ERROR"}, "No guides selected or soloed: select guides (Select tool), or Ctrl+click one with a brush")
            return {"CANCELLED"}
        try:
            tools.set_tube(scalp, guides=guides, shape=context.scene.groomlab_tools.tube_choice)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_tube_smooth(TubeOperator):
    """Smooth the targeted guides' tubes along each guide: what the brush painted evened out, the root's and tip's
    sections kept (Shift+drag with the brush smooths under it)"""
    bl_idname = "groomlab.tube_smooth"
    bl_label = "Smooth"

    def execute(self, context):
        scalp = active(context)
        t = context.scene.groomlab_tools
        guides = tools.groom_tools(scalp).tube_guides
        if not guides:
            self.report({"ERROR"}, "No guides selected or soloed: select guides (Select tool), or Ctrl+click one with a brush")
            return {"CANCELLED"}
        try:
            tools.smooth_tube(scalp, channel=t.tube_channel if t.tube_smooth_what == "current" else "all", guides=guides)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_tube_shape_custom(TubeOperator):
    """Copy this built-in shape into the groom's library as a new shape you can edit (the targeted guides take it)"""
    bl_idname = "groomlab.tube_shape_custom"
    bl_label = "Edit as Custom"

    def execute(self, context):
        scalp = active(context)
        scene = context.scene
        t = scene.groomlab_tools
        base = t.tube_choice or "round"
        try:
            points, smooth = tools.shape_points(scalp, base)
            name = tools.unique_shape_name(scene, base)
            tools.add_tube_shape(scalp, name, points, smooth=smooth)
            guides = tools.groom_tools(scalp).tube_guides
            if guides:
                tools.set_tube(scalp, guides=guides, shape=name)
        except HelperError as e:
            return _fail(self, e)
        with props.quiet():
            t.tube_choice = name
        tools.refresh_tubes(scalp, scene)
        tools.push_safely(context)
        tools.status(scene, "%s: a copy of the built-in in the groom's library." % name)
        return {"FINISHED"}


class GROOMLAB_OT_tube_shape_new(TubeOperator):
    """A new library shape (a circle to start from)"""
    bl_idname = "groomlab.tube_shape_new"
    bl_label = "New Shape"

    duplicate: BoolProperty(name="Duplicate", default=False, options={"SKIP_SAVE"},
                            description="A copy of the shape picked in the list, to change on its own")

    @classmethod
    def description(cls, context, properties):
        return ("A copy of this shape in the library, to change on its own" if properties.duplicate
                else "A new library shape (a circle to start from)")

    def execute(self, context):
        scalp = active(context)
        scene = context.scene
        t = scene.groomlab_tools
        try:
            if self.duplicate:
                picked = _library_pick(context)
                if picked is None:
                    self.report({"ERROR"}, "No library shape picked")
                    return {"CANCELLED"}
                points, smooth = tools.shape_points(scalp, picked.name)
                name = tools.unique_shape_name(scene, picked.name)
            else:
                points, smooth = tools.shape_points(scalp, "round")
                smooth, name = True, tools.unique_shape_name(scene, "shape")
            tools.add_tube_shape(scalp, name, points, smooth=smooth)
        except HelperError as e:
            return _fail(self, e)
        with props.quiet():
            t.tube_choice = name
        tools.refresh_tubes(scalp, scene)
        names = [x.name for x in t.tube_shapes]
        with props.quiet():
            t.tube_shape_index = names.index(name) if name in names else t.tube_shape_index
        tools.push_safely(context)
        return {"FINISHED"}


class GROOMLAB_OT_tube_shape_rename(TubeOperator):
    """Rename this library shape (its guides keep it)"""
    bl_idname = "groomlab.tube_shape_rename"
    bl_label = "Rename Shape"

    name: StringProperty(name="New Name", options={"SKIP_SAVE"})

    @classmethod
    def poll(cls, context):
        return TubeOperator.poll(context) and _library_pick(context) is not None

    def invoke(self, context, event):
        self.name = _library_pick(context).name
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        old = _library_pick(context).name
        new = self.name.strip()
        if not new or new == old:
            return {"CANCELLED"}
        t = context.scene.groomlab_tools
        try:
            tools.rename_tube_shape(active(context), old, new)
        except HelperError as e:
            return _fail(self, e)
        if t.tube_choice == old:
            with props.quiet():
                t.tube_choice = new
            tools.refresh_tubes(active(context), context.scene)
            tools.push_safely(context)
        return {"FINISHED"}


class GROOMLAB_OT_tube_shape_remove(TubeOperator):
    """Remove this shape from the library (its guides become round)"""
    bl_idname = "groomlab.tube_shape_remove"
    bl_label = "Delete Shape"

    @classmethod
    def poll(cls, context):
        return TubeOperator.poll(context) and _library_pick(context) is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        picked = _library_pick(context)
        t = context.scene.groomlab_tools
        name, users = picked.name, picked.users
        try:
            tools.remove_tube_shape(active(context), name)
        except HelperError as e:
            return _fail(self, e)
        if t.tube_choice == name:
            with props.quiet():
                t.tube_choice = "round"
            tools.push_safely(context)
        tools.status(context.scene, "Deleted %s: %d guide%s went round." % (name, users, "" if users == 1 else "s"))
        return {"FINISHED"}


# ---- Correctives and Sculpt nodes (the Correctives tab) -------------------------------------------

class CorrectiveOperator(GroomToolOperator):
    """An edit of a corrective: `name`, else the one picked in the list."""
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(name="Corrective", options={"SKIP_SAVE"})

    @classmethod
    def description(cls, context, properties):
        return _description(cls, properties, cls.__doc__ or "")

    def corrective(self, context):
        if self.name:
            return self.name
        kind, name = tools.picked_corrective(context.scene)
        if kind != "layer":
            self.report({"ERROR"}, "Pick a corrective.")
            return None
        return name


class GROOMLAB_OT_corrective_new(GroomToolOperator):
    """New corrective on every frame (unkeyed weight 1)"""
    bl_idname = "groomlab.corrective_new"
    bl_label = "New Corrective"
    bl_options = {"REGISTER", "UNDO"}

    fix: BoolProperty(name="Fix Frame", default=False, options={"SKIP_SAVE"},
                      description="A corrective on this frame, keyed in over the 6 frames before and out over the 6 after")

    @classmethod
    def description(cls, context, properties):
        return _description(cls, properties, cls.__doc__)

    def execute(self, context):
        scalp = active(context)
        try:
            if self.fix:
                name = tools.corrective_new(scalp, frame=float(context.scene.frame_current), fade=6.0)
            else:
                name = tools.corrective_new(scalp, all_frames=True)
        except HelperError as e:
            return _fail(self, e)
        tools.status(context.scene, "Brushes sculpt into %s." % name)
        return {"FINISHED"}


class GROOMLAB_OT_corrective_move(CorrectiveOperator):
    """Move up the stack"""
    bl_idname = "groomlab.corrective_move"
    bl_label = "Move Corrective"

    step: IntProperty(default=1)

    def execute(self, context):
        name = self.corrective(context)
        if not name:
            return {"CANCELLED"}
        try:
            tools.corrective_move(active(context), name, self.step)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_corrective_solo(CorrectiveOperator):
    """Solo: only this corrective on (again: all on)"""
    bl_idname = "groomlab.corrective_solo"
    bl_label = "Solo Corrective"

    def execute(self, context):
        name = self.corrective(context)
        if not name:
            return {"CANCELLED"}
        try:
            tools.corrective_solo(active(context), name)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_corrective_delete(CorrectiveOperator):
    """Delete the corrective and its keys"""
    bl_idname = "groomlab.corrective_delete"
    bl_label = "Delete Corrective"

    def execute(self, context):
        name = self.corrective(context)
        if not name:
            return {"CANCELLED"}
        scalp = active(context)
        try:
            tools.corrective_delete(scalp, name)
        except HelperError as e:
            return _fail(self, e)
        tools.remove_weight_fcurve(scalp, name)
        return {"FINISHED"}


class GROOMLAB_OT_corrective_go_to(CorrectiveOperator):
    """The time slider to the corrective's frame"""
    bl_idname = "groomlab.corrective_go_to"
    bl_label = "Go to Its Frame"

    def execute(self, context):
        name = self.corrective(context)
        if not name:
            return {"CANCELLED"}
        row = next((r for r in context.scene.groomlab_tools.correctives if r.kind == "layer" and r.name == name), None)
        fc = tools.weight_fcurve(active(context), name)
        frame = None
        if fc is not None and len(fc.keyframe_points):
            frame = max(fc.keyframe_points, key=lambda k: k.co[1]).co[0]  # the strongest key
        elif row is not None and row.has_frame:
            frame = row.frame
        if frame is None:
            self.report({"INFO"}, "%s is on every frame" % name)
            return {"CANCELLED"}
        context.scene.frame_set(int(round(frame)))
        return {"FINISHED"}


class GROOMLAB_OT_corrective_key(CorrectiveOperator):
    """Key the weight on this frame"""
    bl_idname = "groomlab.corrective_key"
    bl_label = "Key Weight Here"

    def execute(self, context):
        name = self.corrective(context)
        if not name:
            return {"CANCELLED"}
        try:
            tools.key_corrective(active(context), name)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_corrective_clear_keys(CorrectiveOperator):
    """Delete the weight's keys: the corrective on every frame"""
    bl_idname = "groomlab.corrective_clear_keys"
    bl_label = "Clear Keys"

    def execute(self, context):
        name = self.corrective(context)
        if not name:
            return {"CANCELLED"}
        scalp = active(context)
        tools.remove_weight_fcurve(scalp, name)
        try:
            tools.corrective_clear_keys(scalp, name)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_corrective_rename(CorrectiveOperator):
    """Rename the corrective"""
    bl_idname = "groomlab.corrective_rename"
    bl_label = "Rename Corrective"

    new_name: StringProperty(name="New Name", options={"SKIP_SAVE"}, description="Letters, digits and _")

    def invoke(self, context, event):
        name = self.corrective(context)
        if not name:
            return {"CANCELLED"}
        self.name, self.new_name = name, name
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        self.layout.prop(self, "new_name")

    def execute(self, context):
        name = self.corrective(context)
        new = self.new_name.strip()
        if not name or not new or new == name:
            return {"CANCELLED"}
        scalp = active(context)
        weights = scalp.groomlab_tools.corrective_weights
        try:
            tools.corrective_rename(scalp, name, new)
        except HelperError as e:
            return _fail(self, e)
        with props.quiet():
            if weights.get(name) is not None:
                weights[name].name = new
            # Looked up now, not before the rename: the rename's refresh can rebuild the action's
            # curves, and writing through a curve Blender freed crashed it (FCurve_data_path_set).
            fc = tools.weight_fcurve(scalp, name)
            if fc is not None:
                fc.data_path = tools.weight_path(new)
            gt = tools.groom_tools(scalp)
            if name in gt.pushed_keys:
                gt.pushed_keys[new] = gt.pushed_keys.pop(name)
        tools.refresh_correctives(scalp, context.scene)
        return {"FINISHED"}


class GROOMLAB_OT_sculpt_target(GroomToolOperator):
    """Brushes shape the guides this node hands on; each stroke is kept in the node and follows the guides when what
    feeds it changes (off: the base groom)"""
    bl_idname = "groomlab.sculpt_target"
    bl_label = "Brush This Node's Guides"

    node: StringProperty(name="Node", options={"SKIP_SAVE"}, description="A Sculpt node (empty: the base groom)")
    corrective: StringProperty(name="Corrective", options={"SKIP_SAVE"})

    def execute(self, context):
        scalp = active(context)
        try:
            if self.node:
                tools.sculpt_target(scalp, node=self.node)
            elif self.corrective:
                tools.sculpt_target(scalp, corrective=self.corrective)
            else:
                tools.sculpt_target(scalp, base=True)
            tools.refresh_correctives(scalp, context.scene)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_sculpt_clear(GroomToolOperator):
    """Drop every stroke kept in this node: its guides as it is fed them (one undo step)"""
    bl_idname = "groomlab.sculpt_clear"
    bl_label = "Clear Sculpt Edits"
    bl_options = {"REGISTER", "UNDO"}

    node: StringProperty(name="Node", options={"SKIP_SAVE"})

    def execute(self, context):
        node = self.node
        if not node:
            kind, name = tools.picked_corrective(context.scene)
            node = name if kind == "node" else ""
        if not node:
            self.report({"ERROR"}, "Pick a Sculpt node.")
            return {"CANCELLED"}
        try:
            tools.sculpt_clear(active(context), node)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# ---- Strand colors ---------------------------------------------------------------------------------

class GROOMLAB_OT_show_strand_colors(GroomToolOperator):
    """Show the strand colors in the 3D views (Solid shading: Attribute color)"""
    bl_idname = "groomlab.show_strand_colors"
    bl_label = "Show in Viewport"

    def execute(self, context):
        display.show_attribute_colors(context)
        return {"FINISHED"}


# ---- The pie menu (Maya's marking menu) ----------------------------------------------------------

def _tool_button(layout, tool, label=None):
    info = tools.tool_info(tool)
    text = label or (info["label"] if info else tool.capitalize())
    op = layout.operator("groomlab.tool", text=text, icon_value=icons.icon(tool))
    op.tool = tool
    return op


def overflow():
    """The catalog's tools not on the radial nor excluded (marking_menu.json's overflow: Select and
    Freeze come last, in the tail)."""
    radial = set(mode for _, mode in RADIAL)
    exclude = set(defs.get("markingMenu/overflow").get("exclude") or ())
    return [t["id"] for t in tools.cached_catalog().get("tools", []) if t["id"] not in radial
            and t["id"] not in exclude]


def tail():
    """The marking menu's tail: [("tool", id) | ("action", id)]."""
    return [("tool", e["tool"]) if isinstance(e, dict) else ("action", e) for e in defs.get("markingMenu/tail")]


class GROOMLAB_MT_pie(bpy.types.Menu):
    bl_idname = "GROOMLAB_MT_pie"
    bl_label = "GroomLab"

    @classmethod
    def poll(cls, context):
        return active(context) is not None

    def draw(self, context):
        try:
            tools.catalog()
        except HelperError:
            pass
        pie = self.layout.menu_pie()
        by_dir = dict(RADIAL)
        for direction in PIE_ORDER:
            if direction == "S":
                # South: Place, and the rest of the catalog in a list below the pie (Maya's overflow).
                col = pie.column()
                _tool_button(col, by_dir["S"])
                col.separator()
                box = col.box().column(align=True)
                for tool in overflow():
                    _tool_button(box, tool)
                box.separator()
                last = None
                for kind, item in tail():
                    if kind == "action" and last == "tool":
                        box.separator()
                    if kind == "tool":
                        _tool_button(box, item)
                    elif actions.bound(item):
                        from . import menu
                        menu.draw_action(box, {"id": item, "label": defs.label(item), "icon": defs.action(item).get("icon")},
                                         context)
                    last = kind
            else:
                _tool_button(pie, by_dir[direction])


# ---- Keymaps ---------------------------------------------------------------------------------------

_keymaps = []


def register_keymaps():
    kc = bpy.context.window_manager.keyconfigs.addon if bpy.context.window_manager else None
    if kc is None:
        return
    km = kc.keymaps.new(name="Object Mode", space_type="EMPTY")
    kmi = km.keymap_items.new("wm.call_menu_pie", PIE_KEY, "PRESS")
    kmi.properties.name = GROOMLAB_MT_pie.bl_idname
    _keymaps.append((km, kmi))


def unregister_keymaps():
    for km, kmi in _keymaps:
        try:
            km.keymap_items.remove(kmi)
        except (ReferenceError, RuntimeError):
            pass
    _keymaps.clear()


classes = (GROOMLAB_OT_tool_modal, GROOMLAB_OT_tool, GROOMLAB_OT_tool_exit, GROOMLAB_OT_tool_undo,
           GROOMLAB_OT_step_level, GROOMLAB_OT_group_new, GROOMLAB_OT_group_edit, GROOMLAB_OT_guides_edit,
           GROOMLAB_OT_map_new, GROOMLAB_OT_paint_param, GROOMLAB_OT_map_remove, GROOMLAB_OT_map_fill,
           GROOMLAB_OT_map_import, GROOMLAB_OT_map_relink, GROOMLAB_OT_map_rename, GROOMLAB_OT_map_convert,
           GROOMLAB_OT_map_set_channels, GROOMLAB_OT_map_reload, GROOMLAB_OT_map_show_file,
           GROOMLAB_OT_alpha_add, GROOMLAB_OT_alpha_folder, GROOMLAB_OT_alpha_clear,
           GROOMLAB_OT_group_color, GROOMLAB_OT_group_promote_demote, GROOMLAB_OT_guides_mirror_group,
           GROOMLAB_OT_guides_reparent, GROOMLAB_OT_part_remove, GROOMLAB_OT_parts_clear, GROOMLAB_OT_flow_clear,
           GROOMLAB_OT_length_edit,
           GROOMLAB_OT_tube_apply, GROOMLAB_OT_tube_smooth, GROOMLAB_OT_tube_shape_custom, GROOMLAB_OT_tube_shape_new,
           GROOMLAB_OT_tube_shape_rename, GROOMLAB_OT_tube_shape_remove, GROOMLAB_OT_corrective_new,
           GROOMLAB_OT_corrective_move, GROOMLAB_OT_corrective_solo, GROOMLAB_OT_corrective_delete,
           GROOMLAB_OT_corrective_go_to, GROOMLAB_OT_corrective_key, GROOMLAB_OT_corrective_clear_keys,
           GROOMLAB_OT_corrective_rename, GROOMLAB_OT_sculpt_target, GROOMLAB_OT_sculpt_clear,
           GROOMLAB_OT_show_strand_colors, GROOMLAB_MT_pie)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    register_keymaps()


def unregister():
    unregister_keymaps()
    _session[0] = None
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
