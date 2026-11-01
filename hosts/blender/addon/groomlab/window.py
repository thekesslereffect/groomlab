# SPDX-License-Identifier: GPL-3.0-or-later
"""The GroomLab window (preview): Groom Tools drawn over the 3D view in the app's look (uikit.py).

Its content is the sidebar's Groom Tools panel (palette.py) and its sub-panels, run against
drawn.DrawnLayout and laid out as the window's widgets: the same sections, settings and actions as
the sidebar, from the same code.

Input comes through a gizmo, not an operator left running: the gizmo's test_select says whether the
pointer is over the window (and tracks what it is over, for hover and tooltips); a press there starts
GROOMLAB_OT_window_interact, which runs only for that press, drag or typing. Blender skips autosave
while a modal operator runs, so none is kept running. The gizmo group's keymap adds the wheel
(scroll the window, or the list under the pointer). Viewport gizmos turned off (the overlay's Gizmos
toggle) leave the window drawn but deaf, which it says in its first row."""

import json
import os
import time

import bpy

from . import drawn, groom, palette, uikit

TITLE = "GroomLab"
state = uikit.WindowState()
DEBUG = bool(os.environ.get("GROOMLAB_WINDOW_DEBUG"))
_draw_handle = [None]
_list_areas = {}       # the lists drawn last: id -> (rect, rows they can scroll)
_color_target = [None]  # (owner, property) the color picker edits
_cache = {}            # region -> (time, Window): what test_select reuses between mouse moves
CACHE_SECONDS = 0.1
TOGGLE_KEY = ("G", {"ctrl": True, "shift": True})  # Ctrl+Shift+G over a 3D view (Preferences rebinds it)
_keymaps = []


# ---- Kept between sessions -------------------------------------------------------------------
# Whether the window shows, where, and its folds, in the extension's user folder (as updates.py keeps
# its state): it opens again where it was after Blender restarts.

def _settings_path():
    try:
        folder = bpy.utils.extension_path_user(__package__, path="", create=True)
    except (ValueError, RuntimeError, AttributeError):
        return ""
    return os.path.join(folder, "window.json")


def save_settings():
    path = _settings_path()
    if not path:
        return
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"shown": state.shown, "right": state.right, "top": state.top, "folds": state.folds,
                       "page": state.page}, f)
    except OSError:
        pass


def load_settings():
    path = _settings_path()
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return
    state.right = max(0.0, float(data.get("right", 0.0)))
    state.top = max(0.0, float(data.get("top", 0.0)))
    state.folds.update({str(k): bool(v) for k, v in (data.get("folds") or {}).items()})
    state.page = data.get("page") if data.get("page") in ("tools", "node") else "tools"
    if data.get("shown") and not bpy.app.background:
        state.shown, state.area = True, 0  # (the first 3D view that draws takes it)


# ---- Content ---------------------------------------------------------------------------------

def _pointer_choice(data, name, label, rna):
    """A dropdown for a pointer property: the grooms for the current groom, else the objects its
    definition accepts."""
    if getattr(rna, "fixed_type", None) is None or rna.fixed_type.identifier != "Object":
        return None
    if name == "groom":
        objects = groom.scalps()
    else:
        deferred = getattr(type(data), "__annotations__", {}).get(name)
        accepts = getattr(deferred, "keywords", {}).get("poll")
        objects = [o for o in bpy.data.objects if accepts is None or accepts(data, o)]
    current = getattr(data, name)

    def set_(obj_name):
        obj = bpy.data.objects.get(obj_name)
        if obj is not None:
            setattr(data, name, obj)
            uikit.changed.append(label or name)

    return uikit.Choice(label, [(o.name, o.name, "") for o in objects], lambda: current.name if current else "", set_,
                        rna.description)


def _preview_of(icon_value):
    """The add-on's preview with this icon_value (an alpha's thumbnail, a tool's icon), or None."""
    from . import alphas, icons
    for collection in (alphas._previews[0], icons._previews[0]):
        if collection is None:
            continue
        for preview in collection.values():
            if preview.icon_id == icon_value:
                return preview
    return None


