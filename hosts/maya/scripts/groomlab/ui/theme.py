# SPDX-License-Identifier: Apache-2.0
"""GroomLab UI themes: colors and shapes for the Groom Tools, the Graph Editor
and the viewport's guides, from JSON files.

Four ship with GroomLab: "charcoal" (graphite greys with sections on soft
cards and one iris-violet accent; the default), "playful" (charcoal and
primaries, round; the base every theme extends), "studio" (low-key DCC greys,
one steel-blue accent) and "paper" (light: warm paper and ink with one deep
teal, for bright rooms and reading the groom against light). Pick one in GroomLab >
Theme, or Theme in the Groom Tools' More menu or the Graph Editor's Edit menu
(a pick is remembered: a user who made one keeps it). Make your own:

    from groomlab.ui import theme
    path = theme.create_theme("mine")      # a copy of the current theme, to edit
    theme.set_theme("mine")                # after editing: applies everywhere at once
    theme.list_themes()                    # ["charcoal", "mine", "paper", "playful", "studio"]

Themes are looked up, in order, in every directory on GROOMLAB_THEME_PATH,
the user's folder (<Maya user app dir>/groomlab/themes) and the shipped
folder. A theme names colors as hex ("#2F7BF6"), by its palette ("blue"), or
"accent"; it may set only some keys: the rest come from the theme it
"extends" (default "playful"). Optional ui keys: "outline" (a thin border
around buttons, cards and lists; none by default), "selection" and
"selectionText" (selected rows, menu items and the active tool; the accent by
default), "textMuted" (the quietest text: section headers; textDim by
default), "rowHeight" (the height of fields, combo boxes and buttons; their
text's by default), "font" (the font family of GroomLab's windows, "Inter"
in the shipped themes: loaded from the module's fonts folder; Maya's own font
when that family can't be had) and "glass" (the standalone app's translucent
panels, true by default; false: solid panels with a thin border). Graph keys:
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
# The fonts GroomLab's windows use (Inter, SIL OFL 1.1, its license beside it): the installed module's
# fonts folder, else the source tree's (hosts/app/fonts, the app's own) for runs from a checkout.
FONT_DIRS = (os.path.normpath(os.path.join(SHIPPED_DIR, os.pardir, "fonts")),
             os.path.normpath(os.path.join(SHIPPED_DIR, os.pardir, os.pardir, "app", "fonts")))

_current = None
_listeners = []  # (weakref to object, method name)
_slider_count = [0]
_font_families = []  # the families the shipped fonts gave (loaded once, with a QApplication)
_fonts_loaded = [False]


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

    # -- The derived tokens (charcoal.json "derived", as the app's theme.cpp makes them) ---------

    def is_light(self):
        """A light theme: its ui.panel's luminance over 0.5 (the contrast steps then go toward dark)."""
        h = self.color("ui.panel", "#000000").lstrip("#")
        r, g, b = [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
        return 0.2126 * r + 0.7152 * g + 0.0722 * b > 0.5

    def _own(self, key):
        """The theme's own ui.<key> as a hex color, or None (a key it leaves out, or null)."""
        value = self.get("ui." + key)
        if value is None:
            return None
        color = self.hex(value, default=None)
        return color

    def token(self, name):
        """A component token as "#RRGGBB": the theme's own ui key when it sets one, else made from
        its base tones by the formula every host uses (card, sunken, accentSoft, accentInk, cardRule,
        accentEdge, accentFill, textSoft, textDim, textMuted, textDisabled, segmentOn, sliderFill);
        any other ui key or palette name as is."""
        if name in _DERIVED:
            if name not in ("textDim", "accentEdge"):  # (the app's dim text and the chosen edge: always made)
                own = self._own(name)
                if own:
                    return own
            return _DERIVED[name](self)
        own = self._own(name)
        return own if own else self.hex(name)

    def role(self, name, fallback="#FF00FF"):
        """A color a component names: a derived token, a ui key, a palette name or "#RRGGBB"."""
        if not isinstance(name, str):
            return fallback
        if name.startswith("#"):
            return name
        if name in _DERIVED or self.get("ui." + name) is not None:
            return self.token(name)
        return self.hex(name, fallback)


def _ui(t, key, fallback="#808080"):
    return t.color("ui." + key, fallback)


def _accent(t):
    return t.hex("accent")


# The derived tokens' formulas (charcoal.json "derived"; mix(a, b, t) = a toward b by t).
_DERIVED = {
    "card": lambda t: _blend(_ui(t, "panel"), _ui(t, "raised"), 0.45),
    "accentSoft": lambda t: _blend(t.token("card"), _accent(t), 0.16 if t.is_light() else 0.22),
    "accentInk": lambda t: _blend(_accent(t), _ui(t, "text"), 0.45),
    "cardRule": lambda t: _blend(t.token("card"), _ui(t, "hover"), 0.6),
    "accentEdge": lambda t: _blend(t.token("accentSoft"), _accent(t), 0.45),
    "accentFill": _accent,
    "textSoft": lambda t: _blend(_ui(t, "text"), _ui(t, "textDim"), 0.33),
    # The app's dim text (its textDim()): the theme's textDim a third of the way back toward the text.
    "textDim": lambda t: _blend(_ui(t, "text"), _ui(t, "textDim"), 0.66),
    "textMuted": lambda t: _ui(t, "textDim"),
    "textDisabled": lambda t: t.token("textMuted"),
    "sunken": lambda t: (_blend(_ui(t, "panel"), _ui(t, "text"), 0.07) if t.is_light()
                         else _blend(_ui(t, "background"), _ui(t, "panel"), 0.3)),
    "segmentOn": lambda t: "#FFFFFF" if t.is_light() else _blend(_ui(t, "hover"), _ui(t, "text"), 0.08),
    # A fill field's filled part: the field a step toward the accent.
    "sliderFill": lambda t: _blend(_ui(t, "raised"), _accent(t), 0.18),
}


_components = [None]


def components():
    """The components block (sizes and color roles of the parts every host draws): Charcoal's,
    whatever the theme (as the app reads it)."""
    if _components[0] is None:
        try:
            _components[0] = load(DEFAULT).get("components") or {}
        except (ValueError, IOError, OSError):
            _components[0] = {}
    return _components[0]


def component(name, key, default=None):
    """A component's value (components.<name>.<key>), or `default`."""
    part = components().get(name)
    if not isinstance(part, dict) or key not in part:
        return default
    return part[key]


def component_px(name, key, default, index=None):
    """A component's size in pixels at Maya's interface scale (`index`: one of a list's)."""
    value = component(name, key, default)
    if index is not None:
        value = value[index] if isinstance(value, (list, tuple)) and len(value) > index else default
    try:
        return px(float(value))
    except (TypeError, ValueError):
        return px(default)


def component_color(name, key, fallback=None, t=None):
    """A component's color in the theme (its role resolved: a derived token, a ui key...)."""
    t = t or current()
    value = component(name, key)
    if value is None:
        return t.role(fallback) if fallback else "#FF00FF"
    return t.role(value, t.role(fallback) if fallback else "#FF00FF")


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
    _components[0] = None
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
                obj.setStyleSheet(stylesheet(t, surface=getattr(obj, "_groomlab_surface", None)))
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


def _load_fonts():
    """The shipped fonts added to this Qt's (Maya's own windows keep their font: only GroomLab's
    name the family), once; [] until there is a QApplication (tried again then)."""
    if _fonts_loaded[0]:
        return _font_families
    try:
        from groomlab.ui.qt import QtGui
    except ImportError:
        return []
    if QtGui.QGuiApplication.instance() is None:
        return []
    _fonts_loaded[0] = True
    for folder in FONT_DIRS:
        try:
            names = sorted(os.listdir(folder))
        except OSError:
            continue
        loaded = False
        for name in names:
            if not name.lower().endswith((".ttf", ".otf")):
                continue
            font_id = QtGui.QFontDatabase.addApplicationFont(os.path.join(folder, name))
            if font_id < 0:
                continue
            loaded = True
            for family in QtGui.QFontDatabase.applicationFontFamilies(font_id):
                if family not in _font_families:
                    _font_families.append(family)
        if loaded:  # the installed module's: the source tree's is only for runs from a checkout
            break
    return _font_families


def ui_font(t=None):
    """The font family GroomLab's windows and the Graph Editor's cards use: the theme's ui.font
    ("Inter") when it loaded from the shipped fonts or the system has it, else None (Maya's font)."""
    t = t or current()
    family = t.get("ui.font")
    if not family or not isinstance(family, str):
        return None
    if family in _load_fonts():
        return family
    if not _fonts_loaded[0]:
        return None  # no QApplication yet
    from groomlab.ui.qt import QtGui
    return family if QtGui.QFontInfo(QtGui.QFont(family)).family().lower() == family.lower() else None


def tabular_digits(f):
    """Fixed-width digits (OpenType tnum) on a QFont, so a value that changes keeps its width:
    QFont.setFeature is Qt 6.7 and later (Maya 2027); before, the font's own digits."""
    from groomlab.ui.qt import QtGui
    tag = getattr(QtGui.QFont, "Tag", None)
    if tag is not None and hasattr(f, "setFeature"):
        try:
            f.setFeature(tag("tnum"), 1)
        except (TypeError, ValueError, AttributeError):
            pass
    return f


def font(pixels, bold=False, tabular=False, base=None):
    """A QFont in the UI family (ui_font) at a pixel size; `tabular`: fixed-width digits, for numbers."""
    from groomlab.ui.qt import QtGui
    f = QtGui.QFont(base) if base is not None else QtGui.QFont()
    family = ui_font()
    if family:
        f.setFamily(family)
    f.setPixelSize(max(1, int(round(pixels))))
    f.setBold(bold)
    return tabular_digits(f) if tabular else f


def slider_accent():
    """The next slider's color name: sliders cycle through the theme's list."""
    names = current().get("ui.sliders", ["accent"]) or ["accent"]
    _slider_count[0] += 1
    return names[_slider_count[0] % len(names)]


def style(widget, surface=None):
    """Style a GroomLab window with the active theme, now and whenever it changes. `surface`: the
    ui key its background takes ("panel": a side panel whose sections sit on cards, as the Groom
    Tools; the theme's background by default)."""
    from groomlab.ui.qt import QtCore
    widget.setAttribute(QtCore.Qt.WA_StyledBackground, True)  # a QWidget subclass paints its background only so
    widget._groomlab_surface = surface
    widget.setStyleSheet(stylesheet(surface=surface))
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


def _blend(a, b, f):
    """Hex color a moved toward b by f (0-1): the blocks' faint hairline."""
    ha, hb = a.lstrip("#"), b.lstrip("#")
    if len(ha) < 6 or len(hb) < 6:
        return a
    ra = [int(ha[i:i + 2], 16) for i in (0, 2, 4)]
    rb = [int(hb[i:i + 2], 16) for i in (0, 2, 4)]
    return "#%02X%02X%02X" % tuple(int(round(x + (y - x) * f)) for x, y in zip(ra, rb))


def stylesheet(t=None, surface=None):
    """Qt style sheet for GroomLab's windows (made once per loaded theme and surface: the ui key
    a window's background takes, the theme's background by default)."""
    t = t or current()
    cache = t.__dict__.setdefault("_sheets", {})
    cached = cache.get(surface)
    if cached is None:
        cached = cache[surface] = _stylesheet(t, surface)
    return cached


def _outline_rules(t):
    """With ui.outline, a thin edge around the boxed lists (the fields, chips and buttons stay
    flat, as the app's: a theme's ui.fieldEdge draws theirs)."""
    if not t.get("ui.outline"):
        return ""
    return """
QListWidget, QTreeWidget, QTableWidget, QListView, QTreeView { border: {{1}}px solid %(outline)s; }
QListWidget[role="rows"], QTreeWidget[role="rows"] { border: none; }
""" % {"outline": t.hex(t.get("ui.outline"))}


def _row_rules(t):
    """With ui.rowHeight, every field, combo box and button one row tall (their padding then
    only sideways)."""
    row = t.get("ui.rowHeight")
    if not row:
        return ""
    # min-height leaves out the borders (1px each side)
    inner = max(px(12), px(float(row)) - 2 * px(1))
    return """
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QComboBox:editable {
    min-height: %(inner)dpx; padding-top: 0; padding-bottom: 0; }
QPushButton, QPushButton[role="primary"], QPushButton[role="disclosure"] { min-height: %(inner)dpx; padding-top: 0; padding-bottom: 0; }
QComboBox QLineEdit { min-height: 0; }
QFrame[role="propField"] QComboBox, QFrame[role="propField"] QLineEdit, QFrame[role="propField"] QSpinBox,
QFrame[role="propField"] QDoubleSpinBox, QFrame[role="propField"] QPushButton { min-height: %(nested)dpx; }
QPushButton[role="section"], QPushButton[role="section"]:checked, QPushButton[role="section"]:pressed {
    min-height: 0; padding-top: {{6}}px; padding-bottom: {{2}}px; }
QPushButton[role="step"] { min-height: 0; padding-top: {{3}}px; padding-bottom: {{3}}px; }
""" % {"inner": inner, "nested": max(px(12), inner - 4 * px(1))}


def _check_icon(color):
    """A check mark in `color` for checked boxes: an SVG written once per color, as a style-sheet url."""
    import tempfile
    folder = os.path.join(tempfile.gettempdir(), "groomlab_ui")
    path = os.path.join(folder, "check_%s.svg" % color.lstrip("#"))
    if not os.path.exists(path):
        try:
            if not os.path.isdir(folder):
                os.makedirs(folder)
            with open(path, "w", encoding="utf-8") as f:
                f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 14 14">'
                        '<path d="M3.4 7.3 L5.9 9.7 L10.6 4.6" fill="none" stroke="%s" stroke-width="1.8" '
                        'stroke-linecap="round" stroke-linejoin="round"/></svg>' % color)
        except (IOError, OSError):
            return ""
    return path.replace("\\", "/")


