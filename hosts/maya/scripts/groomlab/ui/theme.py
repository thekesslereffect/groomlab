# SPDX-License-Identifier: Apache-2.0
"""GroomLab UI themes: colors and shapes for the Groom Tools, the Graph Editor
and the viewport's guides, from JSON files.

Four ship with GroomLab: "charcoal" (calm neutral greys, one blue accent,
thin borders, muted color only where it means something; the default),
"playful" (charcoal and primaries, flat and round; the base every theme
extends), "studio" (low-key greys, one accent) and "polar" (near-black and
monochrome with one indigo accent, after polar.sh). Pick one in GroomLab >
Theme, or Theme in the Groom Tools' More menu or the Graph Editor's Edit menu
(a pick is remembered: a user who made one keeps it). Make your own:

    from groomlab.ui import theme
    path = theme.create_theme("mine")      # a copy of the current theme, to edit
    theme.set_theme("mine")                # after editing: applies everywhere at once
    theme.list_themes()                    # ["charcoal", "mine", "playful", "polar", "studio"]

Themes are looked up, in order, in every directory on GROOMLAB_THEME_PATH,
the user's folder (<Maya user app dir>/groomlab/themes) and the shipped
folder. A theme names colors as hex ("#2F7BF6"), by its palette ("blue"), or
"accent"; it may set only some keys: the rest come from the theme it
"extends" (default "playful"). Optional ui keys: "outline" (a thin border
around buttons, cards and lists; none by default), "selection" and
"selectionText" (selected rows, menu items and the active tool; the accent by
default), "textMuted" (the quietest text: section headers; textDim by
default), "rowHeight" (the height of fields, combo boxes and buttons; their
text's by default) and "glass" (the standalone app's translucent panels, true
by default; false: solid panels with a thin border). Graph keys:
"headerStyle" ("tinted", "filled" or "minimal") and "canvasStyle" ("dots",
"lines" or "plain").
"""

import copy
import json
import os
import re
import weakref

THEME_VAR = "groomLabTheme"
DEFAULT = "charcoal"  # the theme a user who never picked one gets
BASE = "playful"      # the theme every theme extends unless it says otherwise (it sets every key)
SHIPPED_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), os.pardir, os.pardir, os.pardir, "themes"))

_current = None
_listeners = []  # (weakref to object, method name)
_slider_count = [0]


# ---------------------------------------------------------------------------
# Finding and loading

def user_dir():
    import groomlab
    return groomlab.library_user_dir("GROOMLAB_USER_THEMES", "themes")


def search_dirs():
    import groomlab
    return groomlab.library_dirs("GROOMLAB_THEME_PATH", user_dir, SHIPPED_DIR)


def list_themes():
    names = set()
    for d in search_dirs():
        if os.path.isdir(d):
            names.update(f[:-5] for f in os.listdir(d) if f.endswith(".json"))
    return sorted(names)


def label(name):
    """A theme's name as menus show it ("my_theme" -> "My Theme")."""
    return name.replace("_", " ").title()


def menu_entries():
    """(name, label, active) for every theme, as the Theme menus list them."""
    active = current().key
    return [(name, label(name), name == active) for name in list_themes()]


def theme_path(name):
    for d in search_dirs():
        path = os.path.join(d, name + ".json")
        if os.path.isfile(path):
            return path
    raise ValueError("no theme named %r (looked in %s)" % (name, ", ".join(search_dirs())))


def _merge(base, over):
    out = copy.deepcopy(base)
    for key, value in over.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _merge(out[key], value)
        else:
            out[key] = copy.deepcopy(value)
    return out


def load(name, _seen=None):
    """The theme's data, filled in from the theme it extends."""
    seen = _seen or set()
    if name in seen:
        raise ValueError("themes extend each other in a loop: %s" % ", ".join(sorted(seen)))
    seen.add(name)
    with open(theme_path(name), encoding="utf-8") as f:
        try:
            data = json.load(f)
        except ValueError as e:
            raise ValueError("theme %r is not valid JSON: %s" % (name, e))
    base = data.get("extends", BASE if name != BASE else None)
    return _merge(load(base, seen), data) if base else data


