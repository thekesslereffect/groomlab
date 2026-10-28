# SPDX-License-Identifier: Apache-2.0
"""Shared widgets for GroomLab's panels (Qt: groomlab.ui.qt).

SoftSlider: a slider with a number field. The slider covers a soft range
guessed from the value (0-1 for fractions, a round number above larger
values); typing a value outside it stretches the range to reach it, and the
right-click menu fits or resets it. Stretched ranges are remembered per key
(for example "Clump.strength") across Maya sessions.

icon(name): a GroomLab icon, in the theme's colors (see ICON_GROUPS).
theme_menu(parent): the Theme menu (pick, make, reload, open the folder).
"""

import json
import math
import os

from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtGui, QtWidgets

ICON_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), os.pardir, os.pardir, os.pardir, "icons"))
RANGES_VAR = "groomLabSoftRanges"
_ranges = None  # key -> [lo, hi], loaded from the optionVar on first use


# Icons are drawn with placeholder colors (tools/make_icons.py) that the theme
# replaces: each tool group has its color (theme "icons": {group: color}), a
# darker shade and a lighter tint of it; "ink" is hair and neutral parts.
ICON_GROUPS = {
    "create": ("place", "draw", "part", "flow", "select"),
    "shape": ("comb", "slide", "smooth", "straighten", "lift", "twist", "curl", "attract", "direction", "noise"),
    "trim": ("cut", "length", "shape", "erase"),
    "paint": ("paint", "freeze", "unfreeze"),
}
PLACEHOLDERS = {"main": "#3D7EFF", "shade": "#1F4FB3", "tint": "#A8C7FF", "ink": "#F2F2F5", "shine": "#FFFFFF",
                "red": "#FF4D50", "yellow": "#FFC53E", "blue": "#2F7BF7", "emerald": "#2ECC90"}
_svg_cache = {}


# ---------------------------------------------------------------------------
# Small screens: a window opens within the screen it is on (QScreen.availableGeometry: a 1366x768
# laptop, or 1920x1080 with Maya's UI at 125-150%, has less room than GroomLab's defaults ask).

def available_geometry(widget=None):
    """(x, y, width, height) of the usable area of the screen `widget` is on, else Maya's main
    window's, else the primary screen's; None without a screen."""
    screen = None
    for w in (widget, _maya_main_window()):
        if w is None:
            continue
        try:
            screen = w.screen() if hasattr(w, "screen") else None
        except RuntimeError:
            screen = None
        if screen is None and hasattr(QtGui.QGuiApplication, "screenAt"):
            try:
                screen = QtGui.QGuiApplication.screenAt(w.mapToGlobal(w.rect().center()))
            except RuntimeError:
                screen = None
        if screen is not None:
            break
    if screen is None:
        screen = QtGui.QGuiApplication.primaryScreen()
    if screen is None:
        return None
    g = screen.availableGeometry()
    return (g.x(), g.y(), g.width(), g.height())


def _maya_main_window():
    try:
        from maya import OpenMayaUI
        pointer = OpenMayaUI.MQtUtil.mainWindow()
        return qt.wrap_instance(pointer, QtWidgets.QWidget) if pointer else None
    except Exception:  # standalone, a stand-in Maya, no Qt binding for the pointer
        return None


def fit_size(width, height, available, share=0.9, minimum=(480, 320)):
    """(width, height) no larger than `share` of `available` ((x, y, w, h), None: as asked) and
    no smaller than `minimum` unless the screen itself is."""
    if not available:
        return int(width), int(height)
    room_w, room_h = available[2], available[3]
    w = min(int(width), int(room_w * share))
    h = min(int(height), int(room_h * share))
    return min(max(w, minimum[0]), room_w), min(max(h, minimum[1]), room_h)


def fit_to_screen(widget, width, height, share=0.9):
    """Resize `widget` to (width, height) fitted to its screen (fit_size); returns the size."""
    size = fit_size(width, height, available_geometry(widget), share)
    widget.resize(*size)
    return size


def crowds_screen(size, available, share=(0.72, 0.62)):
    """Whether a floating window of `size` would cover so much of the screen (more than `share` of
    its width or height) that the viewport behind it could not be seen: better docked then."""
    if not available:
        return False
    return size[0] > available[2] * share[0] or size[1] > available[3] * share[1]


def alive(widget):
    """False once Qt has deleted the widget under its Python wrapper (a dock closed without
    closeEvent, a workspace change): callbacks and scriptJobs check it before touching the widget."""
    return qt.shiboken.isValid(widget)


class busy(object):
    """While a window's long action runs (Generate, Transfer, Bake Maps...): its buttons disabled, the
    wait cursor, and `text` in its status label, repainted before the work starts. Everything back
    after, whatever happens.

        with widgets.busy([self.generate_button], self.message, "Generating the guides..."):
            ...
    """

    def __init__(self, buttons=(), label=None, text=None):
        self.buttons = [b for b in buttons if b is not None]
        self.label, self.text = label, text
        self.was = []

    def __enter__(self):
        self.was = [b.isEnabled() for b in self.buttons]
        for b in self.buttons:
            b.setEnabled(False)
        if self.label is not None and self.text:
            self.label.setText(self.text)
            self.label.repaint()
        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        # Paint the disabled buttons and the text now: the work holds the event loop.
        QtWidgets.QApplication.processEvents(QtCore.QEventLoop.ExcludeUserInputEvents)
        return self

    def __exit__(self, *_exc):
        QtWidgets.QApplication.restoreOverrideCursor()
        for b, on in zip(self.buttons, self.was):
            if alive(b):
                b.setEnabled(on)
        return False


def repolish(widget):
    """Apply the theme's style sheet to `widget` again: after a dynamic property its rules select
    on changed (role, done). Setting the style sheet again is Qt's way to have it recomputed.

    Not widget.style().unpolish/polish: under a style sheet, style() is Qt's own QStyleSheetStyle,
    and PySide keeps its wrapper alive from the widget's (a "__style__" reference). shiboken's
    exit-time cleanup, which walks every wrapper still alive when Maya quits, frees that style
    wrapper's C++ pointers when it reaches it first and then crashes reading them again through
    the widget's reference."""
    widget.setStyleSheet(widget.styleSheet())


def shrinkable(root):
    """Let the fields under `root` shrink with a narrow panel instead of pushing it wider than its
    view (where the right side would be cut off): spin boxes (their width otherwise fits the
    largest number of their range), sliders, drop-downs (their longest entry) and ramps get small
    minimum widths; a field of a fixed width keeps it."""
    def free(w):
        return w.minimumWidth() != w.maximumWidth()  # not setFixedWidth'd
    for box in root.findChildren(QtWidgets.QAbstractSpinBox):
        if free(box):
            box.setMinimumWidth(theme_px(44))
            box.setSizePolicy(QtWidgets.QSizePolicy.Expanding, box.sizePolicy().verticalPolicy())
    for slider in root.findChildren(QtWidgets.QSlider):
        if free(slider):
            slider.setMinimumWidth(theme_px(36))
    for combo in root.findChildren(QtWidgets.QComboBox):
        if free(combo):
            combo.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
            combo.setMinimumContentsLength(min(combo.minimumContentsLength() or 6, 6))
            combo.setSizePolicy(QtWidgets.QSizePolicy.Expanding, combo.sizePolicy().verticalPolicy())
    for line in root.findChildren(QtWidgets.QLineEdit):
        if free(line) and not isinstance(line.parentWidget(), (QtWidgets.QAbstractSpinBox, QtWidgets.QComboBox)):
            line.setMinimumWidth(theme_px(48))
    for ramp in root.findChildren(RampEditor):
        if free(ramp):
            ramp.setMinimumWidth(theme_px(140))


