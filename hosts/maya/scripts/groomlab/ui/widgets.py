# SPDX-License-Identifier: Apache-2.0
"""Shared widgets for GroomLab's panels (Qt: groomlab.ui.qt).

SoftSlider: a number as a fill field (its label inside, filled by where the
value is in a soft range; drag to scrub, click to type). The range is guessed
from the value (0-1 for fractions, a round number above larger values); typing
a value outside it stretches the range to reach it, and the right-click menu
fits or resets it. Ranges set there are remembered per key (for example
"Clump.strength") across Maya sessions.

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
    "trim": ("cut", "length", "lengths", "shape", "erase"),
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


def icon_colors(name, t=None):
    """{placeholder role: hex} for an icon in the theme (a checked tool keeps them: its tile is marked
    by the selection fill and an accent border, not by recoloring the icon)."""
    from groomlab.ui import theme
    t = t or theme.current()
    group = next((g for g, names in ICON_GROUPS.items() if name in names), "app")
    main = t.hex(t.get("icons." + group, "accent"))
    colors = {"main": main, "shade": _mix(main, "#000000", 0.38), "tint": _mix(main, "#FFFFFF", 0.5),
              "ink": t.hex(t.get("icons.ink", "white")), "shine": "#FFFFFF"}
    for name_ in ("red", "yellow", "blue", "emerald"):
        colors[name_] = t.hex(name_)
    return colors


def icon_svg(name, t=None):
    """The icon's SVG text in the theme's colors; None if there is no such icon."""
    path = os.path.join(ICON_DIR, "groomLab_%s.svg" % name)
    if path not in _svg_cache:
        if not os.path.exists(path):
            return None
        with open(path, encoding="utf-8") as f:
            _svg_cache[path] = f.read()
    svg = _svg_cache[path]
    colors = icon_colors(name, t)
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


_icon_cache = {}  # (svg text, size) -> QIcon: rendered once per theme colors


def icon(name, size=64):
    """QIcon for "comb", "place", ... in the active theme's colors (the same checked or not); empty if missing."""
    svg = icon_svg(name)
    if svg is None:
        return QtGui.QIcon()
    key = (svg, size)
    result = _icon_cache.get(key)
    if result is None:
        result = QtGui.QIcon()
        result.addPixmap(_pixmap(svg, size), QtGui.QIcon.Normal, QtGui.QIcon.Off)
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
    # The Groom Tools' lists and cards: add, import, key, help, duplicate, curves.
    "plus": '<path d="M12 5 V19 M5 12 H19" fill="none" stroke="{c}" stroke-width="1.9" stroke-linecap="round"/>',
    "import": '<rect x="3.5" y="6" width="13" height="13" rx="2" fill="none" stroke="{c}" stroke-width="1.7"/>'
              '<path d="M3.8 16 L8.2 11.8 L12.5 16" fill="none" stroke="{c}" stroke-width="1.7" stroke-linejoin="round"/>'
              '<path d="M18.5 3 V9.5 M15.25 6.25 H21.75" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>',
    "key": '<path d="M12 4.5 L19.5 12 L12 19.5 L4.5 12 Z" fill="none" stroke="{c}" stroke-width="1.8" '
           'stroke-linejoin="round"/>',
    "keyed": '<path d="M12 4.5 L19.5 12 L12 19.5 L4.5 12 Z" fill="{c}" stroke="{c}" stroke-width="1.8" '
             'stroke-linejoin="round"/>',
    "help": '<circle cx="12" cy="12" r="8.6" fill="none" stroke="{c}" stroke-width="1.7"/>'
            '<path d="M9.6 9.6 A2.5 2.5 0 1 1 13.2 11.9 C12.4 12.3 12 12.8 12 13.8" fill="none" stroke="{c}" '
            'stroke-width="1.7" stroke-linecap="round"/><circle cx="12" cy="16.7" r="1.1" fill="{c}"/>',
    "duplicate": '<rect x="8" y="8" width="11.5" height="11.5" rx="2" fill="none" stroke="{c}" stroke-width="1.7"/>'
                 '<path d="M15.5 5.2 V5 A1.5 1.5 0 0 0 14 3.5 H6 A1.5 1.5 0 0 0 4.5 5 V14 A1.5 1.5 0 0 0 6 15.5 H6.2" '
                 'fill="none" stroke="{c}" stroke-width="1.7" stroke-linecap="round"/>',
    "curves": '<path d="M4 19 C8 19 7 5 12 5 C17 5 16 19 20 19" fill="none" stroke="{c}" stroke-width="1.8" '
              'stroke-linecap="round"/><circle cx="4" cy="19" r="1.8" fill="{c}"/><circle cx="20" cy="19" r="1.8" '
              'fill="{c}"/>',
}


def glyph_button(name, tip="", slot=None, checkable=False, menu=None):
    """A small borderless button showing a line glyph (GLYPHS; it follows the theme's text color):
    the Groom Tools' header and list actions. `slot` runs on a click; `menu` opens instead."""
    from groomlab.ui import theme
    b = QtWidgets.QToolButton()
    b.setIcon(glyph(name))
    b.glyph = name
    b.setIconSize(QtCore.QSize(theme.px(16), theme.px(16)))
    b.setFixedSize(theme.px(24), theme.px(24))
    b.setAutoRaise(True)
    b.setCheckable(checkable)
    b.setProperty("role", "glyph")
    b.setToolTip(tip)
    if menu is not None:
        b.setMenu(menu)
        b.setPopupMode(QtWidgets.QToolButton.InstantPopup)
    elif slot is not None:
        b.clicked.connect(lambda _=False: slot())
    return b


