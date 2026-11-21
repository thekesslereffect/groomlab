# SPDX-License-Identifier: Apache-2.0
"""GroomLab's radial menu in Maya (Alt+S with the GroomLab hotkey set), the standalone app's
(hosts/app/src/marking_menu.cpp) in Python, from engine/ui/marking_menu.json's radialMenu: rings of
pills around the cursor, eight compass points each, as GroomBear's radial menu for Houdini and
RadialZ for ZBrush.

    Down        Masking: Full (every guide unfrozen), Empty (every guide frozen), Select (the
                Select tool: the rest frozen), Quick Select (one pick, then back to the brush),
                Mask Brush (the Freeze brush)
    Left        Visibility: Select (lasso the guides to keep: the rest hide and lock), Show All,
                Invert, Hide/Show Hair, Hide/Show Guides, Groups (the guide groups: one picked
                hides or shows it)
    Up-left     Brushes A: Draw, Comb, Slide, Smooth, Tube, Flow, Place
    Up-right    Brushes B: Lift, Swing, Attract, Part, Cut, Erase
    Right       Brush Options: the picked brush's (Comb's Stretch; Smooth's All, Length, Shape,
                Curve; Tube's Draw Root, Edit Roots, Width, Flatness, Twist, Always Visible)

Every item works on the current groom only (groomlab.active_groom(): the groom picked, selected or
made), its node's name (groomLab1, not groomLab1Shape) shown over the middle: Hide Hair hides that groom's hair, the others' left
as they are.

Alt+S (Radial Menu Settings... sets another shortcut, and the size; the mouse wheel over the
open menu sizes it too) pops it around the cursor at once (show()); move toward an item and let S go to pick it
(key_released(), the hotkey's release, or the S release the menu sees itself). A submenu opens
around the cursor once the cursor reaches its pill (or most of the way to it). Let S go without
moving and the menu stays for clicks: a click picks or opens, Esc / Backspace / the right button /
a click in the middle goes back a ring (at the root: closed), a click away closes it.

    import groomlab.ui.radial_menu as rm
    rm.show()                  # around the cursor
    rm.items()                 # the rings as dicts, as they would show now
    rm.run(item)               # what a pick does
"""

import math

from maya import cmds

import groomlab
from groomlab.ui import definitions, messages, theme, widgets
from groomlab.ui.qt import QtCore, QtGui, QtWidgets

POSITIONS = ("N", "NE", "E", "SE", "S", "SW", "W", "NW")
DEAD_ZONE = 18.0
REACH_X, REACH_Y = 100.0, 72.0
PILL_H, ICON, PAD, GAP, CHECK = 28.0, 16.0, 11.0, 7.0, 7.0
OPEN_SHARE = 0.8
MIN_SCALE, MAX_SCALE = 0.75, 2.5
SCALE_VAR = "groomLabRadialMenuScale"  # the size (Radial Menu Settings..., the wheel over the menu)
KEY_VAR = "groomLabRadialMenuKey"      # the shortcut, as "Alt+S" (hotkeys.install binds it)
DEFAULT_KEY = "Alt+S"
MARGIN = 8.0

_menu = None
_quick_jobs = []


def _direction(position):
    a = position * math.pi / 4.0
    return math.sin(a), -math.cos(a)


def _palette():
    from groomlab.ui import palette
    return palette.current()


def _ctx_exists(ctx):
    return bool(cmds.contextInfo(ctx, exists=True))


# ---------------------------------------------------------------------------
# What the items do, and their state


def current_tool():
    """The GroomLab brush picked: the Groom Tools' tool, else the brush context's mode while it is
    the current tool; "" none."""
    window = _palette()
    if window is not None and getattr(window, "tool", None):
        return window.tool
    ctx = groomlab.BRUSH_CONTEXT
    if _ctx_exists(ctx) and cmds.currentCtx() == ctx:
        return cmds.groomLabBrushCtx(ctx, query=True, mode=True) or ""
    return ""


def tool(mode):
    """A tool started as the Groom Tools' tile (or the hotkeys) starts it."""
    from groomlab.ui import hotkeys
    hotkeys.tool(mode)


def _select(hide):
    """The Select tool, its solo freezing the rest (Masking) or hiding it (Visibility)."""
    tool("select")
    if _ctx_exists(groomlab.SELECT_CONTEXT):
        cmds.groomLabSelectCtx(groomlab.SELECT_CONTEXT, edit=True, hideOthers=bool(hide))
    if hide:
        messages.say("lasso the guides to keep: every other guide hides (a pick of nothing shows them all)",
                     warning=False)


def _groom():
    groom = groomlab.active_groom()
    if not groom:
        raise ValueError("select a groom first")
    return groom


def _layers(groom):
    try:
        return cmds.groomLabGuides(groom, listAttributes=True) or []
    except RuntimeError:
        return []


def mask_full():
    groomlab.unfreeze_all(_groom())