def theme_px(value):
    from groomlab.ui import theme
    return theme.px(value)


# -- Maya quitting -------------------------------------------------------------------------------
#
# Maya does not delete its docks' widgets when it quits, nor finalize Python: the panels' wrappers
# are all still alive when the process exits, and shiboken's own static cleanup then walks them,
# their children and the objects they reference, with Qt and Python half gone. Each panel
# registers a step (at_exit) that stops its callbacks, timers and scriptJobs and deletes its
# widgets while Maya is still whole (kMayaExiting), so nothing of GroomLab's is left for that walk
# and nothing of it fires while Maya shuts down.

_exit_steps = []
_exit_callback = [None]


def at_exit(step):
    """Run `step` (no arguments) once when Maya quits; registering the same step again is a no-op."""
    if step not in _exit_steps:
        _exit_steps.append(step)
    if _exit_callback[0] is None:
        try:
            from maya.api import OpenMaya as om
            _exit_callback[0] = om.MSceneMessage.addCallback(om.MSceneMessage.kMayaExiting, run_exit_steps)
        except (ImportError, RuntimeError):  # outside Maya
            pass


def run_exit_steps(*_):
    """The steps at_exit registered, each once (a failing one does not stop the others: nothing may
    raise into Maya while it quits)."""
    steps = list(_exit_steps)
    del _exit_steps[:]
    for step in steps:
        try:
            step()
        except Exception:
            pass


GEOMETRY_VAR = "groomLabGeometry_"  # + a window's key: its geometry (Qt's saveGeometry, base64)


class _GeometryKeeper(QtCore.QObject):
    """Writes its window's geometry to an optionVar soon after it moves or resizes, and as it hides:
    in the prefs whenever Maya quits, with the window open or not."""

    def __init__(self, window, var):
        super(_GeometryKeeper, self).__init__(window)
        self.window, self.var = window, var
        self.timer = QtCore.QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setInterval(500)
        self.timer.timeout.connect(self.keep)

    def eventFilter(self, obj, event):
        if obj is self.window:
            if event.type() in (QtCore.QEvent.Move, QtCore.QEvent.Resize) and obj.isVisible():
                self.timer.start()
            elif event.type() in (QtCore.QEvent.Hide, QtCore.QEvent.Close):
                self.keep()
        return False

    def keep(self):
        from maya import cmds
        if alive(self.window) and self.window.isVisible():
            data = bytes(self.window.saveGeometry().toBase64()).decode("ascii")
            cmds.optionVar(stringValue=(self.var, data))


def remember_geometry(window, key):
    """A standalone window (not a dock) opens where it was last: its place, size and monitor, from
    this session or an earlier one (an update's too: an optionVar in Maya's prefs). Call before its
    first show."""
    from maya import cmds
    var = GEOMETRY_VAR + key
    if cmds.optionVar(exists=var):
        window.restoreGeometry(QtCore.QByteArray.fromBase64(cmds.optionVar(query=var).encode("ascii")))
    window.installEventFilter(_GeometryKeeper(window, var))


def into_dock(panel, ui_script, restoring=False):
    """Put a dockable panel (MayaQWidgetDockableMixin, named) in the dock (workspace control
    `<name>WorkspaceControl`) Maya has for it, where it was: floating on another monitor, docked or
    tabbed, its size. Maya rebuilds a dock with a `ui_script` (Python) as it restores its workspace
    after a restart (an update's too): `restoring` is that call, the dock Maya's current parent;
    otherwise the dock still there (its panel gone at_exit, or from before a reload of the module).
    False when there is none: show(dockable=True, uiScript=...) makes one."""
    from maya import cmds
    from maya.app.general import mayaMixin
    import maya.OpenMayaUI as omui
    control = panel.objectName() + "WorkspaceControl"
    if restoring:
        parent = omui.MQtUtil.getCurrentParent()
    else:
        parent = omui.MQtUtil.findControl(control) if cmds.workspaceControl(control, exists=True) else None
    if not parent:
        return False
    dock = qt.wrap_instance(parent, QtWidgets.QWidget)
    for old in dock.findChildren(QtWidgets.QWidget, panel.objectName()):  # a reload's: its panel goes
        if old is not panel:
            dispose(old)
    omui.MQtUtil.addWidgetToMayaLayout(int(qt.shiboken.getCppPointer(panel)[0]), int(parent))
    mayaMixin.mixinWorkspaceControls[control] = panel
    cmds.workspaceControl(control, edit=True, uiScript=ui_script)
    return True


def _fixed_width(cell):
    """Whether a splitter's `cell` holds only workspace controls Maya keeps at a fixed width (the
    ToolBox)."""
    from maya import cmds
    names = [w.objectName() for w in [cell] + cell.findChildren(QtWidgets.QWidget)]
    controls = [n for n in names if n and cmds.workspaceControl(n, exists=True)]
    return bool(controls) and all(cmds.workspaceControl(n, query=True, widthProperty=True) == "fixed" for n in controls)


def stuck_width(control):
    """Whether a docked workspace `control` cannot change its width: in a row (a horizontal
    splitter) whose only cells beside it are fixed-width ones, as left of the ToolBox, where Maya
    puts a panel dragged onto the left edge past the ToolBox. Each handle beside it would resize the
    fixed one, so neither moves. (The wrappers of its parents go as this returns, and PySide then
    invalidates `control`'s, Maya's widget: wrap it again to use it after.)"""
    cell, x = control, control.parentWidget()
    while x is not None:
        if isinstance(x, QtWidgets.QSplitter) and x.orientation() == QtCore.Qt.Horizontal and x.count() > 1:
            i = x.indexOf(cell)
            return all(_fixed_width(x.widget(j)) for j in (i - 1, i + 1) if 0 <= j < x.count())
        cell, x = x, x.parentWidget()
    return False


def _workspace_control(name):
    """The workspace control `name`'s widget, wrapped now (None when there is none)."""
    from maya import cmds
    import maya.OpenMayaUI as omui
    pointer = omui.MQtUtil.findControl(name) if cmds.workspaceControl(name, exists=True) else None
    return qt.wrap_instance(pointer, QtWidgets.QWidget) if pointer else None


class _DockWidthKeeper(QtCore.QObject):
    """Moves its panel's dock to the main window's left dock area (the same place, next to the
    ToolBox, but with a width handle) when it is docked where its width is stuck (stuck_width).
    It belongs to the panel and keeps only the dock's name: a wrapper of Maya's widget does not
    stay valid (see stuck_width)."""

    def __init__(self, panel, name):
        super(_DockWidthKeeper, self).__init__(panel)
        self.name, self.watched = name, None
        self.timer = QtCore.QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setInterval(300)  # after Maya's drop has settled
        self.timer.timeout.connect(self.check)

    def watch(self):
        """Watch the dock (a new one after Maya restores its workspace) for being docked again, and
        check where it is now (a workspace saved that way)."""
        control = _workspace_control(self.name)
        if control is None:
            return
        pointer = qt.shiboken.getCppPointer(control)[0]
        if pointer != self.watched:
            control.installEventFilter(self)
            self.watched = pointer
        self.timer.start()

    def eventFilter(self, obj, event):
        if event.type() in (QtCore.QEvent.Show, QtCore.QEvent.ParentChange):  # docked again: Maya shows it in its new place
            self.timer.start()
        return False

    def check(self):
        from maya import cmds
        control = _workspace_control(self.name)
        if control is not None and not cmds.workspaceControl(self.name, query=True, floating=True) and stuck_width(control):
            cmds.workspaceControl(self.name, edit=True, dockToMainWindow=("left", False))


