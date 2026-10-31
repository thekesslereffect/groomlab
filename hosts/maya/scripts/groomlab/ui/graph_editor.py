# SPDX-License-Identifier: Apache-2.0
"""Node-based graph editor for grooms (Qt: groomlab.ui.qt).

Edits a GraphModel (groomlab.graph) and writes it back to the groom:
- drag from an output port (right) to an input port (left) to connect; a node
  with several outputs (a Map node: luminance, r, g, b, a) shows one labelled
  port per output, stacked down its right edge, and a link starts at the one
  dragged from;
- Delete removes selected nodes or connections; Alt+Shift+drag across links cuts them (Hypershade's gesture);
- double-click a link to put a pin on it (Unreal's reroute nodes): links run through their pins,
  which move like nodes, and a pin dropped on another carrying the same signal joins it (a bundle);
  drag from a pin's nub to an input for another link of its signal, drop a link from an output
  (or an input) on a pin to feed every link through it from there (or that input from the pin);
  Delete takes a selected pin out. Snap (the top bar) puts nodes and pins on the grid;
- right-click a node for Set Output / Rename / Delete;
- parameters of the selected node appear on the right; numbers get a slider
  whose range stretches to any value you type (right-click it for the
  range), and dragging previews live; mappable ones can switch between a
  value and a texture, color set, attribute, expression or painted map;
- right-click a param > Set Key (or S with the mouse over it) keys it: the first key links it to
  an attribute of its own on the groom (groomlab.set_param_key), and its field takes the Channel
  Box's colors (red: a key here, pink: animated, yellow: driven) and value on every frame.
With Live on (default), every edit is applied to the groom (undoable) as soon
as the graph is valid; Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) in the editor are Maya's
undo and redo (Edit menu), and the editor follows them; in a text field with
typing to undo, Ctrl+Z undoes the typing first. Node positions go in the groom's graph (each
node's "ui", as the standalone app keeps them, so groom files carry them) outside undo; no node
evaluates again for a move (every one is a cache hit).
Node types come from the plugin's operator catalog: the Add Node menu's categories, their order, each
type's label and search words are the core's (node_catalog.h, in each spec), and each node's one-line
description (the panel's top, the search) is its spec's.

    import groomlab.ui.graph_editor as ge
    ge.show()            # dockable, for the selected groom
"""

import json
import math
import os

from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtGui, QtWidgets

from groomlab import graph as gm
from groomlab.ui import definitions, theme, widgets
from groomlab.ui.messages import plain as plain_message
from groomlab.ui.shortcuts import keys as shortcut_keys
from groomlab.ui.widgets import shrinkable


_SLIDER_PRESSED = [True]  # timeControl -pressed: newer Mayas only (Maya 2022 has no such flag)


def _time_moving():
    """Whether the scene's time is playing or being scrubbed (the time slider held: Mayas whose
    timeControl tells; others, playing only)."""
    from maya import cmds, mel
    try:
        if cmds.play(query=True, state=True):
            return True
        if not _SLIDER_PRESSED[0]:
            return False
        slider = mel.eval("$tmpVar = $gPlayBackSlider")
        return bool(slider) and bool(cmds.timeControl(slider, query=True, pressed=True))
    except TypeError:  # "Invalid flag 'pressed'"
        _SLIDER_PRESSED[0] = False
        return False
    except RuntimeError:
        return False


NODE_W = 208
HEADER_H = 40
ROW_H = 22
PORT_R = 5.5      # drawn radius
PORT_HIT = 11.0   # picking radius: ports are easy to grab
SNAP = 28.0       # a dragged link snaps to a compatible port this close (scene units)
CORNER = 9.0

# The port types' and the node categories' colors, every host's (engine/ui/graph_ui.json; a theme's
# graph.ports and graph.categories override them: apply_theme).
TYPE_COLORS = dict((name, QtGui.QColor(color)) for name, color in definitions.get("graph/portColors").items())
NODE_BG = QtGui.QColor(43, 45, 50)
NODE_BG_TOP = QtGui.QColor(50, 53, 59)
NODE_BORDER = QtGui.QColor(24, 25, 28)
TEXT = QtGui.QColor(232, 234, 237)
MUTED = QtGui.QColor(142, 150, 158)
SELECTED = QtGui.QColor(255, 190, 70)
CANVAS = QtGui.QColor(33, 34, 37)
GRID_DOT = QtGui.QColor(58, 60, 66)
# Status colors (the theme's graph.error, warning, note, driven, reads, writes; apply_theme): a node's
# badge and outline, the param panel's error line, a driven param's tag, the attribute chips.
ERROR = QtGui.QColor(240, 80, 70)
WARNING = QtGui.QColor(235, 170, 60)
NOTE = QtGui.QColor(128, 132, 140)
DRIVEN = QtGui.QColor(214, 122, 214)
READS = QtGui.QColor(90, 150, 220)
WRITES = QtGui.QColor(214, 150, 70)
FOCUS = QtGui.QColor(90, 210, 240)   # the Focus toggle on (the palette's blue)
# A node's cook time in its title row: muted (the theme's ui.textMuted), in the warning color from
# SLOW_MS (the redesign's status badges, as the app's cards). Bypassed cards paint at BYPASSED_OPACITY.
TIME_MUTED = QtGui.QColor(115, 115, 115)
SLOW_MS = 10.0
BYPASSED_OPACITY = 0.5

# The Add Node menu (its categories, their order, each node's label and search words) is the core's
# (node_catalog.h, in every operator spec): Maya, the app and Blender list the same nodes the same way.
HIDDEN_TYPES = {"Compound", "Input", "Script"}  # made by Group, not from the Add menu (the specs' "hidden")
# Nodes whose `path` names a file: the file dialog's filter for it.
FILE_PATH_FILTERS = dict(definitions.get("graph/filePathFilters"))
# When a node runs (its Evaluate menu, choices.json's evaluation): (mode, label, what it does); the
# tag under a node set so.
EVALUATION_CHOICES = [(c["value"], c["label"], c["tip"]) for c in definitions.choices("evaluation")]
EVALUATION_TAGS = dict((c["value"], c["tag"]) for c in definitions.choices("evaluation") if "tag" in c)
CATEGORY_COLORS = dict((name, QtGui.QColor(color)) for name, color in definitions.get("graph/categoryColors").items())

# Params naming the host's inputs (the external data a groom feeds the graph):
# plumbing for scripts and compounds, hidden from the panel unless changed.
PLUMBING = set(tuple(pair) for pair in definitions.get("graph/plumbing"))

# Reads whose "no node upstream writes it" note waits until the user sets them, or the param that
# uses them, off its default (as the core's own note on HairColor does): a fresh groom's HairColor
# shares its randomness per clump (clumpShare) with no Clump before it, and nothing needs fixing
# until one is added or the share is changed. (node type, reading param) -> the param using it.
QUIET_READS = {("HairColor", "clumpChannel"): "clumpShare"}

SCATTER_LENGTH_TIP = ("An Interpolate after this Scatter gives its strands the guides' shapes and lengths, so this "
                      "length only shows where no guide reaches (and before any guide is placed). To make the hair "
                      "longer or shorter, brush the guides (Length, Cut) or trim with the Length node.")


def guides_shape(model, name):
    """Whether node `name`'s strands reach an Interpolate with guides wired in (which then sets their
    shape and length), following the connections downstream."""
    seen, todo = set(), [name]
    while todo:
        node = todo.pop()
        if node in seen:
            continue
        seen.add(node)
        for c in model.connections:
            if c["from"] != node:
                continue
            target = c["to"]
            if target in model.nodes and model.node_type(target) == "Interpolate" and any(
                    d["to"] == target and d["input"] == "guides" for d in model.connections):
                return True
            todo.append(target)
    return False


HEADER_STYLE = "tinted"  # "filled": the header in the category color; "tinted": a wash and an accent bar;
#                          "minimal": no header, a category-colored dot by the title
HEADER_STYLES = ("tinted", "filled", "minimal")
CANVAS_STYLE = "dots"   # the canvas: "dots" (a dot per grid step), "lines" (grid lines) or "plain"
CANVAS_STYLES = ("dots", "lines", "plain")
SHADOW = True

UNDO_KEYS = [QtGui.QKeySequence("Ctrl+Z")]
REDO_KEYS = [QtGui.QKeySequence("Ctrl+Y"), QtGui.QKeySequence("Ctrl+Shift+Z")]


def _key_of(event):
    """(key, modifiers) of a key event, the keypad flag dropped."""
    return event.key(), qt.to_int(event.modifiers()) & ~qt.to_int(QtCore.Qt.KeypadModifier)


def undo_key(event):
    """Ctrl+Z: Maya's undo."""
    return _key_of(event) == (QtCore.Qt.Key_Z, qt.to_int(QtCore.Qt.ControlModifier))


def redo_key(event):
    """Ctrl+Y or Ctrl+Shift+Z: Maya's redo."""
    key, mods = _key_of(event)
    return (key == QtCore.Qt.Key_Y and mods == qt.to_int(QtCore.Qt.ControlModifier)) or (
        key == QtCore.Qt.Key_Z and mods == qt.to_int(QtCore.Qt.ControlModifier | QtCore.Qt.ShiftModifier))


def text_history(widget, redo=False):
    """Whether a text field has typing to undo (or redo): then Ctrl+Z is its own, not Maya's."""
    if isinstance(widget, QtWidgets.QAbstractSpinBox):
        widget = widget.findChild(QtWidgets.QLineEdit)
    if isinstance(widget, QtWidgets.QLineEdit):
        if widget.isReadOnly():
            return False
        return widget.isRedoAvailable() if redo else widget.isUndoAvailable()
    if isinstance(widget, (QtWidgets.QTextEdit, QtWidgets.QPlainTextEdit)):
        if widget.isReadOnly():
            return False
        doc = widget.document()
        return doc.isRedoAvailable() if redo else doc.isUndoAvailable()
    return False


def load_model(groom, catalog=None):
    """The groom's graph as a GraphModel, laid out (from_groom places nodes that have no position):
    what the editor shows on open, Reload, and after the groom's graph changes elsewhere (a preset,
    a script, undo and redo). A graph this GroomLab cannot read (a newer GroomLab's) gives an empty
    model saying why (GraphModel.unreadable), never written back."""
    return gm.GraphModel.from_groom(groom, catalog, tolerant=True)


def apply_theme(t=None):
    """Take the graph's colors and shapes from a theme (the active one by default)."""
    global NODE_BG, NODE_BG_TOP, NODE_BORDER, TEXT, MUTED, SELECTED, CANVAS, GRID_DOT, CORNER, HEADER_STYLE, SHADOW
    global CANVAS_STYLE, ERROR, WARNING, NOTE, DRIVEN, READS, WRITES, FOCUS, TIME_MUTED
    t = t or theme.current()
    NODE_BG = t.qcolor("graph.node")
    NODE_BG_TOP = t.qcolor(t.get("graph.nodeTop") or "graph.node") if t.get("graph.nodeTop") else QtGui.QColor(NODE_BG)
    NODE_BORDER = t.qcolor("graph.nodeBorder")
    TEXT = t.qcolor("graph.text")
    MUTED = t.qcolor("graph.textDim")
    SELECTED = t.qcolor(t.get("graph.selected", "white"))
    CANVAS = t.qcolor("graph.canvas")
    GRID_DOT = t.qcolor("graph.grid")
    CORNER = float(t.get("graph.cornerRadius", 9))
    HEADER_STYLE = t.get("graph.headerStyle", "tinted")
    if HEADER_STYLE not in HEADER_STYLES:  # a style this GroomLab doesn't paint (a newer theme's)
        HEADER_STYLE = "tinted"
    CANVAS_STYLE = t.get("graph.canvasStyle", "dots")
    if CANVAS_STYLE not in CANVAS_STYLES:
        CANVAS_STYLE = "dots"
    SHADOW = bool(t.get("graph.shadow", True))
    ERROR = t.qcolor(t.get("graph.error", "red"))
    WARNING = t.qcolor(t.get("graph.warning", t.get("ui.warning", "yellow")))
    NOTE = t.qcolor(t.get("graph.note", "graph.textDim"))
    DRIVEN = t.qcolor(t.get("graph.driven", "#D67AD6"))
    READS = t.qcolor(t.get("graph.reads", "blue"))
    WRITES = t.qcolor(t.get("graph.writes", "#D6964A"))
    FOCUS = t.qcolor("blue")
    TIME_MUTED = t.qcolor("ui.textMuted") if t.get("ui.textMuted") else QtGui.QColor(MUTED)
    for name, value in (t.get("graph.categories") or {}).items():
        CATEGORY_COLORS[name] = t.qcolor(value)
    for name, value in (t.get("graph.ports") or {}).items():
        TYPE_COLORS[name] = t.qcolor(value)


def readable_on(color):
    """Dark or light text, whichever reads on `color`."""
    luma = 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF()
    return QtGui.QColor(28, 29, 33) if luma > 0.55 else QtGui.QColor(255, 255, 255)


def time_text(ms):
    """A node's cook time as its card shows it ("0.4 ms", "12 ms"); None under a twentieth of a
    millisecond (nothing worth a badge), as the app's cards."""
    if ms is None or ms < 0.05:
        return None
    return ("%.1f ms" % ms) if ms < 10.0 else ("%d ms" % int(round(ms)))


def time_color(ms):
    """Its color: muted, the warning color from SLOW_MS."""
    return QtGui.QColor(WARNING if ms >= SLOW_MS else TIME_MUTED)


def warning_triangle(painter, center, color, size=11.0):
    """The status badge of a node with a warning or a note: an outlined triangle with a "!"."""
    h = size * 0.9
    top, left, right = (center + QtCore.QPointF(0, -h / 2.0), center + QtCore.QPointF(-size / 2.0, h / 2.0),
                        center + QtCore.QPointF(size / 2.0, h / 2.0))
    pen = QtGui.QPen(color, 1.4)
    pen.setJoinStyle(QtCore.Qt.RoundJoin)
    pen.setCapStyle(QtCore.Qt.RoundCap)
    painter.setPen(pen)
    painter.setBrush(QtCore.Qt.NoBrush)
    painter.drawPolygon(QtGui.QPolygonF([top, right, left]))
    painter.drawLine(center + QtCore.QPointF(0, -h * 0.12), center + QtCore.QPointF(0, h * 0.18))
    painter.setPen(QtCore.Qt.NoPen)
    painter.setBrush(color)
    painter.drawEllipse(center + QtCore.QPointF(0, h * 0.34), 0.8, 0.8)


# -- Keyed params: the param fields in the Channel Box's colors ------------------------------------

KEYABLE_KINDS = ("float", "int", "bool", "vec3")
# A state's theme color (ui.keyed, ...) and Maya's own where a theme has none (graph_ui.json's keyStates).
KEY_COLORS = dict((k, (v["theme"], v["color"])) for k, v in definitions.get("graph/keyStates").items() if "theme" in v)
KEY_TIPS = dict((k, v["tip"]) for k, v in definitions.get("graph/keyStates").items())


def key_state(groom, node, param, model):
    """groomlab.param_key_state, or None where it cannot be read (no groom, a scene change)."""
    if model.control_of(node, param) is None:
        driver = model.driver_of(node, param)
        return "driven" if driver and model.node_type(driver) == "Control" else None
    if not groom:
        return None
    import groomlab
    try:
        return groomlab.param_key_state(groom, node, param, model)
    except (RuntimeError, ValueError):
        return None


def _key_fields(field):
    """The editable boxes of a param's field: a slider (a fill field), a vector's three, a checkbox."""
    return list(getattr(field, "boxes", None) or [field])


def show_value(field, value):
    """Show a keyed param's value on this frame, without signals (nothing is written back)."""
    if isinstance(field, widgets.SoftSlider):
        field.setValue(float(value) * getattr(field, "unit_factor", 1.0))
    elif isinstance(field, QtWidgets.QCheckBox):
        field.blockSignals(True)
        field.setChecked(bool(value))
        field.blockSignals(False)
    elif getattr(field, "boxes", None) and isinstance(value, (list, tuple)):
        for box, v in zip(field.boxes, value):
            if box.hasFocus():
                continue  # being typed into
            box.blockSignals(True)
            box.setValue(float(v))
            box.blockSignals(False)
        if getattr(field, "swatch", None) is not None:  # a keyed color: its swatch follows
            field.swatch.set_color([float(v) for v in value])


def paint_key_state(field, state):
    """Color a param's field by its state as the Channel Box does (None: the theme's own look)."""
    field.setProperty("keyState", state or "")
    color = None
    if state in KEY_COLORS:
        path, fallback = KEY_COLORS[state]
        color = QtGui.QColor(theme.current().color(path, fallback))
    for box in _key_fields(field):
        if isinstance(box, widgets.SoftSlider):
            box.set_key_color(color)  # painted: the field tinted, its border in the color
        elif color is None:
            box.setStyleSheet("")
        else:
            box.setStyleSheet("background-color: %s; color: %s;" % (color.name(), readable_on(color).name()))
        base = box.property("glBaseTip")
        if base is None:
            base = box.toolTip() or ""
            box.setProperty("glBaseTip", base)
        box.setToolTip("\n".join(t for t in (KEY_TIPS.get(state or ""), base) if t))


# What an attribute param does with the attribute it names (the spec's "channel").
ATTRIBUTE_TIPS = {
    "writes": "Writes the attribute named here: a value per strand that nodes after it read by name (an attribute "
              "map, a Curl or Cards by clump, a renderer). A name a node upstream writes is overwritten from here on.",
    "reads": "Reads the attribute named here, written by a node upstream (the list shows those reaching this node "
             "and which node writes each). A name that does not reach this node is ignored.",
}


def pretty(name):
    """A param name as a label: "guideRadius" reads "Guide radius" (the name stays in the tooltip)."""
    import re
    words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", name).replace("_", " ").strip()
    words = " ".join(w if w.isupper() and len(w) > 1 else w.lower() for w in words.split())
    return words[:1].upper() + words[1:]


def search_rank(text, op_type, words):
    """Where node type `op_type` ranks for the search `text` (lower first), None when it doesn't
    match: its name or label starting with it, containing it, a search keyword or its category title, a param's name, its
    description, then a param's description. `words`: NodeSearch's entry (lowercase texts)."""
    if not text:
        return 5
    name = op_type.lower()
    label = words.get("label", name)
    parts = text.split()
    if name.startswith(text) or label.startswith(text):
        return 0
    if text in name or text in label or text.replace(" ", "") in name:
        return 1
    for rank, key in ((2, "keywords"), (3, "params"), (4, "about")):
        if all(part in words.get(key, "") or part in name for part in parts):
            return rank
    everything = " ".join([name] + [words.get(k, "") for k in ("keywords", "params", "about", "details")])
    if all(part in everything for part in parts):
        return 5
    return None


ADVANCED = "Advanced"  # the section the advanced params fold away in, last (and a group of that name)


def param_sections(specs):
    """[(section, [spec, ...])] in the panel's order: the main params ("" : no header), each group in
    the order the spec lists them, then Advanced (every advanced param, whatever its group). With
    fewer than two groups there are no group headers: the params read as one list, as before."""
    groups = []
    for spec in specs:
        group = spec.get("group") or ""
        if group and group != ADVANCED and not spec.get("advanced") and group not in groups:
            groups.append(group)
    headed = len(groups) >= 2

    def section_of(spec):
        group = spec.get("group") or ""
        if spec.get("advanced") or group == ADVANCED:
            return ADVANCED
        return group if headed else ""

    sections = [(key, [s for s in specs if section_of(s) == key]) for key in [""] + groups + [ADVANCED]]
    return [(key, members) for key, members in sections if members]


def shown_when(spec, value_of, spec_of=None):
    """Whether a param shows: its spec's showWhen holds (graph.h ParamCondition): {"param", "in"}
    for the node's values (value_of(param)): a bool as "true" or "false", a number as JSON writes
    it; and the param it names shows too (spec_of(name): that param's spec, None: unknown), so
    Clump's count hides with clumpBy. No condition: always."""
    seen = set()
    while True:
        condition = spec.get("showWhen") or {}
        param = condition.get("param")
        if not param:
            return True
        value = value_of(param)
        if isinstance(value, bool):
            text = "true" if value else "false"
        elif isinstance(value, str):
            text = value
        else:
            text = json.dumps(value)
        if text not in (condition.get("in") or []):
            return False
        seen.add(spec.get("name"))
        spec = spec_of(param) if spec_of else None
        if spec is None or spec.get("name") in seen:
            return True


def unit_of(spec, value_of):
    """What a param measures for the node now: its spec's unit while its unitWhen holds (graph.h
    ParamSpec::unitWhen, read as showWhen's param part: Noise's frequency is per scene unit in
    world space, per strand length in strand space), else "" (unitless)."""
    unit = spec.get("unit") or ""
    when = spec.get("unitWhen")
    if unit and when and not shown_when({"showWhen": {"param": when.get("param"), "in": when.get("in")}}, value_of):
        return ""
    return unit


def spec_category(spec):
    """The Add Node category an operator spec names, as the menu titles it ("Utility" when none)."""
    spec = spec or {}
    return spec.get("categoryTitle") or (spec.get("category") or "Utility").title()


def category_of(op_type, catalog=None):
    """A node type's category title (the core's node catalog, through its spec)."""
    if op_type == "Compound":
        return "Compound"
    if catalog is not None:
        try:
            return spec_category(catalog.spec(op_type))
        except gm.GraphError:
            pass
    return "Utility"


def label_of(op_type, catalog=None):
    """What a node type is called on screen: its spec's label ("Linked Groom" for Reference, "Blend
    Neighbors"); the type itself stays what files store."""
    if catalog is not None:
        try:
            return catalog.spec(op_type).get("label") or op_type
        except gm.GraphError:
            pass
    return op_type


def describe(op_type, catalog):
    """What a node type does, in a line: its operator spec's description (the core's, as every host
    shows it); empty for a type the catalog does not know."""
    if catalog is None:
        return ""
    try:
        return catalog.spec(op_type).get("description", "")
    except gm.GraphError:
        return ""


def refresh_library(catalog):
    """The node library's compound nodes (groomlab.nodes) put in the catalog as Add Node entries
    ("node:<id>"), read afresh; a stamp of them (None when the library cannot be read)."""
    if catalog is None:
        return None
    try:
        from groomlab import nodes
        return catalog.set_library(nodes.library_nodes())
    except Exception:  # a library that cannot be read never stops the menus
        return None


def library_category(info):
    """The category title a library node's card takes (its file's category when it is a built-in
    one, by id or title), else "Compound"."""
    wanted = (info or {}).get("category", "").strip().lower()
    for title in CATEGORY_COLORS:
        if wanted and wanted in (title.lower(), title.split()[0].lower()):
            return title
    return "Compound"


def categories(catalog):
    """[(category title, [types])] to offer, in the core's menu order (node_catalog.h): only the
    types the plugin has, none of the hidden ones (Compound, Input)."""
    if catalog is None:
        return []
    specs = [catalog.spec(t) for t in catalog.types()]
    specs = [s for s in specs if not s.get("hidden") and s["type"] not in HIDDEN_TYPES]
    specs.sort(key=lambda s: (s.get("categoryOrder", 99), s.get("order", 999), s["type"]))
    result = []
    for spec in specs:
        title = spec_category(spec)
        if not result or result[-1][0] != title:
            result.append((title, []))
        result[-1][1].append(spec["type"])
    return result


# ---------------------------------------------------------------------------
# Scene items


# Nodes and ports cache their painting (see NodeItem): a 30-node graph pans at under 2 ms a frame
# instead of ~11. An Alt+right-drag zoom scales a picture of the view taken when it starts
# (GraphView.paintEvent), sharp again on release.
ITEM_CACHE = QtWidgets.QGraphicsItem.DeviceCoordinateCache


# Qt's pixmap cache holds those pixmaps; its default 10 MB is a dozen nodes at 1:1 on a 150% screen,
# past which every frame paints them all again: raised (never lowered) when an editor opens.
PIXMAP_CACHE_KB = 64 * 1024


def make_room_for_cache():
    if QtGui.QPixmapCache.cacheLimit() < PIXMAP_CACHE_KB:
        QtGui.QPixmapCache.setCacheLimit(PIXMAP_CACHE_KB)


class PortItem(QtWidgets.QGraphicsItem):
    """A port dot: hollow when free, filled when linked; larger to pick than to see."""

    def __init__(self, node_item, name, port_type, is_output, is_param=False, value_kind=None):
        super(PortItem, self).__init__(node_item)
        self.node_item = node_item
        self.name = name
        self.port_type = port_type
        self.value_kind = value_kind or "any"  # a Value port's: number, integer, bool, vec3, choice, text
        self.is_output = is_output
        self.is_param = is_param   # an exposed param's input (Field, Curve or Value)
        self.highlight = False     # a compatible target while a link is dragged
        self.hovered = False
        self.setAcceptHoverEvents(True)
        kind = gm.port_label(port_type, self.value_kind)
        self.setToolTip("%s (%s)%s" % (name, kind, " — exposed param" if is_param else ""))
        self.setCursor(QtCore.Qt.CrossCursor)
        self.setCacheMode(ITEM_CACHE)

    def boundingRect(self):
        return QtCore.QRectF(-PORT_HIT, -PORT_HIT, 2 * PORT_HIT, 2 * PORT_HIT)

    def shape(self):
        path = QtGui.QPainterPath()
        path.addEllipse(self.boundingRect())
        return path

    def linked(self):
        return self.node_item.editor.is_linked(self.node_item.name, self.name, self.is_output)

    def paint(self, painter, option, widget=None):
        color = TYPE_COLORS.get(self.port_type, QtGui.QColor(200, 200, 200))
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        r = PORT_R + (2.0 if self.hovered or self.highlight else 0.0)
        if self.highlight:
            glow = QtGui.QColor(color)
            glow.setAlpha(70)
            painter.setPen(QtCore.Qt.NoPen)
            painter.setBrush(glow)
            painter.drawEllipse(QtCore.QPointF(0, 0), r + 5, r + 5)
        if self.port_type == "Curves":  # strands or guides: half each color
            self.paint_either(painter, r)
            return
        painter.setPen(QtGui.QPen(color, 2.0))
        painter.setBrush(color if self.linked() else CANVAS)
        if self.is_param:  # a diamond: a param, not a data input
            poly = QtGui.QPolygonF([QtCore.QPointF(0, -r), QtCore.QPointF(r, 0), QtCore.QPointF(0, r),
                                    QtCore.QPointF(-r, 0)])
            painter.drawPolygon(poly)
        else:
            painter.drawEllipse(QtCore.QPointF(0, 0), r, r)

    def paint_either(self, painter, r):
        """A port taking strands or guides: the left half in the strands color, the right in the guides'."""
        rect = QtCore.QRectF(-r, -r, 2 * r, 2 * r)
        linked = self.linked()
        for color, start in ((TYPE_COLORS["Strands"], 90 * 16), (TYPE_COLORS["Guides"], 270 * 16)):
            if linked:
                painter.setPen(QtCore.Qt.NoPen)
                painter.setBrush(color)
                painter.drawPie(rect.adjusted(-1, -1, 1, 1), start, 180 * 16)
            else:
                painter.setBrush(QtCore.Qt.NoBrush)
                painter.setPen(QtGui.QPen(color, 2.0))
                painter.drawArc(rect, start, 180 * 16)

    def hoverEnterEvent(self, event):
        self.hovered = True
        self.update()

    def hoverLeaveEvent(self, event):
        self.hovered = False
        self.update()

    def scene_center(self):
        return self.mapToScene(QtCore.QPointF(0, 0))