_shapes = {"at": 0.0, "scalp": None, "list": []}


def _tube_shape(t):
    """The library row picked (the Shape tool's Profile): {name, points, smooth, builtin}, or None.
    The library is asked again at most twice a second (the window lays out on every mouse move)."""
    from . import tools
    from .ops import active_groom
    scalp = active_groom(bpy.context)
    if scalp is None or not (0 <= t.tube_shape_index < len(t.tube_shapes)):
        return None
    name = t.tube_shapes[t.tube_shape_index].name
    if _shapes["scalp"] != scalp.name or time.time() - _shapes["at"] > 0.5:
        try:
            _shapes["list"] = tools.tube_shapes(scalp)
        except Exception:  # noqa: BLE001 - no helper yet: no editor
            _shapes["list"] = []
        _shapes["at"], _shapes["scalp"] = time.time(), scalp.name
    shape = next((dict(s) for s in _shapes["list"] if s.get("name") == name), None)
    if shape is not None:
        shape["scalp"] = scalp
    return shape


def _tube_preview(t, shape, points):
    from . import tools
    if shape:
        try:
            tools.preview_tube_shape(shape["scalp"], shape["name"], points, shape.get("smooth", True))
        except Exception as e:  # noqa: BLE001 - a shape the core refuses mid-drag (crossing): no preview
            if DEBUG:
                print("tube preview:", e)


def _tube_commit(t, shape, points):
    """An edit settled: the points stored (one undo step), or (None) the preview dropped."""
    from . import tools
    if not shape:
        return
    scalp = shape["scalp"]
    try:
        if points is None:
            tools.end_preview(scalp)
        else:
            tools.set_tube_shape_points(scalp, shape["name"], points)
    except Exception as e:  # noqa: BLE001 - the core refuses a crossing outline: say why, keep the old
        tools.end_preview(scalp)
        tools.status(bpy.context.scene, str(getattr(e, "message", e)))
    _shapes["at"] = 0.0  # read the library again


def _open_color_picker(context, prop):
    _color_target[0] = (prop.owner, prop.name)
    bpy.ops.wm.call_panel(name=GROOMLAB_PT_window_color.__name__, keep_open=True)


ACTIONS = {
    "color": _open_color_picker,
    "pointer": _pointer_choice,
    "list_scroll": lambda list_id: state.lists.get(list_id, 0),
    "list_area": lambda list_id, rect, overflow: _list_areas.__setitem__(list_id, (rect, overflow)),
    "preview": _preview_of,
    "tube_shape": _tube_shape,
    "tube_preview": _tube_preview,
    "tube_commit": _tube_commit,
}


PAGES = [("tools", "Groom Tools", "The Groom Tools: tools, maps, correctives, photos and their settings"),
         ("node", "Node", "The node selected in the Graph Editor: its settings, as Maya's and the app's side panel")]


def _page_choice():
    def set_page(page):
        state.page = page
        state.scroll = 0.0
        save_settings()
    return uikit.Choice("", PAGES, lambda: state.page, set_page)


def _groom_tree(context):
    from . import graph_tree
    from .ops import active_groom
    return graph_tree.tree_of(active_groom(context))


def _graph_editor_context():
    """A Graph Editor showing the current groom's graph, as a context override (its window, area
    and region), for a node's actions: they ask the node editor for their tree. None: none is open."""
    tree = _groom_tree(bpy.context)
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            space = area.spaces.active
            if area.type == "NODE_EDITOR" and getattr(space, "edit_tree", None) == tree and tree is not None:
                region = next((r for r in area.regions if r.type == "WINDOW"), None)
                return {"window": window, "area": area, "region": region, "space_data": space}
    return None


NODE_ACTIONS = dict(ACTIONS, op_context=_graph_editor_context)