def mask_empty():
    groom = _groom()
    cmds.undoInfo(openChunk=True, chunkName="freeze all guides")
    try:
        if "freeze" in _layers(groom):
            cmds.groomLabGuides(groom, removeAttribute="freeze")
        cmds.groomLabGuides(groom, invertAttribute="freeze")  # (nothing frozen: all of them)
    finally:
        cmds.undoInfo(closeChunk=True)


def _kill_quick_jobs():
    while _quick_jobs:
        job = _quick_jobs.pop()
        if cmds.scriptJob(exists=job):
            cmds.scriptJob(kill=job, force=True)


def quick_select():
    """The Select tool for one pick (the rest frozen), then back to the tool it came from."""
    groom = _groom()
    _kill_quick_jobs()
    back_tool = current_tool() if cmds.currentCtx() == groomlab.BRUSH_CONTEXT else ""
    back_ctx = cmds.currentCtx()
    _select(False)

    def back():
        _kill_quick_jobs()
        if cmds.currentCtx() != groomlab.SELECT_CONTEXT:
            return
        if back_tool:
            tool(back_tool)
        elif back_ctx and back_ctx != groomlab.SELECT_CONTEXT:
            cmds.setToolTo(back_ctx)

    def watch():
        # Another tool picked first: nothing to go back to.
        _quick_jobs.append(cmds.scriptJob(event=["ToolChanged", _kill_quick_jobs], runOnce=True))

    _quick_jobs.append(cmds.scriptJob(attributeChange=[groom + ".guideSelection", lambda: cmds.evalDeferred(back)],
                                      runOnce=True))
    cmds.evalDeferred(watch)  # (after the Select tool's own ToolChanged)
    messages.say("pick the guides to work on" + (": then back to %s" % back_tool.capitalize() if back_tool else ""),
                 warning=False)


def show_all_guides():
    groom = _groom()
    if "hidden" not in _layers(groom):
        messages.say("no guide is hidden", warning=False)
        return
    cmds.groomLabGuides(groom, removeAttribute="hidden")


def invert_hidden():
    cmds.groomLabGuides(_groom(), invertAttribute="hidden")


def _current():
    """The groom the menu works on (the current groom: picked, selected or made), None none."""
    return groomlab.active_groom()


def context():
    """What the menu works on, shown over its middle: the current groom's node (its transform,
    groomLab1 for groomLab1Shape), "" none."""
    groom = _current()
    if not groom:
        return ""
    parent = cmds.listRelatives(groom, parent=True) or []
    return (parent[0] if parent else groom).split("|")[-1]


def hair_shown():
    from groomlab import groom as g
    groom = _current()
    return bool(groom) and g.hair_visible(groom)


def guides_shown():
    from groomlab import groom as g
    groom = _current()
    return bool(groom) and g.guides_visible(groom)


def toggle_hair():
    """The current groom's hair shown or hidden (the others' left as they are)."""
    from groomlab import groom as g
    groom = _groom()
    g.set_hair_visible(groom, not g.hair_visible(groom))


def toggle_guides():
    """The current groom's guides shown or hidden (the others' left as they are)."""
    from groomlab import groom as g
    groom = _groom()
    g.set_guides_visible(groom, not g.guides_visible(groom))


def tubes_always():
    window = _palette()
    if window is not None and hasattr(window, "tube_always"):
        return window.tube_always.isChecked()
    ctx = groomlab.BRUSH_CONTEXT
    return bool(_ctx_exists(ctx) and cmds.groomLabBrushCtx(ctx, query=True, tubesAlwaysVisible=True))


def toggle_tubes_always():
    on = not tubes_always()
    window = _palette()
    if window is not None and hasattr(window, "tube_always"):
        window.tube_always.setChecked(on)  # (it tells the tool)
    elif _ctx_exists(groomlab.BRUSH_CONTEXT):
        cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, tubesAlwaysVisible=on)


def _root_card():
    """The Groom Tools' Tube card's root block, the Tube tool up (the palette opened for it)."""
    from groomlab.ui import palette
    window = palette.current() or palette.show()
    if window.tool != "shape":
        window.activate("shape")
    return window.root_card


def root_mode(which):
    window = _palette()
    if window is None or getattr(window, "tool", None) != "shape":
        return False
    card = window.root_card
    return (card.draw_root if which == "draw" else card.edit_root).isChecked()


def draw_root():
    _root_card().draw_root.click()


def edit_roots():
    _root_card().edit_root.click()


# id: (run, state or None). A state makes the item checked (or, with offLabel, its label).
ACTIONS = {
    "mask.full": (mask_full, None),
    "mask.empty": (mask_empty, None),
    "mask.quickSelect": (quick_select, None),
    "visibility.select": (lambda: _select(True), None),
    "visibility.showAll": (show_all_guides, None),
    "visibility.invert": (invert_hidden, None),
    "display.toggleHair": (toggle_hair, hair_shown),
    "display.toggleGuides": (toggle_guides, guides_shown),
    "display.tubesAlways": (toggle_tubes_always, tubes_always),
    "tube.drawRoot": (draw_root, lambda: root_mode("draw")),
    "tube.editRoot": (edit_roots, lambda: root_mode("edit")),
}