def keep_dock_resizable(panel):
    """Keep a docked panel's (MayaQWidgetDockableMixin, named) width adjustable wherever it is
    docked (see _DockWidthKeeper)."""
    keeper = panel.findChild(_DockWidthKeeper)
    if keeper is None:
        keeper = _DockWidthKeeper(panel, panel.objectName() + "WorkspaceControl")
    keeper.watch()


def dispose(widget):
    """Delete `widget` (and its children) now, not on a later event loop turn: when Maya quits
    there is none. Its Python wrapper stays, invalid (alive() False); its dock stays, as Maya saves
    it (into_dock when the panel is made again)."""
    if widget is not None and alive(widget):
        qt.shiboken.delete(widget)


def _mix(a, b, f):
    """Hex color a blended toward b by f (0-1)."""
    ca, cb = QtGui.QColor(a), QtGui.QColor(b)
    return QtGui.QColor(int(round(ca.red() + (cb.red() - ca.red()) * f)),
                        int(round(ca.green() + (cb.green() - ca.green()) * f)),
                        int(round(ca.blue() + (cb.blue() - ca.blue()) * f))).name().upper()


def icon_colors(name, active=False, t=None):
    """{placeholder role: hex} for an icon in the theme; `active` for one on the accent (a checked tool)."""
    from groomlab.ui import theme
    t = t or theme.current()
    group = next((g for g, names in ICON_GROUPS.items() if name in names), "app")
    main = t.hex(t.get("icons." + group, "accent"))
    colors = {"main": main, "shade": _mix(main, "#000000", 0.38), "tint": _mix(main, "#FFFFFF", 0.5),
              "ink": t.hex(t.get("icons.ink", "white")), "shine": "#FFFFFF"}
    for name_ in ("red", "yellow", "blue", "emerald"):
        colors[name_] = t.hex(name_)
    if active:  # on the accent: the icon in the accent's text color, its highlights in the accent
        on, accent = t.color("ui.accentText", "#FFFFFF"), t.hex("accent")
        colors = {role: on for role in colors}
        colors["shade"] = _mix(on, accent, 0.3)
        colors["shine"] = accent
    return colors


def icon_svg(name, active=False, t=None):
    """The icon's SVG text in the theme's colors; None if there is no such icon."""
    path = os.path.join(ICON_DIR, "groomLab_%s.svg" % name)
    if path not in _svg_cache:
        if not os.path.exists(path):
            return None
        with open(path, encoding="utf-8") as f:
            _svg_cache[path] = f.read()
    svg = _svg_cache[path]
    colors = icon_colors(name, active, t)
    for role, placeholder in PLACEHOLDERS.items():
        svg = svg.replace(placeholder, "\0%s\0" % role)  # two passes: a color may equal another placeholder
    for role, color in colors.items():
        svg = svg.replace("\0%s\0" % role, color)
    return svg


def _pixmap(svg, size):
    renderer = qt.QtSvg().QSvgRenderer(QtCore.QByteArray(svg.encode("utf-8")))
    image = QtGui.QImage(size, size, QtGui.QImage.Format_ARGB32_Premultiplied)
    image.fill(QtCore.Qt.transparent)
    painter = QtGui.QPainter(image)
    renderer.render(painter)
    painter.end()
    return QtGui.QPixmap.fromImage(image)


_icon_cache = {}  # (svg text, active svg text, size) -> QIcon: rendered once per theme colors


def icon(name, size=64):
    """QIcon for "comb", "place", ... in the active theme's colors (checked: the active look); empty if missing."""
    svg = icon_svg(name)
    if svg is None:
        return QtGui.QIcon()
    active = icon_svg(name, active=True)
    key = (svg, active, size)
    result = _icon_cache.get(key)
    if result is None:
        result = QtGui.QIcon()
        result.addPixmap(_pixmap(svg, size), QtGui.QIcon.Normal, QtGui.QIcon.Off)
        result.addPixmap(_pixmap(active, size), QtGui.QIcon.Normal, QtGui.QIcon.On)
        _icon_cache[key] = result
    return result


# Small line glyphs for header buttons, drawn in the theme's text color (24x24 viewBox).
GLYPHS = {
    "eye": '<path d="M2.5 12 C5 6.8 8.3 5 12 5 C15.7 5 19 6.8 21.5 12 C19 17.2 15.7 19 12 19 C8.3 19 5 17.2 2.5 12 Z" '
           'fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
           '<circle cx="12" cy="12" r="3.2" fill="{c}"/>',
    "eyeOff": '<path d="M2.5 12 C5 6.8 8.3 5 12 5 C15.7 5 19 6.8 21.5 12 C19 17.2 15.7 19 12 19 C8.3 19 5 17.2 2.5 12 Z" '
              'fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round" opacity="0.5"/>'
              '<path d="M4 20 L20 4" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>',
    # Guides: three strands from their roots (the guides' eye), and struck through when hidden.
    "guides": '<path d="M6 20 C6 15 4.5 11 5.5 6 M12 20 C12 14 13 10 12 4 M18 20 C18 16 19.5 12 19 8" fill="none" '
              'stroke="{c}" stroke-width="1.8" stroke-linecap="round"/><circle cx="6" cy="20" r="1.8" fill="{c}"/>'
              '<circle cx="12" cy="20" r="1.8" fill="{c}"/><circle cx="18" cy="20" r="1.8" fill="{c}"/>',
    "guidesOff": '<g opacity="0.5"><path d="M6 20 C6 15 4.5 11 5.5 6 M12 20 C12 14 13 10 12 4 M18 20 C18 16 19.5 12 19 8" '
                 'fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/><circle cx="6" cy="20" r="1.8" '
                 'fill="{c}"/><circle cx="12" cy="20" r="1.8" fill="{c}"/><circle cx="18" cy="20" r="1.8" fill="{c}"/></g>'
                 '<path d="M4 20 L20 4" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>',
    # A push pin (Maya's pinned panels): standing up when pinned, leaning (outlined) when not.
    "pin": '<g transform="rotate(40 12 12)"><path d="M9 4 H15 M10.5 4 V10 L7.5 14 H16.5 L13.5 10 V4 M12 14 V20.5" '
           'fill="none" stroke="{c}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></g>',
    "pinned": '<path d="M10.5 4 H13.5 V10 L16.5 14 H7.5 L10.5 10 Z" fill="{c}"/><path d="M9 4 H15 M12 14 V20.5" '
              'fill="none" stroke="{c}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    "more": '<circle cx="5.5" cy="12" r="1.9" fill="{c}"/><circle cx="12" cy="12" r="1.9" fill="{c}"/>'
            '<circle cx="18.5" cy="12" r="1.9" fill="{c}"/>',
    "rename": '<path d="M5 19 L5.8 15.4 L15.6 5.6 A1.6 1.6 0 0 1 17.9 5.6 L18.4 6.1 A1.6 1.6 0 0 1 18.4 8.4 '
              'L8.6 18.2 Z" fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
              '<path d="M13.8 7.4 L16.6 10.2" stroke="{c}" stroke-width="1.8"/>',
    "select": '<path d="M5 9 V5 H9 M15 5 H19 V9 M19 15 V19 H15 M9 19 H5 V15" fill="none" stroke="{c}" '
              'stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/><circle cx="12" cy="12" r="2.6" fill="{c}"/>',
    "delete": '<path d="M5 7 H19 M10 4.5 H14 M7 7 L8 19 H16 L17 7" fill="none" stroke="{c}" stroke-width="1.8" '
              'stroke-linecap="round" stroke-linejoin="round"/><path d="M10.5 10.5 V15.5 M13.5 10.5 V15.5" '
              'stroke="{c}" stroke-width="1.6" stroke-linecap="round"/>',
    "up": '<path d="M12 19 V5 M6 11 L12 5 L18 11" fill="none" stroke="{c}" stroke-width="1.9" '
          'stroke-linecap="round" stroke-linejoin="round"/>',
    "down": '<path d="M12 5 V19 M6 13 L12 19 L18 13" fill="none" stroke="{c}" stroke-width="1.9" '
            'stroke-linecap="round" stroke-linejoin="round"/>',
    "convert": '<path d="M4 8.5 H18 M14.5 5 L18 8.5 L14.5 12 M20 15.5 H6 M9.5 12 L6 15.5 L9.5 19" fill="none" '
               'stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>',
    "solo": '<circle cx="12" cy="12" r="7.5" fill="none" stroke="{c}" stroke-width="1.8"/>'
            '<circle cx="12" cy="12" r="3.2" fill="{c}"/>',
    "eyedropper": '<path d="M15.2 3.8 A2.4 2.4 0 0 1 18.6 3.8 L20.2 5.4 A2.4 2.4 0 0 1 20.2 8.8 L18 11 L13 6 Z" '
                  'fill="{c}"/><path d="M11.6 7.2 L16.8 12.4" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
                  '<path d="M13.4 9.8 L6.2 17 L5 19 L7 17.8 L14.2 10.6" fill="none" stroke="{c}" stroke-width="1.8" '
                  'stroke-linejoin="round"/>',
}