class NodeItem(QtWidgets.QGraphicsItem):
    """A node card: category accent, name and type, ports with labels, a summary of changed params."""

    def __init__(self, editor, name):
        super(NodeItem, self).__init__()
        self.editor = editor
        self.name = name
        model = editor.model
        self.op_type = model.node_type(name)
        self.category = "Output" if model.output == name else category_of(self.op_type, model.catalog)
        # A node from the node library: its label and category, and whether the library has a newer copy.
        self.library = model.library_info(name) if self.op_type in ("Compound", "Script") else None
        if self.library:
            self.category = library_category(self.library)
        spec = model.node_spec(name)
        self.inputs = {}
        y = HEADER_H + ROW_H / 2 + 4
        for port in spec["inputs"]:
            item = PortItem(self, port["name"], port["type"], False, port.get("param", False), port.get("valueKind"))
            item.setPos(0, y)
            self.inputs[port["name"]] = item
            y += ROW_H
        # One port per output, stacked down the right edge (the primary first). A shaping node's
        # output shows what it gives as connected: strands, guides (or either, unconnected).
        self.outputs = {}
        y = HEADER_H + ROW_H / 2 + 4
        for port in spec["outputs"]:
            item = PortItem(self, port["name"], model.output_type(name, port["name"]), True, value_kind=port.get("valueKind"))
            item.setPos(NODE_W, y)
            self.outputs[port["name"]] = item
            y += ROW_H
        self.output = self.outputs[spec["outputs"][0]["name"]]  # the primary
        self.summary = self._summary(spec)
        rows = max(1, len(spec["inputs"]), len(spec["outputs"]))
        self.height = HEADER_H + ROW_H * rows + 10 + (18 if self.summary else 0)
        self.edges = []  # before setPos: itemChange updates edges
        self.hovered = False
        self.setAcceptHoverEvents(True)
        self.setFlags(QtWidgets.QGraphicsItem.ItemIsMovable | QtWidgets.QGraphicsItem.ItemIsSelectable |
                      QtWidgets.QGraphicsItem.ItemSendsGeometryChanges)
        tip = label_of(self.op_type, model.catalog)
        if self.library:
            tip = "%s (library node %s, version %d)" % (self.library["label"], self.library["id"], self.library["version"])
            if self.library.get("updateAvailable"):
                tip += "\nUpdate available: the library has version %d (right-click > Update Node)" % self.library["latest"]
        self.setToolTip(tip)
        self.setPos(*model.position(name))
        # Painted once into a pixmap at the view's resolution, then blitted: a pan (and a node drag)
        # repaints none of its text and shapes (anything it shows changing calls update()).
        self.setCacheMode(ITEM_CACHE)

    def _summary(self, spec):
        """A few params that differ from their defaults, e.g. "density 300 · seed 7"."""
        parts = []
        model = self.editor.model
        for p in spec["params"]:
            if p["name"] == "definition":
                continue
            value = model.param(self.name, p["name"])
            if value == p["default"]:
                continue
            if isinstance(value, bool):
                text = "on" if value else "off"
            elif isinstance(value, (int, float)):
                text = "%g" % value
            elif isinstance(value, dict):
                source = next((k for k in ("texture", "colorSet", "attribute", "expression", "guide") if k in value), "map")
                text = {"colorSet": "color set", "guide": "map"}.get(source, source)
            elif isinstance(value, str) and len(value) < 14:
                text = value
            else:
                text = "…"
            parts.append("%s %s" % (p["name"], text))
            if len(parts) == 2:
                break
        driven = [p for p in model.param_inputs(self.name) if model.driver_of(self.name, p)]
        if driven and len(parts) < 2:
            parts.append("%s × field" % driven[0])
        return " · ".join(parts)

    def boundingRect(self):
        return QtCore.QRectF(-PORT_HIT, -12, NODE_W + 2 * PORT_HIT, self.height + 30)

    def status(self):
        """(bypassed, viewed, error text, milliseconds, warning text, note text) from the editor."""
        editor = self.editor
        return (editor.model.bypassed(self.name), editor.viewed == self.name,
                editor.node_errors.get(self.name), editor.node_times.get(self.name),
                editor.node_warnings.get(self.name), editor.note_of(self.name))

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        rect = QtCore.QRectF(0, 0, NODE_W, self.height)
        accent = CATEGORY_COLORS.get(self.category, MUTED)
        bypassed, viewed, error, ms, warning, note = self.status()
        painter.setOpacity(BYPASSED_OPACITY if bypassed else 1.0)

        # Soft drop shadow (flat themes go without).
        for i, alpha in enumerate((40, 24, 12) if SHADOW else ()):
            painter.setPen(QtCore.Qt.NoPen)
            painter.setBrush(QtGui.QColor(0, 0, 0, alpha))
            painter.drawRoundedRect(rect.translated(0, 3 + i).adjusted(-i, -i, i, i), CORNER + i, CORNER + i)

        # Body with a gentle vertical gradient.
        gradient = QtGui.QLinearGradient(0, 0, 0, self.height)
        gradient.setColorAt(0.0, NODE_BG_TOP)
        gradient.setColorAt(1.0, NODE_BG)
        selected = self.isSelected()
        border = SELECTED if selected else (MUTED if self.hovered else NODE_BORDER)
        painter.setPen(QtGui.QPen(border, 2.0 if selected else 1.0))
        painter.setBrush(gradient)
        painter.drawRoundedRect(rect, CORNER, CORNER)

        # The header: filled with the category color, a wash under an accent bar, or (minimal)
        # none at all: a dot in the category color beside the name says the category.
        header = QtCore.QRectF(0, 0, NODE_W, HEADER_H)
        filled = HEADER_STYLE == "filled"
        minimal = HEADER_STYLE == "minimal"
        if not minimal:
            clip = QtGui.QPainterPath()
            clip.addRoundedRect(rect, CORNER, CORNER)
            painter.save()
            painter.setClipPath(clip)
            if filled:
                painter.fillRect(header, accent)
            else:
                tint = QtGui.QColor(accent)
                tint.setAlpha(34)
                painter.fillRect(header, tint)
                painter.fillRect(QtCore.QRectF(0, 0, NODE_W, 3), accent)
            painter.restore()
        if not filled and not minimal:
            painter.setPen(QtGui.QPen(QtGui.QColor(255, 255, 255, 14), 1))
            painter.drawLine(QtCore.QPointF(10, HEADER_H), QtCore.QPointF(NODE_W - 10, HEADER_H))
        on_header = readable_on(accent) if filled else TEXT
        self.header_ink = on_header if filled else None

        # Name, and the type underneath (pixel sizes: the same at any screen scale), in the theme's
        # font (ui.font: Inter).
        font = theme.font(13, bold=True, base=painter.font())
        small = QtGui.QFont(font)
        small.setBold(False)
        small.setPixelSize(10)
        small.setLetterSpacing(QtGui.QFont.AbsoluteSpacing, 0.6)
        name_h = QtGui.QFontMetrics(font).height()
        left = 12.0
        # The status badges in the title row, left of the header toggles (the redesign's): a warning
        # triangle (a warning or a note; an error keeps its red outline and corner "!") and the
        # cook time. Laid out right to left; the name gets the room left of them.
        badges_right = min(r.left() for r in self.toggle_rects().values()) - 6.0
        row_center = 5 + name_h / 2.0
        stamp = time_text(ms)
        number = theme.font(10, tabular=True, base=painter.font())
        x = badges_right
        stamp_rect = alert_center = None
        if stamp:
            width = QtGui.QFontMetricsF(number).horizontalAdvance(stamp)
            x -= width
            stamp_rect = QtCore.QRectF(x, row_center - 8, width + 1, 16)
            x -= 6.0
        if (warning or note) and not error:
            alert_center = QtCore.QPointF(x - 5.5, row_center)
            x -= 15.0
        if minimal:
            painter.setPen(QtCore.Qt.NoPen)
            painter.setBrush(accent)
            painter.drawEllipse(QtCore.QPointF(left + 3.5, 5 + name_h / 2.0), 3.5, 3.5)
            painter.setBrush(QtCore.Qt.NoBrush)
            left += 13.0
        painter.setFont(font)
        painter.setPen(on_header)
        room = max(24.0, x - left - 2)
        painter.drawText(QtCore.QRectF(left, 5, room, name_h), QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter,
                         QtGui.QFontMetrics(font).elidedText(self.name, QtCore.Qt.ElideRight, room))
        painter.setFont(small)
        if filled:
            kind = QtGui.QColor(on_header)
            kind.setAlpha(165)
            painter.setPen(kind)
        elif minimal:
            painter.setPen(MUTED)
        else:
            painter.setPen(accent.lighter(115))
        label = label_of(self.op_type, self.editor.catalog).upper() if self.category != "Output" else "OUTPUT"
        if self.library and self.category != "Output":
            label = self.library["label"].upper()
        painter.drawText(QtCore.QRectF(left, 5 + name_h, NODE_W - left - 12, QtGui.QFontMetrics(small).height()),
                         QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter, label)

        # Port labels.
        body = QtGui.QFont(font)
        body.setBold(False)
        body.setPixelSize(11)
        painter.setFont(body)
        for name, port in self.inputs.items():
            painter.setPen(TYPE_COLORS.get("Field").lighter(120) if port.is_param else TEXT)
            painter.drawText(QtCore.QRectF(14, port.pos().y() - ROW_H / 2, NODE_W * 0.6, ROW_H),
                             QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft, name)
        painter.setPen(TEXT)
        for name, port in self.outputs.items():
            painter.drawText(QtCore.QRectF(NODE_W * 0.4, port.pos().y() - ROW_H / 2, NODE_W * 0.6 - 14, ROW_H),
                             QtCore.Qt.AlignVCenter | QtCore.Qt.AlignRight, name)
        small.setLetterSpacing(QtGui.QFont.AbsoluteSpacing, 0.0)

        # The status badges (dimmed with a bypassed card, as the design's).
        ink = getattr(self, "header_ink", None)  # on a filled header: its readable color
        if stamp_rect is not None:
            color = QtGui.QColor(ink) if ink is not None else time_color(ms)
            if ink is not None:
                color.setAlpha(190)
            painter.setFont(number)
            painter.setPen(color)
            painter.drawText(stamp_rect, QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter, stamp)
        if alert_center is not None:
            warning_triangle(painter, alert_center, QtGui.QColor(ink) if ink is not None else WARNING)

        painter.setOpacity(1.0)
        # The header toggles (Expand Inputs, View, Bypass), the error badge, a library update.
        badge = QtGui.QFont(small)
        badge.setBold(True)
        painter.setFont(badge)
        self.paint_toggles(painter, viewed, bypassed)
        if error:
            painter.setPen(QtGui.QPen(ERROR, 2.0))
            painter.setBrush(QtCore.Qt.NoBrush)
            painter.drawRoundedRect(rect.adjusted(-2, -2, 2, 2), CORNER + 2, CORNER + 2)
            painter.setBrush(ERROR)
            painter.setPen(QtCore.Qt.NoPen)
            painter.drawEllipse(QtCore.QPointF(NODE_W, 0), 8, 8)
            painter.setPen(readable_on(ERROR))
            painter.drawText(QtCore.QRectF(NODE_W - 8, -8, 16, 16), QtCore.Qt.AlignCenter, "!")
        if self.library and self.library.get("updateAvailable"):
            # The library has a newer copy of this node: a mark on its left corner (Update Node).
            painter.setBrush(SELECTED)
            painter.setPen(QtCore.Qt.NoPen)
            painter.drawEllipse(QtCore.QPointF(0, 0), 8, 8)
            painter.setPen(readable_on(SELECTED))
            painter.drawText(QtCore.QRectF(-8, -8, 16, 16), QtCore.Qt.AlignCenter, u"\u2191")
        # When it runs, unless always: a tag under the node's left corner.
        tag = EVALUATION_TAGS.get(self.editor.model.evaluation(self.name))
        if tag:
            painter.setPen(MUTED)
            painter.setFont(small)
            painter.drawText(QtCore.QRectF(4, self.height + 1, NODE_W * 0.6, 14),
                             QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter, tag)
        painter.setOpacity(BYPASSED_OPACITY if bypassed else 1.0)

        if self.summary:
            painter.setPen(MUTED)
            painter.setFont(small)
            painter.drawText(QtCore.QRectF(12, self.height - 22, NODE_W - 24, 16),
                             QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter,
                             QtGui.QFontMetrics(small).elidedText(self.summary, QtCore.Qt.ElideRight, NODE_W - 24))

    # -- Header toggles: exposed inputs, Focus (view up to here) and Bypass, as in Yeti --

    def viewable(self):
        """Focus shows a node giving strands (a Clump's centers as dots)."""
        return self.output.port_type == "Strands"

    def toggle_rects(self):
        """Header button rectangles; only nodes with linkable params get Expand Inputs."""
        size = 18.0
        rects = {"disable": QtCore.QRectF(NODE_W - 10 - size, 8, size, size)}
        left = NODE_W - 14 - 2 * size
        if self.viewable():
            rects["focus"] = QtCore.QRectF(left, 8, size, size)
            left -= size + 4
        if self.mappable():
            rects["inputs"] = QtCore.QRectF(left, 8, size, size)
        return rects

    def paint_toggles(self, painter, viewed, bypassed):
        rects = self.toggle_rects()
        painter.save()
        painter.setOpacity(1.0)
        for kind, rect in rects.items():
            available = self.mappable() if kind == "inputs" else []
            on = (viewed if kind == "focus" else
                  (all(p in self.editor.model.param_inputs(self.name) for p in available) if kind == "inputs" else bypassed))
            ink = getattr(self, "header_ink", None)  # on a filled header: its readable color
            idle = QtGui.QColor(ink) if ink is not None else QtGui.QColor(MUTED)
            if ink is not None:
                idle.setAlpha(150)
            active = FOCUS if kind in ("focus", "inputs") else WARNING
            color = (ink if ink is not None else active) if on else idle
            if on:  # lit: a tile of its color under a bolder glyph (View in the blue: plainly on)
                fill = QtGui.QColor(color)
                fill.setAlpha(70)
                painter.setPen(QtCore.Qt.NoPen)
                painter.setBrush(fill)
                painter.drawRoundedRect(rect, 4, 4)
            painter.setPen(QtGui.QPen(color, 1.8 if on else 1.6))
            painter.setBrush(QtCore.Qt.NoBrush)
            c = rect.center()
            if kind == "focus":  # an eye
                path = QtGui.QPainterPath(c + QtCore.QPointF(-6.5, 0))
                path.quadTo(c + QtCore.QPointF(0, -6), c + QtCore.QPointF(6.5, 0))
                path.quadTo(c + QtCore.QPointF(0, 6), c + QtCore.QPointF(-6.5, 0))
                painter.drawPath(path)
                painter.setBrush(color)
                painter.drawEllipse(c, 2.2, 2.2)
            elif kind == "inputs":  # Maya Hypershade's three-line expand-inputs glyph
                for dy in (-4, 0, 4):
                    painter.drawLine(c + QtCore.QPointF(-5, dy), c + QtCore.QPointF(5, dy))
            else:  # a power symbol
                arc = QtCore.QRectF(c.x() - 5.5, c.y() - 5, 11, 11)
                painter.drawArc(arc, 120 * 16, 300 * 16)
                painter.drawLine(c + QtCore.QPointF(0, -7), c + QtCore.QPointF(0, -1))
        painter.restore()

    def mousePressEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton:
            for kind, rect in self.toggle_rects().items():
                if rect.adjusted(-2, -2, 2, 2).contains(event.pos()):
                    event.accept()
                    editor, name = self.editor, self.name
                    if kind == "focus":
                        later(lambda: editor.view_node(None if editor.viewed == name else name))
                    elif kind == "inputs":
                        later(lambda: editor.toggle_param_inputs(name))
                    else:
                        later(lambda: editor.set_bypass([name], not editor.model.bypassed(name)))
                    return
        super(NodeItem, self).mousePressEvent(event)

    def hoverMoveEvent(self, event):
        tips = {"focus": "Focus: show the groom up to this node in the viewport (%s); a Clump's centers show as "
                         "dots" % shortcut_keys("graph.view"),
                "inputs": "Expand Inputs: show or hide all connectable parameter ports",
                "disable": "Bypass: pass the input straight through (%s)" % shortcut_keys("graph.bypass")}
        over = next((k for k, r in self.toggle_rects().items() if r.contains(event.pos())), None)
        self.setToolTip(tips[over] if over else (self.editor.node_errors.get(self.name)
                                                 or self.editor.node_warnings.get(self.name)
                                                 or self.editor.note_of(self.name)
                                                 or label_of(self.op_type, self.editor.catalog)))

    def hoverEnterEvent(self, event):
        self.hovered = True
        self.update()

    def hoverLeaveEvent(self, event):
        self.hovered = False
        self.update()

    def itemChange(self, change, value):
        if change == QtWidgets.QGraphicsItem.ItemPositionChange and self.editor.snapping(self):
            return snapped(value)
        if change == QtWidgets.QGraphicsItem.ItemPositionHasChanged:
            for edge in self.edges:
                edge.update_path()
            self.editor.node_moved(self.name, self.pos())
        return super(NodeItem, self).itemChange(change, value)

    # What the node's right-click menu runs (engine/ui/graph_ui.json's nodeMenu): functions of the
    # item. Every action rebuilds the scene (this item included): each runs after the menu's event.
    ACTIONS = {
        "node.setOutput": lambda self: later(lambda: self.editor.set_output(self.name)),
        "node.view": lambda self, on: later(lambda: self.editor.view_node(self.name if on else None)),
        "node.bypass": lambda self, on: later(lambda: self.editor.set_bypass([self.name], on)),
        # When it runs (Yeti's per-node evaluation): renders and exports skip a viewport-only node,
        # the viewport a render-only one.
        "node.evaluate": lambda self, mode, _on: later(lambda: self.editor.set_evaluation([self.name], mode)),
        "node.rename": lambda self: later(lambda: self.editor.rename_node(self.name)),
        "node.duplicate": lambda self: later(lambda: self.editor.duplicate([self.name])),
        # What brushes shape: this node's guides (selecting it picks it too), or the base groom.
        "node.brushSculpt": lambda self, on: later(lambda: self.editor.target_sculpt(self.name if on else None)),
        "sculpt.clearEdits": lambda self: later(lambda: self.editor.clear_sculpt_edits(self.name)),
        "node.bake": lambda self: later(lambda: self.editor.bake_to_guides(self.name)),
        "node.expose": lambda self, param, on: later(lambda: self.editor.set_param_exposed(self.name, param, on)),
        "script.edit": lambda self: later(lambda: self.editor.show_script_editor(self.name)),
        "script.saveToLibrary": lambda self: later(lambda: self.editor.save_script_to_library(self.name)),
        "node.update": lambda self: later(lambda: self.editor.update_library_node(self.name)),
        "compound.ungroup": lambda self: later(lambda: self.editor.expand([self.name])),
        # Its inner nodes' params, each shown on the compound (checked) or not: what Save as Node keeps.
        "compound.showInner": lambda self, target, on: later(lambda: self.editor.show_inner_param(self.name, target, on)),
        "compound.saveAsNode": lambda self: later(lambda: self.editor.show_save_node(self.name)),
        "compound.saveToLibrary": lambda self: later(lambda: self.editor.save_to_library(self.name)),
        "compound.groupSelection": lambda self: later(self.editor.collapse_selected),
        "backdrop.add": lambda self: later(self.editor.add_backdrop),
        "node.delete": lambda self: later(lambda: self.editor.delete_nodes([self.name])),
    }

    def menu_state(self):
        """The conditions the node menu's entries need: what kind of node it is, its library copy."""
        state = set()
        if self.viewable():
            state.add("node.viewable")
        if self.op_type == "Sculpt":
            state.add("node.sculpt")
        if self.output.port_type in ("Strands", "Guides") and self.op_type != "Output":
            state.add("node.bakeable")
        if self.mappable():
            state.add("node.mappable")
        if self.op_type in ("Script", "Compound"):
            state.add("node." + self.op_type.lower())
        if self.library:
            state.add("node.library")
            if self.library.get("updateAvailable"):
                state.add("node.updateAvailable")
        return state

    def mappable(self):
        """The params Expose Param as Input offers (the linkable ones)."""
        return [p["name"] for p in self.editor.model.node_spec(self.name)["params"] if gm.linkable(p)]

    def menu_generators(self):
        """The node menu's generated entries: the params it can expose, a compound's inner params."""
        def expose(entry):
            return [{"id": entry["action"], "value": param, "label": param, "tip": ""} for param in self.mappable()]

        def inner(entry):
            definition = (self.editor.model.nodes[self.name].get("params") or {}).get("definition") or {}
            subs = []
            for node in definition.get("graph", {}).get("nodes", []):
                try:
                    specs = self.editor.catalog.spec(node.get("type", ""))["params"]
                except gm.GraphError:
                    continue
                specs = [p for p in specs if p.get("kind") in ("float", "int", "bool", "vec3", "string")
                         and p["name"] not in ("definition",) and not p.get("channel")]
                if not specs or node.get("type") == "Input":
                    continue
                subs.append({"submenu": node["name"], "items": [
                    {"id": entry["action"], "value": "%s.%s" % (node["name"], p["name"]), "label": pretty(p["name"]),
                     "tip": p.get("description", "")} for p in specs]})
            return subs
        return {"exposeParams": expose, "innerParams": inner}

    def contextMenuEvent(self, event):
        editor, name = self.editor, self.name
        current = editor.model.evaluation(name)
        exposed = editor.model.param_inputs(name)
        definition = (editor.model.nodes[name].get("params") or {}).get("definition") or {}
        shown = set((definition.get("expose") or {}).values())
        checked = {"node.view": lambda item: editor.viewed == name,
                   "node.bypass": lambda item: editor.model.bypassed(name),
                   "node.evaluate": lambda item: item["value"] == current,
                   "node.brushSculpt": lambda item: editor.sculpt_target() == name,
                   "node.expose": lambda item: item["value"] in exposed,
                   "compound.showInner": lambda item: item["value"] in shown}
        menu = QtWidgets.QMenu()
        widgets.fill_menu(menu, definitions.get("graph/nodeMenu"), definitions.binder(self.ACTIONS, self),
                          self.menu_state(), values={"latest": (self.library or {}).get("latest", "")},
                          generators=self.menu_generators(), checked=lambda item: checked[item["id"]](item))
        qt.exec_(menu, event.screenPos())


def suggest_box(value, options, set_value, placeholder=""):
    """A text field with a dropdown of what exists: pick one or type another name."""
    box = QtWidgets.QComboBox()
    box.setEditable(True)
    box.setInsertPolicy(QtWidgets.QComboBox.NoInsert)
    # As wide as the panel gives it (names are read, not guessed from a few letters).
    box.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
    box.setMinimumContentsLength(12)
    box.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
    for option in options:
        box.addItem(option)
    box.lineEdit().setPlaceholderText(placeholder)
    box.setCurrentText(str(value))
    box.activated.connect(lambda i: set_value(box.itemText(i)))
    box.lineEdit().editingFinished.connect(lambda: set_value(box.currentText().strip()))
    return box


def names_editor(value, options, set_value, placeholder="", pick_tip="Pick", details=None):
    """A list of names (space separated) typed or picked: the "+" menu checks names on and off."""
    w = QtWidgets.QWidget()
    row = QtWidgets.QHBoxLayout(w)
    row.setContentsMargins(0, 0, 0, 0)
    row.setSpacing(4)
    line = QtWidgets.QLineEdit(str(value))
    line.setPlaceholderText(placeholder)
    line.editingFinished.connect(lambda: set_value(" ".join(line.text().replace(",", " ").split())))
    pick = QtWidgets.QToolButton()
    pick.setText("+")
    pick.setToolTip(pick_tip)
    pick.setPopupMode(QtWidgets.QToolButton.InstantPopup)
    menu = QtWidgets.QMenu(pick)
    current = str(value).replace(",", " ").split()
    for option in options:
        action = menu.addAction(option)
        action.setCheckable(True)
        action.setChecked(option in current)
        if details and option in details:
            action.setToolTip(details[option])

        def toggle(on, n=option):
            names = [x for x in line.text().replace(",", " ").split() if x != n]
            if on:
                names.append(n)
            line.setText(" ".join(names))
            set_value(" ".join(names))
        action.toggled.connect(toggle)
    if not options:
        menu.addAction("(none yet)").setEnabled(False)
    pick.setMenu(menu)
    row.addWidget(line, 1)
    row.addWidget(pick)
    w.line, w.menu = line, menu
    return w


class MeshCombo(QtWidgets.QComboBox):
    """A menu of scene meshes, listed again each time it opens (the scene changes under it):
    `choices()` gives [(label, mesh shape)]; `current` (a mesh shape, or None: "none") is shown."""

    def __init__(self, choices, current, parent=None):
        super(MeshCombo, self).__init__(parent)
        self.choices, self.current = choices, current
        self.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.setMinimumContentsLength(8)
        self.fill()

    def fill(self):
        self.blockSignals(True)
        try:
            self.clear()
            choices = self.choices()
            if self.current is None or not any(mesh == self.current for _, mesh in choices):
                self.addItem("none" if self.current is None else self.current.split("|")[-1], self.current)
            for label, mesh in choices:
                self.addItem(label, mesh)
            index = self.findData(self.current)
            self.setCurrentIndex(max(index, 0))
        finally:
            self.blockSignals(False)

    def showPopup(self):
        self.fill()
        super(MeshCombo, self).showPopup()


def later(fn):
    """Run fn once the current event is over. Anything that rebuilds the scene
    (deleting the node, its ports and links) must not run inside an event of
    one of those items: Qt still uses the item after the handler returns."""
    def run(*_):
        try:
            fn()
        except RuntimeError:  # the editor went away meanwhile
            pass
    QtCore.QTimer.singleShot(0, run)


class EdgeItem(QtWidgets.QGraphicsPathItem):
    """A link, drawn from the output port it reads (`output`: a name; None: the node's first)."""

    def __init__(self, src_item, dst_item, input_name, output=None):
        super(EdgeItem, self).__init__()
        self.src_item, self.dst_item, self.input_name = src_item, dst_item, input_name
        self.src_port = src_item.outputs.get(output) if output else src_item.output
        if self.src_port is None:  # an output the node no longer has: drawn from its first
            self.src_port = src_item.output
        self.output = self.src_port.name
        self.color = TYPE_COLORS.get(self.src_port.port_type, QtGui.QColor(200, 200, 200))
        self.setZValue(-1)
        self.setFlag(QtWidgets.QGraphicsItem.ItemIsSelectable)
        self.setAcceptHoverEvents(True)
        self.hovered = False
        self.cutting = False  # crossed by an Alt+Shift cut in progress: drawn as about to go
        self.pins = []        # the PinItems it runs through, source first (the editor's rebuild sets them)
        src_item.edges.append(self)
        dst_item.edges.append(self)
        self.update_path()

    def points(self):
        """Where it runs: its source port, its pins, its input port."""
        return ([self.src_port.scene_center()] + [pin.scenePos() for pin in self.pins] +
                [self.dst_item.inputs[self.input_name].scene_center()])

    def update_path(self):
        points = self.points()
        path = QtGui.QPainterPath(points[0])
        for a, b in zip(points, points[1:]):
            curve(path, a, b)
        self.setPath(path)

    def piece_at(self, pos):
        """Which piece between its points (0: source to the first pin) runs nearest a scene point:
        where a pin put there goes in its route."""
        points = self.points()
        best, nearest = 0, None
        for k, (a, b) in enumerate(zip(points, points[1:])):
            piece = curve(QtGui.QPainterPath(a), a, b)
            for i in range(17):
                d = piece.pointAtPercent(i / 16.0) - pos
                d2 = d.x() * d.x() + d.y() * d.y()
                if nearest is None or d2 < nearest:
                    best, nearest = k, d2
        return best

    def mouseDoubleClickEvent(self, event):
        # A pin where it was double-clicked (Unreal's reroute node): the link runs through it.
        editor, link, at = self.dst_item.editor, self.link(), event.scenePos()
        index = self.piece_at(at)
        later(lambda: editor.add_pin(link, at, index))()
        event.accept()

    def bypassed(self):
        """Whether either end's node is bypassed (its wires dashed, as the card dims)."""
        model = self.dst_item.editor.model
        try:
            return bool(model.bypassed(self.src_item.name) or model.bypassed(self.dst_item.name))
        except (gm.GraphError, KeyError):
            return False

    def link(self):
        """(src, output, dst, input): the link this draws, as add_node and insert_on_wire take it."""
        return self.src_item.name, self.output, self.dst_item.name, self.input_name

    def shape(self):
        stroker = QtGui.QPainterPathStroker()
        stroker.setWidth(12)  # easy to click
        return stroker.createStroke(self.path())

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        color = ERROR if self.cutting else SELECTED if self.isSelected() else (self.color.lighter(125) if self.hovered else self.color)
        width = 3.0 if self.isSelected() or self.hovered or self.cutting else 2.2
        if self.bypassed():  # into or out of a bypassed node: short dashes at the card's opacity
            color = QtGui.QColor(color)
            color.setAlphaF(color.alphaF() * BYPASSED_OPACITY)
            pen = QtGui.QPen(color, width, QtCore.Qt.CustomDashLine)
            pen.setDashPattern([1.5, 2.0])
        elif self.dst_item.inputs[self.input_name].is_param:
            pen = QtGui.QPen(color, width, QtCore.Qt.DashLine)
        else:
            pen = QtGui.QPen(color, width)
        pen.setCapStyle(QtCore.Qt.RoundCap)
        painter.setPen(pen)
        painter.drawPath(self.path())

    def hoverEnterEvent(self, event):
        self.hovered = True
        self.update()

    def hoverLeaveEvent(self, event):
        self.hovered = False
        self.update()


def bezier(a, b):
    return curve(QtGui.QPainterPath(a), a, b)


def curve(path, a, b):
    """A link's curve from a to b onto `path` (at a): leaving a and reaching b level, as a link
    leaves an output and reaches an input (and a pin, as Unreal's reroute nodes)."""
    dx = max(abs(b.x() - a.x()) * 0.5, 50.0)
    path.cubicTo(a + QtCore.QPointF(dx, 0), b - QtCore.QPointF(dx, 0), b)
    return path


GRID = 24  # the canvas's dot grid (GraphScene.GRID_STEP): what Snap puts nodes and pins on


def snapped(point):
    """A scene point on the grid."""
    return QtCore.QPointF(round(point.x() / GRID) * GRID, round(point.y() / GRID) * GRID)


class PinPort(object):
    """A pin as a link drag's start (dragged from its nub): an output port of the signal it carries,
    drawn from the pin, so the scene's link drag treats it as that output."""

    is_output = True
    is_param = False
    highlight = False

    def __init__(self, pin_item):
        self.pin_item = pin_item
        edge = pin_item.edges[0]
        self.node_item, self.name = edge.src_item, edge.output
        self.port_type, self.value_kind = edge.src_port.port_type, edge.src_port.value_kind

    def scene_center(self):
        return self.pin_item.scenePos()

    def update(self):
        pass


class PinItem(QtWidgets.QGraphicsItem):
    """A reroute point links run through (Unreal's Blueprint reroute node): moved and selected like a
    node, in the color of the signal it carries; dropped on another pin of the same signal, it
    joins it, so links from one output run through it as a bundle."""

    RADIUS = 6.0
    NUB = 10.0  # the nub links are dragged out of: right of the pin, as an output port

    def __init__(self, editor, pin, color):
        super(PinItem, self).__init__()
        self.editor, self.pin, self.color = editor, pin, color
        self.edges = []
        self.hovered = False
        self.setZValue(-0.5)  # over the links, under the nodes
        self.setAcceptHoverEvents(True)
        self.setFlags(QtWidgets.QGraphicsItem.ItemIsMovable | QtWidgets.QGraphicsItem.ItemIsSelectable |
                      QtWidgets.QGraphicsItem.ItemSendsGeometryChanges)
        self.setToolTip("Pin: drag to route the link; drop on another pin of the same link to join them. "
                        "Drag from its nub (right) for another link of this signal; drop a link on it to feed "
                        "its links from there. Delete removes it")
        self.setPos(*editor.model.pin_position(pin))

    def boundingRect(self):
        r = self.RADIUS + 3.0
        return QtCore.QRectF(-r, -r, 2 * r + self.NUB, 2 * r)

    def shape(self):
        path = QtGui.QPainterPath()
        r = self.RADIUS + 3.0
        path.addEllipse(QtCore.QRectF(-r, -r, 2 * r, 2 * r))
        path.addEllipse(QtCore.QPointF(self.NUB, 0), 5.0, 5.0)
        return path

    def nub_at(self, scene_pos):
        """Whether a scene point is on the nub (the right of the pin, where links are dragged out)."""
        local = self.mapFromScene(scene_pos)
        return local.x() > self.RADIUS * 0.5 and abs(local.y()) < self.RADIUS + 3.0

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        r = self.RADIUS + (1.0 if self.hovered else 0.0)
        painter.setPen(QtGui.QPen(SELECTED if self.isSelected() else self.color.darker(160), 2.0))
        painter.setBrush(self.color.lighter(120) if self.hovered else self.color)
        painter.drawEllipse(QtCore.QPointF(0, 0), r, r)
        if self.hovered or self.isSelected():  # the nub, as an output port's arrow
            painter.setPen(QtCore.Qt.NoPen)
            painter.setBrush(self.color)
            painter.drawPolygon(QtGui.QPolygonF([QtCore.QPointF(self.NUB - 3.0, -4.0), QtCore.QPointF(self.NUB + 3.0, 0.0),
                                                 QtCore.QPointF(self.NUB - 3.0, 4.0)]))

    def hoverEnterEvent(self, event):
        self.hovered = True
        self.update()

    def hoverLeaveEvent(self, event):
        self.hovered = False
        self.update()

    def itemChange(self, change, value):
        if change == QtWidgets.QGraphicsItem.ItemPositionChange and self.editor.snapping(self):
            return snapped(value)
        if change == QtWidgets.QGraphicsItem.ItemPositionHasChanged:
            for edge in self.edges:
                edge.update_path()
            self.editor.pin_moved(self.pin, self.pos())
        return super(PinItem, self).itemChange(change, value)

    def mouseReleaseEvent(self, event):
        super(PinItem, self).mouseReleaseEvent(event)
        onto = next((i for i in self.collidingItems() if isinstance(i, PinItem)), None)
        if onto is not None:
            editor, keep, drop = self.editor, onto.pin, self.pin
            later(lambda: editor.merge_pins(keep, drop))()  # it rebuilds the scene: after this event


# Tab search's entries for the groom's painted maps: MAP_ENTRY + the map's name, in this section.
MAP_ENTRY = "map:"
PAINTED_MAPS = "Painted maps"


class NodeSearch(QtWidgets.QFrame):
    """Type to find a node type (name, category or description); Enter or click adds it."""

    def __init__(self, editor, at, on_link=None):
        super(NodeSearch, self).__init__(editor, QtCore.Qt.Popup)
        self.editor, self.at, self.on_link = editor, at, on_link
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        self.field = QtWidgets.QLineEdit()
        self.field.setPlaceholderText("Add node: type to search (a name, or what it does)")
        self.list = QtWidgets.QListWidget()
        self.list.setMinimumSize(theme.px(300), theme.px(240))
        # What the current entry does, under the list (each entry's tooltip says the same).
        self.about = QtWidgets.QLabel()
        self.about.setObjectName("nodeSearchAbout")
        self.about.setProperty("role", "about")
        self.about.setWordWrap(True)
        self.about.setMinimumHeight(theme.px(34))
        layout.addWidget(self.field)
        layout.addWidget(self.list)
        layout.addWidget(self.about)
        self.entries = []
        self.labels = {}
        catalog = editor.catalog
        refresh_library(catalog)
        for category, types in categories(catalog):
            for op_type in types:
                spec = catalog.spec(op_type) if catalog is not None else {}
                params = spec.get("params", [])
                self.labels[op_type] = (spec.get("label") or op_type, spec.get("description", ""))
                self.entries.append((op_type, category, {
                    "label": (spec.get("label") or pretty(op_type)).lower(),
                    "keywords": " ".join([spec.get("keywords") or "", category]).lower(),
                    "params": " ".join([p["name"] for p in params] + [pretty(p["name"]) for p in params]).lower(),
                    "about": " ".join([category, spec.get("description", "")]).lower(),
                    "details": " ".join(p.get("description", "") for p in params).lower()}))
        # The groom's painted maps: each a Map node reading it, found by its name (or map, mask, paint).
        for m in editor.painted_maps():
            key = MAP_ENTRY + m["name"]
            kind = {"guides": "a layer on the guides", "vertices": "on the scalp's vertices",
                    "texture": "a texture"}.get(m.get("kind"), m.get("kind", ""))
            self.labels[key] = ("Map: %s" % m["name"], "A Map node reading the painted map %s (%s): wire it to a "
                                "setting to drive it" % (m["name"], kind))
            self.entries.append((key, PAINTED_MAPS, {
                "label": ("map %s %s" % (m["name"], pretty(m["name"]))).lower(),
                "keywords": "painted map mask paint texture %s" % PAINTED_MAPS.lower(),
                "params": "", "about": kind, "details": ""}))
        self.field.textChanged.connect(self.fill)
        self.field.returnPressed.connect(self.accept_first)
        self.list.itemActivated.connect(lambda item: self.choose(item.data(QtCore.Qt.UserRole)))
        self.list.itemClicked.connect(lambda item: self.choose(item.data(QtCore.Qt.UserRole)))
        self.list.currentItemChanged.connect(self.show_about)
        self.fill("")

    def fill(self, text):
        text = text.lower().strip()
        self.list.clear()
        scored = []
        for op_type, category, words in self.entries:
            score = search_rank(text, op_type, words)
            if score is not None:
                scored.append((score, op_type, category))
        order = dict((t, i) for i, (t, _c, _w) in enumerate(self.entries))  # the menu's order breaks ties
        for _, _i, op_type, category in sorted((sc, order.get(t, 0), t, c) for sc, t, c in scored):
            label, about = self.labels.get(op_type, (op_type, ""))
            item = QtWidgets.QListWidgetItem("%s    %s" % (label, category))
            item.setData(QtCore.Qt.UserRole, op_type)
            item.setToolTip(about)
            color = CATEGORY_COLORS.get(category)
            if color is not None:
                item.setForeground(color.lighter(120))
            self.list.addItem(item)
        if self.list.count():
            self.list.setCurrentRow(0)
        else:
            self.about.setText("No node matches. Try what it does: symmetry, density, frizz, collision...")

    def show_about(self, item, _previous=None):
        if item is None:
            return
        self.about.setText(self.labels.get(item.data(QtCore.Qt.UserRole), ("", ""))[1])

    def keyPressEvent(self, event):
        if event.key() in (QtCore.Qt.Key_Down, QtCore.Qt.Key_Up):
            row = self.list.currentRow() + (1 if event.key() == QtCore.Qt.Key_Down else -1)
            self.list.setCurrentRow(max(0, min(self.list.count() - 1, row)))
            return
        super(NodeSearch, self).keyPressEvent(event)

    def accept_first(self):
        item = self.list.currentItem()
        if item is not None:
            self.choose(item.data(QtCore.Qt.UserRole))

    def choose(self, op_type):
        self.close()
        params = name = None
        if op_type.startswith(MAP_ENTRY):  # a painted map: a Map node reading it, named after it
            params = {"name": op_type[len(MAP_ENTRY):]}
            op_type, name = "Map", self.editor.model.unique_name(params["name"])
        # Where Tab was pressed: on a wire, inserted into it; else wired after the selected node
        # as Add Node wires it.
        self.editor.add_node(op_type, at=self.at, wire=True, on_link=self.on_link, params=params, name=name)