def _node_content(rec, context):
    """The node page: the groom's graph's selected node, as the Graph Editor's sidebar draws it."""
    from . import graph_editor
    tree = _groom_tree(context)
    if tree is None:
        rec.label(text="No groom here yet")
        return
    if not graph_editor.check_tree(tree):
        rec.label(text="Reading the groom's graph...", icon="TIME")
        return
    node = tree.nodes.active
    if node is not None and node.bl_idname == "NodeFrame" and node.select:
        graph_editor.draw_backdrop_panel(rec, context, node)
        return
    if node is None or not graph_editor.ours(node) or not node.select:
        graph_editor.wrap(rec, "Select a node in the Graph Editor to edit it here.")
        rec.operator("groomlab.graph_open", icon="NODE")
        return
    graph_editor.draw_node_panel(rec, context, node)
    if _graph_editor_context() is None:
        rec.separator()
        graph_editor.wrap(rec, "Some of the node's actions run in the Graph Editor: open it to use them.",
                          icon="INFO")


def content(layout, context):
    """Groom Tools (palette.py) and its sub-panels, or the selected node, drawn into the window."""
    rec = drawn.DrawnLayout("column")
    space = context.space_data
    if space is not None and not space.show_gizmo:
        rec.label(text="Turn on the viewport's Gizmos to use this window", icon="ERROR")
    drawn.set_folds(state.folds)
    palette.CHARS[0] = max(20, int(layout.w / (6.4 * layout.s)))  # wrapped hints at the window's width
    actions = ACTIONS
    try:
        if state.page == "node":
            _node_content(rec, context)
            actions = NODE_ACTIONS
        else:
            drawn.draw_panel(palette.VIEW3D_PT_groomlab_tools, rec, context)
            for cls in (palette.VIEW3D_PT_groomlab_tools_sculpting, palette.VIEW3D_PT_groomlab_tools_display):
                if cls.poll(context):
                    head, body = rec.panel("sub:" + cls.__name__, default_closed=True)
                    head.label(text=cls.bl_label)
                    if body is not None:
                        drawn.draw_panel(cls, body, context)
    finally:
        palette.CHARS[0] = None
    _list_areas.clear()
    # The pages' switch on top, then the page.
    choice = _page_choice()
    x, w, h, s = layout.x, layout.w, layout.row, layout.s
    for i, (ident, label, tip) in enumerate(PAGES):
        seg = uikit.Segment("page:%s" % ident, (x + i * (w / 2.0 + 1 * s), layout.y - h, w / 2.0 - 1 * s, h), choice,
                            ident, label, tooltip=tip)
        layout.widgets.append(seg)
    layout.y -= h + 8 * s
    drawn.place(rec, layout, context, actions)


_last_node = [None]


def _watch_node():
    """The node page follows the Graph Editor's selection (a node picked there redraws the window)."""
    if not state.shown or state.page != "node":
        return 0.5
    tree = _groom_tree(bpy.context)
    node = tree.nodes.active if tree is not None else None
    key = (node.name, node.select) if node is not None else None
    if key != _last_node[0]:
        _last_node[0] = key
        _redraw()
    return 0.3


def _window(context, region=None, reuse=False):
    """This frame's window in `region` (the context's); reuse: the one built a moment ago, while the
    pointer only moves (test_select runs on every mouse move)."""
    region = region or context.region
    key = region.as_pointer()
    if reuse:
        hit = _cache.get(key)
        if hit is not None and time.time() - hit[0] < CACHE_SECONDS and state.interaction is None:
            return hit[1]
    win = uikit.Window(state, context, region, TITLE, "logo", content)
    _cache[key] = (time.time(), win)
    return win


def _shows_here(context):
    area = context.area
    return state.shown and area is not None and area.type == "VIEW_3D" and area.as_pointer() == state.area