def glyph(name, color=None, size=48):
    """A QIcon of a line glyph (GLYPHS) in `color` (default: the theme's text color)."""
    from groomlab.ui import theme
    color = color or theme.current().color("ui.text", "#F5F5F7")
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">%s</svg>'
           % GLYPHS[name].replace("{c}", color))
    return QtGui.QIcon(_pixmap(svg, size))


class ToolGrid(QtWidgets.QWidget):
    """Tool tiles that reflow to the panel's width: up to `columns` a row, at least two, so a
    narrow dock shows every tile instead of clipping the last column."""

    def __init__(self, buttons, columns=4, parent=None):
        super(ToolGrid, self).__init__(parent)
        from groomlab.ui import theme
        self.buttons = list(buttons)
        self.max_columns = columns
        self.columns = None
        self.grid = QtWidgets.QGridLayout(self)
        self.grid.setContentsMargins(0, 0, 0, 0)
        self.grid.setSpacing(theme.px(6))
        self.grid.setSizeConstraint(QtWidgets.QLayout.SetNoConstraint)  # the width may go below 4 tiles
        self._place(columns)

    def _tile(self):
        return max(b.minimumWidth() for b in self.buttons) if self.buttons else 0

    def _place(self, columns):
        columns = max(1, min(columns, self.max_columns))  # a short row keeps the tiles' size
        if columns == self.columns:
            return
        self.columns = columns
        for b in self.buttons:
            self.grid.removeWidget(b)
        for i, b in enumerate(self.buttons):
            self.grid.addWidget(b, i // columns, i % columns)
        for c in range(self.max_columns):
            self.grid.setColumnStretch(c, 1 if c < columns else 0)
        self.updateGeometry()

    def columns_for(self, width):
        spacing = self.grid.spacing()
        return max(2, (width + spacing) // max(1, self._tile() + spacing))

    def resizeEvent(self, event):
        self._place(self.columns_for(event.size().width()))
        super(ToolGrid, self).resizeEvent(event)

    def minimumSizeHint(self):
        spacing = self.grid.spacing()
        columns = min(2, len(self.buttons) or 1)
        return QtCore.QSize(columns * self._tile() + (columns - 1) * spacing, self.grid.minimumSize().height())

    def sizeHint(self):
        return QtCore.QSize(self.grid.sizeHint().width(), self.grid.minimumSize().height())


class Pages(QtWidgets.QWidget):
    """A segmented control over pages, as tabs: only the page showing takes room, so what sits
    under it follows right below (the QTabWidget API: addTab, currentIndex, setCurrentIndex,
    currentChanged, count, widget)."""

    currentChanged = QtCore.Signal(int)

    def __init__(self, parent=None):
        super(Pages, self).__init__(parent)
        from groomlab.ui import theme
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(8))
        track = QtWidgets.QFrame()
        track.setProperty("role", "segments")
        row = QtWidgets.QHBoxLayout(track)
        pad = theme.px(3)
        row.setContentsMargins(pad, pad, pad, pad)
        self.bar = QtWidgets.QTabBar()
        self.bar.setExpanding(True)
        self.bar.setUsesScrollButtons(False)  # a narrow panel shortens the names rather than scroll them
        self.bar.setElideMode(QtCore.Qt.ElideRight)
        self.bar.setDrawBase(False)
        self.bar.setDocumentMode(True)
        row.addWidget(self.bar)
        layout.addWidget(track)
        self._pages = []
        self.bar.currentChanged.connect(self._show)

    def addTab(self, widget, title):
        self._pages.append(widget)
        self.layout().addWidget(widget)
        widget.setVisible(False)
        self.bar.addTab(title)
        self._show(self.bar.currentIndex(), emit=False)
        return len(self._pages) - 1

    def _show(self, index, emit=True):
        for i, page in enumerate(self._pages):
            page.setVisible(i == index)
        if emit:
            self.currentChanged.emit(index)

    def currentIndex(self):
        return self.bar.currentIndex()

    def setCurrentIndex(self, index):
        self.bar.setCurrentIndex(index)

    def count(self):
        return len(self._pages)

    def widget(self, index):
        return self._pages[index]


class StatusLabel(QtWidgets.QLabel):
    """A message line that takes no room while it has nothing to say."""

    def __init__(self, parent=None):
        super(StatusLabel, self).__init__(parent)
        self.setWordWrap(True)
        self.setProperty("role", "status")
        self.setVisible(False)

    def setText(self, text):
        super(StatusLabel, self).setText(text)
        self.setVisible(bool(text))


class Section(QtWidgets.QWidget):
    """A titled section that folds away: click its header to show or hide `content`."""

    def __init__(self, title, content, expanded=False, parent=None):
        super(Section, self).__init__(parent)
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
        self.title = title
        self.header = QtWidgets.QPushButton()
        self.header.setCheckable(True)
        self.header.setFlat(True)
        self.header.setProperty("role", "section")
        self.header.toggled.connect(self.set_expanded)
        self.content = content
        layout.addWidget(self.header)
        layout.addWidget(content)
        self.set_expanded(expanded)

    def set_expanded(self, on):
        self.header.blockSignals(True)
        self.header.setChecked(on)
        self.header.blockSignals(False)
        self.header.setText(("\u25be  " if on else "\u25b8  ") + self.title.upper().replace("&", "&&"))
        self.content.setVisible(on)

    def expanded(self):
        return self.header.isChecked()


def _theme_run(fn, *args):
    """A theme action; a theme file that can't be read is said (no dialog), the theme kept."""
    from groomlab.ui import messages
    try:
        fn(*args)
    except (ValueError, IOError, OSError) as e:
        messages.say("theme not applied: %s" % messages.plain(e))


def new_theme_from_current(parent=None):
    """Theme > New Theme from Current...: a copy of the active theme in the user's themes folder,
    made active and its folder opened to edit it."""
    from groomlab.ui import messages, theme
    name, ok = QtWidgets.QInputDialog.getText(parent, "New Theme", "Theme name:", text="my_theme")
    if not ok or not name:
        return None
    try:
        path = theme.create_theme(name)
    except (ValueError, IOError, OSError) as e:
        messages.say("no new theme: %s" % messages.plain(e))
        return None
    theme.set_theme(name)
    messages.done("made %s from the current theme: edit its colors, then Theme > Reload Theme" % path, seconds=6)
    QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(os.path.dirname(path)))
    return path