class BackdropItem(QtWidgets.QGraphicsItem):
    """A labelled frame behind nodes: moving it moves the nodes inside; drag the corner to resize."""

    GRIP = 14

    def __init__(self, editor, data):
        super(BackdropItem, self).__init__()
        self.editor, self.data = editor, data
        self.setZValue(-20)
        self.setFlags(QtWidgets.QGraphicsItem.ItemIsMovable | QtWidgets.QGraphicsItem.ItemIsSelectable |
                      QtWidgets.QGraphicsItem.ItemSendsGeometryChanges)
        self.setPos(data["x"], data["y"])
        self._carried = []
        self._resizing = None

    def rect(self):
        return QtCore.QRectF(0, 0, self.data["w"], self.data["h"])

    def boundingRect(self):
        return self.rect().adjusted(-2, -2, 2, 2)

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        color = QtGui.QColor(*self.data.get("color", definitions.get("graph/backdropColors")[0]["rgb"]))
        fill = QtGui.QColor(color)
        fill.setAlpha(45)
        painter.setPen(QtGui.QPen(SELECTED if self.isSelected() else color, 1.5))
        painter.setBrush(fill)
        painter.drawRoundedRect(self.rect(), 10, 10)
        header = QtGui.QColor(color)
        header.setAlpha(120)
        clip = QtGui.QPainterPath()
        clip.addRoundedRect(self.rect(), 10, 10)
        painter.save()
        painter.setClipPath(clip)
        painter.fillRect(QtCore.QRectF(0, 0, self.data["w"], 28), header)
        painter.restore()
        painter.setFont(theme.font(14, bold=True, base=painter.font()))
        painter.setPen(TEXT)
        painter.drawText(QtCore.QRectF(12, 0, self.data["w"] - 24, 28), QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft,
                         self.data.get("title", "Backdrop"))
        painter.setPen(QtGui.QPen(color.lighter(130), 1.5))
        r = self.rect()
        for k in (4, 8, 12):
            painter.drawLine(QtCore.QPointF(r.right() - k, r.bottom() - 2), QtCore.QPointF(r.right() - 2, r.bottom() - k))

    def _in_grip(self, pos):
        r = self.rect()
        return pos.x() > r.right() - self.GRIP and pos.y() > r.bottom() - self.GRIP

    def mousePressEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton and self._in_grip(event.pos()):
            self._resizing = (event.scenePos(), self.data["w"], self.data["h"])
            event.accept()
            return
        # Nodes inside move with it.
        area = self.sceneBoundingRect()
        self._carried = [(i, i.pos() - self.pos()) for i in self.editor.items.values()
                         if area.contains(i.sceneBoundingRect().center())]
        super(BackdropItem, self).mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if self._resizing:
            start, w, h = self._resizing
            d = event.scenePos() - start
            self.prepareGeometryChange()
            self.data["w"], self.data["h"] = max(120.0, w + d.x()), max(80.0, h + d.y())
            self.update()
            return
        super(BackdropItem, self).mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        self._resizing = None
        self._carried = []
        super(BackdropItem, self).mouseReleaseEvent(event)
        self.editor._layout_dirty = True
        self.editor.commit_layout()

    def itemChange(self, change, value):
        if change == QtWidgets.QGraphicsItem.ItemPositionHasChanged:
            self.data["x"], self.data["y"] = self.pos().x(), self.pos().y()
            for item, offset in self._carried:
                item.setPos(self.pos() + offset)
            self.editor._layout_dirty = True
        return super(BackdropItem, self).itemChange(change, value)

    def mouseDoubleClickEvent(self, event):
        title, ok = QtWidgets.QInputDialog.getText(None, "Backdrop", "Title:", text=self.data.get("title", ""))
        if ok:
            self.data["title"] = title
            self.editor._layout_dirty = True
            self.editor.commit_layout()
            self.update()

    # What the backdrop's right-click menu runs (engine/ui/graph_ui.json's backdropMenu).
    ACTIONS = {
        "backdrop.rename": lambda self: self.mouseDoubleClickEvent(None),
        "backdrop.color": "_recolor",
        "backdrop.delete": lambda self: later(lambda editor=self.editor, data=self.data: editor.remove_backdrop(data)),
    }

    def contextMenuEvent(self, event):
        menu = QtWidgets.QMenu()
        widgets.fill_menu(menu, definitions.get("graph/backdropMenu"), definitions.binder(self.ACTIONS, self))
        qt.exec_(menu, event.screenPos())

    def _recolor(self, rgb):
        self.data["color"] = rgb
        self.editor._layout_dirty = True
        self.editor.commit_layout()
        self.update()