def chip(text, tip="", checked=None, name=None):
    """A chip: a small rounded button (QToolButton[role="chip"]); toggles when `checked` is given
    (its state), else a plain action."""
    b = QtWidgets.QToolButton()
    b.setText(text)
    b.setToolTip(tip)
    b.setProperty("role", "chip")
    if name:
        b.setObjectName(name)
    if checked is not None:
        b.setCheckable(True)
        b.setChecked(bool(checked))
    return b


class FlowLayout(QtWidgets.QLayout):
    """Items left to right, wrapping onto a new line when the width runs out (the chips' rows)."""

    def __init__(self, parent=None, spacing=None):
        super(FlowLayout, self).__init__(parent)
        from groomlab.ui import theme
        self._items = []
        self._gap = theme.px(4) if spacing is None else spacing
        self.setContentsMargins(0, 0, 0, 0)

    def addItem(self, item):
        self._items.append(item)

    def count(self):
        return len(self._items)

    def itemAt(self, index):
        return self._items[index] if 0 <= index < len(self._items) else None

    def takeAt(self, index):
        return self._items.pop(index) if 0 <= index < len(self._items) else None

    def hasHeightForWidth(self):
        return True

    def heightForWidth(self, width):
        return self._arrange(QtCore.QRect(0, 0, width, 0), move=False)

    def setGeometry(self, rect):
        super(FlowLayout, self).setGeometry(rect)
        self._arrange(rect, move=True)

    def sizeHint(self):
        return self.minimumSize()

    def minimumSize(self):
        size = QtCore.QSize()
        for item in self._items:
            if not item.isEmpty():
                size = size.expandedTo(item.minimumSize())
        m = self.contentsMargins()
        return size + QtCore.QSize(m.left() + m.right(), m.top() + m.bottom())

    def _arrange(self, rect, move):
        m = self.contentsMargins()
        x, y = rect.x() + m.left(), rect.y() + m.top()
        right = rect.right() - m.right()
        line = 0
        for item in self._items:
            if item.isEmpty():
                continue
            hint = item.sizeHint()
            if x > rect.x() + m.left() and x + hint.width() - 1 > right:
                x = rect.x() + m.left()
                y += line + self._gap
                line = 0
            if move:
                item.setGeometry(QtCore.QRect(QtCore.QPoint(x, y), hint))
            x += hint.width() + self._gap
            line = max(line, hint.height())
        return y + line - rect.y() + m.bottom()


def chip_row(buttons, parent=None):
    """A wrapping row of chips (FlowLayout)."""
    row = QtWidgets.QWidget(parent)
    flow = FlowLayout(row)
    for b in buttons:
        flow.addWidget(b)
    row.flow = flow
    return row


def hairline(faint=False):
    """A 1px rule across the column (QFrame[role="rule"]; faint: the blocks' lighter one)."""
    from groomlab.ui import theme
    line = QtWidgets.QFrame()
    line.setProperty("role", "ruleFaint" if faint else "rule")
    line.setFixedHeight(max(1, theme.px(1)))
    return line


def small_label(text, role="muted", wrap=False):
    label = QtWidgets.QLabel(text)
    label.setProperty("role", role)
    label.setWordWrap(wrap)
    return label


class Block(QtWidgets.QWidget):
    """One of a tool card's labeled blocks: a small semibold dim title over a faint hairline, any
    action glyphs on the title's right, then its rows (add())."""

    def __init__(self, title, actions=(), name=None, parent=None):
        super(Block, self).__init__(parent)
        from groomlab.ui import theme
        self.setProperty("role", "options")
        if name:
            self.setObjectName(name)
        column = QtWidgets.QVBoxLayout(self)
        column.setContentsMargins(0, theme.px(4), 0, 0)
        column.setSpacing(theme.px(4))
        column.addWidget(hairline(faint=True))
        head = QtWidgets.QHBoxLayout()
        head.setContentsMargins(0, 0, 0, 0)
        head.setSpacing(theme.px(2))
        self.title = QtWidgets.QLabel(title)
        self.title.setProperty("role", "block")
        head.addWidget(self.title, 1)
        self.head = head
        for a in actions:
            head.addWidget(a)
        column.addLayout(head)
        self.body = QtWidgets.QVBoxLayout()
        self.body.setContentsMargins(0, 0, 0, 0)
        self.body.setSpacing(theme.px(4))
        column.addLayout(self.body)

    def add(self, item, stretch=0):
        """A widget or a layout as the next row."""
        if isinstance(item, QtWidgets.QLayout):
            self.body.addLayout(item)
        else:
            self.body.addWidget(item, stretch)
        return item

    def add_action(self, button):
        self.head.addWidget(button)
        return button

    def labeled(self, text, *controls, **kwargs):
        """A "name | control" row (combos and toggles): the name in the dim text, ~38% wide."""
        from groomlab.ui import theme
        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(6))
        label = QtWidgets.QLabel(text)
        label.setProperty("role", "rowLabel")
        label.setMinimumWidth(theme.px(kwargs.get("label_width", 84)))
        row.addWidget(label)
        for c in controls:
            if isinstance(c, QtWidgets.QLayout):
                row.addLayout(c, 1)
            else:
                row.addWidget(c, 1 if isinstance(c, (QtWidgets.QComboBox, QtWidgets.QLineEdit)) else 0)
        self.body.addLayout(row)
        row.label = label
        return label

    def named(self, text, widget):
        """A row with its name above it (chips don't fit a label column)."""
        self.body.addWidget(small_label(text))
        self.body.addWidget(widget)
        return widget