class Theme(object):
    """A loaded theme: data, plus colors resolved from names."""

    def __init__(self, key, data):
        self.key = key
        self.data = data
        self.name = data.get("name", key)

    def get(self, path, default=None):
        node = self.data
        for part in path.split("."):
            if not isinstance(node, dict) or part not in node:
                return default
            node = node[part]
        return node

    def hex(self, value, default="#FF00FF"):
        """A color value (hex, palette name, "accent") as "#RRGGBB"."""
        for _ in range(4):  # names may point at names
            if isinstance(value, str) and not value.startswith("#"):
                value = self.get("ui.accent") if value == "accent" else self.get("palette." + value, default)
            else:
                break
        if isinstance(value, (list, tuple)) and len(value) >= 3:
            return "#%02X%02X%02X" % tuple(int(round(255 * max(0.0, min(1.0, c)))) for c in value[:3])
        return value if isinstance(value, str) and value.startswith("#") else default

    def color(self, path, default="#FF00FF"):
        """The hex color at a path, e.g. "ui.panel" or "graph.categories.Look"."""
        return self.hex(self.get(path, default), default)

    def qcolor(self, path_or_value, alpha=None):
        from groomlab.ui.qt import QtGui
        value = self.color(path_or_value) if "." in str(path_or_value) else self.hex(path_or_value)
        c = QtGui.QColor(value)
        if alpha is not None:
            c.setAlpha(alpha)
        return c

    def linear(self, value):
        """A color value as linear RGB floats (viewport colors are color managed)."""
        h = self.hex(value).lstrip("#")
        rgb = [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
        return [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in rgb]


def current():
    """The active theme (the optionVar's, else the default)."""
    global _current
    if _current is None:
        name = DEFAULT
        try:
            from maya import cmds
            if cmds.optionVar(exists=THEME_VAR):
                name = cmds.optionVar(query=THEME_VAR)
        except ImportError:
            pass
        try:
            _current = Theme(name, load(name))
        except (ValueError, IOError, OSError):
            _current = Theme(DEFAULT, load(DEFAULT))
    return _current


def set_theme(name):
    """Make `name` the active theme (remembered across sessions) and restyle everything open."""
    global _current
    _current = Theme(name, load(name))  # raises before anything changes if it doesn't load
    from maya import cmds
    cmds.optionVar(stringValue=(THEME_VAR, name))
    apply()
    return _current


def reload():
    """Read the active theme's file again (after editing it) and restyle everything."""
    global _current
    key = current().key
    _current = None
    _current = Theme(key, load(key))
    apply()
    return _current


def create_theme(name, base=None):
    """A new theme file in the user's folder: a copy of `base` (default: the
    active theme) to edit. Returns its path."""
    if not name or any(c in name for c in '\\/:*?"<>|'):
        raise ValueError("invalid theme name %r" % name)
    data = load(base) if base else copy.deepcopy(current().data)
    data["name"] = name
    data.pop("extends", None)
    path = os.path.join(user_dir(), name + ".json")
    if os.path.exists(path):
        raise ValueError("a theme named %r exists: %s" % (name, path))
    if not os.path.isdir(os.path.dirname(path)):
        os.makedirs(os.path.dirname(path))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    return path


# ---------------------------------------------------------------------------
# Applying

def listen(obj, method="apply_theme"):
    """Call obj.<method>(theme) whenever the theme changes (held weakly)."""
    _listeners.append((weakref.ref(obj), method))


_callbacks = []


def on_change(fn):
    """Call fn(theme) whenever the theme changes (for module-level things like the menu)."""
    if fn not in _callbacks:
        _callbacks.append(fn)


def apply():
    """Push the active theme to the viewport and every listening window."""
    t = current()
    apply_viewport(t)
    alive = []
    for ref, method in _listeners:
        obj = ref()
        if obj is None:
            continue
        try:
            if method is None:
                obj.setStyleSheet(stylesheet(t))
            else:
                getattr(obj, method)(t)
            alive.append((ref, method))
        except RuntimeError:  # its Qt widget is gone
            pass
    _listeners[:] = alive
    for fn in list(_callbacks):
        try:
            fn(t)
        except RuntimeError:
            pass


def apply_viewport(t=None):
    """The guide and overlay colors the viewport draws with (needs the plugin)."""
    t = t or current()
    try:
        from maya import cmds
        if not cmds.pluginInfo("GroomLab", query=True, loaded=True):
            return False
    except (ImportError, RuntimeError):
        return False

    def one(key, fallback):
        return "%g %g %g" % tuple(t.linear(t.get("viewport." + key, fallback)))

    v = "viewport."
    cmds.groomLabDisplayTheme(
        pointSize=float(t.get(v + "pointSize", 6)),
        selected=one("selected", "white"), softSelected=one("softSelected", "yellow"),
        frozen=one("frozen", "blue"), parts=one("parts", "red"), flow=one("flow", "yellow"),
        flowField=one("flowField", "#8FA3BF"), tubes=one("tubes", "#8A93A0"), instances=one("instances", "white"),
        clumpCenters=one("clumpCenters", "#FF4FD8"))
    return True


_scale = [None]


def scale():
    """Maya's interface scale (Preferences > Interface > UI Scaling, or the
    display's), e.g. 1.5 at 150%: every pixel size in GroomLab's windows is
    multiplied by it, as Maya's own UI is."""
    if _scale[0] is None:
        value = 1.0
        try:
            from maya import OpenMayaUI
            value = float(OpenMayaUI.MQtUtil.dpiScale(1.0))
        except (ImportError, AttributeError, RuntimeError, TypeError):
            try:
                from maya import cmds
                value = float(cmds.mayaDpiSetting(query=True, realScaleValue=True))
            except (ImportError, AttributeError, RuntimeError, TypeError):
                value = 1.0
        _scale[0] = value if value > 0.0 else 1.0
    return _scale[0]


def px(value):
    """A size in pixels at 100%, scaled to Maya's interface scale."""
    return int(round(value * scale()))


def slider_accent():
    """The next slider's color name: sliders cycle through the theme's list."""
    names = current().get("ui.sliders", ["accent"]) or ["accent"]
    _slider_count[0] += 1
    return names[_slider_count[0] % len(names)]


def style(widget):
    """Style a GroomLab window with the active theme, now and whenever it changes."""
    from groomlab.ui.qt import QtCore
    widget.setAttribute(QtCore.Qt.WA_StyledBackground, True)  # a QWidget subclass paints its background only so
    widget.setStyleSheet(stylesheet())
    if not getattr(widget, "_groomlab_themed", False):
        widget._groomlab_themed = True
        _listeners.append((weakref.ref(widget), None))  # None: restyle it


def _arrow_icon(color):
    """A small chevron in `color` for combo boxes (a styled drop-down hides Qt's own):
    an SVG written once per color next to the user's prefs, as a style-sheet url."""
    import tempfile
    folder = os.path.join(tempfile.gettempdir(), "groomlab_ui")
    path = os.path.join(folder, "arrow_%s.svg" % color.lstrip("#"))
    if not os.path.exists(path):
        try:
            if not os.path.isdir(folder):
                os.makedirs(folder)
            with open(path, "w", encoding="utf-8") as f:
                f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">'
                        '<path d="M2 3.5 L5 6.5 L8 3.5" fill="none" stroke="%s" stroke-width="1.6" '
                        'stroke-linecap="round" stroke-linejoin="round"/></svg>' % color)
        except (IOError, OSError):
            return ""
    return path.replace("\\", "/")