def tokens(t=None, surface=None):
    """{name: "#RRGGBB" or a size in pixels}: what the style sheet is made from (the app's sheet's
    tokens, hosts/app/src/theme.cpp)."""
    t = t or current()
    c = t.color
    light = t.is_light()
    outline = t.get("ui.outline")
    field_edge = t.get("ui.fieldEdge")
    popup_edge = t.get("ui.popupEdge")
    accent = t.hex("accent")
    radius = int(t.get("ui.radius", 10))
    r = px(radius)          # controls: buttons, fields, tool tiles
    fs = px(float(t.get("ui.fontSize", 12)))
    return {
        "bg": c("ui." + surface) if surface else c("ui.background"), "background": c("ui.background"),
        "panel": c("ui.panel"), "raised": c("ui.raised"), "hover": c("ui.hover"), "border": c("ui.border"),
        "edge": t.hex(outline) if outline else c("ui.border"), "text": c("ui.text"), "soft": t.token("textSoft"),
        "dim": t.token("textDim"), "muted": t.token("textMuted"), "disabled": t.token("textDisabled"),
        "accent": accent, "accentText": c("ui.accentText"), "accentFill": t.token("accentFill"),
        "accentSoft": t.token("accentSoft"), "accentInk": t.token("accentInk"), "accentEdge": t.token("accentEdge"),
        "card": t.token("card"), "cardRule": t.token("cardRule"), "sunken": t.token("sunken"),
        "segOn": t.token("segmentOn"), "sliderFill": t.token("sliderFill"),
        "selection": t.hex(t.get("ui.selection") or "accent"),
        "selectionText": c("ui.selectionText") if t.get("ui.selectionText") else c("ui.accentText"),
        "warning": t.hex(t.get("ui.warning", "yellow")), "white": t.hex("white"),
        "fieldEdge": t.hex(field_edge) if field_edge else "transparent",
        "popupEdge": t.hex(popup_edge) if popup_edge else _blend(c("ui.raised"), c("ui.text"), 0.18),
        "hoverEdge": _blend(c("ui.hover"), c("ui.text"), 0.12),
        "checkEdge": (t.hex(t.get("ui.checkEdge")) if light and t.get("ui.checkEdge")
                      else _blend(c("ui.hover"), t.token("textMuted"), 0.8)),
        "checkOn": t.token("accentFill") if light else t.token("accentSoft"),
        "checkOnEdge": t.token("accentFill") if light else accent,
        "scroll": _blend(c("ui.hover"), c("ui.text"), 0.25), "scrollHover": _blend(c("ui.hover"), c("ui.text"), 0.45),
        "railFill": component_color("iconRail", "fill", "panel", t),
        "railEdge": component_color("iconRail", "edge", "cardRule", t),
        "railHover": component_color("iconRail", "hover", "hover", t),
        "railOn": component_color("iconRail", "on", "accentSoft", t),
        "r": r, "small": max(px(2), r - px(2)), "panel_r": r + px(2),
        "card_r": px(int(t.get("ui.cardRadius") or radius + 2)),
        "check": px(max(0, min(7, int(t.get("ui.checkRadius") or max(2, radius - 2))))),
        "seg_r": component_px("segTrack", "radius", 8), "seg_on_r": component_px("segTrack", "segmentRadius", 6),
        "rail_r": component_px("iconRail", "radius", 8),
        "fs": fs, "fs_small": max(px(9), fs - px(1)), "fs_tiny": max(px(9), fs - px(1)),
        "fs_title": fs + px(3), "fs_card": fs + px(1),
    }