class ItemBox(QtWidgets.QFrame):
    """Under a list: the picked item's own actions (glyphs, on the right) and settings, in a small
    bordered box."""

    def __init__(self, actions=(), parent=None):
        super(ItemBox, self).__init__(parent)
        from groomlab.ui import theme
        self.setProperty("role", "itemBox")
        column = QtWidgets.QVBoxLayout(self)
        pad = theme.px(6)
        column.setContentsMargins(pad, theme.px(3), pad, pad)
        column.setSpacing(theme.px(4))
        self.head = QtWidgets.QHBoxLayout()
        self.head.setContentsMargins(0, 0, 0, 0)
        self.head.setSpacing(theme.px(2))
        self.head.addStretch(1)
        for a in actions:
            self.head.addWidget(a)
        column.addLayout(self.head)
        self.body = column

    def add(self, item):
        if isinstance(item, QtWidgets.QLayout):
            self.body.addLayout(item)
        else:
            self.body.addWidget(item)
        return item

    def add_action(self, button):
        self.head.addWidget(button)
        return button


def glyph(name, color=None, size=48):
    """A QIcon of a line glyph (GLYPHS) in `color` (default: the theme's text color)."""
    from groomlab.ui import theme
    color = color or theme.current().color("ui.text", "#F5F5F7")
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">%s</svg>'
           % GLYPHS[name].replace("{c}", color))
    return QtGui.QIcon(_pixmap(svg, size))