def _lighter(color, f):
    """Hex color blended toward white by f (0-1): a hover shade of the accent."""
    h = color.lstrip("#")
    rgb = [int(h[i:i + 2], 16) for i in (0, 2, 4)]
    return "#%02X%02X%02X" % tuple(int(round(c + (255 - c) * f)) for c in rgb)


def stylesheet(t=None):
    """Qt style sheet for GroomLab's windows (made once per loaded theme)."""
    t = t or current()
    cached = getattr(t, "_sheet", None)
    if cached is not None:
        return cached
    t._sheet = _stylesheet(t)
    return t._sheet


def _outline_rules(t):
    """With ui.outline, thin borders where the flat themes have none (buttons, chips, lists).
    Tabs are underlined, tool tiles bordered only when checked and sections ruled in every theme."""
    if not t.get("ui.outline"):
        return ""
    return """
QPushButton, QToolButton[role="chip"], QFrame[role="empty"],
QGroupBox, QListWidget, QTreeWidget, QTableWidget, QListView, QTreeView, QLabel[role="status"] {
    border: {{1}}px solid %(outline)s; }
QPushButton:hover, QToolButton[role="chip"]:hover { border-color: %(hover)s; }
QPushButton:checked, QPushButton:pressed, QPushButton[role="primary"] { border-color: %(accent)s; }
QPushButton[role="step"] { border: none; }
QPushButton[role="section"], QPushButton[role="section"]:hover, QPushButton[role="section"]:checked,
QPushButton[role="section"]:pressed { border: none; border-top: {{1}}px solid %(border)s; }
""" % {"border": t.color("ui.border"), "outline": t.hex(t.get("ui.outline")), "accent": t.hex("accent"), "hover": _lighter(t.color("ui.hover"), 0.08)}