class GraphScene(QtWidgets.QGraphicsScene):
    """Link dragging (with snapping and highlighted targets), cutting links (Alt+Shift drag),
    deletion, a dot grid."""

    def __init__(self, editor):
        super(GraphScene, self).__init__()
        self.editor = editor
        self.setBackgroundBrush(CANVAS)
        self._drag_port = None     # the output (or input) a new link is dragged from
        self._drag_line = None
        self.drag_reason = None    # why the port under a dragged link can't take it (its tooltip)
        self._detach = None        # (node, input) whose existing link is being moved
        self._drop_wire = None     # the wire under an unwired node being dragged: dropped, it goes in it
        self._cut_line = None      # an Alt+Shift cut in progress: its blade, where it began, the links it crosses
        self._cut_start = None
        self._cut_edges = set()
        self._grid = {}            # (zoom, dot color) -> QBrush: the dot tile at that zoom

    GRID_STEP = GRID

    def grid_brush(self, zoom):
        """A brush tiling one dot (or, with the theme's canvasStyle "lines", one grid line each
        way) per GRID_STEP scene units, drawn at device resolution for `zoom` (so the dots stay
        crisp); made once per zoom level."""
        key = (round(zoom, 3), GRID_DOT.rgba(), CANVAS_STYLE)
        brush = self._grid.get(key)
        if brush is None:
            size = max(1, int(round(self.GRID_STEP * zoom)))
            tile = QtGui.QPixmap(size, size)
            tile.fill(QtCore.Qt.transparent)
            painter = QtGui.QPainter(tile)
            if CANVAS_STYLE == "lines":  # through the tile's middle, where the dots sit
                painter.fillRect(QtCore.QRectF(0, size // 2, size, 1), GRID_DOT)
                painter.fillRect(QtCore.QRectF(size // 2, 0, 1, size), GRID_DOT)
            else:
                painter.setRenderHint(QtGui.QPainter.Antialiasing)
                painter.setPen(QtCore.Qt.NoPen)
                painter.setBrush(GRID_DOT)
                painter.drawEllipse(QtCore.QPointF(size / 2.0, size / 2.0), 0.9, 0.9)  # one dot, whole, per tile
            painter.end()
            brush = QtGui.QBrush(tile)
            brush.setTransform(QtGui.QTransform.fromScale(self.GRID_STEP / float(size), self.GRID_STEP / float(size)))
            if len(self._grid) > 32:
                self._grid.clear()
            self._grid[key] = brush
        return brush

    def drawBackground(self, painter, rect):
        super(GraphScene, self).drawBackground(painter, rect)
        zoom = painter.worldTransform().m11()
        if CANVAS_STYLE == "plain" or self.GRID_STEP * zoom < 4:  # zoomed far out: the dots would merge into a haze
            return
        painter.save()
        painter.setPen(QtCore.Qt.NoPen)
        painter.setBrushOrigin(QtCore.QPointF(0, 0))
        painter.fillRect(rect, self.grid_brush(zoom))
        painter.restore()

    def ports(self):
        return [i for i in self.items() if isinstance(i, PortItem)]

    def port_at(self, pos):
        for item in self.items(pos):
            if isinstance(item, PortItem):
                return item
        return None

    def compatible(self, start, port):
        if port.node_item is start.node_item or port.is_output == start.is_output:
            return False
        out, inp = (start, port) if start.is_output else (port, start)
        if gm.types_match(out.port_type, inp.port_type, out.value_kind, inp.value_kind):
            return True
        return inp.is_param and self.editor.value_kind_for_input(out.node_item.name, inp.node_item.name, inp.name) is not None

    def incompatible_reason(self, start, port):
        """Why a link from `start` can't end on `port` (said while hovering it), None when it can."""
        if port is start or self.compatible(start, port):
            return None
        if port.node_item is start.node_item:
            return "A node can't feed itself"
        if port.is_output == start.is_output:
            return "Link an output to an input (both are %ss)" % ("output" if port.is_output else "input")
        out, inp = (start, port) if start.is_output else (port, start)
        return "%s.%s gives %s; %s.%s takes %s" % (out.node_item.name, out.name, out.port_type,
                                                  inp.node_item.name, inp.name, inp.port_type)

    def _begin(self, port, scene_pos):
        self._drag_port = port
        pen = QtGui.QPen(TYPE_COLORS.get(port.port_type, QtGui.QColor(230, 230, 230)), 2.2, QtCore.Qt.DashLine)
        self._drag_line = self.addPath(QtGui.QPainterPath(), pen)
        self._drag_line.setZValue(10)
        for p in self.ports():
            p.highlight = self.compatible(port, p)
            p.update()
        self._update_line(scene_pos)

    def _update_line(self, pos):
        a = self._drag_port.scene_center()
        target = self._snap(pos)
        b = target.scene_center() if target is not None else pos
        self._drag_line.setPath(bezier(a, b) if self._drag_port.is_output else bezier(b, a))

    def _snap(self, pos):
        """The compatible port under the cursor, else the nearest one within SNAP."""
        under = self.port_at(pos)
        if under is not None and under.highlight:
            return under
        best, best_d = None, SNAP
        for p in self.ports():
            if not p.highlight:
                continue
            d = QtCore.QLineF(p.scene_center(), pos).length()
            if d < best_d:
                best, best_d = p, d
        return best

    def _end(self):
        try:
            for p in self.ports():
                p.highlight = False
                p.update()
            if self._drag_line is not None:
                self.removeItem(self._drag_line)
        except RuntimeError:  # the scene was rebuilt under the drag
            pass
        finally:
            self._drag_port = self._drag_line = None

    def cancel_drag(self):
        """Drop a link drag in progress (before a rebuild, or when a popup takes the mouse)."""
        if self._drag_port is not None or self._drag_line is not None:
            self._end()
        if self._cut_line is not None:
            self._end_cut()

    # A cut (Maya Hypershade's Alt+Shift drag): a straight blade from where the drag began, every
    # link it crosses drawn as about to go, and on release those links disconnected as one edit.

    @staticmethod
    def cuts(event):
        wanted = QtCore.Qt.AltModifier | QtCore.Qt.ShiftModifier
        return event.button() == QtCore.Qt.LeftButton and (event.modifiers() & wanted) == wanted

    def _begin_cut(self, pos):
        pen = QtGui.QPen(ERROR, 2.0, QtCore.Qt.DashLine)
        pen.setCapStyle(QtCore.Qt.RoundCap)
        self._cut_start = pos
        self._cut_edges = set()
        self._cut_line = self.addPath(QtGui.QPainterPath(), pen)
        self._cut_line.setZValue(20)

    def _move_cut(self, pos):
        blade = QtGui.QPainterPath()
        blade.moveTo(self._cut_start)  # (not QPainterPath(point): some Qt versions start a subpath at 0, 0 too)
        blade.lineTo(pos)
        stroker = QtGui.QPainterPathStroker()
        stroker.setWidth(2.0)
        stroke = stroker.createStroke(blade)
        crossed = {i for i in self.items(stroke.boundingRect()) if isinstance(i, EdgeItem) and i.shape().intersects(stroke)}
        for edge in self._cut_edges ^ crossed:
            edge.cutting = edge in crossed
            edge.update()
        self._cut_edges = crossed
        self._cut_line.setPath(blade)

    def _end_cut(self):
        """Ends the cut; returns the links it crossed."""
        links = [edge.link() for edge in self._cut_edges]
        for edge in self._cut_edges:
            edge.cutting = False
            edge.update()
        self.removeItem(self._cut_line)
        self._cut_line = self._cut_start = None
        self._cut_edges = set()
        return links

    def mousePressEvent(self, event):
        if self.cuts(event):
            self.clearSelection()
            self._begin_cut(event.scenePos())
            event.accept()
            return
        pin = self.pin_at(event.scenePos())
        if pin is not None and pin.edges and pin.nub_at(event.scenePos()) and event.button() == QtCore.Qt.LeftButton:
            event.accept()  # a link out of the pin's nub (Blueprints'), of the signal it carries
            self._detach = None
            self._begin(PinPort(pin), event.scenePos())
            return
        port = self.port_at(event.scenePos())
        if port is not None and event.button() == QtCore.Qt.LeftButton:
            if self._drag_port is not None:
                # Recover if a transient window intercepted the previous mouse release.
                self._end()
                self._detach = None
                self.drag_reason = None
                QtWidgets.QToolTip.hideText()
            event.accept()  # else the view also starts a rubber-band selection
            model = self.editor.model
            source = None if port.is_output else model.source_of(port.node_item.name, port.name)
            if source is not None and source in self.editor.items:
                # Dragging a linked input picks the link up, from its source's output it reads.
                self._detach = (port.node_item.name, port.name)
                item = self.editor.items[source]
                self._begin(item.outputs.get(model.output_of(port.node_item.name, port.name), item.output),
                            event.scenePos())
            else:
                self._detach = None
                self._begin(port, event.scenePos())
            return
        super(GraphScene, self).mousePressEvent(event)

    def hover_reason(self, pos, screen_pos=None):
        """While a link is dragged: the port under the cursor that can't take it says why (a
        tooltip, and the status line). Returns the reason, None over nothing or a fitting port."""
        under = self.port_at(pos)
        reason = self.incompatible_reason(self._drag_port, under) if under is not None else None
        if reason != self.drag_reason:
            self.drag_reason = reason
            if reason:
                if screen_pos is not None:
                    # Keep the tooltip away from the port receiving the mouse release. A tooltip
                    # under the cursor can intercept it and leave the scene stuck in wire-drag mode.
                    QtWidgets.QToolTip.showText(screen_pos + QtCore.QPoint(18, 18), reason, self.editor.view)
                self.editor.report(reason)
            else:
                QtWidgets.QToolTip.hideText()
        return reason

    def mouseMoveEvent(self, event):
        if self._cut_line is not None:
            self._move_cut(event.scenePos())
            event.accept()
            return
        if self._drag_port is not None:
            self._update_line(event.scenePos())
            self.hover_reason(event.scenePos(), event.screenPos())
            event.accept()
            return
        super(GraphScene, self).mouseMoveEvent(event)
        self._hover_drop_wire(event.scenePos())

    def pin_at(self, pos):
        """The pin at a scene point, or None."""
        return next((i for i in self.items(pos) if isinstance(i, PinItem)), None)

    def edge_at(self, pos, exclude=None):
        """The wire at a scene point (none of `exclude`'s, a NodeItem's own), or None."""
        return next((i for i in self.items(pos) if isinstance(i, EdgeItem) and
                     (exclude is None or i not in exclude.edges)), None)

    def dragged_node(self):
        """The one node being dragged with no wires of its own (one dropped on a wire goes in it)."""
        grabbed = self.mouseGrabberItem()
        selected = [i for i in self.selectedItems() if isinstance(i, NodeItem)]
        if isinstance(grabbed, NodeItem) and selected == [grabbed] and not grabbed.edges:
            return grabbed
        return None

    def edge_under_node(self, node):
        """The wire any part of `node` lies over (not its own), the one nearest its middle; or None."""
        rect = node.sceneBoundingRect()
        wires = [i for i in self.items(rect, QtCore.Qt.IntersectsItemShape)
                 if isinstance(i, EdgeItem) and i not in node.edges]
        if not wires:
            return None
        middle = rect.center()

        def distance(wire):  # to the nearest of points along it
            path = wire.path()
            return min(QtCore.QLineF(middle, path.pointAtPercent(k / 32.0)).length() for k in range(33))
        return min(wires, key=distance)

    def _hover_drop_wire(self, pos):
        """While an unwired node is dragged: the wire it lies over lights up (dropped: inserted)."""
        node = self.dragged_node()
        wire = self.edge_under_node(node) if node is not None else None
        if wire is not self._drop_wire:
            for edge in (self._drop_wire, wire):
                if edge is not None:
                    edge.hovered = edge is wire
                    edge.update()
            self._drop_wire = wire

    def mouseReleaseEvent(self, event):
        if self._cut_line is not None and event.button() == QtCore.Qt.LeftButton:
            self._move_cut(event.scenePos())
            links = self._end_cut()
            event.accept()
            if links:
                self.editor.cut_links(links)
            return
        if self._drag_port is not None:
            start, end, detach = self._drag_port, self._snap(event.scenePos()), self._detach
            refused = self.hover_reason(event.scenePos()) if end is None else None
            self._end()
            self._detach = None
            self.drag_reason = None
            QtWidgets.QToolTip.hideText()
            event.accept()
            if refused:
                # Dropped on a port that can't take it: nothing changes (a picked-up link stays).
                self.editor.report("Not linked: %s" % refused)
            elif end is not None:
                out, inp = (start, end) if start.is_output else (end, start)
                if isinstance(out, PinPort):
                    self.editor.link_from_pin(out.pin_item.pin, inp.node_item.name, inp.name)
                else:
                    self.editor.link(out.node_item.name, inp.node_item.name, inp.name, detach=detach, output=out.name)
            elif self.pin_at(event.scenePos()) is not None and not isinstance(start, PinPort):
                # Dropped on a pin: an output feeds the pin's links from there; an input (or a link's
                # end picked up off one) takes the pin's signal.
                pin = self.pin_at(event.scenePos()).pin
                if detach:
                    self.editor.link_from_pin(pin, *detach)
                elif start.is_output:
                    self.editor.feed_pin(pin, start.node_item.name, start.name)
                else:
                    self.editor.link_from_pin(pin, start.node_item.name, start.name)
            elif detach:
                self.editor.unlink(*detach)  # a picked-up link dropped on nothing: removed
            elif not start.is_output:
                self.editor.unlink(start.node_item.name, start.name)
            return
        node, wire = self.dragged_node(), self._drop_wire
        self._drop_wire = None
        if wire is not None:
            wire.hovered = False
            wire.update()
        super(GraphScene, self).mouseReleaseEvent(event)
        self.editor.commit_layout()
        if node is not None and wire is not None:
            editor, name, link = self.editor, node.name, wire.link()
            later(lambda: editor.insert_on_wire(name, link))()  # it rebuilds the scene: after this event

    def keyPressEvent(self, event):
        ctrl = bool(event.modifiers() & QtCore.Qt.ControlModifier)
        key = event.key()
        selected = [i.name for i in self.selectedItems() if isinstance(i, NodeItem)]
        if undo_key(event):  # reached when the editor's shortcut did not take the key first
            self.editor.undo()
        elif redo_key(event):
            self.editor.redo()
        elif key in (QtCore.Qt.Key_Delete, QtCore.Qt.Key_Backspace):
            self.editor.delete_selected()
        elif ctrl and key == QtCore.Qt.Key_C:
            self.editor.copy_selected()
        elif ctrl and key == QtCore.Qt.Key_V:
            self.editor.paste()
        elif ctrl and key == QtCore.Qt.Key_D:
            self.editor.duplicate(selected)
        elif ctrl and key == QtCore.Qt.Key_G and event.modifiers() & QtCore.Qt.ShiftModifier:
            self.editor.expand_selected()
        elif ctrl and key == QtCore.Qt.Key_G:
            self.editor.collapse_selected()
        elif key == QtCore.Qt.Key_V and len(selected) == 1:
            self.editor.view_node(None if self.editor.viewed == selected[0] else selected[0])
        elif key == QtCore.Qt.Key_B and selected:
            self.editor.set_bypass(selected, not all(self.editor.model.bypassed(n) for n in selected))
        else:
            super(GraphScene, self).keyPressEvent(event)
            return
        event.accept()

    def contextMenuEvent(self, event):
        if any(isinstance(i, (NodeItem, PortItem, BackdropItem)) for i in self.items(event.scenePos())):
            super(GraphScene, self).contextMenuEvent(event)  # the item's own menu
            return
        event.accept()
        self.editor.show_search(event.scenePos(), event.screenPos())


class GraphView(QtWidgets.QGraphicsView):
    """Wheel zoom, Alt+right-drag zoom (as in Maya's viewports: right or down zooms in),
    middle-drag (or Alt+drag) pan anywhere, F frames the selection (or everything), A frames
    everything."""

    # The canvas: far past any graph, so panning and zooming aren't held to the nodes.
    CANVAS = 100000.0

    def __init__(self, scene):
        super(GraphView, self).__init__(scene)
        make_room_for_cache()
        scene.setSceneRect(-self.CANVAS, -self.CANVAS, 2 * self.CANVAS, 2 * self.CANVAS)
        self.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        self.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        self.setRenderHint(QtGui.QPainter.Antialiasing)
        # The whole view painted on every change (the nodes from their cached pictures, the dots from
        # one cached tile): a cached background, or a partial update, scrolled old pixels along
        # and left larger pictures of the nodes behind while zooming.
        self.setViewportUpdateMode(QtWidgets.QGraphicsView.FullViewportUpdate)
        self.setDragMode(QtWidgets.QGraphicsView.RubberBandDrag)
        self.setTransformationAnchor(QtWidgets.QGraphicsView.AnchorUnderMouse)
        self.setFocusPolicy(QtCore.Qt.StrongFocus)  # a click gives it the keys (Delete, Ctrl+Z...)
        self._pan = None
        self._zoom = None  # an Alt+right-drag: (its press point, the scene point there, the zoom then)
        self._snapshot = None  # the view's picture an Alt+right-drag scales (paintEvent)
        self._zoomed = False  # the right button's release ended a zoom: no add menu
        self.auto_frame = True  # keep the graph framed on resize until the user zooms or pans
        self.setTransform(QtGui.QTransform.fromScale(theme.scale(), theme.scale()))  # 1:1 at Maya's UI scale
        self.setAcceptDrops(True)  # .glnode files dropped in: imported into the library and added

    @staticmethod
    def node_files(event):
        """The .glnode files a drag carries."""
        mime = event.mimeData()
        if not mime.hasUrls():
            return []
        return [u.toLocalFile() for u in mime.urls() if u.toLocalFile().lower().endswith(".glnode")]

    def dragEnterEvent(self, event):
        if self.node_files(event):
            event.acceptProposedAction()
        else:
            super(GraphView, self).dragEnterEvent(event)

    def dragMoveEvent(self, event):
        if self.node_files(event):
            event.acceptProposedAction()
        else:
            super(GraphView, self).dragMoveEvent(event)

    def dropEvent(self, event):
        files = self.node_files(event)
        if not files:
            super(GraphView, self).dropEvent(event)
            return
        event.acceptProposedAction()
        at = self.mapToScene(qt.event_pos(event))
        editor = self.scene().editor
        later(lambda: editor.import_nodes(files, at=(at.x(), at.y())))()

    def wheelEvent(self, event):
        factor = 1.15 if event.angleDelta().y() > 0 else 1 / 1.15
        zoom = self.transform().m11() * factor
        if self.ZOOM_MIN * theme.scale() <= zoom <= self.ZOOM_MAX * theme.scale():
            self.scale(factor, factor)
        self.auto_frame = False

    ZOOM_MIN, ZOOM_MAX = 0.05, 4.0  # x Maya's interface scale
    ZOOM_RATE = 0.005  # Alt+right-drag: the zoom doubles every ~140 px

    def zoom_drag(self, pos):
        """Alt+right-drag: zoom by how far right and down the drag went, the point pressed staying
        under the cursor."""
        start, anchor, zoom = self._zoom
        moved = (pos.x() - start.x()) + (pos.y() - start.y())
        least, most = self.ZOOM_MIN * theme.scale(), self.ZOOM_MAX * theme.scale()
        zoom = min(max(zoom * math.exp(moved * self.ZOOM_RATE), least), most)
        self.setTransform(QtGui.QTransform.fromScale(zoom, zoom))
        off = self.mapFromScene(anchor) - start
        self.horizontalScrollBar().setValue(self.horizontalScrollBar().value() + off.x())
        self.verticalScrollBar().setValue(self.verticalScrollBar().value() + off.y())
        self.auto_frame = False
        self.viewport().update()

    # The longest side, in pixels, of the picture an Alt+right-drag scales (a bigger graph is drawn
    # into it a little softer, sharp again on release).
    ZOOM_PICTURE_MAX = 4096

    def zoom_picture(self):
        """The scene, its dots too, over the whole graph and three views' worth round the one shown,
        at the view's zoom and the screen's resolution (capped at ZOOM_PICTURE_MAX): what an
        Alt+right-drag scales, so zooming out shows the rest of the graph. (picture, where its
        top-left was in the view, view pixels a picture pixel spans)."""
        port = self.viewport()
        ratio = port.devicePixelRatioF()
        shown = self.mapToScene(port.rect()).boundingRect()
        area = shown.adjusted(-shown.width(), -shown.height(), shown.width(), shown.height())
        graph = self.scene().itemsBoundingRect()
        if not graph.isEmpty():
            area = area.united(graph.adjusted(-60, -60, 60, 60))
        zoom = self.transform().m11()
        scale = min(zoom * ratio, self.ZOOM_PICTURE_MAX / max(area.width(), area.height(), 1.0))
        picture = QtGui.QPixmap(max(1, int(math.ceil(area.width() * scale))), max(1, int(math.ceil(area.height() * scale))))
        picture.fill(QtCore.Qt.transparent)
        painter = QtGui.QPainter(picture)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        painter.setRenderHint(QtGui.QPainter.TextAntialiasing)
        self.scene().render(painter, QtCore.QRectF(picture.rect()), area, QtCore.Qt.IgnoreAspectRatio)
        painter.end()
        return picture, self.viewportTransform().map(area.topLeft()), zoom / scale

    def paintEvent(self, event):
        """While an Alt+right-drag zooms: the graph's picture taken when it started, scaled about
        the point pressed (one blit a frame); else the scene."""
        if self._snapshot is None or self._zoom is None:
            super(GraphView, self).paintEvent(event)
            return
        start, _anchor, zoom = self._zoom
        picture, corner, span = self._snapshot
        k = self.transform().m11() / zoom
        port = self.viewport()
        painter = QtGui.QPainter(port)
        painter.fillRect(port.rect(), self.scene().backgroundBrush())
        painter.setRenderHint(QtGui.QPainter.SmoothPixmapTransform)
        p = QtCore.QPointF(start)
        target = QtCore.QRectF(p + (corner - p) * k, QtCore.QSizeF(picture.width() * span * k, picture.height() * span * k))
        painter.drawPixmap(target, picture, QtCore.QRectF(picture.rect()))
        painter.end()

    def contextMenuEvent(self, event):
        if self._zoomed or self._zoom is not None or event.modifiers() & QtCore.Qt.AltModifier:
            self._zoomed = False
            event.accept()  # the end of an Alt+right-drag zoom, not a menu
            return
        super(GraphView, self).contextMenuEvent(event)

    def resizeEvent(self, event):
        super(GraphView, self).resizeEvent(event)
        if self.auto_frame:
            self.frame_readable()

    def frame(self):
        """Fit the selected nodes in the view, or the whole graph when none are selected."""
        selected = [i for i in self.scene().selectedItems() if isinstance(i, NodeItem)]
        if not selected:
            self.frame_all()
            return
        rect = QtCore.QRectF()
        for item in selected:
            rect = rect.united(item.sceneBoundingRect())
        self.frame_rect(rect)

    def frame_all(self):
        """Fit the graph in the view, never zooming in past 1:1 (at Maya's interface scale) nor out
        past READABLE: a long graph is then centered at a size its names read at (pan for the ends)."""
        rect = self.scene().itemsBoundingRect()
        self.frame_rect(rect)
        least = self.READABLE * theme.scale()
        if not rect.isEmpty() and self.transform().m11() < least:
            self.setTransform(QtGui.QTransform.fromScale(least, least))
            self.centerOn(rect.center())

    # The least zoom a graph opens at (x Maya's interface scale): node names stay readable.
    READABLE = 0.5

    def frame_readable(self):
        """The whole graph when it fits at a readable size; else a readable zoom on its output
        end, where look nodes go (A fits everything, pan for the rest)."""
        rect = self.scene().itemsBoundingRect()
        if rect.isEmpty():
            return
        self.frame_rect(rect)
        least = self.READABLE * theme.scale()
        if self.transform().m11() >= least:
            return
        self.setTransform(QtGui.QTransform.fromScale(least, least))
        editor = self.scene().editor

        def node_of(op_type):
            return next((item.sceneBoundingRect() for name, item in editor.items.items()
                         if editor.model is not None and editor.model.node_type(name) == op_type), None)
        output = node_of("Output") or rect
        start = node_of("Interpolate")
        width = self.viewport().width() / least
        if start is not None and output.right() - start.left() + 80 <= width:
            x = (start.left() + output.right()) / 2  # Interpolate to Output: where look nodes go
        else:
            x = output.right() + 40 - width / 2  # the output node near the right edge
        self.centerOn(QtCore.QPointF(x, rect.center().y()))

    def frame_rect(self, rect):
        rect = rect.adjusted(-40, -40, 40, 40)
        if rect.isEmpty():
            return
        self.fitInView(rect, QtCore.Qt.KeepAspectRatio)
        if self.transform().m11() > theme.scale():
            self.setTransform(QtGui.QTransform.fromScale(theme.scale(), theme.scale()))
            self.centerOn(rect.center())

    def keyPressEvent(self, event):
        if event.key() == QtCore.Qt.Key_F:
            self.frame()
            return
        if event.key() == QtCore.Qt.Key_A and not event.modifiers():
            self.frame_all()
            return
        if event.key() == QtCore.Qt.Key_Tab:
            cursor = self.mapFromGlobal(QtGui.QCursor.pos())
            self.scene().editor.show_search(self.mapToScene(cursor), QtGui.QCursor.pos())
            return
        super(GraphView, self).keyPressEvent(event)

    def focusNextPrevChild(self, forward):
        return False  # Tab opens the add menu instead of moving focus

    def mousePressEvent(self, event):
        if event.button() == QtCore.Qt.RightButton and event.modifiers() & QtCore.Qt.AltModifier:
            pos = qt.event_pos(event)
            self._zoom = (pos, self.mapToScene(pos), self.transform().m11())
            self._zoomed = False
            # A picture of the graph, scaled while the drag zooms (Qt 5's item-coordinate caches
            # drew 1.5x too large on a 150% screen).
            self._snapshot = self.zoom_picture()
            self.setCursor(QtCore.Qt.SizeVerCursor)
            event.accept()
            return
        self._zoomed = False
        if event.button() == QtCore.Qt.MiddleButton or (
                event.button() == QtCore.Qt.LeftButton and event.modifiers() & QtCore.Qt.AltModifier
                and not event.modifiers() & QtCore.Qt.ShiftModifier):  # (Alt+Shift: the scene cuts links)
            self._pan = qt.event_pos(event)
            self.setCursor(QtCore.Qt.ClosedHandCursor)
            return
        super(GraphView, self).mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if self._zoom is not None:
            self.zoom_drag(qt.event_pos(event))
            return
        if self._pan is not None:
            delta = qt.event_pos(event) - self._pan
            self.auto_frame = False
            self._pan = qt.event_pos(event)
            self.horizontalScrollBar().setValue(self.horizontalScrollBar().value() - delta.x())
            self.verticalScrollBar().setValue(self.verticalScrollBar().value() - delta.y())
            return
        super(GraphView, self).mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if self._zoom is not None and event.button() == QtCore.Qt.RightButton:
            self._zoom = None
            self._snapshot = None
            self._zoomed = True
            self.unsetCursor()
            self.viewport().update()  # the nodes drawn sharp at the zoom reached
            event.accept()
            return
        if self._pan is not None and event.button() in (QtCore.Qt.MiddleButton, QtCore.Qt.LeftButton):
            self._pan = None
            self.unsetCursor()
            return
        super(GraphView, self).mouseReleaseEvent(event)


# ---------------------------------------------------------------------------
# Parameter panel


class ParamPanel(QtWidgets.QScrollArea):
    """Widgets for the selected node's parameters, generated from its spec."""

    def __init__(self, editor):
        super(ParamPanel, self).__init__()
        self.editor = editor
        self.setWidgetResizable(True)
        self.setMinimumWidth(theme.px(300))
        # The params shrink to the panel's width (shrinkable()); a scroll bar only if something can't.
        self.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAsNeeded)
        self.show_node(None)

    # Folded sections by (operator type, section) for the session: a section folded on one Clump
    # is folded on every Clump shown after. Unset: Advanced folded, the groups open.
    folded = {}

    editing_script = None  # the script node whose editor the panel shows (show_script_editor)
    script_panel = None

    def show_node(self, name):
        if name is not None and name == self.editing_script:
            # Its editor stays while it is edited (a rebuild, an apply, the groom evaluating).
            panel = self.script_panel
            if panel is None or not widgets.alive(panel) or panel.node != name or self.widget() is not panel:
                self.show_script_editor(name)
            return
        self.editing_script = None
        self.node = name
        self.key_widgets = {}  # param -> its number field(s): keyed params show their state and value per frame
        self.sections = []  # [(section, its header or None, [(spec, row)])]: update_visibility's
        self.row_labels = []
        self.row_label_of = {}   # param -> its label (hidden while its fill field shows the name)
        self.inside_labels = {}  # param -> (name, tip) its fill field shows (a number's row)
        self.inside_fields = {}  # param -> the fill field showing its name
        self.built_units = {}  # param -> the unit its row shows, for a param whose unit depends on another
        body = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(body)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        form.setVerticalSpacing(theme.px(8))
        form.setContentsMargins(theme.px(10), theme.px(4), theme.px(10), theme.px(10))
        self.error_line = None
        actions = None
        if name is None:
            empty = QtWidgets.QLabel("Select a node to edit it.\n\nTab or right-click the canvas to add one; "
                                     "drag from a port to wire it." if self.editor.model is not None else
                                     "Select a groom in the viewport (or pick one above) to edit its graph.")
            empty.setWordWrap(True)
            empty.setProperty("role", "about")
            form.addRow(empty)
        else:
            model = self.editor.model
            op_type = model.node_type(name)
            # The node's actions (graph_ui.json's nodeActions and Maya's own: bake, export, simulate...)
            # pinned over the params with its name, in view however far they scroll.
            actions = self.node_actions(name)
            if actions is not None:
                actions.layout().insertWidget(0, self.title_row(name, op_type))
            form.addRow(self.header(name, op_type, title=actions is None))
            # The node's error (or warning) from the last evaluation, in plain words under its header.
            self.error_line = QtWidgets.QLabel()
            self.error_line.setObjectName("nodeError")
            self.error_line.setWordWrap(True)
            self.error_line.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
            form.addRow(self.error_line)
            self.update_error()
            note = self.editor.note_of(name)
            if note:  # the node does nothing as set (a Clump with no centers): a grey line, not an error
                line = QtWidgets.QLabel(u"ⓘ " + note)
                line.setObjectName("nodeNote")
                line.setWordWrap(True)
                line.setStyleSheet("color: %s;" % NOTE.name())
                form.addRow(line)
            resolution = self.editor.resolution_note(name)
            if resolution:  # a Scatter's or Resample's: which Resample sets its strands' resolution
                line = QtWidgets.QLabel(u"ⓘ " + resolution)
                line.setObjectName("resolutionNote")
                line.setWordWrap(True)
                line.setStyleSheet("color: rgb(150, 154, 160);")
                form.addRow(line)
            self.expression = None
            if op_type == "Expression":  # a code block, its variables and the library, not text fields
                from groomlab.ui import expression_editor
                self.expression = expression_editor.ExpressionPanel(
                    self.editor, name, lambda p, v, n=name: self.set_expression_param(n, p, v))
                form.addRow(self.expression)
            scene = self.scene_inputs(op_type)
            if scene:
                form.addRow(scene)
            rows = []  # (spec, label, widget), laid out by section below
            for spec in model.node_spec(name)["params"]:
                if op_type in ("Compound", "Script") and spec["name"] == "definition":
                    continue  # edited by grouping and expose() (a script node: Edit Node...), not as text
                if op_type == "Expression" and spec["name"] in ("expression", "variables"):
                    continue  # in the expression panel above
                if (op_type, spec["name"]) in PLUMBING and model.param(name, spec["name"]) == spec["default"]:
                    continue  # names of the host's inputs: only shown once changed
                driver = model.driver_of(name, spec["name"])
                if driver and len(model.output_names(driver)) > 1:  # which output: "mask.r"
                    driver = "%s.%s" % (driver, model.output_of(name, spec["name"]))
                text, tip = self.param_label_and_tip(name, spec)
                field_driven = driver and (spec.get("mappable") or spec.get("colorField")) and not gm.value_port(spec)
                if spec["kind"] in ("int", "float") and (field_driven or not driver):
                    # A number: a fill field with the name inside it (plain_editor), no label column.
                    self.inside_labels[spec["name"]] = (text, tip)
                if field_driven:
                    # Driven by a field node: the param's own value multiplies the field (a 0-1
                    # mask on a density of 100 gives 0-100), so it stays editable, with the driver named.
                    widget = QtWidgets.QWidget()
                    row = QtWidgets.QHBoxLayout(widget)
                    row.setContentsMargins(0, 0, 0, 0)
                    row.addWidget(self.editor_for(name, spec), 1)
                    times = QtWidgets.QLabel('<span style="color:%s">× <b>%s</b></span>' % (DRIVEN.name(), driver))
                    times.setToolTip("Multiplied by the field node %s: this value where the field is 1, 0 where it "
                                     "is 0" % driver)
                    row.addWidget(times)
                elif driver:
                    # A ramp driven by a Curve node (its shape comes from there), or any param a
                    # Value input sets (its value comes from there).
                    widget = QtWidgets.QLabel('<span style="color:%s">← driven by <b>%s</b></span>'
                                              % (DRIVEN.name(), driver))
                elif op_type == "Scatter" and spec["name"] == "length" and guides_shape(model, name):
                    # Interpolate gives the strands the guides' shapes and lengths: say so beside it,
                    # or it reads as a second length control beside the Length node's.
                    widget = QtWidgets.QWidget()
                    row = QtWidgets.QHBoxLayout(widget)
                    row.setContentsMargins(0, 0, 0, 0)
                    row.addWidget(self.editor_for(name, spec), 1)
                    unused = QtWidgets.QLabel('<span style="color:%s">guides set it</span>' % MUTED.name())
                    unused.setToolTip(SCATTER_LENGTH_TIP)
                    row.addWidget(unused)
                elif spec.get("mappable") and self.editor.groom and gm.linkable(spec):
                    # A mappable param: its field and the Paint chip (as in the app), right-click > Paint's choices.
                    widget = QtWidgets.QWidget()
                    row = QtWidgets.QHBoxLayout(widget)
                    row.setContentsMargins(0, 0, 0, 0)
                    row.setSpacing(theme.px(4))
                    row.addWidget(self.editor_for(name, spec), 1)
                    row.addWidget(self.paint_chip(name, spec["name"]), 0, QtCore.Qt.AlignTop)
                else:
                    widget = self.editor_for(name, spec)
                label = QtWidgets.QLabel(text)
                label.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
                label.customContextMenuRequested.connect(
                    lambda pos, p=spec["name"], l=label: self.param_menu(name, p, l.mapToGlobal(pos)))
                for w in (label, widget):
                    w.setProperty("glParam", spec["name"])  # S over it keys this param
                label.setToolTip(tip)
                widget.setToolTip(tip)
                self.row_label_of[spec["name"]] = label
                # The fill field carries the name (and the ◆ of an exposed param): the label column
                # stays only for the rows without one (a map's fields, a driven param, a checkbox...).
                if self.label_inside(spec["name"]):
                    label.hide()
                rows.append((spec, label, widget))
            self.add_sections(form, op_type, rows)
            if not model.node_spec(name)["params"]:
                form.addRow(QtWidgets.QLabel("No parameters."))
        # The old panel may hold the button or menu whose click got us here:
        # it is deleted once that event is over, not now.
        old = self.takeWidget()
        if old is not None:
            old.hide()
            old.deleteLater()
        self.setWidget(body)
        self.set_pinned(actions)
        self.editor.watch_text_edits(body)
        shrinkable(body)
        self.align_labels()
        self.update_visibility()
        self.refresh_keys()

    def update_error(self):
        """The red line under the node's header: its error from the last evaluation (or an amber
        warning), in plain words; hidden when it has none. Called again after each evaluation."""
        line = self.error_line
        if line is None or not widgets.alive(line):
            return
        node = self.node
        error = self.editor.node_errors.get(node) if node else None
        warning = self.editor.node_warnings.get(node) if node else None
        text = error or warning
        line.setVisible(bool(text))
        if not text:
            return
        line.setText((u"⚠ Error: " if error else u"⚠ ") + plain_message(text))
        line.setToolTip(text)
        line.setStyleSheet("color: %s; font-weight: bold;" % (ERROR if error else WARNING).name())

    def param_label_and_tip(self, name, spec):
        """A param's row name (with a ◆ when exposed as an input) and its tooltip: what it does and
        what its right-click offers."""
        model = self.editor.model
        text = pretty(spec["name"]) + (" ◆" if spec["name"] in model.param_inputs(name) else "")
        tip = "%s: %s" % (spec["name"], spec["description"])
        if spec.get("channel") in ATTRIBUTE_TIPS:
            tip = ATTRIBUTE_TIPS[spec["channel"]] + "\n" + tip
        if spec.get("colorField"):
            tip += "\nRight-click: expose as an input, so a Map node's rgb output colors it per strand."
        elif gm.value_port(spec):
            tip += ("\nRight-click: expose as an input, so a Value node (or an Expression set to give a value) "
                    "sets it for the whole node.")
        elif gm.linkable(spec):
            tip += "\nRight-click: expose as an input, so a field node (Field, FieldNoise, ...) can drive it."
        if spec.get("kind") in KEYABLE_KINDS:
            why = model.keyable(name, spec["name"])
            tip += ("\nRight-click > Set Key, or S over it: key it (red: a key on this frame, pink: animated, "
                    "yellow: driven)" if why is None else "\nNot keyable: " + why)
        return text, tip

    def label_inside(self, param):
        """Whether the param's fill field shows its name now (a mappable param's does while it is a
        plain value, not while a map drives it)."""
        field = getattr(self, "inside_fields", {}).get(param)
        return field is not None and widgets.alive(field) and bool(field.label())

    def update_row_label(self, param):
        """Show the row's label again when its fill field is gone (a map picked), hide it when back."""
        label = getattr(self, "row_label_of", {}).get(param)
        if label is not None and widgets.alive(label) and label.parent() is not None:
            label.setVisible(not self.label_inside(param))
            self.align_labels()

    def align_labels(self):
        """The param labels in one column: each as wide as the widest, styled as the panel styles them
        (the labels shown: a fill field holds its own name)."""
        labels = [l for l in getattr(self, "row_labels", []) if widgets.alive(l) and not l.isHidden()]
        for label in labels:
            label.ensurePolished()
        width = max([l.sizeHint().width() for l in labels] or [0])
        for label in labels:
            label.setMinimumWidth(width)

    def add_sections(self, form, op_type, rows):
        """The param rows by section (param_sections): the main params, each group under a header
        that folds it, Advanced (folded unless unfolded this session) last.

        The rows are a column of their own (a label right-aligned in one width, its field beside it),
        not form rows: a hidden row then takes no room at all (a form keeps its spacing)."""
        if not rows:
            return
        box = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(box)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(form.verticalSpacing())
        form.addRow(box)
        self.row_labels = [label for _, label, _ in rows]  # one width once styled (align_labels)
        specs = [spec for spec, _, _ in rows]
        by_name = {spec["name"]: (spec, label, widget) for spec, label, widget in rows}
        for section, members in param_sections(specs):
            header = None
            if section:
                header = QtWidgets.QPushButton()
                header.setObjectName("paramSection_" + section)
                header.setCheckable(True)
                header.setFlat(True)
                header.setProperty("role", "section")
                header.setChecked(not self.is_folded(op_type, section))
                self._title_section(header, section)
                header.toggled.connect(lambda on, s=section, h=header, t=op_type: self.fold(t, s, not on, h))
                column.addWidget(header)
            entries = []
            for spec in members:
                _, label, widget = by_name[spec["name"]]
                row = QtWidgets.QWidget()
                line = QtWidgets.QHBoxLayout(row)
                line.setContentsMargins(0, 0, 0, 0)
                line.setSpacing(theme.px(6))
                label.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
                label.setMinimumHeight(min(widget.sizeHint().height(), theme.px(26)))
                line.addWidget(label, 0, QtCore.Qt.AlignTop)
                line.addWidget(widget, 1)
                column.addWidget(row)
                entries.append((spec, row))
            self.sections.append((section, header, entries))

    @staticmethod
    def _title_section(header, section):
        header.setText(("▾  " if header.isChecked() else "▸  ") + section.upper().replace("&", "&&"))

    def is_folded(self, op_type, section):
        return ParamPanel.folded.get((op_type, section), section == ADVANCED)

    def fold(self, op_type, section, folded, header=None):
        """Fold (or open) a section of every node of this type, for the session."""
        ParamPanel.folded[(op_type, section)] = folded
        if header is not None and widgets.alive(header):
            self._title_section(header, section)
        self.update_visibility()

    def update_visibility(self):
        """Show the rows whose showWhen holds (in an open section), and the headers of sections with
        a row shown: after the panel is built and after each edit (a mode switched), no rebuild."""
        model, node = self.editor.model, self.node
        if node is None or model is None or node not in model.nodes or not getattr(self, "sections", None):
            return
        op_type = model.node_type(node)

        def value_of(param):
            try:
                return model.param(node, param)
            except gm.GraphError:
                return None

        specs = {spec["name"]: spec for spec in model.node_spec(node)["params"]}  # what a showWhen names
        # A param whose unit follows another (Noise's frequency by its space): built anew when it changed.
        for section, header, entries in self.sections:
            for spec, row in entries:
                if spec.get("unitWhen") and self.built_units.get(spec["name"]) != unit_of(spec, value_of):
                    QtCore.QTimer.singleShot(0, lambda n=node: self.node == n and self.show_node(n))
                    return
        for section, header, entries in self.sections:
            open_ = not section or not self.is_folded(op_type, section)
            any_shown = False
            for spec, row in entries:
                shown = shown_when(spec, value_of, specs.get)
                any_shown = any_shown or shown
                if widgets.alive(row):
                    row.setVisible(shown and open_)
            if header is not None and widgets.alive(header):
                header.setVisible(any_shown)

    def show_script_editor(self, name):
        """A script node's editor in the panel (script_editor.ScriptNodePanel): kept while the node is
        the one shown, until Done or another node. Returns it."""
        from groomlab.ui import script_editor
        self.node = name
        self.editing_script = name
        self.key_widgets = {}
        self.sections = []
        self.row_labels = []
        self.row_label_of, self.inside_labels, self.inside_fields = {}, {}, {}
        self.built_units = {}
        self.error_line = None
        self.expression = None
        self.script_panel = script_editor.ScriptNodePanel(self.editor, name)
        self.setWidget(self.script_panel)
        self.set_pinned(None)
        return self.script_panel

    def show_save_node(self, name):
        """Save as Node: a form in the panel (label, category, description, author, id, version,
        license, the exposed params to keep). Save writes the file and shows the node again; Cancel
        shows the node. Returns the form."""
        import groomlab_core
        model = self.editor.model
        self.node = name
        info = model.library_info(name) or {}
        definition = (model.nodes[name].get("params") or {}).get("definition") or {}
        exposed = [p["name"] for p in model.node_spec(name)["params"] if p["name"] in (definition.get("expose") or {})]
        body = QtWidgets.QWidget()
        body.setObjectName("saveNodeForm")
        form = QtWidgets.QFormLayout(body)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setContentsMargins(theme.px(10), theme.px(4), theme.px(10), theme.px(10))
        title = QtWidgets.QLabel("Save %s as a node" % name)
        title.setProperty("role", "title")
        form.addRow(title)
        about = QtWidgets.QLabel("It goes to your node library (%s) and shows in Add Node and Tab search in every "
                                 "groom, the standalone app and Blender. Grooms that use it keep a copy, so they open "
                                 "anywhere." % groomlab_core.node_user_dir())
        about.setWordWrap(True)
        about.setProperty("role", "about")
        form.addRow(about)
        import re
        words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", name).replace("_", " ")
        label = QtWidgets.QLineEdit(definition.get("label") or " ".join(w[:1].upper() + w[1:] for w in words.split()))
        label.setObjectName("saveNodeLabel")
        category = QtWidgets.QComboBox()
        category.setObjectName("saveNodeCategory")
        category.setEditable(True)
        titles = [c for c in CATEGORY_COLORS if c not in ("Output", "Compound")]
        for entry in groomlab_core.library_nodes()["nodes"]:
            if entry.get("categoryTitle") not in titles and entry.get("categoryTitle") != "My Nodes":
                titles.append(entry["categoryTitle"])
        category.addItems(["My Nodes"] + titles)
        category.setCurrentText(definition.get("category") or "My Nodes")
        category.setToolTip("Where Add Node lists it: a built-in category, or type your own")
        description = QtWidgets.QPlainTextEdit(definition.get("description") or "")
        description.setObjectName("saveNodeDescription")
        description.setPlaceholderText("What it does, in a line (the menus show it)")
        description.setFixedHeight(theme.px(60))
        author = QtWidgets.QLineEdit(definition.get("author") or "")
        author.setObjectName("saveNodeAuthor")
        node_id = QtWidgets.QLineEdit(info.get("id") or "")
        node_id.setObjectName("saveNodeId")
        node_id.setToolTip("Never changes once shared: a newer version of a node keeps its id (yourname.node_name)")
        version = QtWidgets.QSpinBox()
        version.setObjectName("saveNodeVersion")
        version.setRange(1, 1000000)
        license_ = QtWidgets.QLineEdit(definition.get("license") or "")
        license_.setObjectName("saveNodeLicense")
        license_.setPlaceholderText("Optional (CC0-1.0, CC-BY-4.0, your own words)")
        typed = {"id": bool(info)}

        def suggest():
            if not typed["id"]:
                node_id.blockSignals(True)
                node_id.setText(groomlab_core.suggest_node_id(label.text(), author.text()))
                node_id.blockSignals(False)
            versions = dict((e["id"], e["version"]) for e in groomlab_core.library_nodes()["nodes"])
            version.setValue(max(versions.get(node_id.text().strip(), 0), info.get("version", 0)) + 1
                             if node_id.text().strip() in versions or info else 1)
        node_id.textEdited.connect(lambda _t: typed.__setitem__("id", True))
        node_id.editingFinished.connect(suggest)
        label.textEdited.connect(lambda _t: suggest())
        author.textEdited.connect(lambda _t: suggest())
        suggest()
        form.addRow("Label", label)
        form.addRow("Category", category)
        form.addRow("Description", description)
        form.addRow("Author", author)
        form.addRow("Id", node_id)
        form.addRow("Version", version)
        form.addRow("License", license_)
        checks = []
        if exposed:
            heading = QtWidgets.QLabel("Params it shows")
            heading.setProperty("role", "about")
            form.addRow(heading)
            for param in exposed:
                box = QtWidgets.QCheckBox(pretty(param))
                box.setObjectName("saveNodeParam_" + param)
                box.setChecked(True)
                box.setToolTip("Unchecked: its value now is kept inside the node")
                checks.append((param, box))
                form.addRow(box)
        error = QtWidgets.QLabel()
        error.setObjectName("saveNodeError")
        error.setWordWrap(True)
        error.setStyleSheet("color: %s;" % ERROR.name())
        form.addRow(error)
        buttons = QtWidgets.QHBoxLayout()
        save = QtWidgets.QPushButton("Save")
        save.setObjectName("saveNodeSave")
        cancel = QtWidgets.QPushButton("Cancel")
        cancel.setObjectName("saveNodeCancel")
        buttons.addStretch(1)
        buttons.addWidget(cancel)
        buttons.addWidget(save)
        form.addRow(buttons)

        def accept():
            header = {"label": label.text().strip(), "category": category.currentText().strip(),
                      "description": description.toPlainText().strip(), "author": author.text().strip(),
                      "id": node_id.text().strip(), "version": version.value(), "license": license_.text().strip(),
                      "params": [p for p, box in checks if box.isChecked()]}
            if header["category"] == "My Nodes":
                header["category"] = ""
            why = groomlab_core.node_id_error(header["id"])
            if why:
                error.setText(why)
                return
            if self.editor.save_as_node(name, header) is None:
                error.setText(self.editor.status.text())
        save.clicked.connect(accept)
        cancel.clicked.connect(lambda _=False: self.show_node(name))
        self.setWidget(body)
        self.set_pinned(None)
        body.fields = {"label": label, "category": category, "description": description, "author": author,
                       "id": node_id, "version": version, "license": license_, "params": dict(checks),
                       "save": save, "cancel": cancel, "error": error}
        self.save_form = body
        return body

    def header(self, name, op_type, title=True):
        """The node's name (title_row; `title` False: pinned with its actions instead) and what it does."""
        box = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(box)
        column.setContentsMargins(0, 0, 0, theme.px(6))
        column.setSpacing(theme.px(4))
        if title:
            column.addWidget(self.title_row(name, op_type))
        library = self.editor.model.library_info(name) if op_type in ("Compound", "Script") else None
        about = library["description"] if library else describe(op_type, self.editor.catalog)
        if about:
            text = QtWidgets.QLabel(about)
            text.setWordWrap(True)
            text.setProperty("role", "about")
            column.addWidget(text)
        # Its own height, never squeezed when the params below overflow the panel.
        box.setSizePolicy(QtWidgets.QSizePolicy.Preferred, QtWidgets.QSizePolicy.Fixed)
        return box

    def title_row(self, name, op_type):
        """The node's name, its type in its category's color, Upgrade and Update when they apply."""
        box = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(box)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(8))
        title = QtWidgets.QLabel(name)
        title.setProperty("role", "title")
        title.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        row.addWidget(title)
        library = self.editor.model.library_info(name) if op_type in ("Compound", "Script") else None
        color = CATEGORY_COLORS.get(library_category(library) if library else category_of(op_type, self.editor.catalog), MUTED)
        pill = QtWidgets.QLabel(library["label"] if library else op_type)
        if library:
            pill.setToolTip("Library node %s, version %d%s" % (library["id"], library["version"],
                            "" if library.get("embedded", True) else " (this groom holds no copy of it)"))
        pill.setStyleSheet("QLabel { background: %s; color: %s; border-radius: %dpx; padding: %dpx %dpx; "
                           "font-weight: bold; }" % (color.name(), readable_on(color).name(), theme.px(8),
                                                     theme.px(2), theme.px(8)))
        row.addWidget(pill)
        row.addStretch(1)
        saved, newest = self.editor.model.node_version(name)
        if saved < newest:  # saved with an older behaviour of its operator: offer the newest
            upgrade = QtWidgets.QToolButton()
            upgrade.setObjectName("upgradeNode")
            upgrade.setText("Upgrade")
            upgrade.setProperty("role", "chip")
            upgrade.setToolTip(
                "This %s keeps the behaviour of version %d, as it was saved; %s nodes are at version %d now.\n"
                "Upgrade: it evaluates as new %s nodes do (its result may change). One undo step."
                % (op_type, saved, op_type, newest, op_type))
            upgrade.clicked.connect(lambda _=False, n=name: self.editor.upgrade_node(n))
            row.addWidget(upgrade)
        if library and library.get("updateAvailable"):  # the library has a newer copy: offer it
            update = QtWidgets.QToolButton()
            update.setObjectName("updateLibraryNode")
            update.setText("Update to v%d" % library["latest"])
            update.setProperty("role", "chip")
            update.setToolTip("Your node library has version %d of %s (this groom holds version %d).\n"
                              "Update: take it (its values kept where it still has them). One undo step."
                              % (library["latest"], library["label"], library["version"]))
            update.clicked.connect(lambda _=False, n=name: self.editor.update_library_node(n))
            row.addWidget(update)
        return box

    def set_expression_param(self, node, param, value):
        """An edit from the expression panel: into the graph, then the node's error (if any) under the code."""
        self.editor.set_param(node, param, value)
        panel = getattr(self, "expression", None)
        if panel is not None and widgets.alive(panel):
            panel.show_error()

    # -- What a source node reads from the scene ---------------------------------

    def scene_inputs(self, op_type):
        """For Scalp, ColliderMesh and Guides nodes: the mesh or guides they read in the scene, and
        a mesh field (mesh_field) or buttons to change them."""
        import groomlab
        from maya import cmds
        groom = self.editor.groom
        if op_type not in ("Scalp", "Guides", "ColliderMesh") or not groom or not cmds.objExists(groom):
            return None
        model, node = self.editor.model, self.node
        other_scalp = None  # a Scalp node reading another of the groom's scalps: its name
        if op_type == "Scalp" and model.param(node, "input") != "scalp":
            other_scalp = model.param(node, "input")
            if not groomlab.valid_scalp_name(other_scalp):
                return None  # another of the host's inputs: no Maya connection to show
        self.mesh_combo = self.mesh_pick = None
        box = QtWidgets.QFrame()
        t = theme.current()
        box.setStyleSheet("QFrame { background: %s; border-radius: %dpx; }"
                          % (t.color("ui.panel"), int(t.get("ui.radius", 8))))
        grid = QtWidgets.QGridLayout(box)
        grid.setContentsMargins(6, 6, 6, 6)
        s = groomlab.inputs_summary(groom)

        def line(row, label, value, buttons):
            grid.addWidget(QtWidgets.QLabel(label), row, 0)
            text = QtWidgets.QLabel("<b>%s</b>" % value)
            text.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
            grid.addWidget(text, row, 1)
            for i, (caption, slot, tip) in enumerate(buttons):
                b = QtWidgets.QPushButton(caption)
                b.setToolTip(tip)
                b.clicked.connect(lambda _=False, f=slot: f())  # not clicked's `checked` as an argument
                grid.addWidget(b, row, 2 + i)

        top = 0  # the mesh row's
        if op_type == "Scalp" and (other_scalp or s.get("scalps")):
            # Several scalps on the groom: which one the node reads (its `input`).
            grid.addWidget(QtWidgets.QLabel("Scalp"), 0, 0)
            grid.addWidget(self.scalp_choice(model.param(node, "input"), [o["name"] for o in s.get("scalps", [])]),
                           0, 1, 1, 3)
            top = 1
        if op_type == "ColliderMesh":
            name = model.param(node, "name")
            current = next((c["mesh"] for c in s["colliders"] if c["name"] == name), None)
            grid.addWidget(QtWidgets.QLabel("Mesh"), 0, 0)
            grid.addWidget(self.mesh_field("collider", current, name), 0, 1, 1, 3)
        elif other_scalp:
            # Another scalp of the groom (extraScalps): the mesh under its name, or none yet.
            current = next((o["mesh"] for o in s.get("scalps", []) if o["name"] == other_scalp), None)
            grid.addWidget(QtWidgets.QLabel("Mesh"), top, 0)
            grid.addWidget(self.mesh_field("extraScalp", current, other_scalp), top, 1, 1, 3)
        elif op_type == "Scalp":
            short = lambda n: n.split("|")[-1] if n else "none"
            grid.addWidget(QtWidgets.QLabel("Mesh"), top, 0)
            grid.addWidget(self.mesh_field("scalp", s["scalp"]), top, 1, 1, 3)
            # A rest pose only matters for a scalp that moves (skinned, blend shapes, a cache):
            # strands are placed on it so the same strands stay put while the scalp deforms.
            # Grooms on deformed scalps get one on their own; the row shows when it applies.
            moving = bool(s["scalp"]) and bool(cmds.ls(cmds.listHistory(s["scalp"]) or [],
                                                        type=("geometryFilter", "AlembicNode", "cacheFile")))
            if moving or s["rest"]:
                orig = s["scalp"] and groomlab._original_shape(s["scalp"])
                value = "auto (the undeformed scalp)" if s["rest"] and s["rest"] == orig else short(s["rest"])
                line(top + 1, "Rest pose", value, [
                    ("Set", lambda: self._scene_run(groomlab.set_rest_mesh, groom),
                     "The scalp before it deforms: strands are placed on it, so the same strands stay put "
                     "while the scalp moves. Without deformers (an Alembic cache, say): a copy at this frame"),
                    ("Clear", lambda: self._scene_run(groomlab.clear_rest_mesh, groom),
                     "No rest pose: strands are placed on the scalp as it is on each frame")])
        else:
            text = groomlab.guides_text(s) if s["groups"] else "no guides yet"
            if s["curves"]:
                text += " + %d curves" % s["curves"]
            line(0, "Groom has", text, [("Groups...", self.open_groups, "See the groups in the Groom Tools")])
        return box

    def scalp_choice(self, current, others):
        """A Scalp node's menu of the groom's scalps (scalp: its own, then the others by name):
        choosing one sets the node's input, and the panel shows that scalp's mesh."""
        combo = QtWidgets.QComboBox()
        combo.setObjectName("scalpChoice")
        combo.setToolTip("Which of the groom's scalps this node reads: a Scatter fed it grows hair on that mesh alone")
        names = ["scalp"] + [n for n in others if n != "scalp"]
        if current not in names:
            names.append(current)
        for n in names:
            combo.addItem("scalp (the groom's own)" if n == "scalp" else n, n)
        combo.setCurrentIndex(names.index(current))

        def chosen(index):
            value = combo.itemData(index)
            if value != current:
                self.editor.set_param(self.node, "input", value)
                later(lambda: self.node and self.show_node(self.node))
        combo.activated.connect(chosen)
        return combo

    def scene_soon(self):
        """The groom's scalp or colliders were rewired: a Scalp or ColliderMesh node's panel is
        shown again once the change is over (once for many: a collider's mesh and rest mesh)."""
        if getattr(self, "_scene_pending", False):
            return
        self._scene_pending = True

        def refresh():
            self._scene_pending = False
            if (widgets.alive(self) and self.node and self.node in self.editor.model.order
                    and self.editor.model.node_type(self.node) in ("Scalp", "ColliderMesh")):  # (any scalp)
                self.show_node(self.node)
        QtCore.QTimer.singleShot(0, refresh)

    def _scene_run(self, fn, *args):
        try:
            fn(*args)
        except (RuntimeError, ValueError) as e:
            self.editor.report(str(e))
            return
        self.show_node(self.node)

    def mesh_field(self, kind, current, name=None):
        """The scene mesh a Scalp or ColliderMesh node reads (the groom's inMesh, or the collider
        `name`'s colliderMeshes element): a menu of the scene's meshes (listed again each time it
        opens), an eyedropper that picks the next mesh selected in the viewport or the Outliner
        (groomlab.ui.pick, as the Attribute Editor's Pick), and Select. Choosing one rewires the
        connection through the same calls as the Attribute Editor (pick.set_input)."""
        from groomlab.ui import pick
        from maya import cmds
        groom = self.editor.groom
        current = (cmds.ls(current, long=True) or [None])[0] if current else None
        box = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(box)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(4))
        combo = MeshCombo(lambda: self._mesh_choices(kind, current, name), current)
        combo.setObjectName("meshField")
        combo.setToolTip("The mesh this node reads: choose another scene mesh (the menu lists them as it opens)")
        combo.activated.connect(lambda index: later(lambda: self.choose_mesh(kind, combo.itemData(index), name)))
        row.addWidget(combo, 1)
        dropper = QtWidgets.QToolButton()
        dropper.setObjectName("meshPick")
        dropper.setCheckable(True)
        dropper.setIcon(widgets.glyph("eyedropper"))
        dropper.setToolTip("Pick the mesh: click it in the viewport or the Outliner (Esc, or this again, cancels)")
        dropper.setChecked(bool(pick.armed(groom, pick.input_key(kind, name))))
        dropper.clicked.connect(lambda _=False: self.pick_mesh(kind, name))
        row.addWidget(dropper)
        select = QtWidgets.QPushButton("Select")
        select.setToolTip("Select the mesh in the scene")
        select.setEnabled(bool(current))
        select.clicked.connect(lambda _=False: current and cmds.objExists(current) and cmds.select(current))
        row.addWidget(select)
        self.mesh_combo, self.mesh_pick = combo, dropper
        return box

    def _mesh_choices(self, kind, current, name):
        """[(label, mesh shape)] for a mesh field: every scene mesh the input takes, and the one it has."""
        from groomlab.ui import pick
        groom = self.editor.groom
        out = []
        for mesh in pick.scene_meshes():
            if pick._same(mesh, current) or not pick.problem(groom, kind, mesh, name):
                out.append((pick.label(mesh), mesh))
        return out

    def choose_mesh(self, kind, mesh, name=None, ask=True):
        """A mesh chosen in a mesh field: onto the groom (pick.set_input), the panel shown again."""
        from groomlab.ui import pick
        if not mesh or not self.editor.groom:
            return
        current = self.mesh_combo.current if self.mesh_combo is not None and widgets.alive(self.mesh_combo) else None
        if pick._same(mesh, current) or not self._node_on_groom():
            return
        try:
            self.editor.report(pick.set_input(self.editor.groom, kind, mesh, name, ask=ask))
        except (RuntimeError, ValueError) as e:
            self.editor.report(str(e))
        self.show_node(self.node)

    def _node_on_groom(self):
        """The shown node on the groom (a ColliderMesh just added, edits not applied yet: the graph
        applied first), so the collider calls find it there. False when the graph can't be applied."""
        import groomlab
        stored = [n["name"] for n in groomlab.get_graph(self.editor.groom, expand_default=True).get("nodes", [])]
        return self.node in stored or self.editor.apply()

    def pick_mesh(self, kind, name=None, ask=True):
        """The mesh field's eyedropper (pick.pick_input): the panel shows the result when it ends."""
        from groomlab.ui import pick

        def ended(applied):
            later(lambda: self.node and self.show_node(self.node))
        if not self._node_on_groom():
            return "failed"
        result = pick.pick_input(self.editor.groom, kind, name, on_end=ended, ask=ask)
        if self.mesh_pick is not None and widgets.alive(self.mesh_pick):
            self.mesh_pick.setChecked(result == "armed")
        self.editor.report({"armed": "click %s in the viewport or the Outliner (Esc cancels)" % pick.WHAT[kind],
                            "cancelled": "pick cancelled"}.get(result, ""))
        return result

    def pick_position(self, name, param):
        """A position field's Pick (Transform's pivot): where the selected vertices, locator or transform
        are (pick.selected_position, world space as groom data), set as any edit of the field is.
        Returns the position, or None when nothing selected has one (said in the status line)."""
        from groomlab.ui import pick
        try:
            position, what = pick.selected_position(self.editor.groom)
        except ValueError as e:
            self.editor.report(str(e))
            return None
        value = [float(c) for c in position]
        self.editor.set_param(name, param, value)
        self.editor.report("%s.%s: (%.4g, %.4g, %.4g) from %s" % ((name, param) + tuple(value) + (what,)))
        later(lambda: self.node == name and self.show_node(name))  # the fields show it
        return value

    # -- Node actions: what a node needs from the scene, on the node itself -------

    # -- The pinned actions: over the params, in view however far they scroll ---------------

    pinned = None  # the node's actions (set_pinned), or None

    def set_pinned(self, widget):
        """`widget` (the node's actions) pinned over the params, in the scroll area's top margin; None
        takes the old one away (deleted once the click that got here is over)."""
        old = self.pinned
        if old is not None and widgets.alive(old) and old is not widget:
            old.hide()
            old.setParent(None)  # (out of the panel now, as takeWidget does with the params)
            old.deleteLater()
            self._retired = old  # kept until then: its button may be the one clicked
        self.pinned = widget
        if widget is not None:
            widget.setParent(self)
            shrinkable(widget)
            widget.show()
        self.place_pinned()

    def place_pinned(self):
        """The pinned actions across the top, as tall as they need at the panel's width; the params
        below them (the viewport's top margin)."""
        w = self.pinned
        if w is None or not widgets.alive(w):
            self.setViewportMargins(0, 0, 0, 0)
            return
        frame = self.frameWidth()
        # Clear of the vertical scroll bar's place whether it shows or not (no width that changes as
        # the params' length does).
        width = max(1, self.width() - 2 * frame - self.verticalScrollBar().sizeHint().width())
        height = w.heightForWidth(width) if w.hasHeightForWidth() else -1
        if height <= 0:
            height = w.sizeHint().height()
        w.setGeometry(frame, frame, width, height)
        self.setViewportMargins(0, height, 0, 0)

    def resizeEvent(self, event):
        super(ParamPanel, self).resizeEvent(event)
        self.place_pinned()

    def action_runner(self, fn, done=None, busy=None):
        """A button's click: `fn` run once the click is over, its changes into the editor's graph,
        then onto the groom (reloading rebuilds this panel, and the button that was clicked with it).
        `done`: what to report (a text, or a function of the result); `busy`: what a long one says
        while it runs (the panel disabled, the wait cursor)."""
        def act():
            try:
                if busy:
                    with widgets.busy([self], self.editor.status, busy):
                        result = fn()
                else:
                    result = fn()
            except (RuntimeError, ValueError) as e:
                self.editor.report(plain_message(e), trouble=True)
                return
            except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
                import traceback
                traceback.print_exc()  # the Script Editor: the details
                self.editor.report("%s (details in the Script Editor)" % plain_message(e), trouble=True)
                return
            if not self.editor.apply():
                return
            self.editor.reload()
            if done:
                self.editor.report(done(result) if callable(done) else done)

        def go(_=False):
            later(act)
        return go

    def node_action_table(self, name):
        """What graph_ui.json's nodeActions run in Maya for node `name`, on the editor's groom
        ({action id: a callable}): the Cards exports, Simulate's range, bake and clear, Bake Node to
        Guides, a Sculpt node's Clear, a compound's Ungroup and saves."""
        import groomlab
        from maya import cmds
        from groomlab.ui import menu
        editor = self.editor
        run = self.action_runner

        def card_maps():
            from groomlab.ui import card_maps_window
            try:
                card_maps_window.show(editor.groom)
            except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
                import traceback
                traceback.print_exc()
                editor.report("%s (details in the Script Editor)" % plain_message(e), trouble=True)

        def unreal():
            folder = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the Unreal cards",
                                      startingDirectory=groomlab.asset_folder("exports"))
            return groomlab.export_cards_unreal(editor.groom, folder[0]) if folder else None

        def unity():
            folder = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the Unity cards",
                                      startingDirectory=groomlab.asset_folder("exports"))
            return groomlab.export_cards_unity(editor.groom, folder[0]) if folder else None

        def gltf():
            path = cmds.fileDialog2(fileFilter="glTF binary (*.glb);;glTF (*.gltf)", dialogStyle=2, fileMode=0,
                                    caption="Export glTF Cards",
                                    startingDirectory="%s/%s_cards.glb" % (groomlab.asset_folder("exports"),
                                                                            groomlab.asset_name(editor.groom)))
            if not path:
                return None
            return groomlab.export_cards_gltf(editor.groom, path[0], binary=not path[0].lower().endswith(".gltf"))

        return {
            "export.cardMaps": lambda: later(card_maps),
            "export.unrealCards": run(unreal, lambda r: "wrote %s" % r["fbx"] if r else ""),
            "export.unityCards": run(unity, lambda r: "wrote %s" % r["fbx"] if r else ""),
            "export.gltfCards": run(gltf, lambda r: "wrote %s" % r[0] if r else ""),
            "sim.range": lambda: later(lambda: menu.simulate_range(groom=editor.groom)),
            "sim.bake": lambda: later(lambda: menu.bake_core_simulation(groom=editor.groom)),
            "sim.clear": lambda: later(lambda: menu.clear_core_simulation(groom=editor.groom)),
            "node.bake": lambda: later(lambda: editor.bake_to_guides(name)),
            "sculpt.clearEdits": lambda: later(lambda: (editor.clear_sculpt_edits(name), editor.reload())),
            "compound.ungroup": lambda: later(lambda: editor.expand([name])),
            "compound.saveAsNode": lambda: later(lambda: editor.show_save_node(name)),
            "compound.saveToLibrary": lambda: later(lambda: editor.save_to_library(name)),
        }

    def node_action_state(self, name):
        """The conditions graph_ui.json's nodeActions need for node `name`: "type.<its type>", and
        "node.library" for a node from the library."""
        model = self.editor.model
        state = {"type." + model.node_type(name)}
        if model.library_info(name):
            state.add("node.library")
        return state

    def node_actions(self, name):
        """A node's actions, pinned over its params (set_pinned): Maya's own first (Cards: stats, show,
        LOD, bake; Simulate: presets; Map: what it reads, Feed Param, Paint; Braid, Instance, HairColor,
        StrandMesh, Script), then graph_ui.json's nodeActions for its type (the Cards exports, Simulate's
        range, bake and clear, Bake Node to Guides, Clear Sculpt Edits, a compound's). None when it has
        none (or there is no groom)."""
        import groomlab
        from maya import cmds
        node_type = self.editor.model.node_type(name)
        groom = self.editor.groom
        buttons = []
        run = self.action_runner

        if node_type == "Cards":
            # What this node's cards cost now: cards, triangles, textures (its own share).
            try:
                stats = groomlab.card_stats(groom, node=name) if groom else None
            except RuntimeError:
                stats = None
            if stats and stats["cards"]:
                text = "%d cards \u00b7 %d triangles \u00b7 %d textures" % (stats["cards"], stats["triangles"],
                                                                         stats["textures"])
                if stats.get("feathers"):
                    text += " \u00b7 %d feathers" % stats["feathers"]
                info = QtWidgets.QLabel(text)
                info.setObjectName("nodeAction_CardStats")
                info.setToolTip("This node's cards as they are now (at cardDensity). Change them with the params, "
                                "or pick a level of detail. All Cards nodes share one atlas and one bake")
                buttons.append(info)
                if stats.get("stale"):
                    stale = QtWidgets.QLabel("\u26a0 Maps out of date: rebake")
                    stale.setObjectName("nodeAction_CardsStale")
                    stale.setStyleSheet("color: %s; font-weight: bold;" % WARNING.name())
                    stale.setToolTip(self.editor.CARDS_STALE)
                    buttons.append(stale)
                buttons.append(None)
            show = QtWidgets.QCheckBox("Show cards")
            show.setObjectName("nodeAction_ShowCards")
            show.setToolTip("Cards view (A/B): the cards instead of the strands, with the baked maps once "
                            "baked, and the hair cap")
            show.setChecked(bool(groom) and bool(cmds.getAttr(groom + ".displayCards")))
            show.toggled.connect(lambda on: groom and groomlab.compare_cards(groom, on))
            buttons.append(show)
            textures = QtWidgets.QCheckBox("Show textures")
            textures.setObjectName("nodeAction_ShowCardTextures")
            textures.setToolTip("Draw the cards with their baked maps (color, coverage cut-out, AO); off shows "
                                "the flat card geometry (the groom's displayCardTextures)")
            textures.setChecked(bool(groom) and bool(cmds.getAttr(groom + ".displayCardTextures")))
            textures.toggled.connect(lambda on: groom and groomlab.card_display(groom, textures=on))
            buttons.append(textures)
            lod = QtWidgets.QPushButton("LOD")
            lod.setToolTip("Set this node for a budget: triangles, card count, texture variations, atlas size")
            lod_menu = QtWidgets.QMenu(lod)
            try:  # a node carding only feathers takes the feather budgets
                feathers = bool(groom) and groomlab.card_node_kind(groom, name, self.editor.model) == "feathers"
            except Exception:
                feathers = False
            levels = ((("hero", "Hero (~30k triangles, 8 quads a feather, 4K)"),
                       ("mid", "Mid (~8k triangles, 4 quads a feather, 2K)"),
                       ("low", "Low (~2k triangles, a quad a feather, 1K)")) if feathers else
                      (("hero", "Hero (~50k triangles, 4K)"), ("mid", "Mid (~15k triangles, 2K)"),
                       ("low", "Low (~4k triangles, 1K)")))
            for key, label in levels:
                # Into the editor's graph (it is what gets saved), on this node.
                lod_menu.addAction(label, run(lambda k=key: groomlab.cards_preset(groom, k, node=name,
                                                                                  model=self.editor.model),
                                              lambda params, k=key: "%s cards: up to %d cards" % (k, params["maxCards"])))
            lod.setMenu(lod_menu)
            buttons.append(lod)
            buttons.append(None)

            buttons.append(("Bake Maps", "Depth, coverage, tangent, attribute, color, AO and normal maps for the atlas "
                            "(TGA, Unreal's hair card layout) into the scene's groomlab/cards folder; the cards "
                            "view shows them",
                            run(lambda: groomlab.bake_cards(groom),
                                lambda paths: "baked %d card maps into %s" % (len(paths), os.path.dirname(paths[0]) if paths else ""),
                                busy="Baking the card maps (depth, coverage, tangent, color, AO)...")))
            buttons.append(("Bake Scalp", "The groom baked onto its scalp's UVs (hair color and coverage) into the "
                            "scene's groomlab/cards folder, with a hair cap mesh that shows it under the cards: no "
                            "bare skin between low LOD cards",
                            run(lambda: groomlab.bake_scalp(groom),
                                lambda paths: "baked %d scalp maps and a hair cap" % len(paths),
                                busy="Baking the scalp maps and the hair cap...")))
        elif node_type == "Braid":
            def paths():
                curves = cmds.ls(selection=True, dag=True, type="nurbsCurve")
                if not curves:
                    raise ValueError("select curves drawn from the scalp along the braids")
                return groomlab.braid_paths_from_curves(
                    groom, name, [cmds.listRelatives(c, parent=True)[0] for c in curves], model=self.editor.model)
            buttons.append(("Paths from Selected Curves", "Selected curves, drawn from the scalp along the "
                            "braids, become this braid's paths", run(paths, lambda g: "braid paths added to %s" % g)))
        elif node_type == "Map":
            map_name = self.editor.model.param(name, "name") or "mask"
            if not any(c["from"] == name for c in self.editor.model.connections):
                hint = QtWidgets.QLabel("\u26a0 Feeds nothing yet: pick a param under Feed Param")
                hint.setObjectName("nodeAction_MaskUnconnected")
                hint.setStyleSheet("color: %s;" % WARNING.name())
                hint.setToolTip("A map changes the groom only through the params it feeds: expose a mappable param "
                                "as an input (Feed Param, or right-click the param) and connect this node to it")
                buttons.append(hint)
            kind = groomlab.map_kind(groom, map_name) if groom else ""
            if groom and not kind:
                missing = QtWidgets.QLabel("\u26a0 No map named %s yet: Groom Tools > Paint > New..." % map_name)
                missing.setObjectName("nodeAction_MapMissing")
                missing.setStyleSheet("color: %s;" % WARNING.name())
                buttons.append(missing)
            elif kind:
                where = {"guides": "a layer on the guides", "vertices": "a value per scalp vertex",
                         "texture": "a texture at the scalp's UVs"}.get(kind, kind)
                stored = next((m["texture"] for m in groomlab.maps(groom) if m["name"] == map_name), "") \
                    if kind == "texture" else ""
                # A zero-width space after each slash: a long path wraps at its folders, not mid-name.
                shown = stored.replace("\\", "/").replace("/", "/\u200b")
                info = QtWidgets.QLabel("%s: %s" % (map_name, where) + ("\n" + shown if shown else ""))
                info.setObjectName("nodeAction_MapInfo")
                info.setWordWrap(True)
                info.setProperty("role", "about")
                info.setToolTip("Where the map lives now (Convert to in Groom Tools > Paint changes it; the node "
                                "follows by name)" + ("\n" + groomlab.map_texture(groom, map_name) if stored else ""))
                buttons.append(info)
                if stored and map_name in groomlab.missing_map_files(groom):
                    lost = QtWidgets.QLabel("\u26a0 The image is missing: %s" % groomlab.map_texture(groom, map_name))
                    lost.setObjectName("nodeAction_MapImageMissing")
                    lost.setStyleSheet("color: %s;" % WARNING.name())
                    lost.setToolTip("Relink it to the file (or GroomLab > Utilities > Find Missing Files... looks "
                                    "for it in a folder); until then the map reads nothing")
                    buttons.append(lost)
                    buttons.append(("Relink Image...", "Point the map at its image: a PNG is used in place, another "
                                    "format copied as a PNG into the masks folder",
                                    lambda _=False, m=map_name: later(lambda: self.relink_map_image(m))))
                    buttons.append(None)
            feed = QtWidgets.QPushButton("Feed Param")
            feed.setObjectName("nodeAction_FeedParam")
            feed.setToolTip("Feed this map into a node's mappable param (exposed as an input and connected; the "
                            "param's own value multiplies the map)")
            feed_menu = QtWidgets.QMenu(feed)
            feed_menu.submenus = []  # explicit parents, kept: PySide would collect addMenu("title") results
            model = self.editor.model
            # A color map feeds from one of the node's outputs (a channel: packed masks); a gray one
            # from its first.
            color = kind in ("vertices", "texture") and groomlab.map_channels(groom, map_name) == 4
            for output in (model.output_names(name) if color else [None]):
                target = feed_menu
                if output is not None:
                    target = QtWidgets.QMenu("From %s" % output, feed_menu)
                    feed_menu.addMenu(target)
                    feed_menu.submenus.append(target)
                for other in model.order:
                    if other == name or model.node_type(other) in ("Map", "Expression", "FieldNoise", "FieldRemap",
                                                                     "FieldCombine", "Curve", "Control"):
                        continue
                    # rgb (the map's color) feeds color params; the rest feed numbers.
                    mappable = [p["name"] for p in model.node_spec(other)["params"]
                                if (p.get("colorField") if output == "rgb" else p.get("mappable"))]
                    if not mappable:
                        continue
                    sub = QtWidgets.QMenu("%s (%s)" % (other, model.node_type(other)), target)
                    target.addMenu(sub)
                    feed_menu.submenus.append(sub)
                    for param in mappable:
                        sub.addAction(param, run(lambda o=other, q=param, out=output: groomlab.feed_map_node(
                            groom, name, o, q, model=self.editor.model, output=out),
                            lambda fed, out=output: "%s%s feeds %s" % (name, "." + out if out else "", fed)))
            feed.setMenu(feed_menu)
            buttons.append(feed)

            def paint():
                from groomlab.ui import palette
                groomlab.paint_map(groom, map_name)
                palette.show(tab="paint")
                palette.select_map(map_name)
                return map_name
            buttons.append(("Paint...", "Paint this map in Groom Tools > Paint: the Paint brush on the guides, the "
                            "scalp's vertices or its texture, whichever kind the map is (white is 1, Ctrl paints "
                            "black); every param this node feeds follows each stroke",
                            run(paint, lambda c: "painting %s" % c)))
        elif node_type == "Script":
            buttons.append(("Edit Node...", "Edit this script node here: its script, params and header, live",
                            lambda _=False: later(lambda: self.editor.show_script_editor(name))))
            buttons.append(("Save to Library", "Write this script node to your node library (Add Node, Tab search; "
                            "the standalone app and Blender too), one version up when the library has it",
                            lambda _=False: later(lambda: self.editor.save_script_to_library(name))))
        elif node_type == "Sculpt" and groom:
            from groomlab import sculpts
            edited = sculpts.edited_guides(groom, name)
            brushing = sculpts.target(groom) == name
            buttons.append(QtWidgets.QLabel(
                "%s. %s" % ("%d guides sculpted" % edited if edited else "No strokes yet",
                            "Brushes shape this node's guides (shaping brushes)" if brushing else
                            "Select this node (or its row in Sculpt > Correctives) to brush its guides")))
        elif node_type == "Simulate":
            # One click to a preset's settings (the core's, every host's), into the editor's graph.
            from groomlab import sim
            preset = QtWidgets.QPushButton("Preset")
            preset.setObjectName("nodeAction_SimulatePreset")
            preset.setToolTip("Set this node's motion settings to a preset (bend, twist, damping, drag, root "
                              "stiffness and its profile, inertia, iterations); gravity, wind, collisions and "
                              "water stay as they are")
            preset_menu = QtWidgets.QMenu(preset)
            preset_menu.setToolTipsVisible(True)
            for info in sim.simulate_presets():
                action = preset_menu.addAction(info["title"], run(
                    lambda k=info["name"]: sim.simulate_preset(groom, k, node=name, model=self.editor.model),
                    lambda params, t=info["title"]: "%s: %s settings" % (name, t)))
                action.setToolTip(info["description"])
            preset.setMenu(preset_menu)
            buttons.append(preset)
        elif node_type == "ToMesh":
            show = QtWidgets.QCheckBox("Show mesh")
            show.setObjectName("nodeAction_ShowMesh")
            show.setToolTip("The ToMesh nodes' meshes instead of the strands in the viewport (the groom's displayMesh)")
            show.setChecked(bool(groom) and bool(cmds.getAttr(groom + ".displayMesh")))
            show.toggled.connect(lambda on: groom and cmds.setAttr(groom + ".displayMesh", bool(on)))
            buttons.append(show)

            def mesh_in_scene():
                shape = groomlab.to_mesh_shape(groom)
                cmds.select(shape)
                self.editor.report("%s: the ToMesh nodes' live mesh (export it with Alembic)" % shape.split("|")[-1])
            buttons.append(("Mesh in Scene", "A Maya mesh on the groom's outMesh (selected if there is one): the ToMesh "
                            "nodes' meshes, following the animation, to sculpt, model or export (one undo step)",
                            lambda _=False: later(mesh_in_scene)))

            def export_mesh():
                from groomlab.ui import menu
                path = cmds.fileDialog2(fileFilter="OBJ (*.obj);;FBX (*.fbx);;Alembic, this frame (*.abc)",
                                        dialogStyle=2, fileMode=0, caption="Export Mesh",
                                        startingDirectory=menu.default_file(groom, "exports", "_%s.obj" % name))
                if path:
                    written = groomlab.export_mesh(groom, path[0], node=name)
                    self.editor.report("%s: wrote %s" % (name, written))
            buttons.append(("Export Mesh...", "This node's mesh at the current frame as an OBJ, FBX or Alembic file "
                            "(for ZBrush, baking or printing)", lambda _=False: later(export_mesh)))
        elif node_type == "HairColor":
            buttons.append(("Create Hair Shader", "An aiStandardHair rendering this node's colors (its melanin, or "
                            "with mode color or a tip color its RGB), assigned to the groom; it follows later "
                            "changes of the look. Without one, Arnold renders the groom's colors anyway",
                            run(lambda: groomlab.create_hair_shader(groom), lambda s: "hair shader %s" % s)))
        elif node_type in ("Instance", "Scales"):
            def prototype():
                picked = cmds.ls(selection=True, long=True, type="transform") or cmds.ls(selection=True, type="mesh")
                if not picked:
                    raise ValueError("select the mesh to instance")
                return groomlab.set_instance_prototype(groom, name, picked[0], model=self.editor.model)
            buttons.append(("Prototype from Selection", "Instance the selected mesh (hide it: only its "
                            "instances render)", run(prototype, lambda m: "instancing %s" % m)))
        # The shared ones (graph_ui.json's nodeActions) for its type, two to a row.
        table = self.node_action_table(name)
        shared = [item for item in definitions.resolve(definitions.get("graph/nodeActions")["items"],
                                                       self.node_action_state(name))
                  if item["kind"] == "action"]
        if shared:
            buttons.append(None)
        for item in shared:
            buttons.append((item["label"], item["tip"], lambda _=False, fn=table[item["id"]]: fn()))
        if not buttons or not groom:
            return None
        box = QtWidgets.QFrame()
        box.setObjectName("nodeActions")
        box.setToolTip(definitions.get("graph/nodeActions").get("tip", ""))
        rows = QtWidgets.QVBoxLayout(box)
        rows.setContentsMargins(theme.px(10), theme.px(6), theme.px(10), theme.px(4))
        rows.setSpacing(theme.px(4))
        row, pushed = None, 0
        for b in buttons:
            if b is None:  # a new row
                row = None
                continue
            if isinstance(b, QtWidgets.QLabel):  # text: a row of its own, the panel's full width
                b.setWordWrap(True)
                rows.addWidget(b)
                row = None
                continue
            if row is None or (pushed >= 2 and not isinstance(b, QtWidgets.QCheckBox)):
                row, pushed = QtWidgets.QHBoxLayout(), 0  # two buttons to a row: a narrow panel keeps them whole
                rows.addLayout(row)
            if isinstance(b, QtWidgets.QWidget):
                row.addWidget(b)
                pushed += isinstance(b, QtWidgets.QPushButton)
                continue
            label, tip, fn = b
            button = QtWidgets.QPushButton(label)
            button.setObjectName("nodeAction_" + label.replace(" ", "").replace(".", ""))
            button.setToolTip(tip)
            button.clicked.connect(fn)
            row.addWidget(button)
            pushed += 1
        # A line under them: the params scroll below it.
        line = QtWidgets.QFrame()
        line.setFrameShape(QtWidgets.QFrame.HLine)
        line.setFrameShadow(QtWidgets.QFrame.Sunken)
        rows.addSpacing(theme.px(2))
        rows.addWidget(line)
        return box

    def relink_map_image(self, map_name, path=None):
        """Point the groom's map `map_name` at an image (groomlab.set_map_texture): `path`, else picked
        in a file dialog (only a click gets there; scripts pass the path). Returns the stored path."""
        import groomlab
        groom = self.editor.groom
        if not groom:
            return None
        if path is None:
            from groomlab.ui import tool_tabs
            expected = groomlab.map_texture(groom, map_name)
            folder = os.path.dirname(expected) if expected else ""
            if not folder or not os.path.isdir(folder):
                folder = groomlab.scene_folder() or groomlab.project_root()
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Relink Image: %s" % map_name, folder,
                                                            tool_tabs.IMAGE_FILTER)
            if not path:
                return None
        try:
            stored = groomlab.set_map_texture(groom, map_name, path)
        except (RuntimeError, ValueError) as e:
            self.editor.report(str(e).strip())
            return None
        self.editor.reload()
        self.editor.report("%s reads %s" % (map_name, stored))
        return stored

    def param_menu(self, name, param, global_pos):
        menu = self.build_param_menu(name, param)
        if not menu.isEmpty():  # text params have nothing to offer
            qt.exec_(menu, global_pos)

    def param_actions(self, node, param):
        """What a param's menus run (engine/ui/graph_ui.json's paramMenu, keyMenu, paintMenu) for node.param."""
        b = definitions.bound
        return {"param.setKey": b(lambda: self.set_key(node, param)),
                "param.deleteKey": b(lambda: self.delete_key(node, param)),
                "param.removeAnimation": b(lambda: self.remove_animation(node, param)),
                "param.selectAttribute": b(lambda: self.select_attribute(node, param)),
                "param.expose": b(lambda on: self.editor.set_param_exposed(node, param, on)),
                "param.paintGuides": b(lambda: self.paint_param(node, param, "guides")),
                "param.paintVertices": b(lambda: self.paint_param(node, param, "vertices")),
                "param.paintTexture": b(lambda: self.paint_param(node, param, "texture")),
                "param.driveWithSlider": b(lambda: self.add_control(node, param))}

    def build_param_menu(self, name, param):
        menu = QtWidgets.QMenu(self)
        menu.setToolTipsVisible(True)  # why a param cannot be keyed
        self.fill_param_menu(menu, name, param)
        return menu

    def fill_param_menu(self, menu, name, param):
        """The param's menu (graph_ui.json's paramMenu: keys, expose, paint, drive) into `menu`: the
        label's, and atop a fill field's own (Reset to Default, its range)."""
        exposed = param in self.editor.model.param_inputs(name)
        spec = self.editor.model.param_spec(name, param)
        state, values = set(), {}
        if spec and spec.get("kind") in KEYABLE_KINDS:
            state.add("param.keyable")
            state, values = self.key_state(name, param, state)
        if gm.linkable(spec):
            state.add("param.linkable")
            if spec.get("mappable"):
                state.add("param.mappable")
        if self.editor.groom:
            state.add("groom")
        widgets.fill_menu(menu, definitions.get("graph/paramMenu"), definitions.binder(self.param_actions(name, param)),
                          state, values, generators=self.key_generators(name, param), checked=lambda item: exposed)

    PAINT_TIP = definitions.get("graph/paintMenu")["tip"]

    def paint_menu(self, name, param, parent):
        """Paint > On Guides / On Scalp Vertices / As Texture: the param's map, painted (the param
        menu's and the Paint chip's, one list: graph_ui.json's paintMenu)."""
        container = definitions.get("graph/paintMenu")
        paint = QtWidgets.QMenu(container["label"], parent)  # parented: it outlives the caller's references
        paint.setToolTipsVisible(True)
        paint.setToolTip(container["tip"])
        widgets.fill_menu(paint, container["items"], definitions.binder(self.param_actions(name, param)))
        return paint

    def paint_chip(self, name, param):
        """The yellow Paint chip beside a mappable param (the app's Paint pill): a click offers where
        to paint its map, as right-click > Paint does."""
        chip = QtWidgets.QToolButton()
        chip.setObjectName("paintChip")
        chip.setText("Paint")
        chip.setToolTip(self.PAINT_TIP)
        chip.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        chip.setMenu(self.paint_menu(name, param, chip))
        t = theme.current()
        yellow = t.qcolor("yellow")
        chip.setStyleSheet(
            "QToolButton#paintChip { color: %s; background: transparent; border: 1px solid %s; border-radius: %dpx;"
            " padding: 0px %dpx; font-weight: bold; }"
            " QToolButton#paintChip:hover { background: %s; color: %s; }"
            " QToolButton#paintChip::menu-indicator { image: none; width: 0px; }"
            % (yellow.name(), yellow.name(), theme.px(4), theme.px(6), yellow.name(), readable_on(yellow).name()))
        return chip

    def add_control(self, node, param):
        import groomlab
        if not self.editor.apply():
            return
        try:
            attr = groomlab.add_control(self.editor.groom, node, param)
        except (RuntimeError, ValueError, gm.GraphError) as e:
            self.editor.report(str(e))
            return
        self.editor.reload()
        self.editor.report("%s.%s is driven by %s (keyable in the channel box)" % (node, param, attr))

    # -- Keyed params: Set Key on a param, its state in the Channel Box's colors ---------------------

    def key_state(self, node, param, state=()):
        """The key menu's conditions for a param (keyable here, keyed on this frame, linked to its
        attribute) and the values its texts name (why it cannot be keyed)."""
        model, groom = self.editor.model, self.editor.groom
        why = model.keyable(node, param) if groom else "no groom"
        linked = model.control_of(node, param) is not None
        state = set(state)
        if linked or why is None:
            state.add("param.canKey")
        if linked:
            state.add("param.linked")
            if key_state(groom, node, param, model) == "keyed":
                state.add("param.keyed")
        return state, {"why": why or ""}

    def key_generators(self, node, param):
        """The key menu's last entry: the attribute a linked param is set by (disabled, to read)."""
        def control(menu, _item):
            import groomlab
            menu.addAction(groomlab.param_control(self.editor.groom, node, param) or "").setEnabled(False)
        return {"paramControl": widgets.menu_filler(control)}

    def add_key_actions(self, menu, node, param):
        """Set Key, Delete Key, Remove Animation and Select Attribute for a param (in its menus:
        graph_ui.json's keyMenu)."""
        state, values = self.key_state(node, param)
        widgets.fill_menu(menu, definitions.get("graph/keyMenu"), definitions.binder(self.param_actions(node, param)),
                          state, values, generators=self.key_generators(node, param))

    def _key_edit(self, fn, *args):
        """Run a key edit on the groom (the editor's graph applied first), then show the result."""
        if not self.editor.groom or not self.editor.apply():
            return None
        try:
            result = fn(self.editor.groom, *args)
        except (RuntimeError, ValueError, gm.GraphError) as e:
            self.editor.report(str(e))
            return None
        self.editor.sync_now()
        self.refresh_keys()
        return result if result is not None else True

    def set_key(self, node, param):
        """Key the param at the current frame (the first key links it to an attribute of its own)."""
        import groomlab
        from maya import cmds
        why = self.editor.model.keyable(node, param)
        if why and self.editor.model.control_of(node, param) is None:
            self.editor.report("%s.%s cannot be keyed: %s" % (node, param, why))
            return None
        attr = self._key_edit(groomlab.set_param_key, node, param)
        if attr:
            self.editor.report("keyed %s.%s at frame %g (%s)" % (node, param, cmds.currentTime(query=True), attr))
        return attr

    def delete_key(self, node, param):
        import groomlab
        if self._key_edit(groomlab.delete_param_key, node, param):
            self.editor.report("deleted the key of %s.%s" % (node, param))

    def remove_animation(self, node, param):
        import groomlab
        if self._key_edit(groomlab.remove_param_animation, node, param):
            self.editor.report("%s.%s is a plain value again" % (node, param))

    def select_attribute(self, node, param):
        import groomlab
        attr = groomlab.select_param_attribute(self.editor.groom, node, param)
        self.editor.report("selected %s" % attr if attr else "%s.%s is not keyed" % (node, param))

    def hovered_param(self):
        """The param whose row is under the mouse (the label or its field), or None. Qt's own record
        of what the mouse is over (the enter and leave events the widgets got), not the cursor's
        position: a window covering the panel hides it, as it hides the panel's tooltips."""
        body = self.widget()
        if body is None or not self.viewport().underMouse():
            return None
        for widget in body.findChildren(QtWidgets.QWidget):
            param = widget.property("glParam")
            if param and widget.isVisible() and widget.underMouse():
                return param
        return None

    def refresh_keys(self):
        """Each number field's state (red: a key on this frame, pink: animated, yellow: driven) and, for
        a keyed param, its value on this frame: after edits and on every time change."""
        groom, model, node = self.editor.groom, self.editor.model, self.node
        if node is None or model is None or node not in model.nodes or not getattr(self, "key_widgets", None):
            return
        import groomlab
        for param, field in list(self.key_widgets.items()):
            if not widgets.alive(field):
                del self.key_widgets[param]
                continue
            if groom and model.control_of(node, param) is not None:
                try:
                    show_value(field, groomlab.param_value(groom, node, param, model))
                except (RuntimeError, ValueError):
                    pass
            paint_key_state(field, key_state(groom, node, param, model))

    def paint_param(self, node, param, kind):
        """A map (guides, vertices or texture) feeding the param through a Map node, painted in
        Groom Tools > Paint (the map it reads already, when it reads one): Ctrl lowers the param
        toward 0, strokes paint it back to its value."""
        import groomlab
        from groomlab.ui import menu
        groom = self.editor.groom
        if not self.editor.apply():  # the editor's graph first
            return
        if kind == "texture" and not menu.ensure_scene_saved("texture maps"):
            return
        try:
            name = groomlab.paint_param(groom, node, param, kind=kind)
            from groomlab.ui import palette
            palette.show(tab="paint")
            palette.select_map(name)
            where = {"guides": "the guides", "vertices": "the scalp's vertices", "texture": "a texture"}[kind]
            message = "painting %s.%s on %s (map %s): Ctrl lowers it, strokes paint it back" % (node, param, where, name)
        except (RuntimeError, ValueError, gm.GraphError) as e:
            self.editor.report(str(e))
            return
        self.editor.reload()
        self.editor.report(message)

    # -- Widgets per kind --------------------------------------------------

    def _value_of(self, name):
        model = self.editor.model

        def value_of(param):
            try:
                return model.param(name, param)
            except gm.GraphError:
                return None
        return value_of

    def editor_for(self, name, spec):
        value = self.editor.model.param(name, spec["name"])
        if spec.get("unitWhen"):  # its unit as the node's other params make it (shown, and its sliders')
            spec = dict(spec, unit=unit_of(spec, self._value_of(name)))
            self.built_units[spec["name"]] = spec["unit"]
        if self.editor.groom and self.editor.model.control_of(name, spec["name"]) is not None:
            import groomlab  # keyed: its value on this frame
            try:
                value = groomlab.param_value(self.editor.groom, name, spec["name"], self.editor.model)
            except (RuntimeError, ValueError):
                pass
        if spec["mappable"]:
            return MappableEditor(self, name, spec, value)
        return self.plain_editor(name, spec, value)

    def vector_expression_editor(self, spec, value, set_value):
        """A direction: its three numbers, or (f(x) on) an expression giving a vector per strand,
        e.g. normalize(N + [0, -1, 0])."""
        w = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(w)
        row.setContentsMargins(0, 0, 0, 0)
        expression = value.get("expression") if isinstance(value, dict) else None
        vector = [float(c) for c in (value if isinstance(value, (list, tuple)) else spec["default"])]
        if expression is None:
            boxes = []
            for c in vector:
                b = widgets.CompactSpinBox()  # the spec's decimals, trailing zeros dropped
                b.setRange(-1e9, 1e9)
                b.setDecimals(6)
                b.display_decimals = spec.get("decimals", 3)
                b.setValue(c)
                row.addWidget(b)
                boxes.append(b)
            for b in boxes:
                b.editingFinished.connect(lambda: set_value([x.value() for x in boxes]))
            w.boxes = boxes
        else:
            field = QtWidgets.QLineEdit(expression)
            field.setObjectName("vectorExpression")
            field.setToolTip("A vector per strand: [x, y, z], P, N, tip, normalize(...), rotate(...), vnoise(P)... "
                             "(the Expression node's language)")
            field.editingFinished.connect(lambda: set_value({"expression": field.text().strip() or "[0, 0, 0]"}))
            row.addWidget(field, 1)
            w.field = field
        toggle = QtWidgets.QToolButton()
        toggle.setObjectName("vectorExpressionToggle")
        toggle.setText("f(x)")
        toggle.setCheckable(True)
        toggle.setChecked(expression is not None)
        toggle.setToolTip("An expression giving a vector per strand instead of one direction for all")

        def switch(on):
            set_value({"expression": "[%g, %g, %g]" % tuple(vector)} if on else [float(c) for c in spec["default"]])
            self.show_node(self.node)  # the row as the other kind: the expression's field, or three numbers
        toggle.toggled.connect(lambda on: later(lambda: switch(on)))
        row.addWidget(toggle)
        w.toggle = toggle
        return w

    def plain_editor(self, name, spec, value, on_change=None, on_preview=None):
        set_value = on_change or (lambda v, p=spec["name"]: self.editor.set_param(name, p, v))
        preview = on_preview or (lambda v, p=spec["name"]: self.editor.preview_param(name, p, v))
        kind = spec["kind"]
        if kind == "bool":
            w = QtWidgets.QCheckBox()
            w.setChecked(bool(value))
            w.toggled.connect(set_value)
            if name == self.node:
                self.key_widgets[spec["name"]] = w
        elif kind in ("int", "float"):
            # One slider range per operator param (and display unit), shared by every node of that type.
            from groomlab import units
            kind_of_unit = spec.get("unit") or ""
            f = units.factor(kind_of_unit) if kind == "float" else 1.0
            key = "%s.%s" % (self.editor.model.node_type(name), spec["name"])
            if f != 1.0:
                key += "@" + units.unit(kind_of_unit)[0]
            default = spec["default"] if isinstance(spec["default"], (int, float)) else value
            lo, hi = units.soft_range(kind_of_unit) or (None, None)  # density: around real hair's
            if lo is None and spec.get("softMin") is not None and spec.get("softMax") is not None:  # the spec's
                lo, hi = spec["softMin"] * f, spec["softMax"] * f
            hard = [spec[k] * f if spec.get(k) is not None else None for k in ("min", "max")]  # the core clamps there
            slider = widgets.SoftSlider(value * f, lo, hi, integer=kind == "int", minimum=hard[0], maximum=hard[1],
                                        decimals=4, key=key, default=default * f,
                                        shown_decimals=widgets.display_decimals(spec.get("decimals"), f))
            slider.dragged.connect(lambda v: preview(v / f))
            slider.committed.connect(lambda v: set_value(v / f))
            slider.unit_factor = f  # what show_value multiplies a keyed value by
            if name == self.node:
                self.key_widgets[spec["name"]] = slider
                slider.menu_hook = lambda menu, p=spec["name"]: self.add_key_actions(menu, name, p)
                inside = getattr(self, "inside_labels", {}).get(spec["name"])
                if inside:  # the row's name inside the field; its right-click the label's menu, then the range
                    slider.setLabel(inside[0])
                    slider.setToolTip(inside[1])
                    slider.setProperty("glParam", spec["name"])  # S over it keys this param
                    slider.menu_hook = lambda menu, p=spec["name"]: self.fill_param_menu(menu, name, p)
                    self.inside_fields[spec["name"]] = slider
            w = slider
            unit_text = units.param_label(kind_of_unit, self.editor.model.node_type(name), spec["name"])
            if unit_text:  # the unit after the value: cm, %, °, turns, s, px, cm/s²...
                if kind_of_unit == "fraction":
                    unit_tip = "A percentage: 100% is 1 (the node stores 0 to 1)"
                elif kind_of_unit not in units.FIXED:
                    unit_tip = "Units: View > Units in the graph editor"
                else:
                    unit_tip = ""
                slider.setSuffix(unit_text, unit_tip)
        elif kind == "vec3" and (spec.get("vectorExpression") or isinstance(value, dict)):
            w = self.vector_expression_editor(spec, value, set_value)
            if name == self.node and getattr(w, "boxes", None):
                self.key_widgets[spec["name"]] = w
        elif kind == "vec3":
            w = QtWidgets.QWidget()
            row = QtWidgets.QHBoxLayout(w)
            row.setContentsMargins(0, 0, 0, 0)
            boxes = []
            for c in value:
                b = widgets.CompactSpinBox()  # the spec's decimals, trailing zeros dropped
                b.setRange(-1e9, 1e9)
                b.setDecimals(6)
                b.display_decimals = spec.get("decimals", 3)
                b.setValue(float(c))
                row.addWidget(b)
                boxes.append(b)
            w.boxes = boxes
            w.swatch = None
            if spec.get("unit") == "color":
                # A color: its swatch as Maya shows it, a click to pick (widgets.ColorSwatch).
                swatch = widgets.ColorSwatch([float(c) for c in value])

                def picked(rgb, boxes=boxes):
                    for b, c in zip(boxes, rgb):
                        b.blockSignals(True)
                        b.setValue(float(c))
                        b.blockSignals(False)
                    set_value([float(c) for c in rgb])

                swatch.picked.connect(picked)
                row.insertWidget(0, swatch)
                w.swatch = swatch

            def typed(boxes=boxes, w=w):
                rgb = [x.value() for x in boxes]
                if w.swatch is not None:
                    w.swatch.set_color(rgb)
                set_value(rgb)

            for b in boxes:
                b.editingFinished.connect(typed)
            if spec.get("worldDirection") and spec.get("unit") == "length" and on_change is None:
                # A world position (Transform's pivot): Pick takes the selection's.
                picker = QtWidgets.QToolButton()
                picker.setObjectName("positionPick")
                picker.setText("Pick")
                picker.setToolTip("Set it to where the selected vertices, locator or transform are "
                                  "(select them in the viewport or the Outliner first)")
                picker.clicked.connect(lambda _=False, p=spec["name"]: self.pick_position(name, p))
                row.addWidget(picker)
                w.picker = picker
            if name == self.node:
                self.key_widgets[spec["name"]] = w
        elif kind == "ramp":
            keys = value if isinstance(value, (list, dict)) else [[0.0, float(value)], [1.0, float(value)]]
            w = widgets.RampEditor(keys)
            w.dragged.connect(preview)
            w.committed.connect(set_value)
        elif spec.get("choices"):  # a fixed set of values
            w = QtWidgets.QComboBox()
            for choice in spec["choices"]:
                w.addItem(choice)
            if str(value) not in spec["choices"]:
                w.addItem(str(value))
            w.setCurrentText(str(value))
            w.activated.connect(lambda i, w=w: set_value(w.itemText(i)))
        elif spec.get("channel") in ("writes", "reads") and on_change is None:
            w = self.attribute_editor(name, spec, value, set_value)
        elif spec["name"] in ("group", "exclude") and self.editor.model.node_type(name) == "Guides":
            w = self.group_editor(spec["name"], value, set_value)
        elif self.suggestions(name, spec["name"]) is not None:  # free text, with what exists offered
            options, placeholder = self.suggestions(name, spec["name"])
            w = suggest_box(value, options, set_value, placeholder)
        elif spec["name"] == "path" and self.editor.model.node_type(name) in FILE_PATH_FILTERS:
            w = self.path_field(value, FILE_PATH_FILTERS[self.editor.model.node_type(name)], set_value)
        else:  # string
            w = QtWidgets.QLineEdit(str(value))
            w.editingFinished.connect(lambda w=w: set_value(w.text()))
        return w

    def attribute_editor(self, node, spec, value, set_value):
        """A param naming an attribute (a strand channel). Writes: a plain field, and a warning when
        the name is one a node upstream writes (this node overwrites it). Reads: the attributes
        reaching the node to pick from, each with the node that wrote it last (any name can be
        typed), and a note when the one named does not reach it."""
        editor = self.editor
        writes = spec.get("channel") == "writes"
        info = editor.channels().get(node, {})
        holder = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(holder)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(theme.px(2))
        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(6))
        chip = QtWidgets.QLabel("Writes" if writes else "Reads")
        chip.setObjectName("attributeUse")
        color = QtGui.QColor(WRITES if writes else READS)
        chip.setStyleSheet("QLabel { background: %s; color: %s; border-radius: %dpx; padding: %dpx %dpx; font-weight: bold; }"
                           % (color.name(), readable_on(color).name(), theme.px(6), theme.px(1), theme.px(6)))
        chip.setToolTip(ATTRIBUTE_TIPS["writes" if writes else "reads"])
        row.addWidget(chip)

        def commit(v):
            set_value(v)
            later(lambda: self.node == node and self.show_node(node))  # the warning or note as it is now

        if writes:
            field = QtWidgets.QLineEdit(str(value))
            field.setObjectName("attributeName")
            field.setPlaceholderText("none (no attribute)")
            field.editingFinished.connect(lambda: field.text().strip() != str(value) and commit(field.text().strip()))
            row.addWidget(field, 1)
            holder.field = field
        else:
            sources = editor.channel_sources(node)
            box = QtWidgets.QComboBox()
            box.setObjectName("attributeName")
            box.setEditable(True)
            box.setInsertPolicy(QtWidgets.QComboBox.NoInsert)
            box.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
            box.setMinimumContentsLength(12)
            box.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            for source in sources:
                label = "%s (from %s)" % (source["name"], source["node"]) if source.get("node") else source["name"]
                box.addItem(label, source["name"])
            box.lineEdit().setPlaceholderText("none")
            box.setCurrentIndex(-1)
            box.setEditText(str(value))
            names = {box.itemText(i): box.itemData(i) for i in range(box.count())}

            def picked(i):
                name = box.itemData(i)
                box.setEditText(name)
                if name != str(value):
                    commit(name)

            def typed():
                text = box.currentText().strip()
                text = names.get(text, text)
                if text != str(value):
                    commit(text)
            box.activated.connect(picked)
            box.lineEdit().editingFinished.connect(typed)
            box.setToolTip("Attributes reaching this node, each with the node that writes it last; type any other name")
            row.addWidget(box, 1)
            holder.field = box
        column.addLayout(row)

        line = None
        if writes:
            over = [o for o in info.get("overwrites", []) if o["param"] == spec["name"]]
            if over:
                line = QtWidgets.QLabel(u"\u26a0 overwrites " + ", ".join("%s from %s" % (o["name"], o["node"]) for o in over))
                line.setObjectName("attributeOverwrite")
                line.setStyleSheet("color: %s;" % WARNING.name())
                line.setToolTip("A node upstream already writes this attribute: from here on the strands carry this "
                                "node's, so nodes after it read this one. Give each node its own name (chained "
                                "Clumps: lockId, then clumpId) unless replacing it is what you want.")
        else:
            read = next((r for r in info.get("reads", []) if r["param"] == spec["name"]), None)
            if read is not None and not read["reaches"] and not editor.quiet_read(node, spec["name"]):
                why = ("none applies" if read.get("optional")
                       else "this node ignores it")
                line = QtWidgets.QLabel(u"\u24d8 no node upstream writes %s: %s" % (read["name"], why))
                line.setObjectName("attributeMissing")
                line.setStyleSheet("color: %s;" % NOTE.name())
                line.setToolTip("Attributes reaching this node: %s" % (", ".join(c["name"] for c in info.get("incoming", []))
                                                                        or "none"))
        if line is not None:
            line.setWordWrap(True)
            column.addWidget(line)
        holder.note = line
        return holder

    def path_field(self, value, file_filter, set_value):
        """A file param (Reference's groom file, ImportCurves' Alembic file): its path, typed or
        browsed for (stored as the scene stores paths: groomlab.portable_path)."""
        holder = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(holder)
        row.setContentsMargins(0, 0, 0, 0)
        line = QtWidgets.QLineEdit(str(value))
        line.setObjectName("pathField")
        line.editingFinished.connect(lambda: set_value(line.text()))
        browse = QtWidgets.QPushButton("...")
        browse.setFixedWidth(theme.px(28))
        browse.setToolTip("Browse for the file")

        def pick():
            import groomlab
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self.window(), "File", groomlab.resolve_path(line.text())
                                                            if line.text() else "", file_filter)
            if path:
                stored = groomlab.portable_path(path)
                line.setText(stored)
                set_value(stored)
        browse.clicked.connect(lambda _=False: pick())
        row.addWidget(line)
        row.addWidget(browse)
        holder.line = line
        return holder

    def suggestions(self, node, param):
        """([names], placeholder) to offer in a text param naming something that exists, else None."""
        model = self.editor.model
        op_type = model.node_type(node)
        import groomlab
        from maya import cmds
        groom = self.editor.groom
        channel = (model.param_spec(node, param) or {}).get("channel")
        if channel == "reads":  # the attributes reaching this node
            return [c["name"] for c in self.editor.channel_sources(node)], "none"
        if channel == "writes":  # a name of its own: nothing to pick
            return [], "none (no attribute)"
        if op_type == "Part" and param == "part":
            try:
                return (groomlab.list_parts(groom) if groom else []), "every part"
            except RuntimeError:
                return [], "every part"
        if (op_type == "Map" and param == "name") or (op_type == "HairColor" and param == "map") or \
                (op_type == "Interpolate" and param == "regionMap"):
            if op_type in ("HairColor", "Interpolate"):
                try:
                    # Color maps first: they paint hair color and regions (a gray map reads as gray).
                    # Guide layers lie on the guides: regions are painted on the scalp.
                    entries = groomlab.maps(groom) if groom else []
                except RuntimeError:
                    entries = []
                if op_type == "Interpolate":
                    entries = [m for m in entries if m.get("kind") != "guides"]
                names = [m["name"] for m in entries if m.get("channels") == 4] + \
                        [m["name"] for m in entries if m.get("channels") != 4]
                # Then the scalp's own color sets (an image file's path can be typed in).
                names += [s for s in self.editor.color_sets() if s not in names]
                return names, "none (a color map from Groom Tools > Paint, a color set, or an image file)"
            try:
                names = groomlab.list_maps(groom) if groom else []
            except RuntimeError:
                names = []
            return names, "a map (Groom Tools > Paint > New...)"
        if op_type in ("Instance", "Scales") and param == "prototype":
            meshes = cmds.ls(type="mesh", noIntermediate=True, long=False) or []
            return sorted({(cmds.listRelatives(m, parent=True) or [m])[0] for m in meshes})[:200], "a mesh"
        if op_type == "Control" and param == "name":
            names = []
            if groom:
                names = [a[4:] for a in (cmds.listAttr(groom, userDefined=True) or []) if a.startswith("ctl_")]
            return names, "the slider's name"
        return None

    def group_editor(self, param, value, set_value):
        """The groom's guide groups to pick from, one or several: which to read (group) or leave out (exclude)."""
        import groomlab
        groups = groomlab.list_guide_groups(self.editor.groom) if self.editor.groom else []
        tip = ("Guide groups of this groom: %s.\nSee them (counts, levels, where they come from) under Guide "
               "groups & edits in the Groom Tools." % (", ".join(g["name"] for g in groups) or "none yet"))
        details = {g["name"]: "%d guides, level %d" % (g["count"], g["depth"]) for g in groups}
        placeholder = "every group at the node's level" if param == "group" else "none"
        w = names_editor(value, [g["name"] for g in groups], set_value, placeholder,
                         "Pick groups to read" if param == "group" else "Pick groups to leave out", details)
        panel = QtWidgets.QToolButton()
        panel.setText("...")
        panel.setToolTip("Open the guide groups in the Groom Tools")
        panel.clicked.connect(self.open_groups)
        w.layout().addWidget(panel)
        w.setToolTip(tip)
        return w

    def open_groups(self):
        from groomlab.ui import palette
        palette.show(tab="guides")