class ToolGrid(QtWidgets.QWidget):
    """Icon tiles of a fixed size (they never stretch with the panel), left aligned, as many to a
    row as the width holds: columns = (width + spacing) // (tile + spacing), at least one (and up
    to `columns`, when given)."""

    def __init__(self, buttons, columns=None, parent=None):
        super(ToolGrid, self).__init__(parent)
        from groomlab.ui import theme
        self.buttons = list(buttons)
        self.max_columns = columns
        self.columns = None
        # No layout: the tiles are placed by hand (a layout would answer the height for a width in
        # this widget's place, and a grid layout has no such answer).
        self.spacing = theme.px(4)
        for b in self.buttons:
            b.setParent(self)
        policy = QtWidgets.QSizePolicy(QtWidgets.QSizePolicy.Preferred, QtWidgets.QSizePolicy.Preferred)
        policy.setHeightForWidth(True)
        self.setSizePolicy(policy)
        self._place(len(self.buttons) or 1)

    def _tile(self):
        return max(b.minimumWidth() for b in self.buttons) if self.buttons else 0

    def _place(self, columns, force=False):
        columns = max(1, columns if not self.max_columns else min(columns, self.max_columns))
        if columns == self.columns and not force:
            return
        changed = columns != self.columns
        self.columns = columns
        step = self._tile() + self.spacing
        for i, b in enumerate(self.buttons):
            b.move((i % columns) * step, (i // columns) * step)
        if changed:
            self.updateGeometry()

    def columns_for(self, width):
        return max(1, (width + self.spacing) // max(1, self._tile() + self.spacing))

    def rows_for(self, width):
        columns = self.columns_for(width)
        if self.max_columns:
            columns = min(columns, self.max_columns)
        return max(1, -(-len(self.buttons) // max(1, columns)))

    def resizeEvent(self, event):
        self._place(self.columns_for(event.size().width()), force=True)
        super(ToolGrid, self).resizeEvent(event)

    def hasHeightForWidth(self):
        return True

    def heightForWidth(self, width):
        if not self.buttons:
            return 0
        rows = self.rows_for(width)
        return rows * self._tile() + (rows - 1) * self.spacing

    def minimumSizeHint(self):
        # (one row: the height a width needs comes from heightForWidth)
        return QtCore.QSize(self._tile(), self._tile() if self.buttons else 0)

    def sizeHint(self):
        columns = max(1, self.columns or 1)
        spacing = self.spacing
        width = columns * self._tile() + (columns - 1) * spacing
        return QtCore.QSize(width, self.heightForWidth(max(width, self.width())))


class Pages(QtWidgets.QWidget):
    """Underlined tabs over pages (a hairline under the bar, the current tab's name underlined in
    the accent): only the page showing takes room, so what sits under it follows right below
    (the QTabWidget API: addTab, currentIndex, setCurrentIndex, currentChanged, count, widget)."""

    currentChanged = QtCore.Signal(int)

    def __init__(self, parent=None):
        super(Pages, self).__init__(parent)
        from groomlab.ui import theme
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(8))
        track = QtWidgets.QFrame()
        track.setProperty("role", "segments")  # the hairline under the tabs
        self.track = track  # the bar's row: a window may put it elsewhere (the Groom Tools pin it)
        row = QtWidgets.QHBoxLayout(track)
        row.setContentsMargins(0, 0, 0, 0)
        self.bar = QtWidgets.QTabBar()
        self.bar.setExpanding(False)  # each tab as wide as its name, left aligned
        self.bar.setUsesScrollButtons(False)  # a narrow panel shortens the names rather than scroll them
        self.bar.setElideMode(QtCore.Qt.ElideRight)
        self.bar.setDrawBase(False)
        self.bar.setDocumentMode(True)
        row.addWidget(self.bar)
        row.addStretch(1)
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
    """A titled section that folds away: click its header to show or hide `content`. The header:
    a chevron and the title in small spaced capitals, muted, under a hairline (no card)."""

    def __init__(self, title, content, expanded=False, parent=None, count=False, actions=()):
        super(Section, self).__init__(parent)
        from groomlab.ui import theme
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
        self.title = title
        self.header = QtWidgets.QPushButton()
        self.header.setCheckable(True)
        self.header.setFlat(True)
        self.header.setProperty("role", "section")
        font = QtGui.QFont(self.header.font())
        font.setLetterSpacing(QtGui.QFont.PercentageSpacing, 108)  # the style sheet sets its size and weight
        self.header.setFont(font)
        self.header.toggled.connect(self.set_expanded)
        self.content = content
        self.count = None
        if count or actions:
            # A list's section: the title, how many, then its glyphs (add, import...) on the right,
            # all on one row under the hairline.
            self.header.setProperty("inRow", "true")
            self.header.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Fixed)
            self.row = QtWidgets.QFrame()
            self.row.setProperty("role", "sectionRow")
            line = QtWidgets.QHBoxLayout(self.row)
            line.setContentsMargins(0, 0, 0, 0)
            line.setSpacing(theme.px(2))
            line.addWidget(self.header)
            if count:
                self.count = QtWidgets.QLabel()
                self.count.setProperty("role", "count")
                line.addWidget(self.count)
            line.addStretch(1)
            for a in actions:
                line.addWidget(a)
            self.actions_row = line
            layout.addWidget(self.row)
        else:
            layout.addWidget(self.header)
        layout.addWidget(content)
        self.set_expanded(expanded)

    def set_count(self, n):
        if self.count is not None:
            self.count.setText("" if n is None else str(n))

    def add_action(self, button):
        self.actions_row.addWidget(button)
        return button

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


def display_decimals(spec_decimals, factor=1.0):
    """The decimals for a value shown `factor` times larger than the spec's (a fraction as a
    percentage, a width in micrometers): fewer by the factor's decades, 0 to 6."""
    import math
    if spec_decimals is None:
        return None
    shift = int(round(math.log10(abs(factor)))) if factor else 0
    return max(0, min(6, int(spec_decimals) - shift))


def parse_number(text):
    """A typed value: a number or simple arithmetic ("0.5*2", "1/3", "-(2+3)", "," as the decimal
    point), as Maya's and Blender's fields take it; None when it is neither. Only numbers, + - * /,
    parentheses and a sign are read (ast, never eval)."""
    import ast
    s = (text or "").strip().replace(",", ".")
    if not s or len(s) > 200:
        return None
    try:
        tree = ast.parse(s, mode="eval")
    except (SyntaxError, ValueError):
        return None
    num = getattr(ast, "Num", None)  # Python 3.7 (Maya 2022) parses numbers as ast.Num
    binary = {ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b,
              ast.Mult: lambda a, b: a * b, ast.Div: lambda a, b: a / b}

    def value(node):
        if isinstance(node, ast.Expression):
            return value(node.body)
        if isinstance(node, ast.BinOp) and type(node.op) in binary:
            return binary[type(node.op)](value(node.left), value(node.right))
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd)):
            v = value(node.operand)
            return -v if isinstance(node.op, ast.USub) else v
        if isinstance(node, getattr(ast, "Constant", ())):
            constant = node.value
        elif num is not None and isinstance(node, num):
            constant = node.n
        else:
            constant = None
        if isinstance(constant, (int, float)) and not isinstance(constant, bool):
            return float(constant)
        raise ValueError("not a number")

    try:
        v = value(tree)
    except (ValueError, ZeroDivisionError, OverflowError, RecursionError):
        return None
    return v if math.isfinite(v) else None


def _mix_color(a, b, f):
    """QColor a blended toward b by f (0-1)."""
    return QtGui.QColor(int(round(a.red() + (b.red() - a.red()) * f)), int(round(a.green() + (b.green() - a.green()) * f)),
                        int(round(a.blue() + (b.blue() - a.blue()) * f)))


def _readable_on(color):
    """Dark or light text, whichever reads on `color`."""
    luma = 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF()
    return QtGui.QColor(28, 29, 33) if luma > 0.55 else QtGui.QColor(255, 255, 255)


def _has(modifiers, flag):
    return bool(qt.to_int(modifiers) & qt.to_int(flag))


class SoftSlider(QtWidgets.QWidget):
    """A number as a fill field (the redesign's slider): one painted rounded field, its label inside
    on the left and the value (and unit) on the right, filled left to right by where the value is
    in its soft range.

    - Drag (past 3 px) scrubs relative to the press: the field's width is the whole range
      (Shift: a tenth as fast, Ctrl: steps of 10x); dragging stays inside the range.
    - A click without a drag types: an inline field over it, the value selected. Enter, Tab or
      clicking away commits, Esc puts the old value back. A number or simple arithmetic (0.5*2,
      1/3, "," as the decimal point); a typed value past the range stretches it to reach it.
    - Focused: Enter or F2 types, the arrows nudge by a step (Shift: 10).
    - Right-click: Reset to Default, the range (fit, set, reset) and menu_hook's actions (keys).
    - set_key_color: the Channel Box's key colors (keyed, animated, driven) tint the field.

    The range is soft, guessed from the value (0-1 for fractions, a round number above larger
    values), stretched by typed values and remembered per key ("Clump.strength") once set from the
    menu. minimum / maximum are hard limits (the core clamps there).

    Signals: dragged(value) while dragged (for live previews), committed(value) when a value is
    settled (the drag released, a value typed, an arrow, Reset to Default).
    """

    dragged = QtCore.Signal(float)
    committed = QtCore.Signal(float)
    DRAG_PX = 3  # a press moving less than this is a click (types a value)
    TIGHT_SUFFIXES = ("%", u"°", u"×")  # units written right after the number

    def __init__(self, value=0.0, lo=None, hi=None, integer=False, minimum=None, maximum=None, decimals=3,
                 key=None, default=None, parent=None, accent=None, shown_decimals=None, label=None, suffix=None):
        super(SoftSlider, self).__init__(parent)
        self.integer = integer
        self.key = key
        self.minimum = minimum
        self.maximum = maximum
        self.default = default  # the param's default, for Reset to Default
        self.decimals = decimals
        self.shown_decimals = shown_decimals  # the spec's, as every host shows it (None: decimals)
        self.accent = accent  # a theme color name filling the field (None: the theme's selection)
        self.menu_hook = None  # fn(menu): more actions atop the right-click menu (the Graph Editor's keys)
        self.unit_tip = ""     # what the unit means, in the tooltip
        base = default if default is not None else value
        self.default_range = (lo, hi) if lo is not None and hi is not None else default_range(base, integer)
        self.lo, self.hi = remembered_range(key) or self.default_range
        self._value = float(value)
        self._stretch_to(self._value)  # a value set elsewhere may be outside
        self._label = label or ""
        self._suffix = suffix or ""
        self._key_color = None
        self._press = None  # [x at the press, value at the press, moved past DRAG_PX] while the button is down
        self._hover = False
        self._editing = False
        self._typed_from = ""  # the inline field's text when opened: unchanged, nothing is committed

        self.setFocusPolicy(QtCore.Qt.StrongFocus)
        self.setCursor(QtCore.Qt.SizeHorCursor)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        self.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.customContextMenuRequested.connect(self._menu)
        self.editor = QtWidgets.QLineEdit(self)  # the inline field, shown while typing
        self.editor.setObjectName("fillFieldEditor")
        self.editor.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        self.editor.editingFinished.connect(self._typed)
        self.editor.installEventFilter(self)
        self.editor.hide()

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
        self.update()

    def setRange(self, lo, hi):
        self.lo, self.hi = float(lo), float(hi)
        self.update()

    def label(self):
        return self._label

    def setLabel(self, text):
        """The name inside the field, on the left."""
        self._label = text or ""
        self.updateGeometry()
        self.update()

    def suffix(self):
        return self._suffix

    def setSuffix(self, text, tip=None):
        """The unit after the value (px, cm, %, °...); `tip` says what it means in the tooltip."""
        self._suffix = text or ""
        if tip is not None:
            self.unit_tip = tip
        self.updateGeometry()
        self.update()

    def key_color(self):
        return self._key_color

    def set_key_color(self, color):
        """Tint the field with a key state's color (QColor; None: the theme's own look)."""
        self._key_color = QtGui.QColor(color) if color is not None else None
        self.update()

    def fraction(self):
        """Where the value is in the range, 0 to 1: how much of the field is filled."""
        span = self.hi - self.lo
        return max(0.0, min(1.0, (self._value - self.lo) / span)) if span > 0 else 0.0

    def step(self):
        """An arrow's nudge, and what a drag snaps to: 1 for whole numbers, else a round
        hundredth of the range (0.01 over 0-1, 1 over 0-400)."""
        if self.integer:
            return 1.0
        span = self.hi - self.lo
        return 10.0 ** math.floor(math.log10(span / 100.0)) if span > 0 else 0.01

    def text(self):
        """The value as the field shows it, with its unit."""
        if self.integer:
            number = "%d" % self.value()
        else:
            number = format_number(self._value, self.shown_decimals if self.shown_decimals is not None
                                   else self.decimals)
        if not self._suffix:
            return number
        return number + ("" if self._suffix in self.TIGHT_SUFFIXES else " ") + self._suffix

    def _stretch_to(self, v):
        if v > self.hi:
            self.hi = nice_ceil(v) if v > 0 else 0.0
        if v < self.lo:
            self.lo = -nice_ceil(-v) if v < 0 else 0.0

    def _limited(self, v, soft=False):
        """v within the hard limits (and the soft range with `soft`: a drag or an arrow), a whole
        number for an integer field."""
        if soft:
            v = max(self.lo, min(self.hi, v))
        if self.minimum is not None:
            v = max(v, self.minimum)
        if self.maximum is not None:
            v = min(v, self.maximum)
        return float(int(round(v))) if self.integer else float("%.12g" % v)

    @staticmethod
    def _snapped(v, step):
        return round(v / step) * step if step > 0 else v

    # -- Settling a value ---------------------------------------------------------

    def _commit(self, v, stretch=True):
        """Take a value the user settled (typed, nudged) and say so."""
        if v == self._value:
            return
        self._value = v
        # This slider stretches to reach it; the range every node of the type starts from
        # stays (one unusual value must not rescale them all). Set Range... keeps one.
        if stretch:
            self._stretch_to(v)
        self.update()
        self.committed.emit(self.value())

    def commit_text(self, text):
        """Take a typed value (a number or simple arithmetic, the unit allowed after it): True when
        it reads as one."""
        text = (text or "").strip()
        if self._suffix and text.endswith(self._suffix):
            text = text[:-len(self._suffix)]
        v = parse_number(text)
        if v is None:
            return False
        self._commit(self._limited(v))
        return True

    def isSliderDown(self):
        """The mouse button held on the field (a drag, or a click not yet released)."""
        return self._press is not None

    def setSliderDown(self, down):
        """Hold the field as a drag does (scripts and tests: then scrub()); releasing commits."""
        if down and self._press is None:
            self._press = [0, self._value, True]
        elif not down and self._press is not None:
            moved = self._press[2]
            self._press = None
            self.update()
            if moved:
                self.committed.emit(self.value())

    def scrub(self, v):
        """Move to v as a drag does, inside the range: a preview (dragged) while held, else committed."""
        v = self._limited(float(v), soft=True)
        if self._press is None:
            self._commit(v, stretch=False)
            return
        if v != self._value:
            self._value = v
            self.update()
            self.dragged.emit(self.value())

    def busy(self):
        """Being dragged or typed in: a refresh leaves it alone."""
        return self.isSliderDown() or self._editing

    # -- Typing --------------------------------------------------------------------

    def isEditing(self):
        return self._editing

    def edit(self):
        """Open the inline field over the field, its value selected; returns it (None when disabled)."""
        if not self.isEnabled():
            return None
        if self.integer:
            self._typed_from = "%d" % self.value()
        else:
            self._typed_from = "%.10g" % self._value
        e = self.editor
        e.setGeometry(self.rect())
        e.setText(self._typed_from)
        self._editing = True
        e.show()
        e.raise_()
        e.setFocus(QtCore.Qt.OtherFocusReason)
        e.selectAll()
        self.update()
        return e

    def _close_editor(self):
        self._editing = False
        if self.editor.hasFocus():
            self.setFocus(QtCore.Qt.OtherFocusReason)  # back to the field, not the next widget
        self.editor.hide()
        self.update()

    def _typed(self):
        """Enter, Tab or clicking away: the typed value committed (unchanged text: nothing)."""
        if not self._editing or not alive(self):  # (a field deleted while typed in: its focus leaving)
            return
        text = self.editor.text()
        self._close_editor()
        if text.strip() != self._typed_from:
            self.commit_text(text)

    def cancel_edit(self):
        """Esc: the field closed, the value as it was."""
        if self._editing:
            self._close_editor()

    def eventFilter(self, obj, event):
        if obj is self.editor and event.type() in (QtCore.QEvent.KeyPress, QtCore.QEvent.ShortcutOverride):
            key = event.key()
            if key in (QtCore.Qt.Key_Escape, QtCore.Qt.Key_Return, QtCore.Qt.Key_Enter):
                if event.type() == QtCore.QEvent.ShortcutOverride:
                    event.accept()  # the field's keys, not a shortcut's
                elif key == QtCore.Qt.Key_Escape:
                    self.cancel_edit()
                else:
                    self._typed()  # here, not passed on to the field (whose Enter would type again)
                return True
        return super(SoftSlider, self).eventFilter(obj, event)

    # -- Mouse and keys ------------------------------------------------------------------

    def mousePressEvent(self, event):
        if event.button() != QtCore.Qt.LeftButton or self._editing:
            return super(SoftSlider, self).mousePressEvent(event)
        self.setFocus(QtCore.Qt.MouseFocusReason)
        self._press = [qt.event_pos(event).x(), self._value, False]
        event.accept()

    def mouseMoveEvent(self, event):
        press = self._press
        if press is None:
            return super(SoftSlider, self).mouseMoveEvent(event)
        from groomlab.ui import theme
        dx = qt.event_pos(event).x() - press[0]
        if not press[2] and abs(dx) < theme.px(self.DRAG_PX):
            return
        press[2] = True
        mods = event.modifiers()
        speed = 0.1 if _has(mods, QtCore.Qt.ShiftModifier) else 1.0
        v = press[1] + dx / float(max(1, self.width())) * (self.hi - self.lo) * speed
        self.scrub(self._snapped(v, self.step() * (10.0 if _has(mods, QtCore.Qt.ControlModifier) else 1.0)))

    def mouseReleaseEvent(self, event):
        if event.button() != QtCore.Qt.LeftButton or self._press is None:
            return super(SoftSlider, self).mouseReleaseEvent(event)
        moved = self._press[2]
        self._press = None
        self.update()
        if moved:
            self.committed.emit(self.value())  # the drag settled: one undo step
        else:
            self.edit()  # a click: type a value

    def keyPressEvent(self, event):
        key = event.key()
        if key in (QtCore.Qt.Key_Return, QtCore.Qt.Key_Enter, QtCore.Qt.Key_F2):
            self.edit()
            return
        nudge = {QtCore.Qt.Key_Right: 1, QtCore.Qt.Key_Up: 1, QtCore.Qt.Key_Left: -1, QtCore.Qt.Key_Down: -1}
        if key in nudge:
            steps = nudge[key] * (10 if _has(event.modifiers(), QtCore.Qt.ShiftModifier) else 1)
            step = self.step()
            self._commit(self._limited(self._snapped(self._value + steps * step, step), soft=True), stretch=False)
            return
        super(SoftSlider, self).keyPressEvent(event)

    def enterEvent(self, event):
        self._hover = True
        self.update()
        super(SoftSlider, self).enterEvent(event)

    def leaveEvent(self, event):
        self._hover = False
        self.update()
        super(SoftSlider, self).leaveEvent(event)

    def focusInEvent(self, event):
        self.update()
        super(SoftSlider, self).focusInEvent(event)

    def focusOutEvent(self, event):
        self.update()
        super(SoftSlider, self).focusOutEvent(event)

    def resizeEvent(self, event):
        if self._editing:
            self.editor.setGeometry(self.rect())
        super(SoftSlider, self).resizeEvent(event)

    def changeEvent(self, event):
        if event.type() in (QtCore.QEvent.StyleChange, QtCore.QEvent.FontChange):
            self.updateGeometry()  # a theme's row height or font
        super(SoftSlider, self).changeEvent(event)

    def event(self, event):
        if event.type() == QtCore.QEvent.ToolTip:
            tip = "\n".join(t for t in (self.toolTip(), self.unit_tip, self.range_tip()) if t)
            QtWidgets.QToolTip.showText(qt.event_global_pos(event), tip, self)
            return True
        return super(SoftSlider, self).event(event)

    def range_tip(self):
        return ("%g to %g: drag (Shift: finer, Ctrl: steps of 10) or click to type (a value past it stretches "
                "it); right-click to set the range" % (self.lo, self.hi))

    # -- Painting ------------------------------------------------------------------

    @staticmethod
    def row_height():
        """The theme's row height (ui.rowHeight; 26 without one), at Maya's interface scale."""
        from groomlab.ui import theme
        return theme.px(float(theme.current().get("ui.rowHeight") or 26))

    def sizeHint(self):
        from groomlab.ui import theme
        fm = self.fontMetrics()
        advance = getattr(fm, "horizontalAdvance", None) or fm.width
        width = advance(self._label) + advance(self.text()) + theme.px(32)
        return QtCore.QSize(max(theme.px(120), width), self.row_height())

    def minimumSizeHint(self):
        from groomlab.ui import theme
        return QtCore.QSize(theme.px(48), self.row_height())

    def colors(self):
        """(background, fill, border, text, label) as the field paints them now."""
        from groomlab.ui import theme
        t = theme.current()
        raised = QtGui.QColor(t.color("ui.raised"))
        hovered = self._hover and self.isEnabled() and not self._editing
        background = QtGui.QColor(t.color("ui.hover")) if hovered else raised
        if self.accent:
            fill = _mix_color(raised, QtGui.QColor(t.hex(self.accent)), 0.5)
        elif t.get("ui.selection"):
            fill = QtGui.QColor(t.color("ui.selection"))
        else:  # a theme without a selection color: the accent, quietened under the text
            fill = _mix_color(raised, QtGui.QColor(t.hex("accent")), 0.35)
        border = QtGui.QColor(t.color("ui.border"))
        text, label = QtGui.QColor(t.color("ui.text")), QtGui.QColor(t.color("ui.textDim"))
        if self._key_color is not None:  # keyed, animated, driven: the Channel Box's colors
            key = self._key_color
            background = _mix_color(raised, key, 0.6 if hovered else 0.45)
            fill, border = QtGui.QColor(key), QtGui.QColor(key)
            text = _readable_on(key)
            label = QtGui.QColor(text)
            label.setAlpha(200)
        if self.hasFocus() and not self._editing:
            border = QtGui.QColor(t.hex("accent"))
        return background, fill, border, text, label

    def paintEvent(self, _event):
        from groomlab.ui import theme
        p = QtGui.QPainter(self)
        p.setRenderHint(QtGui.QPainter.Antialiasing, True)
        if not self.isEnabled():
            p.setOpacity(0.45)
        background, fill, border, text, label = self.colors()
        line = max(1, theme.px(1))
        box = QtCore.QRectF(self.rect()).adjusted(line / 2.0, line / 2.0, -line / 2.0, -line / 2.0)
        radius = min(float(theme.px(int(theme.current().get("ui.radius", 6)))), box.height() / 2.0)
        path = QtGui.QPainterPath()
        path.addRoundedRect(box, radius, radius)
        p.fillPath(path, background)
        share = self.fraction()
        if share > 0.0:
            p.save()
            p.setClipPath(path)
            p.fillRect(QtCore.QRectF(box.left(), box.top(), box.width() * share, box.height()), fill)
            p.restore()
        p.setPen(QtGui.QPen(border, line))
        p.setBrush(QtCore.Qt.NoBrush)
        p.drawPath(path)
        if self._editing:
            return  # the inline field shows the text
        pad = theme.px(8)
        inner = self.rect().adjusted(pad, 0, -pad, 0)
        fm = self.fontMetrics()
        advance = getattr(fm, "horizontalAdvance", None) or fm.width
        value_text = self.text()
        p.setPen(text)
        if not self._label:
            p.drawText(inner, QtCore.Qt.AlignCenter, value_text)
            return
        p.drawText(inner, QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter, value_text)
        room = inner.width() - advance(value_text) - pad
        if room > 0:
            p.setPen(label)
            p.drawText(QtCore.QRect(inner.left(), inner.top(), room, inner.height()),
                       QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter,
                       fm.elidedText(self._label, QtCore.Qt.ElideRight, room))

    # -- The right-click menu ----------------------------------------------------------

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
        qt.exec_(menu, self.mapToGlobal(pos))

    def _reset_value(self):
        self.setValue(self.default)
        self.committed.emit(self.value())

    def _fit(self):
        lo, hi = default_range(self._value if self._value != 0 else 1.0, self.integer)
        self.lo, self.hi = lo, hi
        remember_range(self.key, lo, hi)
        self.update()

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
            self.update()

    def _reset(self):
        forget_range(self.key)
        self.lo, self.hi = self.default_range
        self._stretch_to(self._value)
        self.update()


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


# -- Brush alphas ----------------------------------------------------------------

_alpha_icons = {}  # path -> (modified time, QIcon)


def alpha_icon(path, size=64):
    """An alpha's thumbnail (white: full weight) as an icon, kept until its file changes; a dashed
    empty square for none ("")."""
    pixmap = QtGui.QPixmap(size, size)
    if not path:
        pixmap.fill(QtCore.Qt.transparent)
        painter = QtGui.QPainter(pixmap)
        pen = QtGui.QPen(QtGui.QColor(140, 140, 140), 2, QtCore.Qt.DashLine)
        painter.setPen(pen)
        painter.drawRect(4, 4, size - 9, size - 9)
        painter.drawLine(8, size - 9, size - 9, 8)
        painter.end()
        return QtGui.QIcon(pixmap)
    try:
        stamp = os.path.getmtime(path)
    except OSError:
        stamp = None
    cached = _alpha_icons.get(path)
    if cached and cached[0] == stamp:
        return cached[1]
    import groomlab_core
    try:
        data = groomlab_core.brush_alpha_thumbnail(path, size)
    except ValueError:
        return alpha_icon("", size)
    image = QtGui.QImage(data, size, size, size, QtGui.QImage.Format_Grayscale8).copy()
    icon = QtGui.QIcon(QtGui.QPixmap.fromImage(image))
    _alpha_icons[path] = (stamp, icon)
    return icon


class AlphaPicker(QtWidgets.QWidget):
    """The Paint brush's alpha: a thumbnail button (and the alpha's name) opening a grid of the
    library's alphas (None first, the user's own, then the shipped ones), with Add Alpha... and the
    folder below it; right-click the button for the same. Emits changed(path) ("" none)."""

    changed = QtCore.Signal(str)
    THUMB = 64

    def __init__(self, parent=None):
        super(AlphaPicker, self).__init__(parent)
        self.path = ""
        row = QtWidgets.QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme_px(8))
        self.button = QtWidgets.QToolButton()
        self.button.setObjectName("alphaButton")
        self.button.setIconSize(QtCore.QSize(theme_px(40), theme_px(40)))
        self.button.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        self.button.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.button.customContextMenuRequested.connect(self.context_menu)
        self.name = QtWidgets.QLabel()
        row.addWidget(self.button)
        row.addWidget(self.name, 1)
        self.popup = QtWidgets.QMenu(self.button)
        self.grid = QtWidgets.QListWidget()
        self.grid.setObjectName("alphaGrid")
        self.grid.setViewMode(QtWidgets.QListView.IconMode)
        self.grid.setIconSize(QtCore.QSize(self.THUMB, self.THUMB))
        self.grid.setGridSize(QtCore.QSize(self.THUMB + theme_px(18), self.THUMB + theme_px(26)))
        self.grid.setResizeMode(QtWidgets.QListView.Adjust)
        self.grid.setMovement(QtWidgets.QListView.Static)
        self.grid.setWordWrap(True)
        self.grid.setFixedSize((self.THUMB + theme_px(18)) * 4 + theme_px(24), (self.THUMB + theme_px(26)) * 3 + theme_px(8))
        self.grid.itemClicked.connect(self._picked)
        panel = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(panel)
        column.setContentsMargins(theme_px(6), theme_px(6), theme_px(6), theme_px(6))
        column.addWidget(self.grid)
        buttons = QtWidgets.QHBoxLayout()
        add = QtWidgets.QPushButton("Add Alpha...")
        add.setToolTip("Copy an image (PNG, JPEG, TGA, PSD...) into your alpha folder: every host lists it")
        add.clicked.connect(self.add)
        folder = QtWidgets.QPushButton("Open Folder")
        folder.setToolTip("Your alpha folder (Documents/GroomLab/alphas): images dropped there are listed too")
        folder.clicked.connect(self.open_folder)
        buttons.addWidget(add)
        buttons.addWidget(folder)
        buttons.addStretch(1)
        column.addLayout(buttons)
        action = QtWidgets.QWidgetAction(self.popup)
        action.setDefaultWidget(panel)
        self.popup.addAction(action)
        self.popup.aboutToShow.connect(self.fill)
        self.button.setMenu(self.popup)
        self.show_alpha()

    def fill(self):
        """The library as it is now (images dropped in a folder since show up)."""
        import groomlab
        self.grid.clear()
        entries = [{"name": "None", "path": ""}] + groomlab.brush_alphas()
        for entry in entries:
            item = QtWidgets.QListWidgetItem(alpha_icon(entry["path"], self.THUMB), entry["name"])
            item.setData(QtCore.Qt.UserRole, entry["path"])
            item.setToolTip(entry["path"] or "No alpha: the brush's falloff alone")
            item.setTextAlignment(QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop)
            self.grid.addItem(item)
            if os.path.normcase(entry["path"]) == os.path.normcase(self.path):
                self.grid.setCurrentItem(item)

    def _picked(self, item):
        self.popup.close()
        self.set_path(item.data(QtCore.Qt.UserRole) or "")

    def set_path(self, path, emit=True):
        """Shows the alpha (a file gone: none) and, with `emit`, tells the brush."""
        path = path if path and os.path.isfile(path) else ""
        changed = path != self.path
        self.path = path
        self.show_alpha()
        if emit and changed:
            self.changed.emit(path)

    def show_alpha(self):
        self.button.setIcon(alpha_icon(self.path, self.THUMB))
        name = os.path.splitext(os.path.basename(self.path))[0] if self.path else "None"
        self.name.setText(name)
        self.button.setToolTip((self.path or "No alpha") + " (click: pick one; right-click: more)")

    def add(self):
        import groomlab
        self.popup.close()
        files, _filter = QtWidgets.QFileDialog.getOpenFileNames(
            self, "Add Alpha", "", "Images (*.png *.jpg *.jpeg *.tga *.bmp *.psd *.gif *.hdr *.pic *.pnm *.ppm *.pgm)")
        added = ""
        for f in files:
            try:
                added = groomlab.add_brush_alpha(f)
            except ValueError as e:
                QtWidgets.QMessageBox.warning(self, "Add Alpha", str(e))
        if added:
            self.set_path(added)

    def open_folder(self):
        import groomlab
        self.popup.close()
        folder = groomlab.brush_alpha_folder()
        if folder:
            QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(folder))

    def context_menu(self, pos):
        menu = QtWidgets.QMenu(self)
        clear = menu.addAction("Clear Alpha")
        clear.setEnabled(bool(self.path))
        clear.triggered.connect(lambda: self.set_path(""))
        menu.addAction("Add Alpha...").triggered.connect(self.add)
        menu.addAction("Open Alpha Folder").triggered.connect(self.open_folder)
        qt.exec_(menu, self.button.mapToGlobal(pos))