def _adopt_area(context):
    """The window's 3D view: the one it was opened in while it exists, else this one."""
    if state.area and any(a.as_pointer() == state.area for a in context.screen.areas):
        return
    if context.area is not None and context.area.type == "VIEW_3D":
        state.area = context.area.as_pointer()


def _draw():
    context = bpy.context
    if not state.shown:
        return
    _adopt_area(context)
    if not _shows_here(context):
        return
    try:
        _window(context).draw()
    except Exception as e:  # a draw callback that raises is called again every redraw: say it once
        if getattr(_draw, "_last", None) != str(e):
            _draw._last = str(e)
            import traceback
            print("GroomLab window:", e)
            traceback.print_exc()


def over(context, event):
    """Whether the event's pointer is over the GroomLab window, or a press in it is still going on:
    the GroomLab brush (a modal operator, ahead of the gizmo) passes such events on to it."""
    if not state.shown:
        return False
    if state.interaction is not None:
        return True
    window = context.window
    if window is None:
        return False
    for area in window.screen.areas:
        if area.as_pointer() != state.area:
            continue
        for region in area.regions:
            if region.type == "WINDOW" and uikit.contains((region.x, region.y, region.width, region.height),
                                                          event.mouse_x, event.mouse_y):
                with context.temp_override(window=window, area=area, region=region):
                    rect = _window(bpy.context, region, reuse=True).rect
                return uikit.contains(rect, event.mouse_x - region.x, event.mouse_y - region.y)
    return False


def _redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.tag_redraw()


def _tooltip_due():
    _redraw()
    return None


# ---- Input -----------------------------------------------------------------------------------

class GROOMLAB_GT_window(bpy.types.Gizmo):
    """The whole window as one gizmo: test_select is the hit test (and tracks what is under the
    pointer); a press starts groomlab.window_interact."""
    bl_idname = "GROOMLAB_GT_window"

    def draw(self, context):
        pass

    def test_select(self, context, location):
        x, y = location
        try:
            win = _window(context, reuse=True)
        except Exception:
            return -1
        over_window = uikit.contains(win.rect, x, y)
        widget = win.widget_at(x, y) if over_window else None
        hover = widget.id if widget is not None else None
        if hover != state.hover:
            state.hover_since = time.time()
            if hover is not None:  # its tooltip after a moment, without the pointer moving
                bpy.app.timers.register(_tooltip_due, first_interval=uikit.TOOLTIP_DELAY + 0.05)
            context.region.tag_redraw()
        state.hover, state.mouse = hover, (x, y)
        return 0 if over_window else -1

    def setup(self):
        self.target_set_operator("groomlab.window_interact")


class GROOMLAB_GGT_window(bpy.types.GizmoGroup):
    bl_idname = "GROOMLAB_GGT_window"
    bl_label = "GroomLab Window"
    bl_space_type = "VIEW_3D"
    bl_region_type = "WINDOW"
    bl_options = {"PERSISTENT", "SCALE"}

    @classmethod
    def poll(cls, context):
        return _shows_here(context)

    @classmethod
    def setup_keymap(cls, keyconfig):
        km = keyconfig.keymaps.new(name="GroomLab Window", space_type="VIEW_3D", region_type="WINDOW")
        km.keymap_items.new("gizmogroup.gizmo_tweak", "LEFTMOUSE", "PRESS", any=True)
        km.keymap_items.new("groomlab.window_context", "RIGHTMOUSE", "PRESS", any=True)
        for key, delta in (("WHEELUPMOUSE", -1), ("WHEELDOWNMOUSE", 1)):
            km.keymap_items.new("groomlab.window_scroll", key, "PRESS", any=True).properties.delta = delta
        return km

    def setup(self, context):
        gz = self.gizmos.new(GROOMLAB_GT_window.bl_idname)
        gz.use_tooltip = False  # (the operator's description otherwise; the widgets have their own)

    def refresh(self, context):
        if state.hover is not None and not _shows_here(context):
            state.hover = None