class MappableEditor(QtWidgets.QWidget):
    """A value, or a texture / color set / attribute / expression / painted map (PG-2)."""

    SOURCES = ["value", "texture", "colorSet", "attribute", "expression", "guide"]
    LABELS = {"value": "value", "texture": "texture", "colorSet": "color set (scalp paint)",
              "attribute": "strand attribute", "expression": "expression", "guide": "painted map"}

    def __init__(self, panel, node, spec, value):
        super(MappableEditor, self).__init__()
        self.panel, self.node, self.spec = panel, node, spec
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(4))
        self.body = QtWidgets.QWidget()
        layout.addWidget(self.body, 1)
        # Where the value comes from: a plain value most of the time, so the choice is a small
        # menu beside it rather than a dropdown row over every mappable param.
        self.source = QtWidgets.QToolButton()
        self.source.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        self.source.setToolTip("Drive this param with a map: painted map (Groom Tools > Paint; works anywhere, "
                               "including Scatter density), texture, color set, strand attribute (a channel "
                               "written upstream) or expression")
        self.source.setProperty("role", "glyph")
        menu = QtWidgets.QMenu(self.source)
        group = qt.QActionGroup(menu)
        self.source_actions = {}
        for key in self.SOURCES:
            action = menu.addAction(self.LABELS[key].capitalize())
            action.setCheckable(True)
            group.addAction(action)
            action.triggered.connect(lambda _=False, k=key: self.switch(k))
            self.source_actions[key] = action
        self.source.setMenu(menu)
        layout.addWidget(self.source, 0, QtCore.Qt.AlignTop)
        self.value = value
        # The number to come back to when switching off a map: the value now, else the map's
        # white value (what a map at 1 gave), else the default.
        if isinstance(value, dict):
            self.last_plain = value.get("max", self.spec["default"])
        else:
            self.last_plain = value
        current = "value"
        if isinstance(value, dict):
            current = gm.map_source(value) or "value"
        self.build(current)

    def switch(self, source):
        if source == "value":
            self.value = self.last_plain
        else:
            plain = self.value if not isinstance(self.value, dict) else self.last_plain
            top = float(plain) if isinstance(plain, (int, float)) and not isinstance(plain, bool) else 1.0
            self.value = {source: "", "component": "r", "min": 0.0, "max": top or 1.0}
        self.build(source)
        # The model takes the change at once, even a map without a name yet: validate() flags
        # it (the status bar says so, and nothing is applied) until a name is given.
        self.commit()

    def build(self, source):
        self.current = source
        for key, action in self.source_actions.items():
            action.setChecked(key == source)
        self.source.setText("Map" if source == "value" else "\u25c9 " + self.LABELS[source].split(" (")[0])
        old = self.body
        self.body = QtWidgets.QWidget()
        self.layout().replaceWidget(old, self.body)
        old.hide()  # gone once the event loop gets to it; until then it must not cover the row
        old.deleteLater()
        getattr(self.panel, "inside_fields", {}).pop(self.spec["name"], None)  # a value's field makes its own
        self._fill(source)
        self.panel.update_row_label(self.spec["name"])  # a map's fields need the row's label; a value's holds it
        self.panel.editor.watch_text_edits(self.body)  # Ctrl+Z in a new text field: its typing first

    def _fill(self, source):
        if source == "value":
            row = QtWidgets.QHBoxLayout(self.body)
            row.setContentsMargins(0, 0, 0, 0)
            plain = self.panel.plain_editor(self.node, self.spec, self.value if not isinstance(self.value, dict)
                                            else self.spec["default"], on_change=self.set_plain,
                                            on_preview=self.preview_plain)
            row.addWidget(plain)
            return
        form = QtWidgets.QFormLayout(self.body)
        form.setContentsMargins(0, 0, 0, 0)
        if source in ("guide", "attribute"):
            # The groom's painted layers to pick from; any other name can be typed.
            name = QtWidgets.QComboBox()
            name.setEditable(True)
            name.addItems(self.layers(source))
            name.setCurrentText(self.value.get(source, ""))
            name.lineEdit().setPlaceholderText("painted map" if source == "guide" else "strand channel or painted map")
            name.lineEdit().editingFinished.connect(lambda: self.set_field(source, name.currentText()))
            name.activated.connect(lambda _: self.set_field(source, name.currentText()))
            form.addRow("map" if source == "guide" else "name", name)
            self.add_range(form, source)
            return
        if source == "colorSet":  # the scalp's color sets to pick from
            box = suggest_box(self.value.get(source, ""), self.panel.editor.color_sets(),
                              lambda v: self.set_field(source, v), "color set on the scalp")
            form.addRow("name", box)
            component = QtWidgets.QComboBox()
            component.addItems(["r", "g", "b", "a", "luminance"])
            component.setCurrentText(self.value.get("component", "r"))
            component.currentTextChanged.connect(lambda c: self.set_field("component", c))
            form.addRow("component", component)
            self.add_range(form, source)
            return
        if source == "expression":
            from groomlab.ui import expression_editor
            code = expression_editor.CodeEdit(self.value.get(source, ""), lines=3,
                                              validator=lambda t: __import__("groomlab.expressions").expressions.check(t))
            code.setToolTip("Type $ for variables, or Ctrl+Space")
            code.committed.connect(lambda text: self.set_field(source, text))
            form.addRow(code)
            self.add_range(form, source)
            return
        name = QtWidgets.QLineEdit(self.value.get(source, ""))
        name.setPlaceholderText({"texture": "image file", "colorSet": "color set on the scalp",
                                 "attribute": "strand channel", "guide": "painted map on the guides",
                                 "expression": "e.g. 0.5 + 0.5 * sin(u * 6.28)"}[source])
        name.editingFinished.connect(lambda: self.set_field(source, name.text()))
        if source == "texture":
            holder = QtWidgets.QWidget()
            row = QtWidgets.QHBoxLayout(holder)
            row.setContentsMargins(0, 0, 0, 0)
            browse = QtWidgets.QPushButton("...")
            browse.setFixedWidth(theme.px(28))
            browse.clicked.connect(lambda: self.browse(name))
            row.addWidget(name)
            row.addWidget(browse)
            form.addRow("file", holder)
        else:
            form.addRow({"expression": "expression", "guide": "map"}.get(source, "name"), name)
        if source in ("texture", "colorSet"):
            component = QtWidgets.QComboBox()
            component.addItems(["r", "g", "b", "a", "luminance"])
            component.setCurrentText(self.value.get("component", "r"))
            component.currentTextChanged.connect(lambda c: self.set_field("component", c))
            form.addRow("component", component)
        self.add_range(form, source)

    def add_range(self, form, source):
        """min and max: what the map's 0 (black) and 1 (white) become (in the param's display units)."""
        from groomlab import units
        kind = self.spec.get("unit") or ""
        f = units.factor(kind)
        suffix = (" " + units.label(kind)) if units.label(kind) else ""
        for key in ("min", "max"):
            box = widgets.CompactSpinBox()
            box.setRange(-1e9, 1e9)
            box.setDecimals(6)
            box.setValue(float(self.value.get(key, 0.0 if key == "min" else 1.0)) * f)
            box.setToolTip("the value where the map is %s" % ("0 (black)" if key == "min" else "1 (white)"))
            box.editingFinished.connect(lambda k=key, b=box: self.set_field(k, b.value() / f))
            form.addRow("%s (%s)%s" % (key, "black" if key == "min" else "white", suffix), box)

    def layers(self, source="guide"):
        """Names to offer: painted maps, and for a strand attribute the channels reaching this node too."""
        editor = self.panel.editor
        names = editor.channels_into(self.node) if source == "attribute" else []
        if editor.groom:
            try:
                import groomlab
                names += [m for m in groomlab.list_maps(editor.groom) if m not in names]
            except Exception:
                pass
        return names

    def browse(self, line_edit):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self.window(), "Map image", line_edit.text(), "Images (*.png *.jpg *.jpeg *.tga *.bmp *.psd *.hdr *.pgm *.ppm)")
        if path:
            line_edit.setText(path)
            self.set_field("texture", path)

    def set_plain(self, v):
        self.value = v
        self.last_plain = v
        self.commit()

    def preview_plain(self, v):
        self.panel.editor.preview_param(self.node, self.spec["name"], v)

    def set_field(self, key, v):
        self.value[key] = v
        self.commit()

    def commit(self):
        self.panel.editor.set_param(self.node, self.spec["name"], self.value)