def open_themes_folder():
    from groomlab.ui import theme
    folder = theme.user_dir()
    if not os.path.isdir(folder):
        os.makedirs(folder)
    QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(folder))


def theme_actions(parent=None):
    """What the Theme menus' action ids run (menus/theme); theme.set is called with the theme's name."""
    from groomlab.ui import theme
    return {"theme.set": lambda name: _theme_run(theme.set_theme, name),
            "theme.new": lambda: new_theme_from_current(parent),
            "theme.reload": lambda: _theme_run(theme.reload),
            "theme.openFolder": open_themes_folder}


def theme_menu_items(parent=None):
    """Every Theme menu's items (engine/ui/menus.json's theme), one list for GroomLab > Theme and the
    windows' own menus: (label, action, checked, tip) with checked True/False for a theme (the
    active one True) and None for an action; None for a divider."""
    from groomlab.ui import definitions, theme
    try:
        entries = theme.menu_entries()
    except (ValueError, IOError, OSError):
        entries = []
    tip = definitions.action("theme.set")["tip"]

    def themes(entry):
        return [{"id": entry["action"], "value": name, "label": label, "checked": bool(active),
                 "tip": definitions.fill(tip, {"label": label})} for name, label, active in entries]
    bind = definitions.binder(theme_actions(parent))
    items = []
    for item in definitions.resolve(definitions.get("menus/theme")["items"], generators={"themes": themes}):
        if item["kind"] == "divider":
            items.append(None)
        else:
            checked = bool(item["entry"]["checked"]) if "checked" in item["entry"] else None
            items.append((item["label"], bind(item), checked, item["tip"]))
    return items


def theme_menu(parent):
    """A Theme menu (Qt): theme_menu_items, the active theme checked."""
    from groomlab.ui import definitions
    menu = QtWidgets.QMenu(definitions.get("menus/theme")["label"], parent)

    def fill():
        menu.clear()
        group = qt.QActionGroup(menu)
        for item in theme_menu_items(parent):
            if item is None:
                menu.addSeparator()
                continue
            label, run, checked, _tip = item
            action = menu.addAction(label)
            action.triggered.connect(lambda _=False, run=run: run())
            if checked is not None:
                action.setCheckable(True)
                action.setChecked(checked)
                group.addAction(action)

    menu.aboutToShow.connect(fill)
    return menu


def fill_menu(menu, entries, bind, state=(), values=None, generators=None, checked=None, wrap=None):
    """Fill a QMenu from UI definitions (engine/ui: a container's entries, definitions.resolve):
    each action's label (its shortcut's keys after a tab), tooltip (with the keys when it has them
    all), check box and enabled state. `bind(item)`: the callable an item runs (definitions.binder:
    a generated entry's value comes first); a checkable one gets the check state. `checked(item)`:
    an item's check state. `generators`: name -> a function giving entries, or filling the menu
    itself (called with the menu and the item) for the ones the files leave to the host. `wrap(fn,
    label)` guards the commands (menu._guarded). Submenus are parented to their menu (PySide would
    collect them) and show tooltips when it does. Returns {action id: QAction} (the last of an id)."""
    from groomlab.ui import definitions, shortcuts
    made = {}
    expanding = dict((k, v) for k, v in (generators or {}).items() if not getattr(v, "fills_menu", False))
    filling = dict((k, v) for k, v in (generators or {}).items() if getattr(v, "fills_menu", False))
    menu.submenus = getattr(menu, "submenus", [])

    def build(target, items):
        for item in items:
            if item["kind"] == "divider":
                target.addSeparator()
            elif item["kind"] == "submenu":
                sub = QtWidgets.QMenu(item["label"].replace("&", "&&"), target)
                sub.setToolTipsVisible(target.toolTipsVisible())
                if item["tip"]:
                    sub.setToolTip(item["tip"])
                target.addMenu(sub)
                menu.submenus.append(sub)
                build(sub, item["items"])
            elif item["kind"] == "generator":
                filling[item["name"]](target, item)
            else:
                label, tip = item["label"], item["tip"]
                if item["shortcut"]:
                    keys = shortcuts.keys(item["shortcut"])
                    if item["shortcutPart"] is not None:
                        keys = (keys.split(" / ") + [""] * (item["shortcutPart"] + 1))[item["shortcutPart"]]
                    elif tip:
                        tip = shortcuts.tip(tip, item["shortcut"])
                    label = "%s	%s" % (label, keys)
                action = target.addAction(label)
                if tip:
                    action.setToolTip(tip)
                fn = bind(item)
                if wrap is not None:
                    fn = wrap(fn, item["label"])
                if item["checkable"]:
                    action.setCheckable(True)
                    action.setChecked(bool(checked(item)) if checked else False)
                    action.triggered.connect(_check_slot(fn))  # a click (or trigger()), not setChecked
                else:
                    action.triggered.connect(lambda _=False, fn=fn: fn())
                action.setEnabled(item["enabled"])
                made[item["id"]] = action

    build(menu, definitions.resolve(entries, state, values, expanding))
    return made


def _check_slot(fn):
    """A slot passing a checkable action's state on to fn: a plain one-argument function, as PySide
    calls a lambda with defaults (lambda on=False, fn=fn) without the state."""
    def slot(on):
        fn(on)
    return slot


def menu_filler(fn):
    """A fill_menu generator that fills the menu itself: fn(menu, item)."""
    fn.fills_menu = True
    return fn


def icon_path(name):
    """A file of the icon in the active theme's colors, for Maya's menus."""
    from groomlab.ui import theme
    import tempfile
    t = theme.current()
    svg = icon_svg(name, t=t)
    if svg is None:
        return os.path.join(ICON_DIR, "groomLab_%s.svg" % name).replace("\\", "/")
    folder = os.path.join(tempfile.gettempdir(), "groomlab_icons", t.key)
    path = os.path.join(folder, "groomLab_%s.svg" % name)
    try:
        if not os.path.isdir(folder):
            os.makedirs(folder)
        with open(path, "w", encoding="utf-8") as f:
            f.write(svg)
    except (IOError, OSError):
        return os.path.join(ICON_DIR, "groomLab_%s.svg" % name).replace("\\", "/")
    return path.replace("\\", "/")


def nice_ceil(x):
    """The smallest of 1, 2, 5 x 10^k at or above x (> 0)."""
    if x <= 0:
        return 1.0
    k = math.floor(math.log10(x))
    for m in (1, 2, 5, 10):
        v = m * 10 ** k
        if v >= x - 1e-12:
            return float(v)
    return float(10 ** (k + 1))