class GROOMLAB_OT_window_toggle(bpy.types.Operator):
    """Show or hide the GroomLab window over this 3D view (preview)"""
    bl_idname = "groomlab.window_toggle"
    bl_label = "GroomLab Window (Preview)"
    bl_options = {"REGISTER"}

    def execute(self, context):
        if state.shown and (context.area is None or context.area.as_pointer() == state.area
                            or context.area.type != "VIEW_3D"):
            uikit.close(state)
            save_settings()
            return {"FINISHED"}
        state.shown = True
        state.area = 0
        if context.area is not None and context.area.type == "VIEW_3D":
            state.area = context.area.as_pointer()
        else:
            views = [a for a in context.screen.areas if a.type == "VIEW_3D"]
            if views:
                state.area = max(views, key=lambda a: a.width * a.height).as_pointer()
        _redraw()
        save_settings()
        return {"FINISHED"}


_RESULTS = {"RUNNING": {"RUNNING_MODAL"}, "FINISHED": {"FINISHED"}, "CANCELLED": {"CANCELLED"},
            "FINISHED_PASS": {"FINISHED", "PASS_THROUGH"}, "CANCELLED_PASS": {"CANCELLED", "PASS_THROUGH"}}


def _push_undo():
    """One undo step for the settings a press changed (the operators a press runs push their own)."""
    if uikit.changed:
        label = uikit.changed[-1]
        del uikit.changed[:]
        try:
            bpy.ops.ed.undo_push(message="GroomLab: %s" % label)
        except RuntimeError:
            pass


class _PressModal(object):
    """What a press in the window runs until it settles: its interaction fed the events (a drag,
    typing, an open menu). Shared by the left press and the right-click operators."""

    def modal(self, context, event):
        interaction = state.interaction
        if interaction is None:
            return {"CANCELLED"}
        win = _window(context)
        state.mouse = (event.mouse_region_x, event.mouse_region_y)
        result = interaction.event(win, context, event)
        if DEBUG:
            print("window event", event.type, event.value, type(interaction).__name__, result)
        if win.interaction is not interaction:  # a slider's click became typing
            state.interaction = win.interaction
            result = "RUNNING"
        if result != "RUNNING":
            state.interaction = None
            if result.startswith("FINISHED"):
                _push_undo()
            else:
                del uikit.changed[:]
            if isinstance(interaction, uikit.MoveInteraction) or not state.shown:
                save_settings()  # where it was moved to, or closed by its ✕
        _redraw()
        return _RESULTS[result]

    def cancel(self, context):
        state.interaction = None


class GROOMLAB_OT_window_interact(_PressModal, bpy.types.Operator):
    """A press in the GroomLab window: a button, a slider's drag or typing, a menu, the title's drag"""
    bl_idname = "groomlab.window_interact"
    bl_label = "GroomLab Window"
    bl_options = {"INTERNAL"}  # (no undo of its own: _push_undo, or the operators it runs)

    def invoke(self, context, event):
        win = _window(context)
        widget = win.widget_at(event.mouse_region_x, event.mouse_region_y)
        if widget is None:
            return {"CANCELLED"}
        state.hover_since = time.time() + 3600.0  # no tooltip over a press
        if widget.disabled:
            return {"FINISHED"}
        del uikit.changed[:]
        interaction = widget.press(win, context, event)
        if DEBUG:
            print("window press", widget.id, event.type, event.value, type(interaction).__name__)
        _redraw()
        if interaction is None:
            _push_undo()
            if not state.shown or isinstance(widget, uikit.PanelHeader):
                save_settings()  # closed by its ✕, or a section folded
            return {"FINISHED"}
        state.interaction = interaction
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