# The brush options: a setting's switch, or one of its values.
def option_value(setting):
    window = _palette()
    ctx = groomlab.BRUSH_CONTEXT
    if setting == "stretch":
        if window is not None and hasattr(window, "stretch"):
            return window.stretch.isChecked()
        return bool(_ctx_exists(ctx) and cmds.groomLabBrushCtx(ctx, query=True, stretch=True))
    if setting == "tubeChannel":
        if window is not None and hasattr(window, "tube_channel_name"):
            return window.tube_channel_name()
        return cmds.groomLabBrushCtx(ctx, query=True, tubeChannel=True) if _ctx_exists(ctx) else "width"
    if setting == "smoothMode":
        return cmds.groomLabBrushCtx(ctx, query=True, smoothMode=True) if _ctx_exists(ctx) else "curve"
    return None


def set_option(setting, value):
    window = _palette()
    ctx = groomlab.BRUSH_CONTEXT
    if setting == "stretch":
        on = not option_value("stretch")
        if window is not None and hasattr(window, "stretch"):
            window.stretch.setChecked(on)  # (it tells the tool)
        elif _ctx_exists(ctx):
            cmds.groomLabBrushCtx(ctx, edit=True, stretch=on)
    elif setting == "tubeChannel":
        buttons = getattr(window, "tube_channel_buttons", {}) if window is not None else {}
        if value in buttons:
            buttons[value].setChecked(True)
            window.push(tubeChannel=value)
        elif _ctx_exists(ctx):
            cmds.groomLabBrushCtx(ctx, edit=True, tubeChannel=value)
    elif setting == "smoothMode" and _ctx_exists(ctx):
        cmds.groomLabBrushCtx(ctx, edit=True, smoothMode=value)
    if value:
        messages.say(definitions.choice_label(setting, value, value), warning=False)
    else:
        messages.say("%s %s" % (setting.capitalize(), "on" if option_value(setting) else "off"), warning=False)


# ---------------------------------------------------------------------------
# The rings


def _brush(tool_id):
    import groomlab_core
    for entry in groomlab_core.brush_catalog():
        if entry["id"] == tool_id:
            return entry
    return None


def _category_color(category):
    key = {"Create": "create", "Shape": "shape", "Trim": "trim", "Paint": "paint"}.get(category)
    t = theme.current()
    return t.qcolor("icons." + key) if key else QtGui.QColor("#FFFFFF")


def _item(kind, item_id, label, position, **more):
    out = {"kind": kind, "id": item_id, "value": "", "label": label, "tip": "", "icon": "", "color": None,
           "position": POSITIONS.index(position) if position in POSITIONS else -1, "checkable": False,
           "checked": False, "enabled": True, "items": []}
    out.update(more)
    return out


def _action_item(action_id, entry, position):
    action = definitions.action(action_id) or {}
    run_state = ACTIONS.get(action_id)
    if run_state is None:
        return None
    item = _item("action", action_id, entry.get("label", action.get("label", action_id)), position,
                 tip=action.get("tip", ""), icon=entry.get("icon", action.get("icon", "")))
    state = run_state[1]
    if state is not None:
        try:
            on = bool(state())
        except (RuntimeError, ValueError):
            on = False
        if "offLabel" in entry:  # the label says the state: Hide Hair while it shows
            if not on:
                item["label"] = entry["offLabel"]
        else:
            item["checkable"], item["checked"] = True, on
    return item


def _brush_options(submenu):
    current = current_tool()
    brush = _brush(current)
    name = brush["label"] if brush else "the brush"
    options = definitions.get("radialMenu/brushOptions")
    positions = options["positions"]
    k = 0
    for row in options["tools"].get(current, []):
        if k >= len(positions):
            break
        position = positions[k]
        if "action" in row:
            item = _action_item(row["action"], row, position)
        else:
            setting, value = row["setting"], row.get("value", "")
            now = option_value(setting)
            if now is None:
                continue
            if value:
                choice = definitions.choice(setting, value) or {}
                item = _item("option", setting, row.get("label", choice.get("label", value)), position, value=value,
                             tip=choice.get("tip", ""), checkable=True, checked=now == value)
            else:
                item = _item("option", setting, row.get("label", setting.capitalize()), position, checkable=True,
                             checked=bool(now))
        if item is None:
            continue
        k += 1
        submenu["items"].append(item)
    if not submenu["items"]:
        submenu["enabled"] = False
        submenu["tip"] = "%s has no options here (pick Comb, Smooth or Tube)" % name
    else:
        submenu["tip"] = "%s's options" % name