def default_range(value, integer=False):
    """A soft slider range for a param whose value (or default) is `value`."""
    v = float(value)
    if integer:
        if v < 0:
            hi = nice_ceil(2 * -v)
            return -hi, hi
        return 0.0, max(10.0, nice_ceil(2 * v))
    if 0.0 <= v <= 1.0:
        return 0.0, 1.0
    if v > 1.0:
        return 0.0, nice_ceil(2 * v)
    hi = nice_ceil(2 * -v)
    return -hi, hi


def _load():
    global _ranges
    if _ranges is None:
        _ranges = {}
        try:
            from maya import cmds
            if cmds.optionVar(exists=RANGES_VAR):
                _ranges = json.loads(cmds.optionVar(query=RANGES_VAR)) or {}
        except Exception:
            _ranges = {}
    return _ranges


def _save():
    try:
        from maya import cmds
        cmds.optionVar(stringValue=(RANGES_VAR, json.dumps(_ranges)))
    except Exception:
        pass


def remembered_range(key):
    r = _load().get(key) if key else None
    return (float(r[0]), float(r[1])) if r else None


def remember_range(key, lo, hi):
    if not key:
        return
    _load()[key] = [lo, hi]
    _save()


def forget_range(key):
    if key and _load().pop(key, None) is not None:
        _save()


class CompactSpinBox(QtWidgets.QDoubleSpinBox):
    """A number field showing 0.5 or 100 rather than 0.500000 or 100.000000. With display_decimals
    (the spec's "decimals", as every host shows the param), at most that many, trailing zeros
    dropped: 0.35, not 0.350; the value itself keeps its precision."""

    display_decimals = None

    def textFromValue(self, value):
        return format_number(value, self.display_decimals)

    def valueFromText(self, text):
        try:
            return float(text)
        except ValueError:
            return self.value()

    def validate(self, text, pos):
        stripped = text.strip()
        if stripped in ("", "-", "+", ".", "-.", "+."):
            return QtGui.QValidator.Intermediate, text, pos
        try:
            float(stripped)
            return QtGui.QValidator.Acceptable, text, pos
        except ValueError:
            return QtGui.QValidator.Invalid, text, pos


def format_number(value, decimals=None):
    """A number as every GroomLab host shows it: `decimals` places at most (the spec's), trailing zeros
    dropped (0.35, 12, 980), but a small number keeps two significant digits (0.035, not 0: a density
    fitted to a large scalp), six places at most; None: six significant digits."""
    if decimals is None:
        return "%.6g" % value
    decimals = max(0, int(decimals))
    if 0.0 < abs(value) < 1.0:
        import math
        decimals = max(decimals, min(6, int(math.ceil(-math.log10(abs(value)))) + 1))
    text = "%.*f" % (decimals, value)
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def with_unit(field, unit, tip=None):
    """`field` with its unit after it (px, cm, %...), as the Graph Editor shows units; the field
    itself when there is no unit. The row keeps the field as .field."""
    if not unit:
        return field
    w = QtWidgets.QWidget()
    row = QtWidgets.QHBoxLayout(w)
    row.setContentsMargins(0, 0, 0, 0)
    row.setSpacing(4)
    row.addWidget(field, 1)
    label = QtWidgets.QLabel(unit)
    label.setProperty("role", "about")
    if tip:
        label.setToolTip(tip)
    row.addWidget(label)
    w.field, w.unit = field, label
    return w


def display_decimals(spec_decimals, factor=1.0):
    """The decimals for a value shown `factor` times larger than the spec's (a fraction as a
    percentage, a width in micrometers): fewer by the factor's decades, 0 to 6."""
    import math
    if spec_decimals is None:
        return None
    shift = int(round(math.log10(abs(factor)))) if factor else 0
    return max(0, min(6, int(spec_decimals) - shift))


class SoftSlider(QtWidgets.QWidget):
    """Slider + number field with a soft, stretchable range.

    Signals: dragged(value) while the slider moves under the mouse (for live
    previews), committed(value) when a value is settled (slider released,
    track clicked, field edited).
    """

    dragged = QtCore.Signal(float)
    committed = QtCore.Signal(float)
    STEPS = 1000

    def __init__(self, value=0.0, lo=None, hi=None, integer=False, minimum=None, maximum=None, decimals=3,
                 key=None, default=None, parent=None, accent=None, shown_decimals=None):
        super(SoftSlider, self).__init__(parent)
        self.integer = integer
        self.key = key
        self.minimum = minimum
        self.maximum = maximum
        self.default = default  # the param's default, for Reset to Default
        self.menu_hook = None  # fn(menu): more actions atop the right-click menu (the Graph Editor's keys)
        base = default if default is not None else value
        self.default_range = (lo, hi) if lo is not None and hi is not None else default_range(base, integer)
        self.lo, self.hi = remembered_range(key) or self.default_range
        self._value = float(value)
        self._stretch_to(self._value)  # a value set elsewhere may be outside

        row = QtWidgets.QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(6)
        self.field = QtWidgets.QSpinBox() if integer else CompactSpinBox()
        if not integer:
            self.field.setDecimals(max(decimals, 6))
            self.field.setSingleStep(0.01)
            self.field.display_decimals = shown_decimals  # the spec's, as every host shows it
        low = minimum if minimum is not None else -1e9
        high = maximum if maximum is not None else 1e9
        if integer:
            self.field.setRange(int(max(low, -2 ** 31 + 1)), int(min(high, 2 ** 31 - 1)))
        else:
            self.field.setRange(low, high)
        self.field.setKeyboardTracking(False)
        from groomlab.ui import theme
        self.field.setFixedWidth(theme.px(70))
        self.field.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        self.field.setAlignment(QtCore.Qt.AlignRight)
        self.slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        self.slider.setProperty("accent", accent or theme.slider_accent())  # the theme colors its fill
        self.slider.setRange(0, self.STEPS)
        self.slider.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.slider.customContextMenuRequested.connect(self._menu)
        row.addWidget(self.field)
        row.addWidget(self.slider, 1)

        self._sync()
        self.field.editingFinished.connect(self._typed)
        self.slider.valueChanged.connect(self._slid)
        self.slider.sliderReleased.connect(lambda: self.committed.emit(self._value))

    # -- Value and range -------------------------------------------------------

    def value(self):
        return int(round(self._value)) if self.integer else self._value

    def setValue(self, v, stretch=True):
        """Set without emitting signals; stretches the range to include v when asked."""
        if float(v) == self._value:
            return
        self._value = float(v)
        if stretch:
            self._stretch_to(self._value)
        self._sync()

    def setRange(self, lo, hi):
        self.lo, self.hi = float(lo), float(hi)
        self._sync()

    def _stretch_to(self, v):
        if v > self.hi:
            self.hi = nice_ceil(v) if v > 0 else 0.0
        if v < self.lo:
            self.lo = -nice_ceil(-v) if v < 0 else 0.0

    def _sync(self):
        for w in (self.field, self.slider):
            w.blockSignals(True)
        self.field.setValue(int(round(self._value)) if self.integer else self._value)
        span = self.hi - self.lo
        t = (self._value - self.lo) / span if span > 0 else 0.0
        self.slider.setValue(int(round(max(0.0, min(1.0, t)) * self.STEPS)))
        self.slider.setToolTip("%g to %g (type a value past it to stretch; right-click to set the range)" % (self.lo, self.hi))
        for w in (self.field, self.slider):
            w.blockSignals(False)

    # -- Signals -----------------------------------------------------------------

    def _typed(self):
        v = float(self.field.value())
        if v == self._value:
            return
        self._value = v
        # This slider stretches to reach it; the range every node of the type starts from
        # stays (one unusual value must not rescale them all). Set Range... keeps one.
        self._stretch_to(v)
        self._sync()
        self.committed.emit(self.value())

    def _slid(self, position):
        v = self.lo + (self.hi - self.lo) * position / float(self.STEPS)
        if self.integer:
            v = float(int(round(v)))
        if self.minimum is not None:
            v = max(v, self.minimum)
        if self.maximum is not None:
            v = min(v, self.maximum)
        if v == self._value:
            return
        self._value = v
        self.field.blockSignals(True)
        self.field.setValue(int(v) if self.integer else v)
        self.field.blockSignals(False)
        if self.slider.isSliderDown():
            self.dragged.emit(self.value())
        else:
            self.committed.emit(self.value())  # track click or keyboard: settled at once

    def _menu(self, pos):
        menu = QtWidgets.QMenu(self)
        menu.setToolTipsVisible(True)
        if self.menu_hook is not None:
            self.menu_hook(menu)
            menu.addSeparator()
        if getattr(self, "default", None) is not None:
            menu.addAction("Reset to Default (%g)" % self.default, self._reset_value)
            menu.addSeparator()
        menu.addAction("Range %g to %g" % (self.lo, self.hi)).setEnabled(False)
        menu.addAction("Fit Range to Value", self._fit)
        menu.addAction("Set Range...", self._ask_range)
        menu.addAction("Reset Range", self._reset)
        qt.exec_(menu, self.slider.mapToGlobal(pos))

    def _reset_value(self):
        self.setValue(self.default)
        self.committed.emit(self.value())

    def _fit(self):
        lo, hi = default_range(self._value if self._value != 0 else 1.0, self.integer)
        self.lo, self.hi = lo, hi
        remember_range(self.key, lo, hi)
        self._sync()

    def _ask_range(self):
        text, ok = QtWidgets.QInputDialog.getText(self, "Slider Range", "Minimum and maximum:",
                                                  text="%g %g" % (self.lo, self.hi))
        if not ok:
            return
        try:
            lo, hi = [float(x) for x in text.replace(",", " ").split()]
        except ValueError:
            return
        if hi > lo:
            self.lo, self.hi = lo, hi
            remember_range(self.key, lo, hi)
            self._sync()

    def _reset(self):
        forget_range(self.key)
        self.lo, self.hi = self.default_range
        self._stretch_to(self._value)
        self._sync()