# ---------------------------------------------------------------------------
# Editor


class GraphEditor(QtWidgets.QWidget):
    """The editor widget; `show()` below wraps it in a dockable Maya panel."""

    # What the Edit, Add Node and Presets menus run (engine/ui/graph_ui.json's editMenu, addMenu,
    # presetMenu, presetItem): method names, or functions of the editor.
    ACTIONS = {
        "graph.undo": "undo",
        "graph.redo": "redo",
        "graph.frame": lambda self: self.view.frame(),
        "graph.frameAll": lambda self: self.view.frame_all(),
        "graph.tidy": "layout_nodes",
        "graph.group": "collapse_selected",
        "graph.ungroup": "expand_selected",
        "graph.delete": "delete_selected",
        "graph.copy": "copy_graph",
        "graph.paste": lambda self: self.paste_graph(),
        "graph.gpuPreview": "set_gpu_preview",
        "graph.reload": "reload",
        "node.newScript": lambda self: self.new_script_node(),
        "node.import": lambda self: self.import_nodes(None),
        "node.openFolder": "open_nodes_folder",
        "preset.save": "save_preset",
        "preset.apply": "load_preset",
        "preset.insert": "insert_preset",
    }

    def edit_generators(self):
        """The Edit menu's submenus filled here: Units (as it opens) and Theme."""
        def units(menu, _item):
            self.units_menu = QtWidgets.QMenu("Units", menu)
            self.units_menu.setToolTip("Show lengths, widths and densities in real units")
            self.units_menu.aboutToShow.connect(self.fill_units_menu)
            menu.addMenu(self.units_menu)
        return {"units": widgets.menu_filler(units),
                "themeMenu": widgets.menu_filler(lambda menu, _item: menu.addMenu(widgets.theme_menu(menu)))}

    def __init__(self, groom=None, catalog=None, parent=None):
        super(GraphEditor, self).__init__(parent)
        apply_theme()
        self.setWindowTitle("GroomLab Graph Editor")
        self.setWindowIcon(widgets.icon("logo"))
        widgets.fit_to_screen(self, theme.px(1200), theme.px(600))  # within a small screen
        self.catalog = catalog
        self.groom = None
        self.model = None
        self.items = {}
        self._linked_inputs = set()   # (node, input) pairs with a link, and
        self._linked_outputs = set()  # (node, output) feeding something: for the port dots, once per rebuild
        self.live = True
        self._preview_base = None  # the stored graph before a slider drag
        self._key_preview = None   # a keyed param's slider drag: its attributes, their values before, now
        self.viewed = None         # the node the viewport shows (the Focus toggle)
        self.node_errors = {}      # node -> error text from the last evaluation
        self.node_warnings = {}    # node -> a warning (a Cards node whose maps want a rebake)
        self.node_notes = {}       # node -> a note: it does nothing as set (a Clump with no centers)
        self.node_times = {}       # node -> ms of the last evaluation
        self.node_channels = {}    # node -> strand channels in its last result

        self.scene = GraphScene(self)
        self.scene.selectionChanged.connect(self.selection_changed)
        self.view = GraphView(self.scene)
        self.params = ParamPanel(self)
        self.status = QtWidgets.QLabel()

        toolbar = QtWidgets.QToolBar()
        # The groom: the current one (the Groom Tools' too: picking one here makes it current, as
        # selecting a groom does), unless pinned: then the graph stays on its groom.
        self.pinned = False
        self._followed = None  # the current groom last followed: the graph moves when it changes
        self.groom_box = QtWidgets.QComboBox()
        self.groom_box.setMinimumWidth(theme.px(180))
        self.groom_box.activated.connect(lambda i: self.pick_groom(self.groom_box.itemText(i)))
        toolbar.addWidget(self.groom_box)
        self.pin_button = QtWidgets.QToolButton()
        self.pin_button.setObjectName("graphPin")
        self.pin_button.setCheckable(True)
        self.pin_button.setAutoRaise(True)
        self.pin_button.setProperty("role", "glyph")
        self.pin_button.toggled.connect(self.set_pinned)
        toolbar.addWidget(self.pin_button)
        self._show_pin()
        toolbar.addSeparator()

        def popup(text, menu, tip):
            button = QtWidgets.QToolButton()
            button.setText(text)
            button.setToolTip(tip)
            button.setPopupMode(QtWidgets.QToolButton.InstantPopup)
            button.setMenu(menu)
            toolbar.addWidget(button)
            return button

        # Parented and kept: an unowned QMenu is garbage-collected and the button does nothing.
        self.add_menu = self.build_add_menu()
        # Made before a groom brings the catalog: filled again once it does, or as it opens.
        self.add_menu.aboutToShow.connect(lambda: self.fill_add_menu(self.add_menu))
        popup("Add Node  ▾", self.add_menu, "Add a node (after the selected one). Also: right-click the canvas, or Tab")
        self.preset_menu = QtWidgets.QMenu(self)
        self.preset_menu.aboutToShow.connect(self.fill_preset_menu)
        self.preset_menu.setToolTipsVisible(True)  # each preset's description
        popup("Presets  ▾", self.preset_menu,
              "Start from a look (hair, fur, feathers), a technique to learn from (blending, layers, maps, "
              "motion...), save this graph as a preset, or copy and paste the whole graph as text to share it")
        spacer = QtWidgets.QWidget()
        spacer.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Preferred)
        spacer.setStyleSheet("background: transparent;")
        toolbar.addWidget(spacer)
        self.snap_box = QtWidgets.QCheckBox("Snap")
        try:
            from maya import cmds
            self.snap_box.setChecked(bool(cmds.optionVar(query=self.SNAP_VAR)))
        except (ImportError, RuntimeError):
            pass
        self.snap_box.setToolTip("Nodes and pins you move land on the grid's dots")
        self.snap_box.toggled.connect(self.set_snapping)
        toolbar.addWidget(self.snap_box)
        self.live_box = QtWidgets.QCheckBox("Live")
        self.live_box.setChecked(True)
        self.live_box.setToolTip("Every edit applies to the groom at once (one undo step each). Off: edit, then Apply")
        self.live_box.toggled.connect(self.set_live)
        toolbar.addWidget(self.live_box)
        self.apply_action = toolbar.addAction("Apply", self.apply)
        self.apply_action.setToolTip("Write the graph to the groom")
        self.apply_action.setVisible(False)  # only while Live is off
        # Everything else: one menu. Undo and redo are Maya's, with their keys taken while the
        # focus is in the editor (Qt widgets, the graph view first, would otherwise keep Ctrl+Z
        # from Maya's hotkeys). QShortcuts rather than action shortcuts: an ambiguous match
        # (another Ctrl+Z shortcut in the same window) still undoes instead of going nowhere.
        # (graph_ui.json's editMenu; Undo and Redo named after the next step as it opens.)
        self.edit_menu = QtWidgets.QMenu(self)
        made = widgets.fill_menu(self.edit_menu, definitions.get("graph/editMenu"), definitions.binder(self.ACTIONS, self),
                                 generators=self.edit_generators())
        self.undo_action, self.redo_action, self.gpu_action = made["graph.undo"], made["graph.redo"], made["graph.gpuPreview"]
        self.edit_menu.aboutToShow.connect(self.fill_edit_menu)
        self.fill_edit_menu(names=False)
        self.edit_menu.aboutToShow.connect(self.sync_gpu_action)
        self.sync_gpu_action()
        self.edit_menu.setToolTipsVisible(True)
        more = popup("", self.edit_menu, "Undo, layout, grouping, copy and paste the whole graph, units, theme")
        more.setIcon(widgets.glyph("more"))
        more.glyph = "more"
        more.setProperty("role", "glyph")
        self._shortcuts = []
        for keys, slot in ((UNDO_KEYS, self.undo), (REDO_KEYS, self.redo)):
            for key in keys:
                shortcut = qt.QShortcut(key, self)
                shortcut.setContext(QtCore.Qt.WidgetWithChildrenShortcut)
                shortcut.activated.connect(slot)
                shortcut.activatedAmbiguously.connect(slot)
                self._shortcuts.append(shortcut)
        # S with the mouse over a param keys it, as over a channel in Maya's Channel Box; elsewhere in
        # the editor it is Maya's Set Key. Text fields keep S for typing (they take it first).
        shortcut = qt.QShortcut(QtGui.QKeySequence("S"), self)
        shortcut.setContext(QtCore.Qt.WidgetWithChildrenShortcut)
        shortcut.activated.connect(self.key_hovered)
        shortcut.activatedAmbiguously.connect(self.key_hovered)
        self._shortcuts.append(shortcut)

        splitter = QtWidgets.QSplitter()
        splitter.addWidget(self.view)
        splitter.addWidget(self.params)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 1)
        # The param panel's share: about 372 px of the default 1200 (sliders, units and notes fit, in Inter too).
        splitter.setSizes([theme.px(828), theme.px(372)])
        splitter.setCollapsible(1, False)
        self.splitter = splitter

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(theme.px(10), theme.px(8), theme.px(10), theme.px(8))
        layout.setSpacing(theme.px(8))
        layout.addWidget(toolbar)
        layout.addWidget(splitter, 1)
        layout.addWidget(self.status)
        self.status.setProperty("role", "about")
        self.status.setObjectName("graphStatus")
        self.status.setWordWrap(True)  # long problems wrap (the whole text in its tooltip too)
        self.status.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self.status.setSizePolicy(QtWidgets.QSizePolicy.Ignored, QtWidgets.QSizePolicy.Preferred)

        theme.style(self)
        theme.listen(self, "restyle")
        self._graph_job = None     # callback: the groom's graph changed outside the editor
        self._time_job = None      # scriptJob: the time changed (timings and errors follow)
        self._sync_pending = False
        self._selection_job = None  # scriptJob: follow the selected groom
        self._scene_jobs = []       # callbacks: File > New and Open
        self._watch_selection()
        self._watch_scene()
        import groomlab
        groomlab.on_current_groom(self.current_groom_changed)
        self._followed = groomlab.active_groom()
        self.refresh_grooms()
        if groom:
            self.set_groom(groom)

    def restyle(self, t):
        """A new theme: the graph's colors, and the param panel's widgets."""
        apply_theme(t)
        for button in self.findChildren(QtWidgets.QToolButton):
            if getattr(button, "glyph", None):
                button.setIcon(widgets.glyph(button.glyph))
        self.scene.setBackgroundBrush(CANVAS)
        if self.model is not None:
            self.rebuild()  # links take their colors when made
        self.scene.update()
        self.params.show_node(self.params.node)

    # -- Keeping up with the scene ---------------------------------------------

    def _kill_job(self, job):
        from maya import cmds
        if job is not None and cmds.scriptJob(exists=job):
            try:
                cmds.scriptJob(kill=job, force=True)
            except RuntimeError:  # the job is running (killed from its own callback): once it is done
                cmds.evalDeferred(lambda: cmds.scriptJob(exists=job) and cmds.scriptJob(kill=job, force=True))

    def _watch_graph(self):
        """Reload when the groom's graph changes elsewhere (a preset or example, a script, undo
        and redo); refresh the timings and errors when its guides or the time change."""
        from maya import cmds
        from maya.api import OpenMaya as om
        self._unwatch_graph()
        if not self.groom:
            return
        try:
            node = om.MSelectionList().add(self.groom).getDependNode(0)
        except RuntimeError:
            return

        def changed(message, plug, other, data):
            if message & (om.MNodeMessage.kConnectionMade | om.MNodeMessage.kConnectionBroken):
                # The scalp or a collider rewired (the Attribute Editor, a script): a Scalp or
                # ColliderMesh node's panel shows the mesh it reads now.
                root = plug.partialName(useLongNames=True, useFullAttributePath=True).split("[")[0].split(".")[0]
                if root in ("inMesh", "colliderMeshes", "extraScalps") and widgets.alive(self):
                    self.params.scene_soon()
                return
            if not (message & om.MNodeMessage.kAttributeSet):
                return
            attr = plug.partialName(useLongNames=True)
            if attr not in self.WATCH and not attr.startswith("ctl_"):
                return
            if not widgets.alive(self):  # the editor was deleted without closing
                try:
                    om.MMessage.removeCallback(job[0])
                except RuntimeError:
                    pass
                return
            if attr == "graph":
                self._graph_changed()
            elif attr.startswith("ctl_"):  # a keyed param's attribute: its field shows the value
                self._keys_soon()
            else:  # a brush stroke, a corrective, flow: times and errors change
                self.status_soon()

        job = [om.MNodeMessage.addAttributeChangedCallback(node, changed)]
        self._graph_job = job[0]
        try:
            self._time_job = cmds.scriptJob(event=["timeChanged", self._time_changed])
        except RuntimeError:  # no UI events (batch)
            self._time_job = None

    # The groom's attributes whose change means new timings and errors (graph: a reload too).
    WATCH = ("graph", "guideData", "correctiveData", "flowData", "partData", "viewNode", "cardMaps")

    def _unwatch_graph(self):
        from maya.api import OpenMaya as om
        if self._graph_job is not None:
            try:
                om.MMessage.removeCallback(self._graph_job)
            except RuntimeError:
                pass
            self._graph_job = None
        self._kill_job(getattr(self, "_time_job", None))
        self._time_job = None

    def _time_changed(self):
        """A new frame: keyed params show their values and states now, timings and errors once it settles."""
        if widgets.alive(self):
            try:
                if self.isVisible():
                    self.params.refresh_keys()
            except RuntimeError:
                pass
        self.status_soon()

    def _keys_soon(self):
        """Refresh the keyed params' fields once the change is done (not inside Maya's callback)."""
        if not getattr(self, "_keys_pending", False):
            self._keys_pending = True

            def refresh():
                self._keys_pending = False
                if widgets.alive(self) and self._key_preview is None:  # not mid-drag: the slider leads
                    self.params.refresh_keys()
            QtCore.QTimer.singleShot(0, refresh)

    def key_hovered(self):
        """S: key the param under the mouse (Maya's Set Key when there is none)."""
        param = self.params.hovered_param() if self.params.node else None
        if param:
            return self.params.set_key(self.params.node, param)
        from maya import mel
        try:
            mel.eval("SetKey")
        except RuntimeError:
            pass
        return None

    def sync_now(self):
        """Show the groom's graph now if it changed (a key edit linked or unlinked a param)."""
        self._sync_pending = False
        self._sync_from_groom()

    def status_soon(self):
        """refresh_status once things settle: strokes and scrubbing fire many changes a second."""
        if not widgets.alive(self):  # the editor was deleted without closing: stop following the time
            job, self._time_job = self._time_job, None
            self._kill_job(job)
            return
        try:
            timer = self._status_timer
        except AttributeError:
            timer = self._status_timer = QtCore.QTimer(self)
            timer.setSingleShot(True)
            timer.setInterval(250)
            timer.timeout.connect(self._status_now)
        if not timer.isActive():
            timer.start()

    def _status_now(self):
        from maya import cmds
        try:
            if not self.isVisible() or not self.groom or not cmds.objExists(self.groom):
                return
        except RuntimeError:
            return
        if self.previewing():
            return  # a slider drag: apply() refreshes when it commits
        if _time_moving():
            self.status_soon()  # playing or scrubbing: once the frame settles
            return
        if self._status_stamp() == getattr(self, "_status_drawn", None):
            return  # nothing evaluated since the badges were drawn
        self.refresh_status()

    def _status_stamp(self):
        """What the status shows depends on: the groom's evaluations (errors, timings, notes) and its
        bake (cardMaps: stale cards). None when it can't be read."""
        from maya import cmds
        try:
            return (cmds.groomLabGraphStats(self.groom, evaluations=True), cmds.getAttr(self.groom + ".cardMaps"))
        except (RuntimeError, ValueError, TypeError):
            return None

    def _graph_changed(self):
        if not self._sync_pending:  # once, after the change settles
            self._sync_pending = True
            QtCore.QTimer.singleShot(0, self._sync_from_groom)

    def _sync_from_groom(self):
        from maya import cmds
        self._sync_pending = False
        # Our own layout write is skipped once, by the sync it set off: an undo later that puts the
        # graph back to that same text (an edit made after a node move, undone) is a change to show.
        layout, self._layout_text = getattr(self, "_layout_text", None), None
        try:
            self.objectName()  # raises once the widget is gone
            if not self.groom or not cmds.objExists(self.groom) or self.model is None:
                return
            text = cmds.getAttr(self.groom + ".graph") or "{}"
        except RuntimeError:
            return
        if self.previewing() or text == self.model.to_json() or text == layout:
            return  # our own edit, our own layout write (edits not applied yet stay), or a live slider drag
        self.model = load_model(self.groom, self.catalog)
        self.rebuild()  # keeps the selection where the names still exist
        self.refresh_status()
        if self.model.unreadable:
            self.report(self.model.unreadable, trouble=True)

    def _watch_selection(self):
        from maya import cmds
        try:
            self._selection_job = cmds.scriptJob(event=["SelectionChanged", self._selection_changed_in_maya])
        except RuntimeError:  # no UI events (batch)
            self._selection_job = None

    def _watch_scene(self):
        """File > New and Open: the editor empties as the scene closes (no nodes of a groom that is
        gone), then shows the opened scene's groom (groomlab.adopt_scene_groom)."""
        from maya.api import OpenMaya as om
        self._unwatch_scene()
        scene = om.MSceneMessage

        def closing(*_):
            if widgets.alive(self):
                self.clear_groom()
            else:  # the editor was deleted without closing
                self._unwatch_scene()

        def opened(*_):
            if widgets.alive(self):
                QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and self.scene_opened())
            else:
                self._unwatch_scene()
        try:
            self._scene_jobs = [scene.addCallback(when, closing) for when in (scene.kBeforeNew, scene.kBeforeOpen)]
            self._scene_jobs += [scene.addCallback(when, opened) for when in (scene.kAfterNew, scene.kAfterOpen)]
        except RuntimeError:
            self._scene_jobs = []

    def _unwatch_scene(self):
        from maya.api import OpenMaya as om
        for job in self._scene_jobs:
            try:
                om.MMessage.removeCallback(job)
            except RuntimeError:
                pass
        self._scene_jobs = []

    def clear_groom(self):
        """No groom: an empty canvas and the empty state (the scene closing)."""
        self.scene.cancel_drag()
        self._unwatch_graph()
        self.groom = self._groom_uuid = self.model = self.viewed = None
        self._preview_base = self._key_preview = None
        self.node_errors, self.node_warnings, self.node_notes, self.node_times, self.node_channels = {}, {}, {}, {}, {}
        self.scene.blockSignals(True)
        try:
            self.scene.clear()
        finally:
            self.scene.blockSignals(False)
        self.items = {}
        self.groom_box.clear()
        self.params.show_node(None)
        self.report("")

    def scene_opened(self):
        """A new or opened scene: its groom (pinned, selected, else the first), or the empty state."""
        import groomlab
        from maya import cmds
        shown = _uuid(self.groom) if self.groom else None
        if self.model is not None and shown is not None and shown == getattr(self, "_groom_uuid", None):
            return  # (showing one of this scene's grooms already: picked since)
        groom = groomlab.adopt_scene_groom()
        if self.pinned:  # the pinned groom went with the old scene
            self.set_pinned(False, follow=False)
        self._followed = groom
        self.refresh_grooms()
        if groom:
            self.set_groom(cmds.ls(groom)[0])
        elif self.groom is not None:
            self.clear_groom()

    def _selection_changed_in_maya(self):
        """Selecting another groom (or its transform) makes it the current groom, and the graph
        shows it, unless the editor is pinned. A groom picked (groomlab.set_current_groom) stays
        current while other things are selected (a groom made takes it over)."""
        try:
            if not self.isVisible():
                return
        except RuntimeError:  # the widget is gone
            self._kill_job(self._selection_job)
            return
        self.follow_current()

    def follow_current(self):
        """Show the current groom's graph (groomlab.active_groom) when it changed since the editor
        last followed it, unless the editor is pinned."""
        import groomlab
        from maya import cmds
        if self.pinned:
            return
        groom = groomlab.active_groom()
        if groom == self._followed and self.groom is not None:
            return  # (a groom shown by name, graph_editor.show(groom), stays until the current one changes)
        self._followed = groom
        if groom and not _same_node(groom, self.groom):
            self.refresh_grooms()
            self.set_groom(cmds.ls(groom)[0])

    def current_groom_changed(self, _groom):
        """groomlab.on_current_groom: a pick in the Groom Tools, a groom made or selected: the graph
        follows (once things settle), unless pinned."""
        import groomlab
        if not widgets.alive(self):  # deleted without closing
            groomlab.remove_current_groom_listener(self.current_groom_changed)
            return
        if not self.pinned:
            QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and self.isVisible() and self.follow_current())

    def pick_groom(self, groom):
        """The groom list's pick: the current groom for the Groom Tools too, unless the editor is
        pinned (then only the graph moves)."""
        import groomlab
        if not groom:
            return
        if not self.pinned:
            groomlab.set_current_groom(groom)
            self._followed = groomlab.active_groom()
        if not _same_node(groom, self.groom):
            self.set_groom(groom)

    def set_pinned(self, on, follow=True):
        """Pin the graph to its groom (the Groom Tools may move on to another), or unpin it: it
        shows the current groom again. For this session (not saved)."""
        self.pinned = bool(on)
        self._show_pin()
        if not self.pinned and follow:
            self._followed = None
            self.follow_current()

    def _show_pin(self):
        button = self.pin_button
        button.blockSignals(True)
        button.setChecked(self.pinned)
        button.blockSignals(False)
        button.glyph = "pinned" if self.pinned else "pin"
        button.setIcon(widgets.glyph(button.glyph))
        button.setToolTip("Pinned: the graph stays on this groom while the Groom Tools move on (click to unpin: it "
                          "shows the current groom again)" if self.pinned else
                          "Pin the graph to this groom: it stays on it while the Groom Tools move on to another. "
                          "Unpinned, it shows the current groom (the Groom Tools')")
        self.groom_box.setToolTip(
            "The groom whose graph this is (pinned: picking another moves only the graph)" if self.pinned else
            "The current groom, the Groom Tools' too: picking one here makes it current, as selecting a groom does")

    def showEvent(self, event):
        super(GraphEditor, self).showEvent(event)
        if self._selection_job is None:  # closed and shown again
            import groomlab
            self._watch_selection()
            self._watch_scene()
            self._watch_graph()
            groomlab.on_current_groom(self.current_groom_changed)
            self._sync_from_groom()

    def closeEvent(self, event):
        self.stop_following()
        super(GraphEditor, self).closeEvent(event)

    def stop_following(self):
        """Stop following Maya (the groom's callback, the time and selection scriptJobs, the status
        timer): closing, or Maya quitting."""
        import groomlab
        self._unwatch_graph()
        self._unwatch_scene()
        self._kill_job(self._selection_job)
        self._graph_job = self._selection_job = None
        groomlab.remove_current_groom_listener(self.current_groom_changed)
        timer = getattr(self, "_status_timer", None)
        if timer is not None and widgets.alive(timer):
            timer.stop()

    @staticmethod
    def silently(fn):
        """Run fn with undo off: layout and view changes never make undo steps."""
        from maya import cmds
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            return fn()
        finally:
            cmds.undoInfo(stateWithoutFlush=True)

    # -- Undo and redo -----------------------------------------------------------

    def undo(self, *_):
        """Maya's undo (Ctrl+Z here): every applied edit is one step of the groom's graph."""
        return self._step_history(redo=False)

    def redo(self, *_):
        """Maya's redo (Ctrl+Y, Ctrl+Shift+Z here)."""
        return self._step_history(redo=True)

    def _step_history(self, redo):
        from maya import cmds
        if self.previewing():
            return False  # a slider drag previews outside undo: releasing it commits the step
        verb = "redo" if redo else "undo"
        if not redo and self.discard_unapplied():
            return True
        name = cmds.undoInfo(query=True, redoName=True) if redo else cmds.undoInfo(query=True, undoName=True)
        if not name:
            self.report("nothing to " + verb)
            return False
        self.scene.cancel_drag()
        try:
            if redo:
                cmds.redo()
            else:
                cmds.undo()
        except RuntimeError as e:
            self.report(str(e).strip())
            return False
        # The graph watcher reloads on the attribute change; this is the same path (once, after
        # the change settles, and nothing when the graph is what the editor shows), for a step
        # that touched something else, or an editor watching nothing yet.
        self._graph_changed()
        self.status_soon()
        self._keys_soon()
        self.report("%s: %s" % (verb, name.strip()))
        return True

    def discard_unapplied(self):
        """An edit the groom never took (it left the graph unable to evaluate: a cut or a deleted
        link with nothing in its place) is undone first, before Maya's history, which never had it:
        the editor goes back to the groom's graph. False when there is none."""
        from maya import cmds
        if not self.groom or not self.model.validate() or not self.same_groom():
            return False
        try:
            text = cmds.getAttr(self.groom + ".graph") or "{}"
        except RuntimeError:
            return False
        if text == self.model.to_json():
            return False
        self.scene.cancel_drag()
        self.model = load_model(self.groom, self.catalog)
        self.rebuild()
        self.refresh_status()
        self.report("undo: the edit not applied (back to the groom's graph)")
        return True

    def fill_edit_menu(self, names=True):
        """The Edit menu's Undo / Redo, named after the next step (as Maya's Edit menu), with their keys."""
        for action, verb, keys, flag in ((self.undo_action, "Undo", UNDO_KEYS, "undoName"),
                                         (self.redo_action, "Redo", REDO_KEYS, "redoName")):
            step = ""
            if names:
                try:
                    from maya import cmds
                    step = (cmds.undoInfo(query=True, **{flag: True}) or "").strip()
                except RuntimeError:
                    step = ""
            # The key after a tab is the menu's shortcut column: shown, without a second shortcut registered.
            action.setText("%s%s\t%s" % (verb, " " + step if step else "",
                                          keys[0].toString(QtGui.QKeySequence.NativeText)))

    TEXT_EDITS = (QtWidgets.QLineEdit, QtWidgets.QTextEdit, QtWidgets.QPlainTextEdit, QtWidgets.QAbstractSpinBox)

    def watch_text_edits(self, root):
        """Text fields under `root` get Ctrl+Z for their own typing (eventFilter): a field with
        nothing left to undo passes the key on to Maya's undo, as the rest of the editor does."""
        for widget in [root] + root.findChildren(QtWidgets.QWidget):
            if isinstance(widget, self.TEXT_EDITS) and not isinstance(widget.parent(), QtWidgets.QAbstractSpinBox):
                widget.installEventFilter(self)

    def eventFilter(self, obj, event):
        if event.type() == QtCore.QEvent.KeyPress and (undo_key(event) or redo_key(event)):
            redo = redo_key(event)
            if text_history(obj, redo):
                return False  # the field undoes (redoes) its typing
            self._step_history(redo)
            return True
        return super(GraphEditor, self).eventFilter(obj, event)

    # -- Groom -------------------------------------------------------------

    def refresh_grooms(self):
        from maya import cmds
        self.groom_box.clear()
        self.groom_box.addItems(sorted(cmds.ls(type="groomLabShape") or []))
        if self.groom:
            self.groom_box.setCurrentText(self.groom)

    def set_groom(self, groom):
        self.groom = groom
        self._groom_uuid = _uuid(groom)
        self.catalog = self.catalog or gm.OperatorCatalog.from_maya()
        if getattr(self.add_menu, "filled_from", None) is not self.catalog:
            self.fill_add_menu(self.add_menu)
        self.model = load_model(groom, self.catalog)
        self._watch_graph()
        self.groom_box.setCurrentText(groom)
        self.rebuild()
        self.refresh_status()
        self.view.auto_frame = True
        QtCore.QTimer.singleShot(0, self.view.frame_readable)  # once the view has its size
        self.report(self.model.unreadable, trouble=bool(self.model.unreadable))

    def reload(self):
        self.refresh_grooms()
        if self.groom:
            self.set_groom(self.groom)

    # -- Scene -------------------------------------------------------------

    def rebuild(self):
        selected = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        self.scene.cancel_drag()
        self.scene.blockSignals(True)
        try:
            self.scene.clear()
            self.items = {}
            for backdrop in self.model.backdrops:
                self.scene.addItem(BackdropItem(self, backdrop))
            for name in self.model.order:
                item = NodeItem(self, name)
                self.scene.addItem(item)
                self.items[name] = item
            self._linked_inputs = {(c["to"], c["input"]) for c in self.model.connections}
            self._linked_outputs = set()
            self.model.prune_pins()
            self.pin_items = {}
            for c in self.model.connections:
                if c["from"] in self.items and c["to"] in self.items:
                    edge = EdgeItem(self.items[c["from"]], self.items[c["to"]], c["input"], c.get("output"))
                    for pin in self.model.route(c["to"], c["input"]):
                        if pin not in self.pin_items:
                            self.pin_items[pin] = PinItem(self, pin, edge.color)
                            self.scene.addItem(self.pin_items[pin])
                        edge.pins.append(self.pin_items[pin])
                        self.pin_items[pin].edges.append(edge)
                    if edge.pins:
                        edge.update_path()
                    self.scene.addItem(edge)
                    self._linked_outputs.add((c["from"], edge.output))
            for name in selected:
                if name in self.items:
                    self.items[name].setSelected(True)
        finally:
            self.scene.blockSignals(False)
        self.update_attribute_notes()
        self.selection_changed()

    def is_linked(self, node, port, is_output):
        """Whether a port has a link (from the sets the last rebuild made: the scene is rebuilt on
        every link change, so no scan of the connections per repaint)."""
        if is_output:
            return (node, port) in self._linked_outputs
        return (node, port) in self._linked_inputs

    # -- Search, view, bypass, copy and paste, backdrops ------------------------

    def show_search(self, scene_pos, screen_pos):
        """Type to find a node type; Enter adds it where you clicked (Tab or right-click the canvas)."""
        if not self.model:
            text = "Select a groom first: its graph opens here, then Tab adds a node"
            self.report(text)
            QtWidgets.QToolTip.showText(screen_pos, text, self.view)
            return None
        self.scene.cancel_drag()  # the popup takes the mouse: a link drag would never end
        wire = self.scene.edge_at(scene_pos)
        popup = NodeSearch(self, (scene_pos.x() - NODE_W / 2, scene_pos.y() - HEADER_H / 2),
                           on_link=wire.link() if wire is not None else None)
        popup.setAttribute(QtCore.Qt.WA_DeleteOnClose)
        popup.move(screen_pos)
        popup.show()
        popup.field.setFocus()
        self._search = popup  # kept while open

    def view_node(self, name):
        """Show a node's strands in the viewport instead of the output's (renders still use the output)."""
        self.viewed = name
        if self.groom:
            from maya import cmds
            self.silently(lambda: cmds.setAttr(self.groom + ".viewNode", name or "", type="string"))
            self.refresh_status()
        for item in self.items.values():
            item.update()
        self.report("viewing %s (renders use the output)" % name if name else "")

    def set_bypass(self, names, on):
        for name in names:
            self.model.set_bypass(name, on)
        self.rebuild()
        self.changed()

    def set_evaluation(self, names, mode):
        """When the nodes run: "always", "viewportOnly" or "renderOnly" (graph.EVALUATION_MODES)."""
        for name in names:
            self.model.set_evaluation(name, mode)
        self.rebuild()
        self.changed()
        labels = {m: label for m, label, _ in EVALUATION_CHOICES}
        self.report("%s: %s" % (", ".join(names), labels.get(mode, mode)) if mode != "always" else "")

    def upgrade_node(self, name):
        """The param panel's Upgrade: the node takes its operator's newest behaviour (applied as one
        undo step when Live). False when it had it already."""
        try:
            done = self.model.upgrade_node(name)
        except gm.GraphError as e:
            self.report(str(e))
            return False
        if done:
            self.rebuild()  # the panel's header drops the badge
            self.changed()
        return done

    def copy_selected(self):
        names = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        if names:
            QtWidgets.QApplication.clipboard().setText(self.model.copy_nodes(names))
            self.report("copied %d node%s" % (len(names), "s" if len(names) > 1 else ""))

    def paste(self, offset=(40.0, 40.0)):
        try:
            names = self.model.paste_nodes(QtWidgets.QApplication.clipboard().text(), offset)
        except (ValueError, gm.GraphError) as e:
            self.report(str(e))
            return []
        self.rebuild()
        self.scene.clearSelection()
        for name in names:
            self.items[name].setSelected(True)
        self.changed()
        return names

    def duplicate(self, names):
        if not names:
            return []
        QtWidgets.QApplication.clipboard().setText(self.model.copy_nodes(names))
        return self.paste()

    def add_backdrop(self, title="Backdrop"):
        """A labelled frame around the selected nodes (or at the view's center); moving it moves them."""
        selected = [self.items[i.name] for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        if selected:
            rect = QtCore.QRectF()
            for item in selected:
                rect = rect.united(item.sceneBoundingRect())
            rect = rect.adjusted(-30, -50, 30, 30)
        else:
            c = self.view.mapToScene(self.view.viewport().rect().center())
            rect = QtCore.QRectF(c.x() - 200, c.y() - 120, 400, 240)
        self.model.backdrops.append({"title": title, "x": rect.x(), "y": rect.y(), "w": rect.width(),
                                     "h": rect.height(), "color": [70, 90, 120]})
        self._layout_dirty = True
        self.commit_layout()
        self.rebuild()

    def remove_backdrop(self, data):
        if data in self.model.backdrops:
            self.model.backdrops.remove(data)
            self._layout_dirty = True
            self.commit_layout()
            self.rebuild()

    CARDS_STALE = ("The baked maps are out of date for this node: its cards changed since Bake Maps (the "
                   "strands it cards or its params), or the bake never had it, so the maps no longer fit them. "
                   "Bake Maps again.")

    def refresh_status(self):
        """Error badges and timing tags from the groom's last evaluation; a warning badge on each
        Cards node whose baked maps are out of date (only those whose own cards changed; a
        compound holding one gets it); a grey note on each node that does nothing as set."""
        import re
        import groomlab_core
        self.node_errors, self.node_times, self.node_warnings, self.node_notes = {}, {}, {}, {}
        if not self.groom:
            self.params.update_error()
            return
        import groomlab
        from maya import cmds
        for error in groomlab.errors(self.groom):
            match = re.match(r"node '([^']+)': (.*)", error, re.S)
            if match:
                self.node_errors[match.group(1)] = match.group(2)
        try:
            stats = json.loads(cmds.groomLabGraphStats(self.groom))
            self.node_times = stats.get("times", {})
            self.node_channels = stats.get("channels", {})
            self.viewed = stats.get("view") or None
            for path, info in (stats.get("cardNodes") or {}).items():
                top = path.split("/")[0]
                if info.get("stale") and top in self.model.nodes:
                    self.node_warnings[top] = self.CARDS_STALE
            # What nodes say that is not an error (a Jiggle with no scalp motion to react to, a
            # Clump with no centers): a grey note.
            for name, note in (stats.get("notes") or {}).items():
                if name in self.model.nodes:
                    self.node_notes[name] = note
        except (RuntimeError, ValueError):
            pass
        # An Instance or Scales naming a mesh the scene does not have (the built-in plates always are).
        for name in self.model.order:
            if self.model.node_type(name) not in ("Instance", "Scales"):
                continue
            missing = groomlab_core.missing_instance_prototypes(self.model.param(name, "prototype") or "",
                                                                cmds.objExists)
            if missing:
                note = "no mesh named %s in the scene: its instances do not draw or render" % ", ".join(missing)
                self.node_notes[name] = note + ("; " + self.node_notes[name] if name in self.node_notes else "")
        for name, item in self.items.items():
            item.setToolTip(self.node_errors.get(name) or self.node_warnings.get(name) or self.note_of(name)
                            or item.op_type)
            item.update()
        self.params.update_error()  # the selected node's error line, as this evaluation left it
        self._status_drawn = self._status_stamp()

    def selection_changed(self):
        nodes = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        # A Sculpt node selected: brushes shape its guides (before its panel shows, which says so).
        if len(nodes) == 1 and self.model.node_type(nodes[0]) == "Sculpt":
            self.target_sculpt(nodes[0])
        self.params.show_node(nodes[0] if len(nodes) == 1 else None)

    def sculpt_target(self):
        """The Sculpt node brushes shape, or None."""
        from groomlab import sculpts
        return sculpts.target(self.groom) if self.groom else None

    def target_sculpt(self, name):
        """Brushes shape Sculpt node `name`'s guides (None: the base groom). A node the groom's graph
        does not hold yet (not applied) is said, not picked. No undo step: what brushes shape is a
        tool setting."""
        from groomlab import sculpts, shots
        if not self.groom:
            return
        if name and name not in sculpts.sculpt_nodes(self.groom):
            self.report("apply the graph to brush %s's guides" % name)
            return
        if (sculpts.target(self.groom) or None) == (name or None):
            return
        self.silently(lambda: sculpts.set_target(self.groom, name) if name else shots.set_target(self.groom, None))

    def clear_sculpt_edits(self, name):
        """Drop Sculpt node `name`'s strokes (one undo step)."""
        from groomlab import sculpts
        if self.groom:
            sculpts.clear_edits(self.groom, name)

    # -- Pins (layout: Unreal's reroute nodes) and snapping ----------------------

    def add_pin(self, link, at, index):
        """A pin at scene point `at` on link (src, output, dst, input), at `index` in its route."""
        if self.model is None:
            return
        _src, _output, dst, input_name = link
        if self.snapping():
            at = snapped(at)
        try:
            pin = self.model.add_pin(dst, input_name, at.x(), at.y(), index)
        except gm.GraphError as e:
            self.report(str(e))
            return
        self._layout_dirty = True
        self.commit_layout()
        self.rebuild()
        self.scene.clearSelection()
        self.pin_items[pin].setSelected(True)
        self.report("pinned %s.%s (drag the pin to route it; Delete removes it)" % (dst, input_name))

    def link_from_pin(self, pin, dst, input_name):
        """Link the signal a pin carries to dst.input_name, through the pins up to it (one undo
        step, as any link)."""
        source = self.model.pin_source(pin)
        if source is None:
            return
        via = self.model.route_to(pin)
        self.link(source[0], dst, input_name, output=source[1])
        if self.model.source_of(dst, input_name) == source[0] and via:
            self.model.set_route(dst, input_name, via)
            self._layout_dirty = True
            self.commit_layout()
            self.rebuild()

    def feed_pin(self, pin, src, output):
        """Every link through the pin comes from src's `output` now (Blueprints' reroute node fed
        again), running on through the pin and the ones after it: one undo step. Nothing changes
        when one of them can't take it (its reason said)."""
        links = self.model.pin_links(pin)
        if not links:
            return
        before = self.model.to_dict()
        try:
            for (to, input_name), index in links:
                via = self.model.routes[(to, input_name)]["via"][index:]
                self.model.connect(src, to, input_name, output=output, by_hand=True)
                self.model.set_route(to, input_name, via)
        except gm.GraphError as e:
            self.model.load_dict(before)
            self.report("Not linked: %s" % e)
            self.rebuild()
            return
        self.rebuild()
        self.changed()
        self.report("%d link%s fed from %s through the pin" % (len(links), "" if len(links) == 1 else "s", src))

    def pin_moved(self, pin, pos):
        if self.model is not None and pin in self.model.pins:
            self.model.move_pin(pin, pos.x(), pos.y())
            self._layout_dirty = True

    def merge_pins(self, keep, drop):
        if self.model is None:
            return
        if self.model.merge_pins(keep, drop):
            self.report("pins joined: the links run through one pin")
        else:
            self.report("pins not joined: they carry different links' signals")
            return
        self._layout_dirty = True
        self.commit_layout()
        self.rebuild()

    SNAP_VAR = "groomLabGraphSnap"

    def snapping(self, item=None):
        """Whether Snap is on (for `item`: and it is being dragged, not placed by a rebuild)."""
        box = getattr(self, "snap_box", None)
        if box is None or not box.isChecked():
            return False
        return item is None or (item.scene() is not None and item.scene().mouseGrabberItem() is not None)

    def set_snapping(self, on):
        try:
            from maya import cmds
            cmds.optionVar(intValue=(self.SNAP_VAR, int(bool(on))))
        except (ImportError, RuntimeError):
            pass
        self.report("snapping nodes and pins to the grid" if on else "snapping off")

    def node_moved(self, name, pos):
        if self.model is not None and name in self.model.nodes:
            self.model.set_position(name, pos.x(), pos.y())
            self._layout_dirty = True

    def commit_layout(self):
        if getattr(self, "_layout_dirty", False) and self.groom:
            self._layout_dirty = False
            self._save_layout()

    def _save_layout(self):
        """Write the layout (outside undo). The text written is remembered for the one sync its
        attribute change sets off (none when the graph's text did not change: nothing to skip)."""
        from maya import cmds
        before = cmds.getAttr(self.groom + ".graph")
        text = self.silently(lambda: self.model.save_layout(self.groom))
        self._layout_text = text if text != before else None

    def layout_nodes(self):
        self.model.auto_layout()
        self.rebuild()
        self.view.frame_all()
        if self.groom:
            self._save_layout()

    # -- Edits (each applies when Live) ---------------------------------------

    def build_add_menu(self, at=None):
        """Node types by category; `at` (a scene point) places the node there instead of wiring it."""
        menu = QtWidgets.QMenu(self)
        menu.setToolTipsVisible(True)
        self.fill_add_menu(menu, at)
        return menu

    def fill_add_menu(self, menu, at=None):
        """(Re)fill an Add Node menu (graph_ui.json's addMenu) from the catalog (the toolbar's: once a
        groom brings the catalog, and as it opens if it is still empty)."""
        menu.clear()
        # Submenus made with an explicit parent and kept: PySide gives addMenu("title")
        # results to Python, which garbage-collects them (empty categories).
        menu.submenus = []
        menu.filled_from = self.catalog
        refresh_library(self.catalog)

        def node_categories(target, _item):
            for category, types in categories(self.catalog):
                sub = QtWidgets.QMenu(category.replace("&", "&&"), target)  # "Fields & Curves": no mnemonic
                sub.setToolTipsVisible(True)  # each node's description
                target.addMenu(sub)
                menu.submenus.append(sub)
                for op_type in types:
                    action = sub.addAction(label_of(op_type, self.catalog), lambda t=op_type: self.add_node(t, at=at))
                    action.setData(op_type)
                    action.setToolTip(describe(op_type, self.catalog))

        # New Script Node and Import Node... add theirs where the menu was asked for.
        actions = dict(self.ACTIONS, **{"node.newScript": definitions.bound(lambda: self.new_script_node(at=at)),
                                        "node.import": definitions.bound(lambda: self.import_nodes(None, at=at))})
        widgets.fill_menu(menu, definitions.get("graph/addMenu"), definitions.binder(actions, self),
                          {"catalog"} if self.catalog is not None else (),
                          generators={"nodeCategories": widgets.menu_filler(node_categories)})
        return menu

    # -- Compound nodes (groomlab.nodes): the .glnode library ------------------------------------

    def show_save_node(self, name):
        """Save as Node: the form in the side panel (never a dialog)."""
        if not self.model or name not in self.model.nodes:
            return None
        self.scene.clearSelection()
        if name in self.items:
            self.items[name].setSelected(True)
        return self.params.show_save_node(name)

    def show_inner_param(self, compound, target, on=True):
        """Show inner param `target` ("node.param") on the compound's panel (named after the param,
        node + Param when taken), or stop showing it. One undo step when Live."""
        try:
            if on:
                inner, _, param = target.partition(".")
                taken = set(p["name"] for p in self.model.node_spec(compound)["params"])
                exposed = param if param not in taken else inner + param[:1].upper() + param[1:]
                self.model.expose(compound, exposed, target)
            else:
                self.model.unexpose(compound, target)
        except gm.GraphError as e:
            self.report(str(e), trouble=True)
            return False
        self.rebuild()
        self.changed()
        return True

    def save_as_node(self, name, header):
        """Save Compound `name` as a library node (header: label, category, description, id,
        version, author, license, params...: groomlab_core.node_file_from_compound's) to the user's
        node folder; the node then holds the saved copy (one undo step when Live). Returns the
        file's path, None (reported) when it cannot."""
        import groomlab_core
        try:
            text = groomlab_core.node_file_from_compound(self.model.to_json(), name, json.dumps(header),
                                                         self.model.groom_size)
            path = groomlab_core.save_node_file(text, "")
            self.model.update_library_node(name, json.loads(text)["id"])
        except (gm.GraphError, ValueError, RuntimeError, IOError, OSError) as e:
            self.report("Save as Node: %s" % e, trouble=True)
            return None
        self.rebuild()
        self.changed()
        self.params.show_node(name)
        saved = json.loads(text)
        self.report("saved %s (version %d) to %s" % (saved["label"], saved["version"], path))
        return path

    def save_to_library(self, name):
        """A library node written over its file as it is now (its values the new defaults), one
        version up."""
        info = self.model.library_info(name) or {}
        version = max(info.get("version", 0), info.get("latest", 0)) + 1
        return self.save_as_node(name, {"version": version})

    def update_library_node(self, name):
        """Update Node: the library's newer copy (one undo step when Live). False when it had it."""
        try:
            done = self.model.update_library_node(name)
        except gm.GraphError as e:
            self.report(str(e), trouble=True)
            return False
        if done:
            self.rebuild()
            self.changed()
            self.report("%s updated to the library's version" % name)
        return done

    def import_nodes(self, paths=None, at=None):
        """Copy .glnode files into the user's node library and add each to the graph (at `at`, a scene
        point). No paths: picked in a file dialog (only a click gets there; scripts pass them).
        Returns the names of the nodes added."""
        from groomlab import nodes
        if paths is None:
            paths, _ = QtWidgets.QFileDialog.getOpenFileNames(self, "Import Node", "", "GroomLab nodes (*.glnode)")
        made = []
        for i, path in enumerate(paths or []):
            try:
                saved = nodes.import_node(path)
                node_id = json.loads(open(saved).read())["id"]
            except (ValueError, IOError, OSError, KeyError) as e:
                self.report("Import Node: %s" % e, trouble=True)
                continue
            if self.model is None:
                self.report("imported %s into your node library" % node_id)
                continue
            point = None if at is None else (at[0] + 30 * i, at[1] + 30 * i)
            name = self.add_node("node:" + node_id, at=point, wire=False if at is not None else None)
            if name:
                made.append(name)
        if made:
            self.report("imported and added %s" % ", ".join(made))
        return made

    def open_nodes_folder(self):
        import os
        from groomlab import nodes
        folder = nodes.user_dir()
        if not os.path.isdir(folder):
            os.makedirs(folder)
        QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(folder))

    # -- Script nodes (groomlab.nodes, groomlab.ui.script_editor) --------------------------------

    def new_script_node(self, at=None, label="My Script"):
        """New Script Node: a script node from the template, wired after the selected node (as Add
        Node wires), its editor open in the side panel. Returns its name."""
        from groomlab import nodes
        if not self.model:
            self.report("Select a groom first: its graph opens here, then add nodes to it")
            return None
        definition = nodes.script_template(label)
        name = self.add_node("Script", at=at, params={"definition": definition},
                             name=self.model.unique_name(nodes.node_name(label)))
        if name:
            self.show_script_editor(name)
        return name

    def show_script_editor(self, name):
        """Edit Node... on a script node: its editor in the side panel (never a dialog). Returns it."""
        if not self.model or name not in self.model.nodes or self.model.node_type(name) != "Script":
            return None
        self.params.editing_script = name
        if name in self.items and not self.items[name].isSelected():
            self.scene.clearSelection()
            self.items[name].setSelected(True)  # the panel follows (ParamPanel.show_node)
        panel = getattr(self.params, "script_panel", None)
        if panel is None or not widgets.alive(panel) or panel.node != name or self.params.widget() is not panel:
            panel = self.params.show_script_editor(name)
        return panel

    def close_script_editor(self, name):
        """Done: the node's params again."""
        self.params.editing_script = None
        self.params.show_node(name if self.model is not None and name in self.model.nodes else None)

    def set_script_definition(self, name, definition):
        """A script node takes an edited definition (the editor's, once it checks): the params the
        header no longer has go (with their links), a param still at its old default takes the new
        one, and the graph applies when Live."""
        model = self.model
        before = dict((p["name"], p.get("default")) for p in model.node_spec(name)["params"])
        old = model.param(name, "definition") or {}
        old_names = set(p.get("name") for p in old.get("params", []))
        new_names = set(p.get("name") for p in definition.get("params", []))
        model.set_param(name, "definition", definition)
        node = model.nodes[name]
        params = node.setdefault("params", {})
        gone = old_names - new_names
        for key in gone:
            params.pop(key, None)
        if node.get("paramInputs"):
            node["paramInputs"] = [p for p in node["paramInputs"] if p not in gone]
        model.connections = [c for c in model.connections if not (c["to"] == name and c["input"] in gone)]
        for p in model.node_spec(name)["params"]:
            if p["name"] in new_names and (p["name"] not in params or params[p["name"]] == before.get(p["name"])):
                params[p["name"]] = p.get("default")
        self.rebuild()
        self.changed()

    def save_script_node(self, name, definition):
        """Write a script node's definition to the user's node library (checked) and give the node
        that copy. Returns the file's path, None (reported) when it cannot."""
        import groomlab_core
        try:
            path = groomlab_core.save_node_file(json.dumps(definition, indent=2), "")
        except (RuntimeError, ValueError, IOError, OSError) as e:
            self.report("Save to Library: %s" % e, trouble=True)
            return None
        if definition != self.model.param(name, "definition"):
            self.set_script_definition(name, definition)
        else:
            self.rebuild()  # (its update mark)
        self.report("saved %s (version %d) to %s" % (definition.get("label"), definition.get("version", 1), path))
        return path

    def save_script_to_library(self, name):
        """Save to Library on a script node: its file one version up when the library has it."""
        panel = getattr(self.params, "script_panel", None)
        if panel is not None and widgets.alive(panel) and panel.node == name and self.params.widget() is panel:
            return panel.save()
        from groomlab import nodes
        definition = nodes.next_version(self.model.param(name, "definition") or {})
        return self.save_script_node(name, definition)

    def add_node(self, op_type, at=None, wire=None, params=None, name=None, on_link=None):
        """Add a node at `at` (a scene point; the view's center by default); with one node selected
        and `wire` (by default: no `at`), wire the new one after it (placed after it unless `at`).
        `on_link` (src, output, dst, input): inserted into that link instead (Tab over a wire).
        `params` and `name`: the new node's, as GraphModel.add_node takes them."""
        if not self.model:
            self.report("Select a groom first: its graph opens here, then add nodes to it")
            return None
        center = self.view.mapToScene(self.view.viewport().rect().center())
        selected = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        try:
            name = self.model.add_node(op_type, name=name, params=params, pos=at or (center.x(), center.y()))
        except (gm.GraphError, ValueError) as e:
            self.report(str(e))
            return None
        wired = refused = None
        if on_link is not None:
            src, output, dst, input_name = on_link
            try:
                self.model.insert_on_link(src, dst, input_name, name, output=output)
                wired = src
            except gm.GraphError as e:
                refused = "Not inserted: %s" % e  # added unlinked
        elif len(selected) == 1 and (at is None if wire is None else wire):
            src = selected[0]
            try:
                inputs = [p["name"] for p in self.model.node_spec(name)["inputs"]]
                downstream = [d for d in self.model.downstream_of(src)
                              if gm.types_match(self.model.output_type(name), self.model.input_spec(d[0], d[1])["type"])]
                if inputs and downstream:
                    self.model.insert_between(src, downstream[0][0], name)
                    wired = src
                elif inputs:
                    self.model.connect(src, name, inputs[0])
                    wired = src
                if at is None:
                    x, y = self.model.position(src)
                    self.model.set_position(name, x + gm.LAYOUT_DX / 2, y + gm.LAYOUT_DY)
            except gm.GraphError:
                wired = None  # incompatible: leave it unconnected
        self.rebuild()
        self.scene.clearSelection()
        self.items[name].setSelected(True)
        self.changed()
        if wired and at is not None:  # Tab: say where it went, before what the apply said
            self.report("; ".join(t for t in ("added %s after %s" % (name, wired), self.status.text()) if t))
        elif refused:
            self.report("; ".join(t for t in (refused, self.status.text()) if t))
        return name

    def painted_maps(self):
        """The groom's painted maps ([{"name", "kind", ...}]; none without a groom): Tab offers a
        Map node reading each."""
        if not self.groom:
            return []
        try:
            import groomlab
            return groomlab.maps(self.groom)
        except Exception:
            return []

    def guide_maps(self):
        """The maps painted on the guides: Interpolate (and ToStrands) turn them into attributes."""
        if not self.groom:
            return []
        try:
            import groomlab
            return [m["name"] for m in groomlab.maps(self.groom) if m.get("kind") == "guides"]
        except Exception:
            return []

    def channels(self):
        """The attributes per node as the graph is edited (GraphModel.channels), kept until it changes."""
        if self.model is None:
            return {}
        maps = self.guide_maps()
        key = (json.dumps(self.model.to_dict(), sort_keys=True), tuple(maps))
        if getattr(self, "_channels_key", None) != key:
            try:
                self._channels = self.model.channels(maps)
            except gm.GraphError:
                self._channels = {}
            self._channels_key = key
        return self._channels

    def update_attribute_notes(self):
        """A grey note (the node's "i") on each node reading an attribute no node upstream writes."""
        notes = {}
        for name, info in self.channels().items():
            missing = [r for r in info.get("reads", []) if not r["reaches"] and not r.get("optional")
                       and not self.quiet_read(name, r["param"])]
            if missing:
                notes[name] = "; ".join("reads the attribute %s (%s), which no node upstream writes: it is ignored"
                                        % (r["name"], r["param"]) for r in missing)
        changed = notes != getattr(self, "attribute_notes", {})
        self.attribute_notes = notes
        if changed:
            for item in self.items.values():
                item.update()

    def quiet_read(self, name, param):
        """Whether node `name`'s attribute-reading `param` keeps its "nothing upstream writes it"
        note to itself (QUIET_READS): both it and the param using it at their defaults."""
        if self.model is None or name not in self.model.nodes:
            return False
        uses = QUIET_READS.get((self.model.node_type(name), param))
        if uses is None:
            return False
        defaults = {s["name"]: s.get("default") for s in self.model.node_spec(name)["params"]}
        try:
            return all(self.model.param(name, p) == defaults.get(p) for p in (param, uses))
        except gm.GraphError:
            return False

    def note_of(self, name):
        """The node's grey note: what its last evaluation said, and an attribute it reads that does not reach it."""
        parts = [p for p in (self.node_notes.get(name), getattr(self, "attribute_notes", {}).get(name)) if p]
        return "; ".join(parts)

    def resolution_note(self, name):
        """A Scatter's or Resample's strand resolution in the core's words: which Resample sets it ('' otherwise)."""
        if self.model is None or name not in self.model.nodes or self.model.node_type(name) not in ("Scatter", "Resample"):
            return ""
        try:
            import groomlab_core
            return groomlab_core.load_graph_json(self.model.to_dict()).strand_resolution_note(name)
        except Exception:
            return ""

    def channel_sources(self, node):
        """[{"name", "node"}] of the attributes reaching `node`, each with the node that wrote it last
        (the graph as set), then any other the last evaluation found on what feeds it (a Reference's)."""
        if self.model is None or node not in self.model.nodes:
            return []
        found = list(self.channels().get(node, {}).get("incoming", []))
        names = {c["name"] for c in found}
        for spec in self.model.node_spec(node)["inputs"]:
            if spec["type"] not in ("Strands", "Curves"):
                continue
            source = self.model.source_of(node, spec["name"])
            for name in self.node_channels.get(source, []) if source else []:
                if name not in names and not name.startswith("_"):
                    found.append({"name": name, "node": source})
                    names.add(name)
        return found

    def channels_into(self, node):
        """The names of the attributes reaching `node` (channel_sources): what its attribute maps
        and reading params can read."""
        return sorted(c["name"] for c in self.channel_sources(node))

    def color_sets(self):
        """The scalp's color sets."""
        import groomlab
        from maya import cmds
        scalp = self.groom and groomlab.scalp_mesh(self.groom)
        if not scalp:
            return []
        return cmds.polyColorSet(scalp, query=True, allColorSets=True) or []

    # Not connect/disconnect: in Maya's dockable window those names reach
    # QObject.connect/disconnect instead, and links silently failed.
    def link(self, src, dst, input_name, detach=None, output=None):
        """Link src's output `output` (a name; None: its first) to dst.input_name; `detach` (node,
        input) is a link being moved: removed in the same step."""
        try:
            if detach and detach != (dst, input_name):
                self.model.disconnect(*detach)
            kind = self.value_kind_for_input(src, dst, input_name)
            if kind is not None:
                self.model.set_param(src, "kind", kind)
            self.model.connect(src, dst, input_name, output=output, by_hand=True)
        except gm.GraphError as e:
            self.report(str(e))
            self.rebuild()
            return
        self.rebuild()
        self.changed()

    def value_kind_for_input(self, src, dst, input_name):
        """A kind an unconnected Value node can adopt for a parameter input, else None."""
        if self.model.node_type(src) != "Value" or self.model.downstream_of(src):
            return None
        spec = self.model.param_spec(dst, input_name)
        if spec is None:
            return None
        kind = spec.get("kind")
        if kind == "float":
            return "number"
        if kind == "int":
            return "integer"
        if kind == "bool":
            return "bool"
        if kind == "vec3":
            return "color" if spec.get("unit") == "color" else "vector"
        if kind == "string":
            return "text"
        return None

    def set_param_exposed(self, name, param, exposed):
        """Give a param an input port (a field node can then drive it), or take it away."""
        try:
            if exposed:
                self.model.expose_param(name, param)
            else:
                self.model.unexpose_param(name, param)
        except gm.GraphError as e:
            self.report(str(e))
            return
        QtCore.QTimer.singleShot(0, self.rebuild)  # not from inside the node's own menu
        self.changed()

    def toggle_param_inputs(self, name):
        """Expose every connectable param, or collapse them all when they are already exposed."""
        try:
            params = [p["name"] for p in self.model.node_spec(name)["params"] if gm.linkable(p)]
            expose = not all(p in self.model.param_inputs(name) for p in params)
            connected = {c["input"] for c in self.model.connections if c["to"] == name}
            for param in params:
                if (param in self.model.param_inputs(name)) != expose:
                    if expose or param not in connected:
                        (self.model.expose_param if expose else self.model.unexpose_param)(name, param)
        except gm.GraphError as e:
            self.report(str(e))
            return
        self.rebuild()
        self.changed()

    def unlink(self, dst, input_name):
        self.model.disconnect(dst, input_name)
        self.rebuild()
        self.changed()

    def cut_links(self, links):
        """Disconnects the links (src, output, dst, input) an Alt+Shift drag crossed: one edit, as
        Delete on them would be (one undo step once applied)."""
        for _src, _output, dst, input_name in links:
            self.model.disconnect(dst, input_name)
        self.rebuild()
        self.changed()
        if not self.model.validate():
            self.report("cut %d link%s" % (len(links), "" if len(links) == 1 else "s"))

    def insert_on_wire(self, name, link):
        """Put node `name` into the link (src, output, dst, input): a node dropped on a wire."""
        src, output, dst, input_name = link
        try:
            self.model.insert_on_link(src, dst, input_name, name, output=output)
        except gm.GraphError as e:
            self.report("Not inserted: %s" % e)
            return
        self.rebuild()
        self.changed()
        self.report("inserted %s between %s and %s" % (name, src, dst))

    def delete_nodes(self, names):
        for name in names:
            self.model.remove_node(name)
        self.rebuild()
        self.changed()

    def collapse(self, names):
        """Selected nodes into one Compound node (PG-6)."""
        try:
            name = self.model.collapse(names)
        except gm.GraphError as e:
            self.report(str(e))
            return None
        self.rebuild()
        self.items[name].setSelected(True)
        self.changed()
        return name

    def collapse_selected(self):
        names = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        if len(names) < 1:
            self.report("select the nodes to group")
            return None
        return self.collapse(names)

    def expand_selected(self):
        names = [i.name for i in self.scene.selectedItems()
                 if isinstance(i, NodeItem) and self.model.node_type(i.name) == "Compound"]
        if not names:
            self.report("select the Compound nodes to ungroup")
            return
        self.expand(names)

    def expand(self, names):
        """Ungroup Compound nodes: their nodes go back into the graph (one undo step)."""
        done = []
        for name in names:
            try:
                self.model.expand(name)  # leaves the graph as it was when it fails
                done.append(name)
            except (gm.GraphError, ValueError) as e:
                self.report("%s: %s" % (name, e))
        if done:
            self.rebuild()
            self.changed()

    def delete_selected(self):
        nodes = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        edges = [i for i in self.scene.selectedItems() if isinstance(i, EdgeItem)]
        backdrops = [i.data for i in self.scene.selectedItems() if isinstance(i, BackdropItem)]
        pins = [i.pin for i in self.scene.selectedItems() if isinstance(i, PinItem)]
        if pins:  # layout: no undo step, the links straighten there
            for pin in pins:
                self.model.remove_pin(pin)
            self._layout_dirty = True
            self.commit_layout()
            if not (nodes or edges or backdrops):
                self.rebuild()
                return
        for e in edges:
            self.model.disconnect(e.dst_item.name, e.input_name)
        for name in nodes:
            self.model.remove_node(name)
        for data in backdrops:
            self.model.backdrops.remove(data)
            self._layout_dirty = True
        if backdrops:
            self.commit_layout()
        if nodes or edges or backdrops:
            self.rebuild()
            self.changed()

    def set_output(self, name):
        self.model.set_output(name)
        self.rebuild()
        self.changed()

    def bake_to_guides(self, name, group=None, density=1.0):
        """A node's strands (or guides) into the groom's guides (groomlab.bake.bake_guides): the root
        group is asked for when not given. The graph is applied first when the groom lacks the node.
        Returns the new guide IDs, or None."""
        import groomlab
        from groomlab import bake
        if not self.groom:
            return None
        stored = [n["name"] for n in groomlab.get_graph(self.groom, expand_default=True).get("nodes", [])]
        if name not in stored and not self.apply():
            return None
        if group is None:
            roots = [g["name"] for g in groomlab.list_guide_groups(self.groom) if g["depth"] == 0]
            choices = ["baked"] + [r for r in roots if r != "baked"]
            group, ok = QtWidgets.QInputDialog.getItem(self, "Bake Node to Guides", "%s's strands into root group:" % name,
                                                       choices, 0, True)
            if not ok or not group.strip():
                return None
            group = group.strip()
        try:
            ids = bake.bake_guides(self.groom, name, group=group, density=density)
        except (RuntimeError, ValueError) as e:
            self.report(str(e))
            return None
        self.report("baked %d guides from %s into %s" % (len(ids), name, group))
        return ids

    def rename_node(self, name):
        new, ok = QtWidgets.QInputDialog.getText(self, "Rename node", "Name:", text=name)
        if ok and new and new != name:
            try:
                self.model.rename_node(name, new)
            except gm.GraphError as e:
                self.report(str(e))
                return
            self.rebuild()
            self.changed()

    def keyed(self, name, param):
        """Whether a param is keyed (linked to its attribute on the groom): edits then go there."""
        return bool(self.groom) and self.model.control_of(name, param) is not None

    def previewing(self):
        """Whether a slider drag is previewing on the groom (outside undo, until it commits)."""
        return (self._preview_base is not None or self._key_preview is not None
                or getattr(self, "_preview_pending", False) or getattr(self, "_gpu_dragging", False))

    def sync_gpu_action(self):
        """The menu's check follows the preference (another editor, a script may have changed it)."""
        try:
            import groomlab
            on = groomlab.gpu_preview()
        except (RuntimeError, AttributeError, TypeError):  # the plugin is not loaded
            on = False
        self.gpu_action.blockSignals(True)
        self.gpu_action.setChecked(on)
        self.gpu_action.blockSignals(False)

    def set_gpu_preview(self, on=None):
        """Preview Drags on the GPU, for this session and the next (groomlab.gpu_preview)."""
        import groomlab
        try:
            groomlab.gpu_preview(self.gpu_action.isChecked() if on is None else bool(on))
        except (RuntimeError, AttributeError, TypeError) as e:
            self.report(str(e))
        self.sync_gpu_action()

    def _gpu_preview(self, name, param, value):
        """The GPU look preview (on unless turned off: groomlab.gpu_preview): a drag of a number, a
        seed or a profile's ramp of a node the GPU runs (Noise, Curl, Width, Clump, Length,
        Interpolate) draws from the GPU, the graph (and a keyed param's attribute) left alone until
        the release commits it on the CPU (set_param: a keyed one keys its attribute). A vec3
        (Curl's weightDirection) goes as a vector. False: preview as ever (the command says
        whether the GPU takes the groom as it is)."""
        flags = gpu_drag_flags(value)
        if (not (self.live and self.groom) or flags is None
                or self._preview_base is not None or self._key_preview is not None):
            return False
        from maya import cmds
        try:
            taken = cmds.groomLabGpuPreview(self.groom, node=name, param=param, **flags)
        except (RuntimeError, TypeError, AttributeError):  # no such command: the switch was off at load
            return False
        if taken:
            if not self.keyed(name, param):  # a keyed param's own value stays (its attribute takes the release)
                self.model.set_param(name, param, value)
            self._gpu_dragging = True
            cmds.refresh(currentView=True, force=False)
        return bool(taken)

    def preview_param(self, name, param, value):
        """Show a value on the groom while a slider is dragged, outside undo; set_param commits it.
        Writes are coalesced (at most one per 40 ms, the latest value): each is a full evaluation.
        A keyed param previews through its attribute instead of the graph."""
        if self._gpu_preview(name, param, value):
            return
        if self.keyed(name, param):
            self._preview_keyed(name, param, value)
            return
        self.model.set_param(name, param, value)
        if not (self.live and self.groom) or self.model.validate():
            return
        if self._preview_base is None:  # a drag is on from now: syncing from the groom must not undo it
            from maya import cmds
            self._preview_base = cmds.getAttr(self.groom + ".graph") or ""
        if not getattr(self, "_preview_pending", False):
            self._preview_pending = True
            QtCore.QTimer.singleShot(40, self._write_preview)

    def _preview_keyed(self, name, param, value):
        """A keyed param's drag: its attribute takes the value live, outside undo. end_preview puts
        the attribute back, so releasing commits one undo step (a key with Auto Key on) from it."""
        import groomlab
        from maya import cmds
        if not (self.live and self.groom) or isinstance(value, dict) or not self.same_groom():
            return
        if self._key_preview is None:
            plugs = groomlab.param_plugs(self.groom, name, param)
            if not plugs:
                return
            self._key_preview = {"plugs": plugs, "before": [cmds.getAttr(p) for p in plugs], "values": None}
        values = list(value) if len(self._key_preview["plugs"]) == 3 else [value]
        self._key_preview["values"] = [float(v) for v in values]
        if not getattr(self, "_preview_pending", False):
            self._preview_pending = True
            QtCore.QTimer.singleShot(40, self._write_preview)

    def _set_silently(self, plugs, values):
        """Set attributes outside undo (a drag's preview, or putting it back)."""
        from maya import cmds
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            for plug, v in zip(plugs, values):
                cmds.setAttr(plug, v)
        except RuntimeError:  # the attribute went (undo, another groom)
            pass
        finally:
            cmds.undoInfo(stateWithoutFlush=True)

    def _write_preview(self):
        self._preview_pending = False
        if self._key_preview is not None:
            if self._key_preview["values"] is not None and self.same_groom():
                from maya import cmds
                self._set_silently(self._key_preview["plugs"], self._key_preview["values"])
                cmds.refresh(currentView=True, force=False)
            return
        if not (self.live and self.groom) or self.model.validate() or not self.same_groom():
            return
        from maya import cmds
        if self._preview_base is None:
            self._preview_base = cmds.getAttr(self.groom + ".graph") or ""
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            cmds.setAttr(self.groom + ".graph", self.model.to_json(), type="string")
        finally:
            cmds.undoInfo(stateWithoutFlush=True)
        cmds.refresh(currentView=True, force=False)

    def end_preview(self):
        """Put back the graph (or a keyed param's attribute) from before a slider drag, silently, so
        the commit is one undo step from it."""
        self._preview_pending = False  # a write still waiting is superseded by the commit
        if getattr(self, "_gpu_dragging", False):
            self._gpu_dragging = False
            try:
                from maya import cmds
                cmds.groomLabGpuPreview(self.groom, endDrag=True)
            except (RuntimeError, TypeError, AttributeError):
                pass
        keyed, self._key_preview = self._key_preview, None
        if keyed is not None:
            self._set_silently(keyed["plugs"], keyed["before"])
        if self._preview_base is None or not self.groom:
            self._preview_base = None
            return
        from maya import cmds
        cmds.undoInfo(stateWithoutFlush=False)
        try:
            cmds.setAttr(self.groom + ".graph", self._preview_base, type="string")
        finally:
            cmds.undoInfo(stateWithoutFlush=True)
        self._preview_base = None

    def set_param(self, name, param, value):
        self.end_preview()
        if self.keyed(name, param):
            # Its attribute: a key with Auto Key on (and keys there), else the value until the
            # time changes, as in Maya's Channel Box.
            import groomlab
            if not self.same_groom():
                return
            if isinstance(value, dict):
                self.report("%s.%s is keyed: Remove Animation (right-click it) before giving it a map" % (name, param))
                later(lambda: self.params.show_node(self.params.node))  # the field back to its value
                return
            try:
                groomlab.set_param_value(self.groom, name, param, value)
            except (RuntimeError, ValueError, TypeError) as e:
                self.report(str(e))
            self.params.refresh_keys()
            return
        self.model.set_param(name, param, value)
        reads_guides = isinstance(value, dict) and (value.get("guide") or value.get("attribute"))
        if reads_guides and self.model.node_type(name) == "Scatter" and self.model.source_of(name, "guides") is None:
            # Before strands exist, a painted map is read from the guides: feed them in.
            source = next((n for n in self.model.order if self.model.node_type(n) in ("Guides", "ImportCurves")), None)
            if source:
                self.model.connect(source, name, "guides")
                QtCore.QTimer.singleShot(0, self.rebuild)  # not while the param widget is mid-signal
        if name == self.params.node:
            self.params.update_visibility()  # rows shown only in some mode follow it
        self.changed()

    def set_live(self, live):
        self.live = live
        self.apply_action.setVisible(not live)
        if live:
            self.apply()

    def changed(self):
        self.update_attribute_notes()
        problems = self.model.validate()
        # A node the output isn't made from (its output unplugged) never stops the graph applying:
        # its problems are only noted.
        notes = ["warning: " + w for w in self.model.warnings()] +             ["note: %s (not used by the output)" % p for p in self.model.unused_problems()]
        self.report("; ".join(problems or notes))
        if self.live and not problems:
            self.apply()

    def same_groom(self):
        """False once the groom is gone or is another node of the same name (File > New or Open):
        then the editor reloads instead of writing this graph onto it."""
        if self.groom and _uuid(self.groom) != getattr(self, "_groom_uuid", None):
            self.report("the groom changed (new scene?): reloaded")
            later(self.reload)
            return False
        return True

    def apply(self):
        """Write the graph to the groom (undoable)."""
        if not self.groom or not self.same_groom():
            return False
        if self.model.unreadable:
            self.report(self.model.unreadable, trouble=True)
            return False
        problems = self.model.validate()
        if problems:
            self.report("not applied: " + "; ".join(problems))
            return False
        from maya import cmds
        # The layout write a sync may still be waiting to skip is behind this edit now: an undo back
        # to that text later is a change to show, not our own write.
        self._layout_text = None
        self.model.save(self.groom)  # one undo step
        count = cmds.getAttr(self.groom + ".outStrandCount")
        import groomlab
        try:  # a hair shader Create Hair Shader made follows the look (a pigment one turning RGB...)
            if groomlab.sync_hair_shader(self.groom):
                self.report("hair shader made again for the new look")
        except (RuntimeError, ValueError) as e:
            cmds.warning("GroomLab: the hair shader could not follow the look: %s" % e)
        errors = groomlab.errors(self.groom)
        self.refresh_status()
        total = sum(self.node_times.values())
        warnings = self.model.warnings()
        self.report("%d strands (preview), %.1f ms" % (count, total) + ("; error: " + "; ".join(errors) if errors else "")
                    + ("; warning: " + "; ".join(warnings) if warnings else ""))
        return True

    # -- Presets -----------------------------------------------------------

    def fill_units_menu(self):
        from groomlab import units
        self.units_menu.clear()
        for kind, title in (("length", "Lengths"), ("width", "Widths"), ("density", "Density")):
            sub = self.units_menu.addMenu(title)
            group = qt.QActionGroup(sub)
            current = units.unit(kind)[0]
            for key, text, _ in units.UNITS[kind]:
                action = sub.addAction(text)
                action.setCheckable(True)
                action.setChecked(key == current)
                group.addAction(action)
                action.triggered.connect(lambda _=False, k=kind, u=key: self.set_units(k, u))

    def set_units(self, kind, key):
        from groomlab import units
        units.set_unit(kind, key)
        self.params.show_node(self.params.node)

    def fill_preset_menu(self):
        """A submenu per category (presets.CATEGORIES first, in order), then the presets with no
        category (your own), each titled, its description as the tooltip and status tip."""
        from groomlab import presets
        self.preset_menu.clear()
        # Submenus made with an explicit parent and kept (see build_add_menu).
        self.preset_menu.submenus = []

        def fill_presets(menu, _item):
            by_category = {}
            for info in presets.list_presets(details=True):
                by_category.setdefault(info["category"], []).append(info)
            order = [c for c in presets.CATEGORIES if c in by_category]
            order += sorted(c for c in by_category if c and c not in presets.CATEGORIES)
            for category in order:
                sub = QtWidgets.QMenu(category, menu)
                sub.setToolTipsVisible(True)
                menu.addMenu(sub)
                menu.submenus.append(sub)
                for info in sorted(by_category[category], key=lambda i: i["title"].lower()):
                    self._preset_action(sub, info)
            if by_category.get(""):
                if order:
                    menu.addSeparator()
                for info in sorted(by_category[""], key=lambda i: i["title"].lower()):
                    self._preset_action(menu, info)

        # (graph_ui.json's presetMenu: the presets, Save as Preset..., Copy Graph and Paste Graph.)
        widgets.fill_menu(self.preset_menu, definitions.get("graph/presetMenu"), definitions.binder(self.ACTIONS, self),
                          generators={"presets": widgets.menu_filler(fill_presets)})

    def copy_graph(self):
        """The groom's whole graph on the clipboard as text (groomlab.copy_graph_text). Returns it."""
        import groomlab
        if not self.groom:
            self.report("select a groom first")
            return None
        if not self.live and not self.apply():  # edits not applied yet: what is copied is what is shown
            return None
        try:
            text = groomlab.copy_graph_text(self.groom)
        except (RuntimeError, ValueError) as e:
            self.report("cannot copy the graph: %s" % e)
            return None
        QtWidgets.QApplication.clipboard().setText(text)
        self.report("copied %s's graph as text (%d nodes): paste it with Presets > Paste Graph on any groom, or "
                    "save it as a .json preset" % (self.groom.split("|")[-1], len(self.model.order)))
        return text

    def paste_graph(self, text=None):
        """Replace the groom's graph with the clipboard's text (or `text`): groomlab.paste_graph_text.
        What did not travel shows in the status bar; text that is no graph is refused there."""
        import groomlab
        if not self.groom or not self.same_groom():
            self.report("cannot paste a graph: select a groom first")
            return None
        if text is None:
            text = QtWidgets.QApplication.clipboard().text()
        try:
            result = groomlab.paste_graph_text(self.groom, text)
        except ValueError as e:
            self.report(str(e))
            return None
        self.set_groom(self.groom)  # the pasted graph, framed
        self.report(result["message"] + "  (Ctrl+Z undoes it)")
        return result

    def _preset_action(self, menu, info):
        """A preset's submenu: Apply (the graph replaced) beside Insert after selected node (its
        look added as one Compound); the description on the submenu's title."""
        sub = QtWidgets.QMenu(info["title"], menu)
        sub.setToolTipsVisible(True)
        menu.addMenu(sub)
        action = sub.menuAction()
        tip = info["description"] or info["name"]
        action.setToolTip(tip)
        action.setStatusTip(tip)
        action.setData(info["name"])
        # Apply and Insert (graph_ui.json's presetItem), each called with the preset's name.
        entries = [e if e == "-" else {"id": e, "value": info["name"]} for e in definitions.get("graph/presetItem")]
        widgets.fill_menu(sub, entries, definitions.binder(self.ACTIONS, self))
        return action

    def insert_preset(self, name):
        """The preset's look (every node between its sources and its Output) as one Compound named
        after the preset, after the selected node or before the Output (the core's
        insert_preset_json), fitted to this scalp, the maps it reads made: one undo step."""
        import groomlab
        import groomlab_core
        from groomlab import presets
        selected = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        after = selected[0] if len(selected) == 1 else ""
        try:
            size = presets.scalp_size(self.groom) if self.groom else None
            text, made = groomlab_core.insert_preset_json(self.model.to_json(), json.dumps(presets.read_preset(name)),
                                                          size, after, groomlab_core.preset_node_name(name))
            model = gm.GraphModel(self.catalog, json.loads(text))
        except (gm.GraphError, ValueError, IOError, OSError, RuntimeError) as e:
            self.report("preset %s: %s" % (name, e))
            return None
        model.groom_size = self.model.groom_size
        with groomlab.undo_chunk("insert preset"):  # the maps it reads and the graph: one step
            if self.groom:
                try:
                    presets.prepare(self.groom, name)
                except (RuntimeError, ValueError) as e:
                    self.report("preset %s: %s" % (name, e))
                    return None
            self.model = model
            self.model.ensure_layout()
            self.rebuild()
            if made in self.items:
                self.scene.clearSelection()
                self.items[made].setSelected(True)
            self.changed()
        self.report("inserted %s as %s after %s" % (name, made, after or "the last node"))
        return made

    def load_preset(self, name):
        from groomlab import presets
        try:
            model = gm.GraphModel(self.catalog, presets.preset_for(self.groom, name))
            backdrops = presets.backdrops(name)
            controls = presets.controls(name, self.groom)
            draw = presets.draw_group(name)
            if self.groom:
                presets.prepare(self.groom, name)  # the maps and guide groups it reads, made when missing
        except (gm.GraphError, ValueError, IOError, OSError, RuntimeError) as e:
            self.report("preset %s: %s" % (name, e))
            return
        self.model = model
        self.model.backdrops = backdrops
        self.model.ensure_layout()
        self.rebuild()
        self.view.frame_all()
        if not (controls and self.groom and self.live):
            self.changed()
        else:
            import groomlab
            with groomlab.undo_chunk("apply preset"):  # the graph and the control values it records: one step
                self.changed()
                presets.set_controls(self.groom, controls)
        if draw and self.groom and self.live:
            # A hand-placed look: the Draw tool draws into its group, every guide drawn grows one.
            from groomlab.ui import menu
            menu.draw_guides_into(self.groom, draw)
            self.report("preset %s: the Draw tool draws into %s, every guide drawn grows one" % (name, draw))

    def save_preset(self):
        import os
        from groomlab import presets
        name, ok = QtWidgets.QInputDialog.getText(self, "Save preset", "Preset name:")
        if not (ok and name):
            return
        overwrite = False
        if os.path.exists(os.path.join(presets.user_dir(), name + ".json")):
            answer = QtWidgets.QMessageBox.question(
                self, "Save preset", "A preset named %s exists. Replace it?" % name,
                QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No, QtWidgets.QMessageBox.No)
            if answer != QtWidgets.QMessageBox.Yes:
                self.report("preset %s kept" % name)
                return
            overwrite = True
        self.apply()
        try:
            presets.save_preset(self.groom, name, overwrite=overwrite)
            self.report("saved preset %s" % name)
        except (ValueError, IOError, OSError) as e:
            self.report(str(e))

    def report(self, text, trouble=None):
        """A line in the status bar (wrapped; the whole of it in its tooltip). `trouble`: whether it
        is a problem (the status color), guessed from its words when not said."""
        text = text or ""
        self.status.setText(text)
        self.status.setToolTip(text)
        if trouble is None:
            trouble = any(w in text.lower() for w in ("error", "not applied", "cannot", "can't", "missing", "no output",
                                                      "invalid", "failed", "select a groom"))
        role = "status" if trouble else "about"
        if self.status.property("role") != role:
            self.status.setProperty("role", role)
            widgets.repolish(self.status)