def _stylesheet(t, surface=None):
    k = tokens(t, surface)
    family = ui_font(t)
    colors = {name: t.hex(name) for name in (t.get("palette") or {})}
    colors["accent"] = t.hex("accent")
    slider_rules = "\n".join(
        'QSlider[accent="%s"]::sub-page:horizontal { background: %s; border-radius: {{3}}px; }' % (name, value)
        for name, value in colors.items())
    # The app's style sheet (hosts/app/src/theme.cpp), the Groom Tools' roles as its: sections on
    # cards (trays) over the panel, flat tool tiles (the chosen one on the soft accent), chips on
    # the raised tone, one-of choices as segments on a sunken track, plain list rows, thin scroll bars.
    sheet = """
QWidget { background: %(bg)s; color: %(text)s; %(family)sfont-size: %(fs)dpx; }
QLabel { background: transparent; }
QLabel[role="heading"] { color: %(muted)s; font-size: %(fs_small)dpx; font-weight: 600; padding-top: {{6}}px; }
QLabel[role="heading"][rule="true"] { border-top: {{1}}px solid %(edge)s; padding-top: {{8}}px; }
QLabel[role="category"] { color: %(dim)s; font-size: %(fs_small)dpx; font-weight: 600; padding: 0 0 {{2}}px {{2}}px; }
QLabel[role="about"] { color: %(dim)s; }
QLabel[role="hint"] { color: %(muted)s; font-size: %(fs_small)dpx; }
QLabel[role="title"] { color: %(text)s; font-size: %(fs_title)dpx; font-weight: 700; }
QLabel[role="subheading"] { color: %(text)s; font-size: %(fs)dpx; font-weight: 600; }
QLabel[role="status"] { color: %(warning)s; background: %(raised)s; border-radius: %(r)dpx; padding: {{5}}px {{8}}px; }
QLabel[role="count"] { color: %(muted)s; font-size: %(fs_small)dpx; }
QLabel[role="block"] { color: %(dim)s; font-size: %(fs_small)dpx; font-weight: 600; }
QLabel[role="muted"] { color: %(muted)s; font-size: %(fs_small)dpx; }
QLabel[role="rowLabel"] { color: %(dim)s; }
QLabel[role="toolTitle"] { color: %(text)s; font-size: %(fs_card)dpx; font-weight: 600; }
QLabel[role="toolHint"] { color: %(dim)s; padding-bottom: {{4}}px; }
QLabel[role="noTool"] { background: %(card)s; border-radius: %(card_r)dpx; padding: {{10}}px {{12}}px; color: %(dim)s; }
QLabel[role="emptyTitle"] { color: %(text)s; font-size: %(fs_card)dpx; font-weight: 700; }
QLabel[role="emptyText"] { color: %(dim)s; font-size: %(fs_small)dpx; }
QPushButton { background: %(raised)s; border: {{1}}px solid %(fieldEdge)s; border-radius: %(r)dpx; padding: {{4}}px {{12}}px; font-weight: 500; }
QPushButton:hover { background: %(hover)s; }
QPushButton:pressed { background: %(selection)s; color: %(selectionText)s; }
QPushButton:checked { background: %(accentSoft)s; color: %(accentInk)s; border-color: %(accentSoft)s; }
QPushButton:disabled { color: %(disabled)s; background: %(panel)s; }
QPushButton[role="primary"] { background: %(accentFill)s; color: %(accentText)s; border-color: %(accentFill)s; font-weight: 600; }
QPushButton[role="primary"]:hover { background: %(accent)s; border-color: %(accent)s; }
QPushButton[role="primary"]:disabled { background: %(raised)s; color: %(disabled)s; border-color: %(raised)s; }
QPushButton[role="disclosure"] { background: transparent; border: {{1}}px solid %(hover)s; color: %(dim)s; padding: {{4}}px {{10}}px; }
QPushButton[role="disclosure"]:hover, QPushButton[role="disclosure"]:pressed { background: %(raised)s; color: %(text)s; }
QPushButton::menu-indicator, QToolBar QToolButton::menu-indicator { image: none; width: 0; }
QFrame[role="empty"], QFrame[role="card"], QFrame[role="toolCard"] { background: %(card)s; border: none; border-radius: %(card_r)dpx; }
QFrame[role="empty"] QLabel, QFrame[role="card"] QLabel, QFrame[role="card"] QCheckBox, QFrame[role="toolCard"] QLabel { background: transparent; }
QFrame[role="emptyQuiet"] { background: transparent; border: none; }
QWidget[role="tray"] { background: %(card)s; border: none; border-radius: %(card_r)dpx; }
QWidget[role="tray"] QLabel, QWidget[role="tray"] QCheckBox { background: transparent; }
/* (a plain QWidget takes the window's background from the first rule: inside a card, the card's) */
QWidget[role="tray"] .QWidget, QFrame[role="toolCard"] .QWidget, QFrame[role="empty"] .QWidget, QFrame[role="card"] .QWidget,
QWidget[role="segTrack"] .QWidget, QFrame[role="propField"] .QWidget { background: transparent; }
QWidget[role="tray"] QWidget[role="segTrack"], QFrame[role="toolCard"] QWidget[role="segTrack"],
QFrame[role="itemBox"] QWidget[role="segTrack"] { background: %(sunken)s; }
QWidget[role="options"] { background: transparent; }
QFrame[role="segments"] { background: transparent; border: none; }
QFrame[role="rule"] { background: %(edge)s; border: none; }
QFrame[role="ruleFaint"] { background: %(cardRule)s; border: none; }
QFrame[role="itemBox"] { background: transparent; border: none; border-top: {{1}}px solid %(cardRule)s; border-radius: 0; }
QFrame[role="itemBox"] QLabel { background: transparent; }
QFrame[role="propField"] { background: %(raised)s; border: {{1}}px solid %(fieldEdge)s; border-radius: %(r)dpx; }
QFrame[role="propField"] QLabel { background: transparent; color: %(dim)s; }
QFrame[role="propField"] QLabel:disabled { color: %(disabled)s; }
QFrame[role="propField"] QComboBox, QFrame[role="propField"] QLineEdit, QFrame[role="propField"] QSpinBox,
QFrame[role="propField"] QDoubleSpinBox { background: transparent; border: {{1}}px solid transparent; }
QFrame[role="propField"] QComboBox { padding-left: 0; }
QFrame[role="propField"] QComboBox:hover, QFrame[role="propField"] QLineEdit:hover, QFrame[role="propField"] QSpinBox:hover,
QFrame[role="propField"] QDoubleSpinBox:hover { background: %(hover)s; border-color: transparent; }
QFrame[role="propField"] QComboBox:focus, QFrame[role="propField"] QLineEdit:focus, QFrame[role="propField"] QSpinBox:focus,
QFrame[role="propField"] QDoubleSpinBox:focus { border-color: %(accent)s; }
QFrame[role="propField"] QPushButton { background: %(hover)s; border: none; }
QToolButton { background: transparent; border: none; border-radius: %(r)dpx; padding: {{3}}px; }
QToolButton:hover { background: %(hover)s; }
QToolButton:checked { background: %(accentSoft)s; color: %(accentInk)s; }
QToolButton:disabled { color: %(disabled)s; }
QToolButton[role="tool"] { background: transparent; border: {{1}}px solid transparent; border-radius: %(r)dpx; padding: {{4}}px 0;
    color: %(dim)s; font-size: %(fs_tiny)dpx; font-weight: 500; }
QToolButton[role="tool"]:hover { background: %(hover)s; color: %(text)s; }
QToolButton[role="tool"]:pressed:!checked { background: %(hover)s; }
QToolButton[role="tool"]:checked { background: %(accentSoft)s; border-color: %(accentEdge)s; color: %(text)s; }
QToolButton[role="tool"]:disabled { color: %(disabled)s; }
QToolButton[role="tool"][wide="true"] { padding: {{4}}px {{10}}px; font-size: %(fs)dpx; }
QToolButton[role="chip"] { background: %(raised)s; border: {{1}}px solid %(fieldEdge)s; border-radius: %(r)dpx; padding: {{4}}px {{8}}px;
    color: %(text)s; font-weight: 500; }
QToolButton[role="chip"]:hover { background: %(hover)s; }
QToolButton[role="chip"]:pressed, QToolButton[role="chip"]:checked { background: %(accentSoft)s; color: %(accentInk)s;
    border-color: %(accentSoft)s; font-weight: 600; }
QToolButton[role="chip"]:disabled { color: %(disabled)s; }
QToolButton[role="chip"]::menu-indicator, QToolButton[role="glyph"]::menu-indicator { image: none; width: 0; }
QFrame[role="toolCard"] QToolButton[role="chip"], QFrame[role="itemBox"] QToolButton[role="chip"] {
    background: %(raised)s; border: {{1}}px solid transparent; color: %(dim)s; padding: {{3}}px {{8}}px; font-size: %(fs_small)dpx; }
QFrame[role="toolCard"] QToolButton[role="chip"]:hover, QFrame[role="itemBox"] QToolButton[role="chip"]:hover {
    background: %(hover)s; color: %(text)s; }
QFrame[role="toolCard"] QToolButton[role="chip"]:checked, QFrame[role="itemBox"] QToolButton[role="chip"]:checked {
    background: %(accentSoft)s; border-color: %(accentSoft)s; color: %(accentInk)s; font-weight: 600; }
QToolButton[role="chip"][disclosure="true"], QFrame[role="toolCard"] QToolButton[role="chip"][disclosure="true"] {
    background: transparent; border: {{1}}px solid %(hover)s; color: %(dim)s; }
QToolButton[role="chip"][disclosure="true"]:hover, QFrame[role="toolCard"] QToolButton[role="chip"][disclosure="true"]:hover {
    background: %(raised)s; color: %(text)s; }
QToolButton[role="chip"][disclosure="true"]:checked, QFrame[role="toolCard"] QToolButton[role="chip"][disclosure="true"]:checked {
    background: %(raised)s; border-color: %(dim)s; color: %(text)s; font-weight: 500; }
QWidget[role="segTrack"] { background: %(sunken)s; border-radius: %(seg_r)dpx; }
QWidget[role="segTrack"] QToolButton[role="chip"], QFrame[role="toolCard"] QWidget[role="segTrack"] QToolButton[role="chip"] {
    background: transparent; border: {{1}}px solid transparent; color: %(dim)s; border-radius: %(seg_on_r)dpx; padding: {{3}}px {{2}}px; }
QWidget[role="segTrack"] QToolButton[role="chip"]:hover, QFrame[role="toolCard"] QWidget[role="segTrack"] QToolButton[role="chip"]:hover {
    background: transparent; color: %(text)s; }
QWidget[role="segTrack"] QToolButton[role="chip"]:checked, QFrame[role="toolCard"] QWidget[role="segTrack"] QToolButton[role="chip"]:checked {
    background: %(segOn)s; border-color: %(segOn)s; color: %(text)s; font-weight: 600; }
QToolButton[role="glyph"], QToolButton[role="glyph"]:checked { background: transparent; border: none; border-radius: %(r)dpx; padding: {{2}}px; }
QToolButton[role="glyph"]:hover { background: %(hover)s; }
QToolButton[role="glyph"]:checked { background: %(selection)s; }
QToolButton[role="glyph"]:disabled, QToolButton[role="glyph"]:checked:disabled { background: transparent; }
QToolButton[role="glyph"][armed="true"] { color: %(warning)s; padding: {{2}}px {{6}}px; }
QPushButton[role="ghost"], QToolButton[role="ghost"] { background: transparent; border: {{1}}px solid transparent; color: %(soft)s;
    padding: {{2}}px {{6}}px; font-weight: 500; }
QPushButton[role="ghost"]:hover, QToolButton[role="ghost"]:hover { background: %(hover)s; color: %(text)s; }
QPushButton[role="ghost"]:disabled, QToolButton[role="ghost"]:disabled { color: %(disabled)s; background: transparent; }
QToolButton[role="ghost"]::menu-indicator, QPushButton[role="ghost"]::menu-indicator { image: none; width: 0; }
QPushButton[role="step"] { background: transparent; border: none; text-align: left; padding: {{3}}px {{4}}px; font-weight: 600; }
QPushButton[role="step"]:hover { background: %(hover)s; }
QPushButton[role="step"][done="true"] { color: %(muted)s; }
QPushButton[role="section"], QPushButton[role="section"]:checked, QPushButton[role="section"]:pressed {
    background: transparent; border: none; border-radius: 0; color: %(text)s; font-size: %(fs)dpx;
    font-weight: 600; text-align: left; padding: {{6}}px 0 {{2}}px 0; }
QPushButton[role="section"]:hover { color: %(accentInk)s; background: transparent; }
QFrame[role="sectionRow"] { background: transparent; border: none; border-radius: 0; }
QWidget[role="iconRail"] { background: %(railFill)s; border: none; border-right: {{1}}px solid %(railEdge)s; }
QToolButton[role="railButton"] { background: transparent; border: none; padding: 0; border-radius: %(rail_r)dpx; }
QToolButton[role="railButton"]:hover { background: %(railHover)s; }
QToolButton[role="railButton"]:checked { background: %(railOn)s; }
QListWidget[role="rows"], QTreeWidget[role="rows"] { background: transparent; border: none; padding: 0; }
QListWidget[role="rows"]::item { border-radius: %(small)dpx; padding: {{3}}px {{2}}px; }
QTreeWidget[role="rows"]::item { border-radius: 0; padding: {{3}}px {{2}}px; }
QListWidget[role="rows"]::item:selected, QTreeWidget[role="rows"]::item:selected { background: %(accentSoft)s; color: %(text)s; }
QListWidget[role="rows"]::item:hover:!selected, QTreeWidget[role="rows"]::item:hover:!selected { background: %(cardRule)s; }
QToolButton[role="glyph"][toggle="true"]:checked { background: %(selection)s; }
QTabWidget::pane { border: none; background: transparent; }
QTabWidget, QTabBar { background: transparent; }
QTabWidget > QWidget, QScrollArea > QWidget > QWidget { background: %(bg)s; }
QTabBar::tab { background: transparent; color: %(dim)s; border: none; border-bottom: {{2}}px solid transparent; border-radius: 0;
    padding: {{6}}px {{2}}px; margin: 0 {{12}}px 0 0; font-weight: 500; }
QTabBar::tab:hover { background: transparent; color: %(text)s; }
QTabBar::tab:selected { background: transparent; color: %(text)s; border-bottom-color: %(accent)s; font-weight: 600; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
    background: %(raised)s; border: {{1}}px solid %(fieldEdge)s; border-radius: %(r)dpx; padding: {{3}}px {{8}}px;
    selection-background-color: %(selection)s; selection-color: %(selectionText)s; }
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: %(hoverEdge)s; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border-color: %(accent)s; }
QLineEdit:read-only { color: %(muted)s; }
QComboBox { padding-right: {{22}}px; }
QComboBox:editable { min-height: {{24}}px; }  /* room for its field's text, descenders included */
QComboBox QLineEdit { padding: 0; border: none; background: transparent; }  /* an editable box's field: the box pads it */
QComboBox::drop-down { border: none; width: {{18}}px; }
QComboBox::down-arrow { image: url(%(arrow)s); width: {{10}}px; height: {{10}}px; }
QComboBox QAbstractItemView { background: %(raised)s; border: {{1}}px solid %(popupEdge)s; selection-background-color: %(selection)s;
    selection-color: %(selectionText)s; outline: none; }
QListWidget, QTreeWidget, QTableWidget, QListView, QTreeView {
    background: %(background)s; border: {{1}}px solid %(fieldEdge)s; border-radius: %(panel_r)dpx; padding: {{4}}px; outline: none; }
QListWidget::item { border-radius: %(small)dpx; padding: {{3}}px; }
QTreeWidget::item { border-radius: 0; padding: {{3}}px; }
QTreeWidget::branch:selected { background: %(selection)s; }
QListWidget::item:selected, QTreeWidget::item:selected { background: %(selection)s; color: %(selectionText)s; }
QListWidget::item:hover, QTreeWidget::item:hover { background: %(hover)s; }
QTreeView { show-decoration-selected: 1; }
QTreeView::branch { background: transparent; }
QTreeView::branch:selected { background: %(selection)s; }
QHeaderView::section { background: transparent; color: %(muted)s; border: none; padding: {{2}}px {{4}}px; font-weight: 600; }
QGroupBox { background: transparent; border: {{1}}px solid %(edge)s; border-radius: %(panel_r)dpx; margin-top: {{14}}px;
    padding: {{8}}px {{4}}px {{4}}px {{4}}px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: {{10}}px; padding: 0 {{4}}px; color: %(dim)s; }
QCheckBox { background: transparent; spacing: {{8}}px; }
QCheckBox::indicator { width: {{14}}px; height: {{14}}px; border-radius: %(check)dpx; background: %(raised)s;
    border: {{1}}px solid %(checkEdge)s; }
QCheckBox::indicator:checked { background: %(checkOn)s; border-color: %(checkOnEdge)s; image: url(%(tick)s); }
QCheckBox::indicator:disabled { background: %(panel)s; }
QCheckBox::indicator:checked:disabled { background: %(hover)s; border-color: %(hover)s; }
QSlider::groove:horizontal { height: {{6}}px; border-radius: {{3}}px; background: %(hover)s; }
QSlider::sub-page:horizontal { background: %(accent)s; border-radius: {{3}}px; }
QSlider::handle:horizontal { background: %(text)s; width: {{14}}px; height: {{14}}px; margin: {{-4}}px 0; border-radius: {{7}}px; }
%(slider_rules)s
QSlider::sub-page:horizontal:disabled, QSlider[accent]::sub-page:horizontal:disabled { background: %(hover)s; }
QSlider::handle:horizontal:disabled { background: %(dim)s; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled, QCheckBox:disabled { color: %(disabled)s; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { background: %(panel)s; }
QScrollBar:vertical { background: transparent; width: {{8}}px; margin: 0; }
QScrollBar::handle:vertical { background: %(scroll)s; border-radius: {{3}}px; min-height: {{24}}px; margin: {{1}}px {{1}}px {{1}}px {{2}}px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QScrollBar:horizontal { background: transparent; height: {{8}}px; margin: 0; }
QScrollBar::handle:horizontal { background: %(scroll)s; border-radius: {{3}}px; min-width: {{24}}px; margin: {{2}}px {{1}}px {{1}}px {{1}}px; }
QScrollBar::handle:hover { background: %(scrollHover)s; }
QScrollBar::handle:pressed { background: %(accent)s; }
QMenu { background: %(raised)s; border: {{1}}px solid %(popupEdge)s; border-radius: %(panel_r)dpx; padding: {{4}}px; }
QMenu::item { padding: {{5}}px {{28}}px {{5}}px {{12}}px; border-radius: %(r)dpx; }
QMenu::item:selected { background: %(selection)s; color: %(selectionText)s; }
QMenu::item:disabled { color: %(disabled)s; }
QMenu::separator { height: {{1}}px; background: %(hover)s; margin: {{4}}px {{8}}px; }
QToolBar { background: %(bg)s; border: none; spacing: {{4}}px; }
QSplitter::handle { background: %(bg)s; }
QScrollArea { border: none; }
%(outline_rules)s%(row_rules)s""" % dict(
        k, family='font-family: "%s"; ' % family if family else "", slider_rules=slider_rules,
        arrow=_arrow_icon(k["muted"]), tick=_check_icon(k["accentText"] if t.is_light() else k["accentInk"]),
        outline_rules=_outline_rules(t), row_rules=_row_rules(t))
    # Fixed sizes ({{N}}px in the template) at Maya's interface scale.
    return re.sub(r"\{\{(-?\d+(?:\.\d+)?)\}\}px", lambda m: "%dpx" % round(float(m.group(1)) * scale()), sheet)