def _guide_groups(submenu):
    """Visibility > Groups: the current groom's guide groups (parents before children), each picked
    to hide or show it, drawn in its color; past eight, the eighth place a More ring."""
    groom = _current()
    if not groom:
        submenu["tip"] = "select a groom first"
        return
    from groomlab import guides
    try:
        groups = guides.list_guide_groups(groom)
        hidden = set(guides.hidden_guide_groups(groom))
        own = guides.guide_group_colors(groom)
    except (RuntimeError, ValueError):
        groups = []
    if not groups:
        submenu["tip"] = "%s has no guide groups yet" % context()
        return
    rows = []
    for group in groups:
        name = group["name"]
        shown = name not in hidden
        color = own.get(name) or group.get("color")
        rows.append(_item("option", "guideGroup", name, "", value=name, checkable=True, checked=shown,
                          tip=("Hide %s (its hair still grows)" if shown else "Show %s") % name,
                          color=QtGui.QColor(color) if color else None))

    def fill(into, start):
        left = len(rows) - start
        here = 7 if left > 8 else left
        for k in range(here):
            row = dict(rows[start + k])
            row["position"] = k
            into["items"].append(row)
        if left > 8:
            more = _item("submenu", "guideGroups.more", "More", "NW", tip="The other %d groups" % (left - 7))
            fill(more, start + 7)
            into["items"].append(more)

    fill(submenu, 0)


def show_group(name, visible):
    """A guide group of the current groom shown or hidden (the Guides panel's checkbox)."""
    from groomlab import guides
    guides.show_guide_group(_groom(), name, visible)
    messages.say("%s %s" % (name, "shown" if visible else "hidden"), warning=False)


def _ring(entries):
    out = []
    for entry in entries:
        position = entry.get("position", "")
        if "tool" in entry:
            brush = _brush(entry["tool"])
            if brush is None:
                continue
            item = _item("tool", entry["tool"], entry.get("label", brush["label"]), position, tip=brush.get("hint", ""),
                         icon=brush.get("icon", entry["tool"]), color=_category_color(brush.get("category")))
        elif "action" in entry:
            item = _action_item(entry["action"], entry, position)
        else:
            item = _item("submenu", entry.get("generator", entry.get("label", "")), entry.get("label", ""), position,
                         tip=entry.get("tip", ""), icon=entry.get("icon", ""))
            if entry.get("generator") == "brushOptions":
                _brush_options(item)
            elif entry.get("generator") == "guideGroups":
                _guide_groups(item)
            else:
                item["items"] = _ring(entry.get("items", []))
            if not item["items"]:
                item["enabled"] = False
        if item is not None:
            out.append(item)
    return out


def items():
    """The root ring as it would show now: dicts with kind ("tool", "action", "option", "submenu"),
    id, value, label, tip, icon, color, position (0 N ... 7 NW), checkable, checked, enabled and
    items (a submenu's ring)."""
    return _ring(definitions.get("radialMenu/items"))


def run(item):
    """What picking an item does: a tool started, an action run, an option set."""
    try:
        if item["kind"] == "tool":
            if item["id"] == "select":
                _select(False)
            else:
                tool(item["id"])
        elif item["kind"] == "action":
            ACTIONS[item["id"]][0]()
        elif item["kind"] == "option" and item["id"] == "guideGroup":
            show_group(item["value"], not item["checked"])
        elif item["kind"] == "option":
            set_option(item["id"], item["value"])
    except (RuntimeError, ValueError) as e:
        messages.say(messages.plain(e))


# ---------------------------------------------------------------------------
# The widget