# ---------------------------------------------------------------------------
# Maya entry point

_window = None


def gpu_drag_flags(value):
    """The groomLabGpuPreview flags a dragged param's value goes to the GPU look preview as:
    {"value": x} for a number (a seed too), {"vector": (x, y, z)} for three numbers (a vec3 param:
    Curl's weightDirection), {"ramp": json} for a profile's keys; None for anything else (a bool,
    text, a map: the CPU previews it)."""
    def number(v):
        return isinstance(v, (int, float)) and not isinstance(v, bool)

    if number(value):
        return {"value": float(value)}
    if isinstance(value, (list, tuple)) and len(value) == 3 and all(number(c) for c in value):
        return {"vector": tuple(float(c) for c in value)}
    if isinstance(value, list) or (isinstance(value, dict) and "keys" in value):
        return {"ramp": json.dumps(value)}
    return None


def _uuid(node):
    from maya import cmds
    try:
        found = cmds.ls(node, uuid=True)
    except (RuntimeError, ValueError):
        return None
    return found[0] if found else None


def _same_node(a, b):
    """Whether two names (short or long) are the same node: False when either is missing."""
    ua = _uuid(a) if a else None
    return ua is not None and ua == (_uuid(b) if b else None)


def show(groom=None):
    """Open (or raise) the dockable graph editor for `groom`, else the current groom (a pinned
    editor stays on its groom)."""
    global _window
    from maya import cmds

    explicit = groom is not None
    if groom is None:  # the groom the tools work on, else the scene's first
        import groomlab
        groom = groomlab.active_groom() or (cmds.ls(type="groomLabShape") or [None])[0]

    if _window is not None and not widgets.alive(_window):  # its dock was deleted (workspace change, reload)
        _window = None
    docked = False
    if _window is None:
        _window = _make(groom)
        # The dock Maya kept opens where it was. A new one: on a small screen (or Maya's UI at
        # 125-150%) a floating editor this big would hide the viewport whose changes it is for: it
        # opens docked under the viewport instead (drag its tab out to float it).
        if not widgets.into_dock(_window, UI_SCRIPT):
            docked = open_docked((_window.width(), _window.height()), widgets.available_geometry(_window))
    elif groom and (explicit or not _window.pinned):
        _window.set_groom(groom)
    if docked:
        _window.show(dockable=True, floating=False, area="bottom", uiScript=UI_SCRIPT)
    else:
        _window.show(dockable=True, uiScript=UI_SCRIPT)
    widgets.keep_dock_resizable(_window)
    return _window