def _unit(x):
    """A curve coordinate, kept between 0 and 1."""
    return min(max(float(x), 0.0), 1.0)


class RampEditor(QtWidgets.QWidget):
    """A curve editor for ramp params: [[position, value], ...], a smooth curve through the keys
    (as the core evaluates them), or {"keys": [...], "linear": true} for straight segments.

    Drag a key to move it; click empty space to add one; right-click a key to
    remove it, or elsewhere for presets. Signals: dragged(keys) while moving,
    committed(keys) when settled. `ends` names the left and right ends under the plot (a brush's
    falloff runs from its center to its edge).
    """

    dragged = QtCore.Signal(object)
    committed = QtCore.Signal(object)
    PRESETS = [("Rising", [[0, 0], [1, 1]]), ("Falling", [[0, 1], [1, 0]]), ("Constant", [[0, 1], [1, 1]]),
               ("Ease In", [[0, 0], [0.5, 0.15], [1, 1]]), ("Ease Out", [[0, 0], [0.5, 0.85], [1, 1]]),
               ("Bell", [[0, 0], [0.5, 1], [1, 0]]), ("Tip Only", [[0, 0], [0.6, 0], [1, 1]]),
               ("Root Only", [[0, 1], [0.4, 0], [1, 0]])]

    def __init__(self, keys, parent=None, ends=("root", "tip")):
        super(RampEditor, self).__init__(parent)
        self.ends = ends
        self.linear = isinstance(keys, dict) and bool(keys.get("linear"))
        if isinstance(keys, dict):
            keys = keys.get("keys")
        self.keys = sorted([_unit(p), _unit(v)] for p, v in (keys or [[0, 0], [1, 1]]))  # curves live in 0-1
        from groomlab.ui import theme
        self.setMinimumSize(theme.px(200), theme.px(96))
        self.setMouseTracking(True)
        self._drag = None
        self._hover = None

    # -- Geometry ----------------------------------------------------------------

    def value_range(self):
        return 0.0, 1.0  # curve points are always 0-1 on both axes

    def plot(self):
        return QtCore.QRectF(8, 6, self.width() - 16, self.height() - 22)

    def to_screen(self, p, v):
        r = self.plot()
        lo, hi = self.value_range()
        return QtCore.QPointF(r.left() + p * r.width(), r.bottom() - (v - lo) / (hi - lo) * r.height())

    def from_screen(self, point):
        r = self.plot()
        lo, hi = self.value_range()
        p = min(max((point.x() - r.left()) / r.width(), 0.0), 1.0)
        v = lo + (r.bottom() - point.y()) / r.height() * (hi - lo)
        return p, min(max(v, lo), hi)

    def key_at(self, point):
        for i, (p, v) in enumerate(self.keys):
            if QtCore.QLineF(self.to_screen(p, v), point).length() < 8:
                return i
        return None

    # -- Drawing -------------------------------------------------------------------

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        r = self.plot()
        from groomlab.ui import theme
        t = theme.current()
        curve = t.qcolor(t.get("ui.sliders", ["yellow"])[0] if t.get("ui.sliders") else "yellow")
        painter.fillRect(self.rect(), t.qcolor("ui.panel"))
        painter.setPen(QtGui.QPen(t.qcolor("ui.raised"), 1))
        for k in range(5):
            x = r.left() + r.width() * k / 4.0
            y = r.top() + r.height() * k / 4.0
            painter.drawLine(QtCore.QPointF(x, r.top()), QtCore.QPointF(x, r.bottom()))
            painter.drawLine(QtCore.QPointF(r.left(), y), QtCore.QPointF(r.right(), y))
        # The curve, flat past the end keys, as it evaluates.
        path = QtGui.QPainterPath(self.to_screen(0.0, self.evaluate(0.0)))
        for i in range(1, 129):
            u = i / 128.0  # not `t`: that is the theme, read again below
            path.lineTo(self.to_screen(u, self.evaluate(u)))
        fill = QtGui.QPainterPath(path)
        fill.lineTo(QtCore.QPointF(r.right(), r.bottom()))
        fill.lineTo(QtCore.QPointF(r.left(), r.bottom()))
        wash = QtGui.QColor(curve)
        wash.setAlpha(40)
        painter.fillPath(fill, wash)
        painter.setPen(QtGui.QPen(curve, 2))
        painter.drawPath(path)
        for i, (p, v) in enumerate(self.keys):
            hot = i in (self._drag, self._hover)
            painter.setPen(QtGui.QPen(t.qcolor("ui.border"), 1))
            painter.setBrush(t.qcolor("ui.text") if hot else curve)
            painter.drawEllipse(self.to_screen(p, v), 5 if hot else 4, 5 if hot else 4)
        painter.setPen(t.qcolor("ui.textDim"))
        lo, hi = self.value_range()
        label = self.ends[0] + " " * 6 + self.ends[1]
        shown = self._drag if self._drag is not None else self._hover
        if shown is not None:
            label = "%.2f : %.3f" % tuple(self.keys[shown])
        painter.drawText(QtCore.QRectF(r.left(), r.bottom() + 1, r.width(), 14),
                         QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter, label)
        painter.drawText(QtCore.QRectF(r.left(), r.bottom() + 1, r.width(), 14),
                         QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter, "%g to %g" % (lo, hi))

    # -- Editing ---------------------------------------------------------------------

    def value(self):
        keys = [list(k) for k in self.keys]
        return {"keys": keys, "linear": True} if self.linear else keys

    def setValue(self, keys):
        if isinstance(keys, dict):
            self.linear = bool(keys.get("linear"))
            keys = keys.get("keys") or []
        self.keys = sorted([_unit(p), _unit(v)] for p, v in keys)
        self.update()

    def evaluate(self, t):
        """The ramp at t, as groomlab's core evaluates it (monotone cubic, or straight segments)."""
        keys = self.keys
        if t <= keys[0][0]:
            return keys[0][1]
        if t >= keys[-1][0]:
            return keys[-1][1]
        i = max(k for k in range(len(keys) - 1) if keys[k][0] <= t)
        (x0, y0), (x1, y1) = keys[i], keys[i + 1]
        h = x1 - x0
        if h <= 0:
            return y1
        f = (t - x0) / h
        if self.linear:
            return y0 + (y1 - y0) * f

        def secant(k):
            span = keys[k + 1][0] - keys[k][0]
            return (keys[k + 1][1] - keys[k][1]) / span if span > 0 else 0.0

        def tangent(k):
            if k == 0:
                return secant(0)
            if k == len(keys) - 1:
                return secant(k - 1)
            d0, d1 = secant(k - 1), secant(k)
            if d0 * d1 <= 0:
                return 0.0
            h0, h1 = keys[k][0] - keys[k - 1][0], keys[k + 1][0] - keys[k][0]
            return 3.0 * (h0 + h1) / ((2.0 * h1 + h0) / d0 + (h1 + 2.0 * h0) / d1)

        m0, m1 = tangent(i) * h, tangent(i + 1) * h
        f2, f3 = f * f, f * f * f
        return ((2 * f3 - 3 * f2 + 1) * y0 + (f3 - 2 * f2 + f) * m0 + (-2 * f3 + 3 * f2) * y1 + (f3 - f2) * m1)

    def mousePressEvent(self, event):
        i = self.key_at(qt.event_pos(event))
        if event.button() == QtCore.Qt.RightButton:
            if i is not None and len(self.keys) > 2:
                del self.keys[i]
                self.update()
                self.committed.emit(self.value())
            elif i is None:
                self._presets(qt.event_global_pos(event))
            return
        if event.button() != QtCore.Qt.LeftButton:
            return
        if i is None:
            p, v = self.from_screen(qt.event_pos(event))
            self.keys.append([p, v])
            self.keys.sort()
            i = self.keys.index([p, v])
        self._drag = i
        self.update()

    def mouseMoveEvent(self, event):
        if self._drag is None:
            hover = self.key_at(qt.event_pos(event))
            if hover != self._hover:
                self._hover = hover
                self.update()
            return
        p, v = self.from_screen(qt.event_pos(event))
        self.keys[self._drag] = [p, v]
        moving = self.keys[self._drag]
        self.keys.sort()
        self._drag = self.keys.index(moving)
        self.update()
        self.dragged.emit(self.value())

    def mouseReleaseEvent(self, event):
        if self._drag is not None:
            self._drag = None
            self.update()
            self.committed.emit(self.value())

    def _flipped(self):
        keys = [[1 - p, v] for p, v in self.keys]
        return {"keys": keys, "linear": True} if self.linear else keys

    def _presets(self, global_pos):
        menu = QtWidgets.QMenu(self)
        for label, keys in self.PRESETS:
            menu.addAction(label, lambda k=keys: (self.setValue(k), self.committed.emit(self.value())))
        menu.addSeparator()
        straight = menu.addAction("Linear (straight segments)")
        straight.setCheckable(True)
        straight.setChecked(self.linear)
        straight.toggled.connect(lambda on: (setattr(self, "linear", on), self.update(),
                                             self.committed.emit(self.value())))
        menu.addAction("Flip (%s to %s)" % (self.ends[1], self.ends[0]), lambda: (self.setValue(self._flipped()),
                                                      self.committed.emit(self.value())))
        qt.exec_(menu, global_pos)