class RadialMenu(QtWidgets.QWidget):
    """A frameless see-through window over the screen the cursor is on, the rings painted on it."""

    def __init__(self, parent=None):
        flags = QtCore.Qt.FramelessWindowHint | QtCore.Qt.Tool | QtCore.Qt.NoDropShadowWindowHint
        super(RadialMenu, self).__init__(parent, flags)
        self.setObjectName("groomLabRadialMenu")
        self.setAttribute(QtCore.Qt.WA_TranslucentBackground)
        self.setMouseTracking(True)
        self.setFocusPolicy(QtCore.Qt.StrongFocus)
        self.root = []
        self.context = ""
        self.levels = []  # [{"path": [...], "center": QPointF, "rects": [QRectF], "opened": -1}]
        self.cursor = QtCore.QPointF()
        self.press_at = QtCore.QPointF()
        self.hovered = -1
        self.key_held = False
        self.moved = False
        self.scale = scale()
        self.key = Qt_key(shortcut())

    # -- the rings --------------------------------------------------------------------

    def dead(self):
        """The dead zone at the menu's size."""
        return DEAD_ZONE * self.scale

    def set_scale(self, value):
        self.scale = min(max(float(value), MIN_SCALE), MAX_SCALE)
        for level in self.levels:
            self.layout_level(level)
        self.update()

    def items_of(self, level):
        ring = self.root
        for i in level["path"]:
            ring = ring[i]["items"]
        return ring

    def ring(self):
        return self.items_of(self.levels[-1]) if self.levels else []

    def center(self):
        return self.levels[-1]["center"] if self.levels else QtCore.QPointF()

    def _font(self):
        font = theme.font(12)
        font.setWeight(QtGui.QFont.Medium)
        return font

    def pill_width(self, item):
        w = QtGui.QFontMetricsF(self._font()).horizontalAdvance(item["label"]) + 2.0 * PAD
        if item["icon"]:
            w += ICON + GAP
        if item["checkable"]:
            w += CHECK * 2.0 + GAP
        if item["kind"] == "submenu":
            w += 10.0
        return math.ceil(w)

    def layout_level(self, level):
        ring = self.items_of(level)
        rects = []
        bounds = QtCore.QRectF(-self.dead(), -self.dead(), 2 * self.dead(), 2 * self.dead())
        for item in ring:
            if item["position"] < 0:
                rects.append(QtCore.QRectF())
                continue
            dx, dy = _direction(item["position"])
            ax, ay = dx * REACH_X, dy * REACH_Y
            w = self.pill_width(item)
            left = ax - w / 2.0
            if dx > 0.3:
                left = ax - 18.0
            elif dx < -0.3:
                left = ax - w + 18.0
            s = self.scale
            r = QtCore.QRectF(left * s, (ay - PILL_H / 2.0) * s, w * s, PILL_H * s)
            rects.append(r)
            bounds = bounds.united(r)
        area = QtCore.QRectF(self.rect()).adjusted(MARGIN, MARGIN, -MARGIN, -MARGIN)
        c = level["center"]
        if area.width() > bounds.width() and area.height() > bounds.height():
            c = QtCore.QPointF(min(max(c.x(), area.left() - bounds.left()), area.right() - bounds.right()),
                               min(max(c.y(), area.top() - bounds.top()), area.bottom() - bounds.bottom()))
            level["center"] = c
        level["rects"] = [r.translated(c) if r.isValid() else r for r in rects]

    def item_rect(self, index):
        rects = self.levels[-1]["rects"] if self.levels else []
        return rects[index] if 0 <= index < len(rects) else QtCore.QRectF()

    def item_at(self, at):
        if not self.levels:
            return -1
        level = self.levels[-1]
        for i, r in enumerate(level["rects"]):
            if r.isValid() and r.contains(at):
                return i
        d = at - level["center"]
        length = math.hypot(d.x(), d.y())
        if length < self.dead():
            return -1
        best, best_dot = -1, math.cos(math.pi / 8.0) - 1e-6
        for i, item in enumerate(self.items_of(level)):
            if item["position"] < 0:
                continue
            dx, dy = _direction(item["position"])
            dot = (d.x() * dx + d.y() * dy) / length
            if dot > best_dot:
                best, best_dot = i, dot
        return best

    def index_of(self, key):
        for i, item in enumerate(self.ring()):
            if item["id"] == key or (item["kind"] == "submenu" and item["label"] == key):
                return i
        return -1

    # -- showing ----------------------------------------------------------------------

    def popup(self, global_pos, key_held=True):
        """Shown around `global_pos` (screen coordinates) over the screen it is on, the items as
        they are now; `key_held`: the hotkey is down (letting it go picks)."""
        self.root = items()
        self.context = context()  # the groom it works on, over the middle
        self.scale = scale()
        self.key = Qt_key(shortcut())
        app = QtWidgets.QApplication.instance()
        screen = app.screenAt(global_pos) if hasattr(app, "screenAt") else None
        geometry = screen.geometry() if screen else app.desktop().screenGeometry(global_pos)
        self.setGeometry(geometry)
        at = QtCore.QPointF(self.mapFromGlobal(global_pos))
        self.cursor = self.press_at = at
        self.key_held = key_held
        self.moved = False
        self.hovered = -1
        self.levels = [{"path": [], "center": at, "rects": [], "opened": -1}]
        self.layout_level(self.levels[0])
        self.show()
        self.raise_()
        self.activateWindow()
        self.setFocus(QtCore.Qt.PopupFocusReason)
        self.grabKeyboard()
        self.hover(at)

    def dismiss(self):
        if not self.isVisible():
            return
        self.key_held = False
        self.hovered = -1
        self.releaseKeyboard()
        self.hide()

    def open(self, index, at=None):
        ring = self.ring()
        if not 0 <= index < len(ring):
            return
        item = ring[index]
        if item["kind"] != "submenu" or not item["enabled"] or not item["items"]:
            return
        level = self.levels[-1]
        level["opened"] = index
        nxt = {"path": level["path"] + [index], "center": at if at is not None else self.item_rect(index).center(),
               "rects": [], "opened": -1}
        self.levels.append(nxt)
        self.layout_level(nxt)
        self.hovered = -1
        self.update()

    def back(self):
        if len(self.levels) <= 1:
            self.dismiss()
            return
        self.levels.pop()
        self.levels[-1]["opened"] = -1
        self.hovered = -1
        self.update()

    def hover(self, at):
        self.cursor = at
        if len(self.levels) > 1:
            c = self.levels[-2]["center"]
            if math.hypot(at.x() - c.x(), at.y() - c.y()) < self.dead():
                self.back()
                self.cursor = at
        self.hovered = self.item_at(at)
        if self.hovered >= 0:
            item = self.ring()[self.hovered]
            r = self.item_rect(self.hovered)
            c = self.center()
            far = math.hypot(at.x() - c.x(), at.y() - c.y()) >= OPEN_SHARE * math.hypot(r.center().x() - c.x(),
                                                                                         r.center().y() - c.y())
            reached = r.contains(at) or (self.key_held and far)
            if item["kind"] == "submenu" and item["enabled"] and item["items"] and reached:
                self.open(self.hovered, at)
                self.hovered = self.item_at(at)
        self.update()

    def pick(self, index):
        item = self.ring()[index]
        self.dismiss()
        # (after the menu is gone: a tool's own window or message shows over Maya)
        QtCore.QTimer.singleShot(0, lambda: run(item))

    def key_released(self):
        """The hotkey let go: what the cursor moved onto is picked (a submenu stays open), else the
        menu stays for clicks."""
        if not self.key_held:
            return
        self.key_held = False
        if not self.moved or self.hovered < 0:
            return
        item = self.ring()[self.hovered]
        if item["kind"] != "submenu" and item["enabled"]:
            self.pick(self.hovered)

    # -- events -----------------------------------------------------------------------

    def mouseMoveEvent(self, event):
        at = QtCore.QPointF(event.position() if hasattr(event, "position") else event.pos())
        if math.hypot(at.x() - self.press_at.x(), at.y() - self.press_at.y()) >= self.dead():
            self.moved = True
        self.hover(at)

    def mousePressEvent(self, event):
        at = QtCore.QPointF(event.position() if hasattr(event, "position") else event.pos())
        if event.button() == QtCore.Qt.RightButton:
            self.back()
            return
        if event.button() != QtCore.Qt.LeftButton:
            self.dismiss()
            return
        index = self.item_at(at)
        if index >= 0:
            item = self.ring()[index]
            if item["kind"] == "submenu":
                self.open(index, at)
            elif item["enabled"]:
                self.pick(index)
            return
        c = self.center()
        if math.hypot(at.x() - c.x(), at.y() - c.y()) < self.dead() * 2.0:
            self.back()
        else:
            self.dismiss()

    def keyPressEvent(self, event):
        if event.key() in (QtCore.Qt.Key_Escape, QtCore.Qt.Key_Backspace):
            self.back()
            return
        if event.key() == self.key and not event.isAutoRepeat() and not self.key_held:
            self.dismiss()  # the shortcut again: closed
            return
        event.accept()

    def wheelEvent(self, event):
        """Up: bigger, down: smaller, a tenth a notch (kept for next time)."""
        notches = int(event.angleDelta().y() / 120)
        if notches:
            self.set_scale(round((self.scale + 0.1 * notches) * 10.0) / 10.0)
            set_scale(self.scale)
        event.accept()

    def keyReleaseEvent(self, event):
        if event.key() == self.key and not event.isAutoRepeat():
            self.key_released()
        event.accept()

    # -- painting ---------------------------------------------------------------------

    def paintEvent(self, _event):
        if not self.levels:
            return
        t = theme.current()
        p = QtGui.QPainter(self)
        p.setRenderHint(QtGui.QPainter.Antialiasing)
        # (an all but clear fill: a see-through window gets no mouse where it is fully clear)
        p.fillRect(self.rect(), QtGui.QColor(0, 0, 0, 1))
        p.setFont(self._font())
        panel = t.qcolor(t.token("card"))
        edge = t.qcolor(t.token("cardRule"))
        accent = t.qcolor(t.color("ui.accent"))
        text_color = t.qcolor(t.color("ui.text"))
        guide = QtGui.QColor(text_color)
        guide.setAlphaF(0.55)

        def readable(fill):
            lum = 0.2126 * fill.redF() + 0.7152 * fill.greenF() + 0.0722 * fill.blueF()
            return QtGui.QColor("#101010") if lum > 0.55 else QtGui.QColor("#FFFFFF")

        s = self.scale

        def draw_pill(item, box, lit, hub):
            # (drawn at the designed size, scaled into its box)
            p.save()
            p.translate(box.topLeft())
            p.scale(s, s)
            r = QtCore.QRectF(0.0, 0.0, box.width() / s, box.height() / s)
            fill = accent if hub or item["color"] is None else item["color"]
            ink = readable(fill) if lit or hub else QtGui.QColor(text_color)
            if not item["enabled"]:
                ink.setAlphaF(0.4)
            p.setPen(QtCore.Qt.NoPen)
            p.setBrush(QtGui.QColor(0, 0, 0, 90))
            p.drawRoundedRect(r.translated(0.0, 2.0), 7.0, 7.0)
            p.setBrush(fill if lit or hub else panel)
            p.setPen(QtGui.QPen(fill if lit or hub else edge, 1.0))
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 7.0, 7.0)
            x = r.left() + PAD
            if item["checkable"]:
                mark = ink if lit or hub else accent
                p.setPen(QtGui.QPen(mark, 1.4))
                p.setBrush(QtGui.QBrush(mark) if item["checked"] else QtGui.QBrush(QtCore.Qt.NoBrush))
                p.drawEllipse(QtCore.QPointF(x + CHECK, r.center().y()), CHECK - 2.0, CHECK - 2.0)
                x += CHECK * 2.0 + GAP
            if item["icon"]:
                icon = widgets.icon(item["icon"])
                if not icon.isNull():
                    icon.paint(p, QtCore.QRect(int(x), int(r.center().y() - ICON / 2.0), int(ICON), int(ICON)))
                x += ICON + GAP
            chevron = item["kind"] == "submenu" and not hub
            right = r.right() - PAD + 2.0 - (10.0 if chevron else 0.0)
            p.setPen(ink)
            p.drawText(QtCore.QRectF(x, r.top(), right - x, r.height()), QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft,
                       item["label"])
            if chevron:
                cx, cy = r.right() - PAD + 1.0, r.center().y()
                p.setPen(QtGui.QPen(ink, 1.4, QtCore.Qt.SolidLine, QtCore.Qt.RoundCap, QtCore.Qt.RoundJoin))
                p.setBrush(QtCore.Qt.NoBrush)
                p.drawPolyline(QtGui.QPolygonF([QtCore.QPointF(cx - 3.0, cy - 3.5), QtCore.QPointF(cx + 0.5, cy),
                                                QtCore.QPointF(cx - 3.0, cy + 3.5)]))
            p.restore()

        # The trail of the rings left behind.
        for a, b in zip(self.levels, self.levels[1:]):
            trail = QtGui.QColor(guide)
            trail.setAlphaF(0.3)
            p.setPen(QtGui.QPen(trail, 1.5 * s, QtCore.Qt.DashLine, QtCore.Qt.RoundCap))
            p.drawLine(a["center"], b["center"])
            p.setPen(QtGui.QPen(trail, 1.5 * s))
            p.setBrush(QtCore.Qt.NoBrush)
            p.drawEllipse(a["center"], 4.0 * s, 4.0 * s)
        level = self.levels[-1]
        c = level["center"]
        d = self.cursor - c
        length = math.hypot(d.x(), d.y())
        if length >= self.dead():
            line = QtGui.QColor(guide)
            line.setAlphaF(0.35)
            p.setPen(QtGui.QPen(line, 1.5 * s, QtCore.Qt.SolidLine, QtCore.Qt.RoundCap))
            p.drawLine(c + d * (6.0 * s / length), self.cursor)
        if len(self.levels) > 1:
            src = self.levels[-2]
            sub = self.items_of(src)[src["opened"]]
            w, h = (self.pill_width(sub) - 10.0) * s, PILL_H * s
            draw_pill(sub, QtCore.QRectF(c.x() - w / 2.0, c.y() - h / 2.0, w, h), False, True)
        else:
            p.setPen(QtGui.QPen(guide, 1.5 * s))
            p.setBrush(QtCore.Qt.NoBrush)
            p.drawEllipse(c, 5.0 * s, 5.0 * s)
            if self.context:  # what it works on, over the middle (the root ring keeps N free)
                p.save()
                p.translate(c)
                p.scale(s, s)
                font = QtGui.QFont(self._font())
                font.setItalic(True)
                p.setFont(font)
                p.setPen(guide)
                p.drawText(QtCore.QRectF(-160.0, -REACH_Y - PILL_H / 2.0, 320.0, PILL_H), QtCore.Qt.AlignCenter,
                           self.context)
                p.restore()
        for i, item in enumerate(self.items_of(level)):
            r = level["rects"][i]
            if r.isValid():
                draw_pill(item, r, i == self.hovered and item["enabled"], False)
        p.end()