class GROOMLAB_OT_window_context(_PressModal, bpy.types.Operator):
    """A right-click in the GroomLab window: a list row's menu, an editor's point removed (the
    viewport's own menu never opens under the window)"""
    bl_idname = "groomlab.window_context"
    bl_label = "GroomLab Window Menu"
    bl_options = {"INTERNAL"}

    def invoke(self, context, event):
        win = _window(context)
        x, y = event.mouse_region_x, event.mouse_region_y
        widget = win.widget_at(x, y)
        open_menu = getattr(widget, "context_menu", None)
        right_press = getattr(widget, "right_press", None)
        if widget is not None and not widget.disabled:
            del uikit.changed[:]
            if open_menu is not None:
                interaction = open_menu(context, win, event)
                if interaction is not None:
                    state.interaction = interaction
                    context.window_manager.modal_handler_add(self)
                    _redraw()
                    return {"RUNNING_MODAL"}
            elif right_press is not None:  # an editor's: a point removed
                right_press(win, context, event)
                _push_undo()
            _redraw()
        return {"FINISHED"} if uikit.contains(win.rect, x, y) else {"PASS_THROUGH"}


class GROOMLAB_OT_window_scroll(bpy.types.Operator):
    """Scroll the GroomLab window, or the list under the pointer"""
    bl_idname = "groomlab.window_scroll"
    bl_label = "Scroll GroomLab Window"
    bl_options = {"INTERNAL"}

    delta: bpy.props.IntProperty()

    def invoke(self, context, event):
        win = _window(context)
        x, y = event.mouse_region_x, event.mouse_region_y
        for list_id, (rect, overflow) in _list_areas.items():
            if overflow > 0 and uikit.contains(rect, x, y):
                state.lists[list_id] = min(max(state.lists.get(list_id, 0) + self.delta, 0), overflow)
                _redraw()
                return {"FINISHED"}
        if win.overflow <= 0:
            return {"PASS_THROUGH"}
        state.scroll = min(max(state.scroll + self.delta * 40.0, 0.0), win.overflow)
        _redraw()
        return {"FINISHED"}


class GROOMLAB_PT_window_color(bpy.types.Panel):
    """A color swatch's picker (Blender's), opened from the window"""
    bl_space_type = "VIEW_3D"
    bl_region_type = "HEADER"
    bl_label = "Color"
    bl_ui_units_x = 10

    def draw(self, context):
        target = _color_target[0]
        if target is None:
            return
        owner, name = target
        try:
            self.layout.template_color_picker(owner, name, value_slider=True)
            self.layout.prop(owner, name, text="")
        except (ReferenceError, TypeError, KeyError):
            _color_target[0] = None


CLASSES = (GROOMLAB_GT_window, GROOMLAB_GGT_window, GROOMLAB_OT_window_toggle, GROOMLAB_OT_window_interact,
           GROOMLAB_OT_window_context, GROOMLAB_OT_window_scroll, GROOMLAB_PT_window_color)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    _draw_handle[0] = bpy.types.SpaceView3D.draw_handler_add(_draw, (), "WINDOW", "POST_PIXEL")
    kc = bpy.context.window_manager.keyconfigs.addon if bpy.context.window_manager else None
    if kc is not None:
        km = kc.keymaps.new(name="3D View", space_type="VIEW_3D")
        key, mods = TOGGLE_KEY
        _keymaps.append((km, km.keymap_items.new(GROOMLAB_OT_window_toggle.bl_idname, key, "PRESS", **mods)))
    load_settings()
    if not bpy.app.background:
        bpy.app.timers.register(_watch_node, first_interval=1.0, persistent=True)


def unregister():
    if _draw_handle[0] is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_draw_handle[0], "WINDOW")
        _draw_handle[0] = None
    for km, kmi in _keymaps:
        try:
            km.keymap_items.remove(kmi)
        except (ReferenceError, RuntimeError):
            pass
    _keymaps.clear()
    if bpy.app.timers.is_registered(_watch_node):
        bpy.app.timers.unregister(_watch_node)
    if bpy.app.timers.is_registered(_tooltip_due):
        bpy.app.timers.unregister(_tooltip_due)
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
    state.shown = False
    _cache.clear()
    from . import editors
    editors.release()
    uikit.release()