# What Maya runs to rebuild the editor in its dock as it restores its workspace (restore).
UI_SCRIPT = "import groomlab.ui.graph_editor; groomlab.ui.graph_editor.restore()"


def _make(groom):
    from maya.app.general.mayaMixin import MayaQWidgetDockableMixin

    class DockableGraphEditor(MayaQWidgetDockableMixin, GraphEditor):
        pass

    window = DockableGraphEditor(groom=groom)  # first positional arg is the Qt parent
    window.setObjectName("groomLabGraphEditor")
    widgets.at_exit(_close_at_exit)
    return window


def restore():
    """Maya restoring its workspace (a restart, an update's too): the editor rebuilt in its dock,
    where it was when Maya last quit, on the groom the tools work on (else the scene's first)."""
    global _window
    import groomlab
    from maya import cmds
    groomlab.ensure_plugin()
    if _window is not None and widgets.alive(_window):
        widgets.dispose(_window)
    _window = _make(groomlab.active_groom() or (cmds.ls(type="groomLabShape") or [None])[0])
    widgets.into_dock(_window, UI_SCRIPT, restoring=True)
    widgets.keep_dock_resizable(_window)


def open_docked(size, available):
    """Whether the Graph Editor opens docked (under the viewport) rather than floating: when a
    floating one of `size` would crowd the screen `available` ((x, y, w, h))."""
    return widgets.crowds_screen(size, available)


def _close_at_exit():
    """Maya quits: the editor stops following Maya and its widgets go while Qt and Python are whole
    (see widgets.at_exit). Its dock stays as Maya saves it."""
    global _window
    window, _window = _window, None
    if window is not None and widgets.alive(window):
        window.stop_following()
        scene = getattr(window, "scene", None)  # the editor's, not the view's: it goes on its own
        widgets.dispose(window)
        widgets.dispose(scene)