# ---------------------------------------------------------------------------
# The person's settings: the size and the shortcut (optionVars, every Maya session)


def scale():
    """The menu's size: 1 as designed, up to MAX_SCALE."""
    if cmds.optionVar(exists=SCALE_VAR):
        return min(max(float(cmds.optionVar(query=SCALE_VAR)), MIN_SCALE), MAX_SCALE)
    return 1.0


def set_scale(value):
    value = min(max(float(value), MIN_SCALE), MAX_SCALE)
    cmds.optionVar(floatValue=(SCALE_VAR, value))
    if _menu is not None:
        try:
            _menu.set_scale(value)
        except RuntimeError:
            pass
    return value


def shortcut():
    """The shortcut as text ("Alt+S")."""
    return (cmds.optionVar(query=KEY_VAR) if cmds.optionVar(exists=KEY_VAR) else "") or DEFAULT_KEY


def Qt_key(text):
    """The key of a shortcut's text, its modifiers aside (S of Alt+S), as Qt names it."""
    seq = QtGui.QKeySequence(text)
    if seq.isEmpty():
        return QtCore.Qt.Key_S
    k = seq[0]
    if hasattr(k, "key"):  # (Qt 6: a QKeyCombination)
        return k.key()
    return int(k) & ~int(QtCore.Qt.KeyboardModifierMask)