# ---------------------------------------------------------------------------
# Color swatches. A color param (a vec3 whose spec unit is "color") holds linear values in the
# rendering space (as Maya's own color attributes and Arnold's colors do); the swatch shows it as
# Maya shows a color: through the view transform with color management on
# (cmds.colorManagementConvert), as it is with it off. Clicking it opens Maya's color editor on
# the value (which works in the rendering space itself); the picked value goes to `picked`.

def display_color(rgb):
    """The display values (0-1) a linear rendering-space color shows as in Maya: through the view
    transform with color management on, the values themselves with it off (or outside Maya)."""
    rgb = [max(0.0, float(c)) for c in list(rgb)[:3]]
    try:
        from maya import cmds
        if cmds.colorManagementPrefs(query=True, cmEnabled=True):
            shown = cmds.colorManagementConvert(toDisplaySpace=rgb)
            if shown and len(shown) >= 3:
                rgb = [float(c) for c in shown[:3]]
    except Exception:
        pass
    return [min(1.0, max(0.0, c)) for c in rgb]


def ask_color(rgb):
    """Maya's color editor on `rgb` (linear, the rendering space); the color picked, or None when
    cancelled. (Modal: scripted sessions call ColorSwatch.apply instead of clicking.)"""
    from maya import cmds
    result = cmds.colorEditor(rgbValue=[float(c) for c in list(rgb)[:3]])
    parts = result.split()
    if len(parts) < 4 or float(parts[3]) == 0.0:  # the last value: 1 OK, 0 cancelled
        return None
    return [float(c) for c in parts[:3]]


class ColorSwatch(QtWidgets.QToolButton):
    """A color param's swatch: the color as Maya displays it (display_color), its linear values in
    the tooltip; a click picks another (`ask`, Maya's color editor by default) and emits `picked`
    with the new linear values. apply(rgb) does what a pick does, without the editor."""

    picked = QtCore.Signal(list)

    def __init__(self, rgb=(1.0, 1.0, 1.0), ask=None, parent=None):
        super(ColorSwatch, self).__init__(parent)
        self.setObjectName("colorSwatch")
        self.setAutoRaise(False)
        self.setFixedSize(QtCore.QSize(36, 20))
        self._ask = ask or ask_color
        self.rgb = [1.0, 1.0, 1.0]
        self.clicked.connect(self.choose)
        self.set_color(rgb)

    def set_color(self, rgb):
        """Show `rgb` (linear) without emitting."""
        self.rgb = [float(c) for c in list(rgb)[:3]]
        shown = self.shown()
        self.setStyleSheet("QToolButton#colorSwatch { background-color: rgb(%d, %d, %d); border: 1px solid #222; }"
                           % tuple(int(round(255 * c)) for c in shown))
        self.setToolTip("Linear %.3f, %.3f, %.3f (click to pick)" % tuple(self.rgb))

    def shown(self):
        """The display color the swatch paints (0-1)."""
        return display_color(self.rgb)

    def apply(self, rgb):
        """Take `rgb` (linear) as picked: show it and emit `picked`."""
        self.set_color(rgb)
        self.picked.emit(list(self.rgb))

    def choose(self):
        rgb = self._ask(self.rgb)
        if rgb is not None:
            self.apply(rgb)