def _row_rules(t):
    """With ui.rowHeight, every field, combo box and button one row tall (their padding then
    only sideways)."""
    row = t.get("ui.rowHeight")
    if not row:
        return ""
    # min-height leaves out the borders: the fields' 1px, the buttons' only with ui.outline
    inner = max(px(12), px(float(row)) - 2 * px(1))
    button = max(px(12), px(float(row)) - (2 * px(1) if t.get("ui.outline") else 0))
    return """
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QComboBox:editable {
    min-height: %(inner)dpx; padding-top: 0; padding-bottom: 0; }
QPushButton, QPushButton[role="primary"] { min-height: %(button)dpx; padding-top: 0; padding-bottom: 0; }
QComboBox QLineEdit { min-height: 0; }
QPushButton[role="section"], QPushButton[role="section"]:checked, QPushButton[role="section"]:pressed {
    padding-top: 0; padding-bottom: 0; }
QPushButton[role="step"] { min-height: 0; padding-top: {{4}}px; padding-bottom: {{4}}px; }
""" % {"inner": inner, "button": button}


def _stylesheet(t):
    c = t.color
    r = px(int(t.get("ui.radius", 10)))  # controls: buttons, fields, tool tiles
    small = max(px(2), r - px(2))         # small parts: chips, list rows, menu items
    panel = r + px(2)                     # panels: lists, group boxes, the empty state
    fs = px(float(t.get("ui.fontSize", 12)))
    colors = {name: t.hex(name) for name in (t.get("palette") or {})}
    colors["accent"] = t.hex("accent")
    slider_rules = "\n".join(
        'QSlider[accent="%s"]::sub-page:horizontal { background: %s; border-radius: {{3}}px; }' % (name, value)
        for name, value in colors.items())
    sheet = """
QWidget { background: %(bg)s; color: %(text)s; font-size: %(fs)dpx; }
QLabel { background: transparent; }
QLabel[role="heading"] { color: %(muted)s; font-size: %(fs_small)dpx; font-weight: bold; letter-spacing: {{1}}px; padding-top: {{6}}px; }
QLabel[role="heading"][rule="true"] { border-top: {{1}}px solid %(border)s; padding-top: {{8}}px; }
QLabel[role="about"] { color: %(dim)s; }
QLabel[role="hint"] { color: %(dim)s; font-size: %(fs_small)dpx; }
QLabel[role="title"] { font-size: %(fs_title)dpx; font-weight: bold; }
QLabel[role="subheading"] { color: %(muted)s; font-size: %(fs_small)dpx; font-weight: bold; letter-spacing: {{1}}px; }
QLabel[role="status"] { color: %(warning)s; background: %(panel)s; border-radius: %(small)dpx; padding: {{6}}px {{10}}px; }
QPushButton { background: %(raised)s; border: none; border-radius: %(r)dpx; padding: {{6}}px {{10}}px; }
QPushButton:hover { background: %(hover)s; }
QPushButton:pressed, QPushButton:checked { background: %(accent)s; color: %(accentText)s; }
QPushButton:disabled { color: %(dim)s; }
QPushButton[role="primary"] { background: %(accent)s; color: %(accentText)s; font-weight: bold; padding: {{8}}px {{14}}px; }
QPushButton[role="primary"]:hover { background: %(accentHover)s; }
QFrame[role="empty"] { background: %(panel)s; border-radius: %(panel_r)dpx; }
QFrame[role="empty"] QLabel { background: transparent; }
QFrame[role="segments"] { background: transparent; border: none; border-bottom: {{1}}px solid %(border)s; border-radius: 0; }
QWidget[role="options"] { background: transparent; }
QToolButton[role="glyph"], QToolButton[role="glyph"]:checked { background: transparent; }
QToolButton[role="glyph"]:hover { background: %(hover)s; }
QToolButton[role="glyph"]::menu-indicator { image: none; width: 0; }
QPushButton[role="step"] { background: transparent; text-align: left; padding: {{4}}px {{4}}px; }
QPushButton[role="step"]:hover { background: %(hover)s; }
QPushButton[role="step"][done="true"] { color: %(dim)s; }
QToolButton[role="chip"] { background: %(raised)s; border-radius: %(small)dpx; padding: {{4}}px {{10}}px; }
QToolButton[role="chip"]:hover { background: %(hover)s; }
QToolButton[role="chip"]::menu-indicator { image: none; width: 0; }
QPushButton::menu-indicator, QToolBar QToolButton::menu-indicator { image: none; width: 0; }
QPushButton[role="section"], QPushButton[role="section"]:checked, QPushButton[role="section"]:pressed {
    background: transparent; border: none; border-top: {{1}}px solid %(border)s; border-radius: 0; color: %(muted)s;
    font-size: %(fs_small)dpx; font-weight: bold; letter-spacing: {{1}}px; text-align: left; padding: {{6}}px 0; }
QPushButton[role="section"]:hover { color: %(dim)s; background: transparent; }
QToolButton { background: transparent; border: none; border-radius: %(r)dpx; padding: {{3}}px; }
QToolButton:hover { background: %(hover)s; }
QToolButton:checked { background: %(accent)s; color: %(accentText)s; }
QToolButton[role="tool"] { background: transparent; border: {{1}}px solid transparent; padding: {{4}}px; }
QToolButton:disabled { color: %(dim)s; }
QToolButton[role="tool"]:hover { background: %(hover)s; }
QToolButton[role="tool"]:pressed { background: %(selection)s; }
QToolButton[role="tool"]:checked { background: %(selection)s; border-color: %(accent)s; color: %(text)s; }
QTabWidget::pane { border: none; background: transparent; }
QTabWidget, QTabBar { background: transparent; }
QTabWidget > QWidget, QScrollArea > QWidget > QWidget { background: %(bg)s; }
QFrame[role="card"] { background: transparent; border: none; }
QFrame[role="card"] QLabel, QFrame[role="card"] QCheckBox { background: transparent; }
QTabBar::tab { background: transparent; color: %(dim)s; border: none; border-bottom: {{2}}px solid transparent; border-radius: 0;
    padding: {{5}}px 0; margin: 0 {{12}}px 0 0; }
QTabBar::tab:hover { background: transparent; color: %(text)s; }
QTabBar::tab:selected { background: transparent; color: %(text)s; border-bottom-color: %(accent)s; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
    background: %(panel)s; border: {{1}}px solid %(border)s; border-radius: %(r)dpx; padding: {{3}}px {{6}}px;
    selection-background-color: %(selection)s; selection-color: %(selectionText)s; }
QComboBox { padding-right: {{22}}px; }
QComboBox:editable { min-height: {{24}}px; }  /* room for its field's text, descenders included */
QComboBox QLineEdit { padding: 0; border: none; background: transparent; }  /* an editable box's field: the box pads it */
QComboBox::drop-down { border: none; width: {{18}}px; }
QComboBox::down-arrow { image: url(%(arrow)s); width: {{10}}px; height: {{10}}px; }
QComboBox QAbstractItemView { background: %(panel)s; border: {{1}}px solid %(border)s; selection-background-color: %(selection)s;
    selection-color: %(selectionText)s; }
QListWidget, QTreeWidget, QTableWidget, QListView, QTreeView {
    background: %(panel)s; border: none; border-radius: %(panel_r)dpx; padding: {{4}}px; outline: none; }
QListWidget::item { border-radius: %(small)dpx; padding: {{3}}px; }
QTreeWidget::item { border-radius: 0; padding: {{3}}px; }
QTreeWidget::branch:selected { background: %(selection)s; }
QListWidget::item:selected, QTreeWidget::item:selected { background: %(selection)s; color: %(selectionText)s; }
QListWidget::item:hover, QTreeWidget::item:hover { background: %(hover)s; }
QTreeView { show-decoration-selected: 1; }
QTreeView::branch { background: transparent; }
QTreeView::branch:selected { background: %(selection)s; }
QHeaderView::section { background: %(panel)s; color: %(dim)s; border: none; padding: {{4}}px; }
QGroupBox { background: %(panel)s; border: none; border-radius: %(panel_r)dpx; margin-top: {{20}}px; padding: {{12}}px {{10}}px {{10}}px {{10}}px; }
QGroupBox QLineEdit, QGroupBox QSpinBox, QGroupBox QDoubleSpinBox, QGroupBox QComboBox, QGroupBox QListWidget {
    background: %(raised)s; border-color: %(raised)s; }
QGroupBox::title { subcontrol-origin: margin; left: {{8}}px; padding: 0 {{2}}px; color: %(dim)s; }
QCheckBox { background: transparent; spacing: {{6}}px; }
QCheckBox::indicator { width: {{16}}px; height: {{16}}px; border-radius: %(check)dpx; background: %(raised)s; border: none; }
QCheckBox::indicator:checked { background: %(accent)s; }
QSlider::groove:horizontal { height: {{6}}px; border-radius: {{3}}px; background: %(raised)s; }
QSlider::sub-page:horizontal { background: %(accent)s; border-radius: {{3}}px; }
QGroupBox QSlider::groove:horizontal { background: %(bg)s; }
QSlider::handle:horizontal { background: %(white)s; width: {{14}}px; height: {{14}}px; margin: {{-4}}px 0; border-radius: {{7}}px; }
%(slider_rules)s
QSlider::sub-page:horizontal:disabled, QSlider[accent]::sub-page:horizontal:disabled { background: %(hover)s; }
QSlider::handle:horizontal:disabled { background: %(dim)s; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled, QCheckBox:disabled { color: %(dim)s; }
QCheckBox::indicator:checked:disabled { background: %(hover)s; }
QScrollBar:vertical { background: %(raised)s; width: {{12}}px; margin: 0; border-radius: {{6}}px; }
QScrollBar::handle:vertical { background: %(hover)s; border-radius: {{6}}px; min-height: {{32}}px; margin: {{1}}px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QScrollBar:horizontal { background: %(raised)s; height: {{12}}px; margin: 0; border-radius: {{6}}px; }
QScrollBar::handle:horizontal { background: %(hover)s; border-radius: {{6}}px; min-width: {{32}}px; margin: {{1}}px; }
QScrollBar::handle:hover { background: %(dim)s; }
QScrollBar::handle:pressed { background: %(accent)s; }
QMenu { background: %(panel)s; border: {{1}}px solid %(border)s; border-radius: %(small)dpx; padding: {{4}}px; }
QMenu::item { padding: {{5}}px {{18}}px; border-radius: %(small)dpx; }
QMenu::item:selected { background: %(selection)s; color: %(selectionText)s; }
QToolBar { background: %(bg)s; border: none; spacing: {{4}}px; }
QSplitter::handle { background: %(bg)s; }
QScrollArea { border: none; }
%(outline_rules)s%(row_rules)s""" % {
        "bg": c("ui.background"), "panel": c("ui.panel"), "raised": c("ui.raised"), "hover": c("ui.hover"),
        "border": c("ui.border"), "text": c("ui.text"), "dim": c("ui.textDim"), "accent": t.hex("accent"),
        "muted": c("ui.textMuted") if t.get("ui.textMuted") else c("ui.textDim"),
        "accentText": c("ui.accentText"), "warning": t.hex(t.get("ui.warning", "yellow")),
        "white": t.hex("white"), "r": r, "small": small, "panel_r": panel, "check": max(px(3), min(px(8), r // 2)), "fs": fs,
        "fs_small": max(px(9), fs - px(2)), "fs_title": fs + px(3),
        "accentHover": _lighter(t.hex("accent"), 0.12), "slider_rules": slider_rules, "arrow": _arrow_icon(c("ui.textDim")),
        "selection": t.hex(t.get("ui.selection") or "accent"),
        "selectionText": c("ui.selectionText") if t.get("ui.selectionText") else c("ui.accentText"),
        "outline_rules": _outline_rules(t), "row_rules": _row_rules(t)}
    # Fixed sizes ({{N}}px in the template) at Maya's interface scale.
    return re.sub(r"\{\{(-?\d+(?:\.\d+)?)\}\}px", lambda m: "%dpx" % round(float(m.group(1)) * scale()), sheet)