def set_shortcut(text):
    """The shortcut changed: kept, and bound in the GroomLab hotkey set (made if missing), the old
    key given back what Maya's set binds to it. Returns the shortcut as kept."""
    from groomlab.ui import hotkeys
    seq = QtGui.QKeySequence(text or DEFAULT_KEY)
    text = seq.toString(QtGui.QKeySequence.PortableText) if not seq.isEmpty() else DEFAULT_KEY
    hotkeys.bind_radial(text, previous=shortcut())
    cmds.optionVar(stringValue=(KEY_VAR, text))
    return text


class SettingsWindow(QtWidgets.QDialog):
    """GroomLab > Radial Menu Settings...: the shortcut and the size, live."""

    def __init__(self, parent=None):
        super(SettingsWindow, self).__init__(parent)
        self.setObjectName("groomLabRadialMenuSettings")
        self.setWindowTitle("Radial Menu Settings")
        form = QtWidgets.QFormLayout()
        self.keys = QtWidgets.QKeySequenceEdit(QtGui.QKeySequence(shortcut()), self)
        if hasattr(self.keys, "setMaximumSequenceLength"):
            self.keys.setMaximumSequenceLength(1)
        self.keys.setToolTip("Click and press the keys that open the radial menu (Alt+S to start with). Hold them, "
                             "move onto an item and let go of the key to pick it. Bound in the GroomLab hotkey set")
        self.keys.editingFinished.connect(self._keys_done)
        self.note = QtWidgets.QLabel(self)
        self.note.setWordWrap(True)
        self.size = QtWidgets.QSpinBox(self)
        self.size.setRange(int(MIN_SCALE * 100), int(MAX_SCALE * 100))
        self.size.setSingleStep(10)
        self.size.setSuffix(" %")
        self.size.setValue(int(round(scale() * 100)))
        self.size.setToolTip("How big the radial menu draws: its pills, text and reach (the mouse wheel over the open "
                             "menu sizes it too)")
        self.size.valueChanged.connect(lambda percent: set_scale(percent / 100.0))
        form.addRow("Shortcut", self.keys)
        form.addRow("", self.note)
        form.addRow("Size", self.size)
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.RestoreDefaults |
                                             QtWidgets.QDialogButtonBox.Close, parent=self)
        buttons.button(QtWidgets.QDialogButtonBox.RestoreDefaults).clicked.connect(self._defaults)
        buttons.rejected.connect(self.close)
        layout = QtWidgets.QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)
        self._say("")

    def _say(self, text):
        self.note.setText(text)
        self.note.setVisible(bool(text))

    def _keys_done(self):
        seq = self.keys.keySequence()
        try:
            text = set_shortcut(seq.toString(QtGui.QKeySequence.PortableText) if not seq.isEmpty() else DEFAULT_KEY)
        except (RuntimeError, ValueError) as e:
            self._say(messages.plain(e))
            return
        self.keys.setKeySequence(QtGui.QKeySequence(text))
        self._say("%s opens the radial menu (the GroomLab hotkey set is on)" % text)

    def _defaults(self):
        self.keys.setKeySequence(QtGui.QKeySequence(DEFAULT_KEY))
        self._keys_done()
        self.size.setValue(100)


_settings = None


def show_settings():
    """GroomLab > Radial Menu Settings...: one window, raised when open."""
    global _settings
    try:
        if _settings is not None and _settings.isVisible():
            _settings.raise_()
            _settings.activateWindow()
            return _settings
    except RuntimeError:
        pass
    _settings = SettingsWindow(widgets._maya_main_window())
    _settings.show()
    return _settings


def menu():
    """The radial menu's widget (made the first time)."""
    global _menu
    try:
        if _menu is not None:
            _menu.isVisible()  # (raises when Maya deleted it)
            return _menu
    except RuntimeError:
        pass
    _menu = RadialMenu(widgets._maya_main_window())
    return _menu


def show():
    """Alt+S: the menu around the cursor (shown: closed)."""
    m = menu()
    if m.isVisible():
        if not m.key_held:  # (a key repeat while S is held leaves it)
            m.dismiss()
        return
    m.popup(QtGui.QCursor.pos(), True)


def key_released():
    """The hotkey's release (when Maya sees it rather than the menu)."""
    if _menu is not None and _menu.isVisible():
        _menu.key_released()
