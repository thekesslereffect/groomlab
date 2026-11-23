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
node's "ui", as the standalone app keeps them, so groom files carry them), each move one undo
step; no node evaluates again for a move (every one is a cache hit).
Node types come from the plugin's operator catalog: the Add Node menu's categories, their order, each
type's label and search words are the core's (node_catalog.h, in each spec), and each node's one-line
description (the panel's top, the search) is its spec's.

    import groomlab.ui.graph_editor as ge
    ge.show()            # dockable, for the selected groom
"""

import json
import math
import os
import sys

from groomlab.base import no_undo
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


def file_filter(op_type, param):
    """The file dialog filter of a param naming a file (graph_ui.json's filePathFilters: a node
    type's "path", or "Type.param"); None for any other param."""
    if "%s.%s" % (op_type, param) in FILE_PATH_FILTERS:
        return FILE_PATH_FILTERS["%s.%s" % (op_type, param)]
    return FILE_PATH_FILTERS.get(op_type) if param == "path" else None
# When a node runs (its Evaluate menu, choices.json's evaluation): (mode, label, what it does); the
# tag under a node set so.
EVALUATION_CHOICES = [(c["value"], c["label"], c["tip"]) for c in definitions.choices("evaluation")]
EVALUATION_TAGS = dict((c["value"], c["tag"]) for c in definitions.choices("evaluation") if "tag" in c)
CATEGORY_COLORS = dict((name, QtGui.QColor(color)) for name, color in definitions.get("graph/categoryColors").items())

# Params naming the host's inputs (the external data a groom feeds the graph):
# plumbing for scripts and compounds, hidden from the panel unless changed.
PLUMBING = set(tuple(pair) for pair in definitions.get("graph/plumbing"))

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
    """A param name as a label: "guideRadius" reads "Guide radius" (the name stays in the tooltip).
    Every host's (node_catalog.h paramLabel)."""
    import groomlab_core
    return groomlab_core.param_label(name)


def search_rank(text, op_type, words):
    """Where node type `op_type` ranks for the search `text` (lower first), None when it doesn't
    match: its name or label starting with it, containing it, a search keyword or its category title, a param's name, its
    description, then a param's description. `words`: NodeSearch's entry (lowercase texts)."""
    import groomlab_core
    # (the core's nodeSearchRank, every host's search)
    return groomlab_core.node_search_rank(text, dict(words, name=op_type.lower(), label=words.get("label", op_type.lower())))


ADVANCED = "Advanced"  # the section the advanced params fold away in, last (and a group of that name)


def param_sections(specs):
    """[(section, [spec, ...])] in the panel's order: the main params ("" : no header), each group in
    the order the spec lists them, then Advanced (every advanced param, whatever its group). With
    fewer than two groups there are no group headers: the params read as one list, as before (the
    core's param_sections, every host's)."""
    import groomlab_core
    of = groomlab_core.param_sections(json.dumps(list(specs)))
    order = [""] + [s for s in dict.fromkeys(of) if s not in ("", ADVANCED)] + [ADVANCED]
    sections = [(key, [s for s, section in zip(specs, of) if section == key]) for key in order]
    return [(key, members) for key, members in sections if members]


def shown_when(spec, value_of, spec_of=None):
    """Whether a param shows: its spec's showWhen holds (graph.h ParamCondition): {"param", "in"}
    for the node's values (value_of(param)): a bool as "true" or "false", a number as JSON writes
    it; and the param it names shows too (spec_of(name): that param's spec, None: unknown), so
    Clump's count hides with clumpBy. No condition: always."""
    # The core's rule, every host's (param_conditions.h paramShown): the chain of specs the
    # conditions name, and the values they read.
    import groomlab_core
    name = spec.get("name") or "_shown"
    chain, names, s = [], set(), dict(spec, name=name)
    while s is not None and s.get("name") not in names:
        chain.append(s)
        names.add(s.get("name"))
        named = (s.get("showWhen") or {}).get("param")
        s = spec_of(named) if named and spec_of else None
    values = {}
    for s in chain:
        named = (s.get("showWhen") or {}).get("param")
        if named:
            values[named] = value_of(named)
    return groomlab_core.param_shown(json.dumps(chain), name, json.dumps(values))


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

    def contextMenuEvent(self, event):
        if self.is_output:
            passed = self.name[len("thru_"):] if self.name.startswith("thru_") else ""
            if not passed:
                event.ignore()  # (the node's menu)
                return
            # A pass-through output's menu (graph_ui.json's outputPortMenu): Rename Output, Move
            # Output Up / Down (where it can go that way).
            editor, node = self.node_item.editor, self.node_item.name
            actions = {"port.renameOutput": lambda: later(lambda: editor.rename_pass_through(node, passed)),
                       "port.outputUp": lambda: later(lambda: editor.move_pass_through(node, passed, -1)),
                       "port.outputDown": lambda: later(lambda: editor.move_pass_through(node, passed, 1))}
            passes = editor.model.pass_through(node) if hasattr(editor.model, "pass_through") else []
            state = set()
            if passed in passes:
                at = passes.index(passed)
                if at > 0:
                    state.add("port.canMoveUp")
                if at + 1 < len(passes):
                    state.add("port.canMoveDown")
            menu = QtWidgets.QMenu()
            widgets.fill_menu(menu, definitions.get("graph/outputPortMenu"), definitions.binder(actions), state)
            qt.exec_(menu, event.screenPos())
            event.accept()
            return
        # An input's menu (graph_ui.json's portMenu): Pass Through as Output.
        editor, node, name = self.node_item.editor, self.node_item.name, self.name
        actions = {"port.passThrough": lambda on: later(lambda: editor.set_pass_through(node, name, on))}
        menu = QtWidgets.QMenu()
        widgets.fill_menu(menu, definitions.get("graph/portMenu"), definitions.binder(actions),
                          checked=lambda item: name in editor.model.pass_through(node))
        qt.exec_(menu, event.screenPos())
        event.accept()

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
        # Show Tubes on its header: a Braid's section tubes, a node giving guides the tubes they carry.
        self.tubes = model.braid_tubes_offered(name)
        self.can_view = model.viewable(name)  # View Up to Here (the core's rule, every host's)
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
            item.label = port.get("label") or port["name"]  # (a renamed pass-through output: its name as shown)
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
                             QtCore.Qt.AlignVCenter | QtCore.Qt.AlignRight, getattr(port, "label", name))
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
        if viewed:
            # In Focus (the viewport shows the groom up to here): a glow of the focus color round the
            # card and a solid ring, so it reads from across the graph.
            for k in (4, 3, 2, 1):
                glow = QtGui.QColor(FOCUS)
                glow.setAlpha(18 + 14 * (4 - k))
                painter.setPen(QtGui.QPen(glow, 3.0))
                painter.setBrush(QtCore.Qt.NoBrush)
                grow = 1.0 + 2.2 * k
                painter.drawRoundedRect(rect.adjusted(-grow, -grow, grow, grow), CORNER + grow, CORNER + grow)
            painter.setPen(QtGui.QPen(FOCUS, 2.5))
            painter.drawRoundedRect(rect.adjusted(-2.5, -2.5, 2.5, 2.5), CORNER + 2.5, CORNER + 2.5)
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
        """Focus shows a node giving strands as connected (GraphEdit.viewable)."""
        return self.can_view

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
            left -= size + 4
        if self.op_type == "Clump":  # Clump Colors: the strands colored by its clumps
            rects["clumpColors"] = QtCore.QRectF(left, 8, size, size)
        elif self.tubes:  # Show Tubes: its section tubes (a Braid's) or its guides' tubes drawn in the viewport
            rects["braidTubes"] = QtCore.QRectF(left, 8, size, size)
        return rects

    def paint_toggles(self, painter, viewed, bypassed):
        rects = self.toggle_rects()
        painter.save()
        painter.setOpacity(1.0)
        for kind, rect in rects.items():
            available = self.mappable() if kind == "inputs" else []
            on = (viewed if kind == "focus" else
                  self.editor.clump_colored(self.name) if kind == "clumpColors" else
                  self.editor.braid_tubes_shown(self.name) if kind == "braidTubes" else
                  (all(p in self.editor.model.param_inputs(self.name) for p in available) if kind == "inputs" else bypassed))
            ink = getattr(self, "header_ink", None)  # on a filled header: its readable color
            idle = QtGui.QColor(ink) if ink is not None else QtGui.QColor(MUTED)
            if ink is not None:
                idle.setAlpha(150)
            active = FOCUS if kind in ("focus", "inputs", "clumpColors", "braidTubes") else WARNING
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
            elif kind == "clumpColors":  # three dots of a clump: in three hues when lit
                hues = (QtGui.QColor(240, 110, 90), QtGui.QColor(90, 200, 120), QtGui.QColor(100, 150, 245))
                for k, at in enumerate((QtCore.QPointF(0, -3.4), QtCore.QPointF(-3.6, 2.6), QtCore.QPointF(3.6, 2.6))):
                    if on:
                        painter.setPen(QtCore.Qt.NoPen)
                        painter.setBrush(hues[k])
                    else:
                        painter.setPen(QtGui.QPen(color, 1.3))
                        painter.setBrush(QtCore.Qt.NoBrush)
                    painter.drawEllipse(c + at, 2.6, 2.6)
            elif kind == "braidTubes":  # a short tube: its far end's half, its two sides, its open near end
                far = QtCore.QRectF(c.x() - 6.6, c.y() - 4.0, 4.4, 8.0)
                near = QtCore.QRectF(c.x() + 2.2, c.y() - 4.0, 4.4, 8.0)
                painter.setPen(QtGui.QPen(color, 1.4))
                painter.drawArc(far, 90 * 16, 180 * 16)
                painter.drawLine(QtCore.QPointF(far.center().x(), far.top()), QtCore.QPointF(near.center().x(), near.top()))
                painter.drawLine(QtCore.QPointF(far.center().x(), far.bottom()),
                                 QtCore.QPointF(near.center().x(), near.bottom()))
                if on:
                    painter.setBrush(color)
                painter.drawEllipse(near)
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
                    elif kind == "clumpColors":
                        later(lambda: editor.toggle_clump_colors(name))
                    elif kind == "braidTubes":
                        later(lambda: editor.toggle_braid_tubes(name))
                    else:
                        later(lambda: editor.set_bypass([name], not editor.model.bypassed(name)))
                    return
        # A drag: Shift carries everything upstream, Ctrl everything downstream (not through
        # portals), a shake pulls the nodes out of their links; Shift+click without a move adds the
        # node to the selection (Ctrl+click toggles it: Qt's own). Several nodes moved together take
        # the pins and dots between them.
        shift = bool(event.modifiers() & QtCore.Qt.ShiftModifier)
        ctrl = not shift and bool(event.modifiers() & QtCore.Qt.ControlModifier)
        self._press = (self.pos(), shift)
        self._before = [i for i in self.scene().selectedItems() if isinstance(i, NodeItem)] if self.scene() else []
        self._carry = []
        self._shaken = False
        if event.button() == QtCore.Qt.LeftButton:
            moving = [self.name]
            if self.isSelected():
                moving = [i.name for i in self._before if i is not self] + [self.name]
            if shift or ctrl:
                moving = self.editor.upstream_of(moving) if shift else self.editor.downstream_of(moving)
                self._carry = [(self.editor.items[n], self.editor.items[n].pos()) for n in moving
                               if n != self.name and n in self.editor.items and self.editor.items[n].isVisible()]
            if len(moving) > 1:
                self._carry += [(item, item.pos()) for item in self.editor.carried_layout(moving)]
        import groomlab_core
        self._shake = groomlab_core.ShakeDetector()
        self._clock = QtCore.QElapsedTimer()
        self._clock.start()
        super(NodeItem, self).mousePressEvent(event)

    def mouseMoveEvent(self, event):
        super(NodeItem, self).mouseMoveEvent(event)
        editor = self.editor
        moving = editor.selected_names() or [self.name]
        if getattr(self, "_carry", None):
            moving = moving + [item.name for item, _start in self._carry if isinstance(item, NodeItem)]
        if not getattr(self, "_growing", False):
            self._growing = True
            editor.begin_grow(moving)
        editor.grow_live(moving, event.screenPos())
        shake = getattr(self, "_shake", None)
        if shake is not None and not self._shaken and shake.add(self._clock.elapsed() / 1000.0, event.screenPos().x(), event.screenPos().y()):
            # Shaken: out of their links at once, the drag going on (Houdini's); several keep theirs.
            self._shaken = True
            names = [n for n in moving if n in editor.items]
            at = editor.view.mapToScene(editor.view.mapFromGlobal(QtGui.QCursor.pos()))
            later(lambda: editor.shake_loose(names, at))

    def mouseReleaseEvent(self, event):
        press = getattr(self, "_press", None)
        super(NodeItem, self).mouseReleaseEvent(event)
        moved = press is not None and self.pos() != press[0]
        if press is not None and press[1] and not moved:
            # Shift+click: added to the selection (the others stay selected).
            for item in self._before:
                item.setSelected(True)
            self.setSelected(True)
        if getattr(self, "_growing", False):
            self.editor.end_grow()
        self._press, self._carry, self._shake, self._shaken, self._growing = None, [], None, False, False

    def mouseDoubleClickEvent(self, event):
        editor, name = self.editor, self.name
        if self.op_type == "Compound" and event.pos().y() > HEADER_H:
            later(lambda: editor.open_compound(name))  # its body: into it (U, the path bar: back out)
        elif self.op_type == "Script":
            later(lambda: editor.show_script_editor(name))
        else:
            later(lambda: editor.rename_node(name))  # its header: renamed
        event.accept()

    def hoverMoveEvent(self, event):
        tips = {"focus": "Focus: show the groom up to this node in the viewport (%s); a Clump's centers show as "
                         "dots" % shortcut_keys("graph.view"),
                "inputs": "Expand Inputs: show or hide all connectable parameter ports",
                "clumpColors": "Clump Colors: color the strands by this node's clumps (its clumpChannel); again: as they were",
                "braidTubes": ("Show Tubes: draws the braid's section tubes in the viewport" if self.op_type == "Braid" else
                               "Show Tubes: draws the tubes of the guides this node gives in the viewport"),
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
            press = getattr(self, "_press", None)
            if press is not None and getattr(self, "_carry", None):
                delta = self.pos() - press[0]
                for item, start in self._carry:
                    if not item.isSelected():
                        item.setPos(start + delta)
        return super(NodeItem, self).itemChange(change, value)

    # What the node's right-click menu runs (engine/ui/graph_ui.json's nodeMenu): functions of the
    # item. Every action rebuilds the scene (this item included): each runs after the menu's event.
    ACTIONS = {
        "node.setOutput": lambda self: later(lambda: self.editor.set_output(self.name)),
        "node.view": lambda self, on: later(lambda: self.editor.view_node(self.name if on else None)),
        "node.colorByChannel": lambda self, channel, on: later(lambda: self.editor.color_by_channel(self.name, channel, on)),
        "node.bypass": lambda self, on: later(lambda: self.editor.set_bypass([self.name], on)),
        # Out of the flow: what fed it feeds what it fed, the node kept below the new link (#80).
        "node.detach": lambda self: later(lambda: self.editor.detach_nodes(self.editor.menu_targets(self.name))),
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
        "node.openCompound": lambda self: later(lambda: self.editor.open_compound(self.name)),
        "align.left": lambda self: later(lambda: self.editor.align_selected("left")),
        "align.centerX": lambda self: later(lambda: self.editor.align_selected("centerX")),
        "align.right": lambda self: later(lambda: self.editor.align_selected("right")),
        "align.top": lambda self: later(lambda: self.editor.align_selected("top")),
        "align.centerY": lambda self: later(lambda: self.editor.align_selected("centerY")),
        "align.bottom": lambda self: later(lambda: self.editor.align_selected("bottom")),
        "align.row": lambda self: later(lambda: self.editor.align_selected("row")),
        "align.column": lambda self: later(lambda: self.editor.align_selected("column")),
        "align.distributeX": lambda self: later(lambda: self.editor.align_selected("distributeX")),
        "align.distributeY": lambda self: later(lambda: self.editor.align_selected("distributeY")),
        "align.tidy": lambda self: later(self.editor.tidy_selection),
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
        if self.editor.model.bakeable(self.name):  # (the core's rule, every host's)
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
            import groomlab_core
            definition = (self.editor.model.nodes[self.name].get("params") or {}).get("definition") or {}
            # The core's list, every host's (graph_controls.h compoundInnerParamsJson).
            inner_params = json.loads(groomlab_core.compound_inner_params_json(json.dumps(definition)))
            return [{"submenu": n["node"], "items": [
                {"id": entry["action"], "value": "%s.%s" % (n["node"], p["name"]), "label": pretty(p["name"]),
                 "tip": p.get("description", "")} for p in n["params"]]} for n in inner_params]
        def channels(entry):
            # The channels its last result carries (the stats'), per strand and per point.
            return [{"id": entry["action"], "value": channel, "label": channel, "tip": "per " + kind}
                    for channel, kind in self.editor.channels_of(self.name)]
        return {"exposeParams": expose, "innerParams": inner, "nodeChannels": channels}

    def contextMenuEvent(self, event):
        editor, name = self.editor, self.name
        current = editor.model.evaluation(name)
        exposed = editor.model.param_inputs(name)
        definition = (editor.model.nodes[name].get("params") or {}).get("definition") or {}
        shown = set((definition.get("expose") or {}).values())
        colored = editor.channel_colored(name)
        checked = {"node.view": lambda item: editor.viewed == name,
                   "node.colorByChannel": lambda item: item["value"] == colored,
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
        self.portal_item = None  # the portal exit it comes out of (the editor's rebuild sets it)
        src_item.edges.append(self)
        dst_item.edges.append(self)
        self.update_path()

    @staticmethod
    def _end(item, port, out):
        """Where it meets a node: its port, or the edge of the folded backdrop hiding the node."""
        folded = getattr(item, "folded_in", None)
        if folded is not None:
            r = folded.sceneBoundingRect().adjusted(2, 2, -2, -2)
            return QtCore.QPointF(r.right() if out else r.left(), r.center().y())
        return port.scene_center()

    def points(self):
        """Where it runs: its source port (or its portal exit), its pins, its input port; a dot or pin
        on a folded backdrop is left out (the backdrop's edge stands for a dot)."""
        end = self._end(self.dst_item, self.dst_item.inputs[self.input_name], False)
        if self.portal_item is not None:
            folded = getattr(self.portal_item, "folded_in", None)
            if folded is not None:
                r = folded.sceneBoundingRect().adjusted(2, 2, -2, -2)
                return [QtCore.QPointF(r.right(), r.center().y()), end]
            return [self.portal_item.nub_pos(), end]
        pins = [pin.scenePos() for pin in self.pins if getattr(pin, "folded_in", None) is None]
        return [self._end(self.src_item, self.src_port, True)] + pins + [end]

    def update_path(self):
        points = self.points()
        path = QtGui.QPainterPath(points[0])
        if len(points) == 2 and self.portal_item is None:
            # Straight from port to port: one running backwards loops under both cards.
            below = max(self.src_item.scenePos().y() + self.src_item.height, self.dst_item.scenePos().y() + self.dst_item.height)
            route(path, points[0], points[1], False, below)
        else:
            for a, b in zip(points, points[1:]):
                curve(path, a, b)
        self.setPath(path)

    # Its right-click menu (graph_ui.json's linkMenu).
    ACTIONS = {
        "link.teleport": lambda self: later(lambda link=self.link(): self.dst_item.editor.teleport_link(link)),
        "link.pin": "_pin_here",
        "link.delete": lambda self: later(lambda: self.dst_item.editor.unlink(self.dst_item.name, self.input_name)),
    }

    def _pin_here(self):
        editor, link, at = self.dst_item.editor, self.link(), self._menu_at
        index = self.piece_at(at)
        later(lambda: editor.add_pin(link, at, index))

    def contextMenuEvent(self, event):
        self._menu_at = event.scenePos()
        menu = QtWidgets.QMenu()
        widgets.fill_menu(menu, definitions.get("graph/linkMenu"), definitions.binder(self.ACTIONS, self))
        qt.exec_(menu, event.screenPos())
        event.accept()

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
        later(lambda: editor.add_pin(link, at, index))
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
    """A link's piece from a to b onto `path` (at a): leaving a and reaching b level, as a link
    leaves an output and reaches an input (and a pin, as Unreal's reroute nodes), in the wire style."""
    return route(path, a, b, True)


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
            later(lambda: editor.merge_pins(keep, drop))  # it rebuilds the scene: after this event


# How links are drawn (graph.wireStyle, remembered in the optionVar WIRE_STYLE_VAR): "smooth",
# "square" or "rounded" (the core's wire_route, the app's too).
WIRE_STYLE = ["smooth"]
WIRE_STYLE_VAR = "groomLabGraphWireStyle"


def route(path, a, b, through=True, below=None):
    """A link's piece from a to b onto `path` (at a), in the wire style (the core's wire_route,
    every host's): `through` between pins or portal dots (no loop under the cards); `below`, a
    backwards link's lowest card edge to loop under."""
    import groomlab_core
    for s in groomlab_core.wire_route(WIRE_STYLE[0], a.x(), a.y(), b.x(), b.y(), below, through):
        if len(s) == 2:
            path.lineTo(s[0], s[1])
        else:
            path.cubicTo(QtCore.QPointF(s[0], s[1]), QtCore.QPointF(s[2], s[3]), QtCore.QPointF(s[4], s[5]))
    return path


class PortalPort(object):
    """A portal exit as a link drag's start (dragged from its nub): its source's output, drawn from
    the dot (PinPort's twin)."""

    is_output = True
    is_param = False
    highlight = False

    def __init__(self, portal_item):
        self.portal_item = portal_item
        self.node_item = portal_item.src_item
        port = portal_item.src_port
        self.name, self.port_type, self.value_kind = port.name, port.port_type, port.value_kind

    def scene_center(self):
        return self.portal_item.nub_pos()

    def update(self):
        pass


class PortalStub(QtWidgets.QGraphicsPathItem):
    """The short wire from a portal's source output to its entry dot (the long one is never drawn)."""

    def __init__(self, portal_item):
        super(PortalStub, self).__init__()
        self.portal_item = portal_item
        self.setZValue(-1)
        pen = QtGui.QPen(portal_item.color, 2.2)
        pen.setCapStyle(QtCore.Qt.RoundCap)
        self.setPen(pen)
        self.update_path()

    def update_path(self):
        a = self.portal_item.src_port.scene_center()
        b = self.portal_item.scenePos() - QtCore.QPointF(PortalItem.RADIUS, 0)
        self.setPath(route(QtGui.QPainterPath(a), a, b))


class PortalItem(QtWidgets.QGraphicsItem):
    """A portal's dot (graph_portals.h): its entry by the source output (exit None), or one of its
    exits, where teleported links come out and run on into their inputs. Moved like a node; named
    beside it; an exit's nub drags another link of the signal out; double-click renames."""

    RADIUS = 7.0
    NUB = 13.0
    NUB_REACH = 10.0  # the nub's pick radius: easy to grab

    def __init__(self, editor, portal, exit_index, pos, src_item, src_port):
        super(PortalItem, self).__init__()
        self.editor, self.portal, self.exit = editor, portal, exit_index
        self.src_item, self.src_port = src_item, src_port
        # A free dot (nothing feeds it yet): grey.
        self.color = MUTED if src_port is None else TYPE_COLORS.get(src_port.port_type, QtGui.QColor(200, 200, 200))
        self.edges = []
        self.setZValue(-0.5)
        self.setAcceptHoverEvents(True)
        self.setFlags(QtWidgets.QGraphicsItem.ItemIsMovable | QtWidgets.QGraphicsItem.ItemIsSelectable |
                      QtWidgets.QGraphicsItem.ItemSendsGeometryChanges)
        self.setToolTip("Portal %s: %s. Drag to place it, double-click to rename%s" % (
            portal, "its entry (the links teleport from here)" if exit_index is None else "an exit",
            "" if exit_index is None else "; drag from its nub for another link of it"))
        self.setPos(*pos)

    def label_rect(self):
        width = QtGui.QFontMetricsF(theme.font(11, bold=True)).horizontalAdvance(self.portal) + 4
        return QtCore.QRectF(-width / 2.0, -self.RADIUS - 21, width, 18)  # its name over it

    def boundingRect(self):
        r = self.RADIUS + 3.0
        return QtCore.QRectF(-r, -r - 4, 2 * r + self.NUB + self.NUB_REACH, 2 * r + 8).united(self.label_rect())

    def shape(self):
        path = QtGui.QPainterPath()
        r = self.RADIUS + 3.0
        path.addEllipse(QtCore.QRectF(-r, -r, 2 * r, 2 * r))
        if self.src_item is not None:
            path.addEllipse(QtCore.QPointF(self.NUB, 0), self.NUB_REACH, self.NUB_REACH)
        return path

    def nub_at(self, scene_pos):
        local = self.mapFromScene(scene_pos)
        return self.src_item is not None and local.x() > self.RADIUS * 0.4 and abs(local.y()) < self.NUB_REACH + 1.0

    def nub_pos(self):
        return self.scenePos() + QtCore.QPointF(self.NUB, 0)

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        painter.setPen(QtGui.QPen(SELECTED if self.isSelected() else self.color, 2.5))
        painter.setBrush(self.color.darker(180) if self.exit is None else self.color)
        painter.drawEllipse(QtCore.QPointF(0, 0), self.RADIUS, self.RADIUS)
        if self.src_item is not None:  # its nub: links drag out of it (an entry's too: a dot organizing the graph)
            painter.setPen(QtCore.Qt.NoPen)
            painter.setBrush(self.color)
            painter.drawPolygon(QtGui.QPolygonF([QtCore.QPointF(self.NUB - 4.5, -6.0), QtCore.QPointF(self.NUB + 4.5, 0.0),
                                                 QtCore.QPointF(self.NUB - 4.5, 6.0)]))
        painter.setFont(theme.font(11, bold=True, base=painter.font()))
        painter.setPen(TEXT)
        painter.drawText(self.label_rect(), QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft, self.portal)

    def itemChange(self, change, value):
        if change == QtWidgets.QGraphicsItem.ItemPositionChange and self.editor.snapping(self):
            return snapped(value)
        if change == QtWidgets.QGraphicsItem.ItemPositionHasChanged:
            for edge in self.edges:
                edge.update_path()
            self.editor.portal_moved(self.portal, self.exit, self.pos())
            if self.isSelected():
                self.editor.update_dot_preview()
        return super(PortalItem, self).itemChange(change, value)

    def mouseDoubleClickEvent(self, event):
        editor, portal = self.editor, self.portal
        later(lambda: editor.rename_portal(portal))
        event.accept()

    ACTIONS = {
        "portal.rename": lambda self: later(lambda: self.editor.rename_portal(self.portal)),
        "portal.removeExit": lambda self: later(lambda: self.editor.remove_portal(self.portal, self.exit)),
        "portal.delete": lambda self: later(lambda: self.editor.remove_portal(self.portal)),
    }

    def contextMenuEvent(self, event):
        menu = QtWidgets.QMenu()
        widgets.fill_menu(menu, definitions.get("graph/portalMenu"), definitions.binder(self.ACTIONS, self),
                          set() if self.exit is None else {"portal.exit"})
        qt.exec_(menu, event.screenPos())


# Tab search's entries for the groom's painted maps: MAP_ENTRY + the map's name, in this section.
MAP_ENTRY = "map:"
PORTAL_ENTRY = "portal:"  # Tab search's entries for the graph's portals (an exit placed)
DOT_ENTRY, BACKDROP_ENTRY = "dot:", "backdrop:"  # Tab search's Dot and Backdrop
PAINTED_MAPS = "Painted maps"


class NodeSearch(QtWidgets.QFrame):
    """Type to find a node type (name, category or description); Enter or click adds it."""

    def __init__(self, editor, at, on_link=None, source=None):
        super(NodeSearch, self).__init__(editor, QtCore.Qt.Popup)
        self.editor, self.at, self.on_link = editor, at, on_link
        self.source = source  # (node, output) of a link dropped on the canvas: what the node added reads
        self.dot_at = at
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
        import groomlab_core
        catalog = editor.catalog
        refresh_library(catalog)
        for category, types in categories(catalog):
            for op_type in types:
                spec = catalog.spec(op_type) if catalog is not None else {}
                self.labels[op_type] = (spec.get("label") or op_type, spec.get("description", ""))
                # What it is found by: the core's (node_search_words, every host's search).
                self.entries.append((op_type, category, groomlab_core.node_search_words(
                    json.dumps(dict(spec, type=op_type, categoryTitle=category)))))
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
        # A Dot (route links through it, name it, or set its Input Portal) and a Backdrop (Shift+O).
        for key, label, about in ((DOT_ENTRY, "Dot", "A dot: route links through it, name it, or set its Input Portal "
                                   "to teleport another dot's signal"),
                                  (BACKDROP_ENTRY, "Backdrop", "A frame round the selected nodes (Shift+O)")):
            self.labels[key] = (label, about)
            self.entries.append((key, "Layout", {"label": label.lower(), "params": "", "about": "", "details": "",
                                                 "keywords": "dot pin portal reroute teleport backdrop frame network box"}))
        # The graph's portals: an exit of one placed here (then linked from its nub).
        for p in editor.model.portals if editor.model is not None else []:
            key = PORTAL_ENTRY + p["name"]
            self.labels[key] = ("Portal: %s" % p["name"], "An exit of the portal %s here: drag from its nub to an input" % p["name"])
            self.entries.append((key, "Portals", {"label": ("portal %s" % p["name"]).lower(), "keywords": "portal teleport link exit",
                                                  "params": "", "about": "", "details": ""}))
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
        if op_type == DOT_ENTRY:
            self.editor.add_dot(self.dot_at, self.source)
            return
        if op_type == BACKDROP_ENTRY:
            self.editor.add_backdrop()
            return
        if op_type.startswith(PORTAL_ENTRY):  # an exit of a portal, where Tab was pressed
            self.editor.add_portal_exit(op_type[len(PORTAL_ENTRY):], self.at)
            return
        if op_type.startswith(MAP_ENTRY):  # a painted map: a Map node reading it, named after it
            params = {"name": op_type[len(MAP_ENTRY):]}
            op_type, name = "Map", self.editor.model.unique_name(params["name"])
        # Where Tab was pressed: on a wire, inserted into it; else wired after the selected node
        # as Add Node wires it.
        if self.source:
            # From a link dropped on the canvas: the node there, fed from that output.
            made = self.editor.add_node(op_type, at=self.at, wire=False, params=params, name=name)
            if made:
                self.editor.link_first_input(self.source, made)
            return
        self.editor.add_node(op_type, at=self.at, wire=True, on_link=self.on_link, params=params, name=name)


class BackdropItem(QtWidgets.QGraphicsItem):
    """A labelled frame behind nodes: moving it moves the nodes inside; drag the corner to resize."""

    GRIP = 14

    def __init__(self, editor, data):
        super(BackdropItem, self).__init__()
        self.editor, self.data = editor, data
        self._carried = []  # before setPos: its itemChange reads them
        self._resizing = None
        # Folded: over the links (its title always shows), under the pins and nodes.
        self.setZValue(-0.9 if self.folded() else -20)
        self.setFlags(QtWidgets.QGraphicsItem.ItemIsMovable | QtWidgets.QGraphicsItem.ItemIsSelectable |
                      QtWidgets.QGraphicsItem.ItemSendsGeometryChanges)
        self.setPos(data["x"], data["y"])

    def folded(self):
        return bool(self.data.get("collapsed"))

    def header(self):
        """Its title bar's height: taller for a bigger title ("fontSize", points)."""
        size = self.data.get("fontSize") or 0
        return max(28.0, size * 1.9) if size else 28.0

    def full_rect(self):
        """Its whole frame in the scene (what holds its nodes, folded or not)."""
        return QtCore.QRectF(self.data["x"], self.data["y"], self.data["w"], self.data["h"])

    def rect(self):
        if self.folded():  # its title bar and a strip (the core's foldedBackdrop), as wide as its title
            import groomlab_core
            title = QtGui.QFontMetricsF(theme.font(self.data.get("fontSize") or 14, bold=True)).horizontalAdvance(
                self.data.get("title", "Backdrop")) + 56.0
            _x, _y, w, h = groomlab_core.folded_backdrop((0.0, 0.0, self.data["w"], self.data["h"]), self.header(),
                                                         max(160.0, title), max(260.0, title))
            return QtCore.QRectF(0, 0, w, h)
        return QtCore.QRectF(0, 0, self.data["w"], self.data["h"])

    def fold_rect(self):
        """The fold chevron at its title bar's right."""
        return QtCore.QRectF(self.rect().right() - 26, (self.header() - 18) / 2.0, 18, 18)

    def boundingRect(self):
        return self.rect().adjusted(-2, -2, 2, 2)

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        color = QtGui.QColor(*self.data.get("color", definitions.get("graph/backdropColors")[0]["rgb"]))
        fill = QtGui.QColor(color)
        fill.setAlpha(150 if self.folded() else 45)
        painter.setPen(QtGui.QPen(SELECTED if self.isSelected() else color, 1.5))
        painter.setBrush(fill)
        painter.drawRoundedRect(self.rect(), 10, 10)
        header = QtGui.QColor(color)
        header.setAlpha(120)
        clip = QtGui.QPainterPath()
        clip.addRoundedRect(self.rect(), 10, 10)
        painter.save()
        painter.setClipPath(clip)
        painter.fillRect(QtCore.QRectF(0, 0, self.rect().width(), self.header()), header)
        painter.restore()
        painter.setFont(theme.font(self.data.get("fontSize") or 14, bold=True, base=painter.font()))
        painter.setPen(TEXT)
        painter.drawText(QtCore.QRectF(12, 0, self.rect().width() - 44, self.header()), QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft,
                         self.data.get("title", "Backdrop"))
        # The fold chevron: down while open, right while folded.
        pen = QtGui.QPen(TEXT, 1.8)
        pen.setCapStyle(QtCore.Qt.RoundCap)
        painter.setPen(pen)
        painter.setBrush(QtCore.Qt.NoBrush)
        c = self.fold_rect().center()
        if self.folded():
            points = [c + QtCore.QPointF(-2.5, -5), c + QtCore.QPointF(2.5, 0), c + QtCore.QPointF(-2.5, 5)]
        else:
            points = [c + QtCore.QPointF(-5, -2.5), c + QtCore.QPointF(0, 2.5), c + QtCore.QPointF(5, -2.5)]
        painter.drawPolyline(QtGui.QPolygonF(points))
        if self.folded():
            held = sum(1 for i in self.editor.items.values() if getattr(i, "folded_in", None) is self)
            painter.setFont(theme.font(11, base=painter.font()))
            painter.setPen(MUTED)
            painter.drawText(QtCore.QRectF(12, 24, self.rect().width() - 24, self.rect().height() - 24),
                             QtCore.Qt.AlignVCenter | QtCore.Qt.AlignLeft, "%d node%s inside" % (held, "" if held == 1 else "s"))
            return
        if self.data.get("description"):
            # Its description on the body, under the title bar.
            painter.setFont(theme.font(12, base=painter.font()))
            painter.setPen(MUTED)
            top = self.header() + 6
            painter.drawText(QtCore.QRectF(12, top, self.rect().width() - 24, max(0.0, self.rect().height() - top - 8)),
                             QtCore.Qt.AlignTop | QtCore.Qt.AlignLeft | QtCore.Qt.TextWordWrap, self.data["description"])
        painter.setPen(QtGui.QPen(color.lighter(130), 1.5))
        r = self.rect()
        for k in (4, 8, 12):
            painter.drawLine(QtCore.QPointF(r.right() - k, r.bottom() - 2), QtCore.QPointF(r.right() - 2, r.bottom() - k))

    def _in_grip(self, pos):
        r = self.rect()
        return not self.folded() and pos.x() > r.right() - self.GRIP and pos.y() > r.bottom() - self.GRIP

    def mousePressEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton and self.fold_rect().contains(event.pos()):
            event.accept()
            editor, data = self.editor, self.data
            later(lambda: editor.fold_backdrop(data, not data.get("collapsed")))
            return
        if event.button() == QtCore.Qt.LeftButton and self._in_grip(event.pos()):
            self._resizing = (event.scenePos(), self.data["w"], self.data["h"])
            event.accept()
            return
        if event.pos().y() > self.header():
            event.ignore()  # its body: a rubber band selects the nodes on it (the view's drag)
            return
        # Nodes inside move with it (a folded one's hidden nodes too), and its pins and dots.
        area = self.full_rect()
        self._carried = [(i, i.pos() - self.pos()) for i in self.editor.items.values()
                         if area.contains(i.sceneBoundingRect().center())]
        self._carried += [(i, i.pos() - self.pos()) for i in self.editor.layout_on_backdrop(area)]
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
        if event is not None and event.pos().y() > self.header():
            event.ignore()  # (its body: the nodes on it, the canvas)
            return
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
        "backdrop.fold": lambda self, on: later(lambda: self.editor.fold_backdrop(self.data, on)),
        "backdrop.delete": lambda self: later(lambda editor=self.editor, data=self.data: editor.remove_backdrop(data)),
    }

    def contextMenuEvent(self, event):
        menu = QtWidgets.QMenu()
        widgets.fill_menu(menu, definitions.get("graph/backdropMenu"), definitions.binder(self.ACTIONS, self),
                          checked=lambda item: item["id"] == "backdrop.fold" and self.folded())
        qt.exec_(menu, event.screenPos())

    def _recolor(self, rgb):
        self.data["color"] = rgb
        self.editor._layout_dirty = True
        self.editor.commit_layout()
        self.update()


class ItemProps(QtWidgets.QFrame):
    """The selected backdrop's or dot's properties, over the param panel: a backdrop's title,
    description and title size; a dot's name and Input Portal (the dot whose signal it takes)."""

    def __init__(self, editor):
        super(ItemProps, self).__init__()
        self.editor, self.target = editor, None
        self.setObjectName("graphItemProps")
        form = QtWidgets.QFormLayout(self)
        form.setContentsMargins(8, 8, 8, 8)
        self.heading = QtWidgets.QLabel()
        self.heading.setStyleSheet("font-weight: bold;")
        form.addRow(self.heading)
        self.name = QtWidgets.QLineEdit()
        form.addRow("Name", self.name)
        self.description = QtWidgets.QPlainTextEdit()
        self.description.setFixedHeight(theme.px(64))
        form.addRow("Description", self.description)
        # Its color: the palette's swatches (graph_ui.json backdropColors, as the menu's Color), then any.
        self.colors = QtWidgets.QWidget()
        swatches = QtWidgets.QHBoxLayout(self.colors)
        swatches.setContentsMargins(2, 3, 2, 3)  # (a selected swatch's thicker border shows whole)
        swatches.setSpacing(theme.px(6))
        self.colors.setMinimumHeight(theme.px(22) + 8)
        self.swatches = []
        for entry in definitions.get("graph/backdropColors"):
            button = QtWidgets.QPushButton()
            button.setFixedSize(theme.px(22), theme.px(22))
            button.setToolTip(entry["label"])
            button.rgb = list(entry["rgb"])
            button.clicked.connect(lambda _=False, rgb=list(entry["rgb"]): self._color_done(rgb))
            swatches.addWidget(button)
            self.swatches.append(button)
        self.custom_color = QtWidgets.QPushButton("...")
        self.custom_color.setFixedSize(theme.px(28), theme.px(22))
        self.custom_color.setToolTip("Any color")
        self.custom_color.clicked.connect(self._pick_color)
        swatches.addWidget(self.custom_color)
        swatches.addStretch(1)
        form.addRow("Color", self.colors)
        form.setVerticalSpacing(theme.px(8))  # (room between the swatches and the title size)
        # Its title size: a slider (shown live while dragged; one undo step when let go).
        self.font_row = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(self.font_row)
        row.setContentsMargins(0, 0, 0, 0)
        self.font_size = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        self.font_size.setObjectName("graphBackdropTitleSize")
        self.font_size.setRange(8, 72)
        self.font_value = QtWidgets.QLabel()
        self.font_value.setMinimumWidth(theme.px(40))
        row.addWidget(self.font_size, 1)
        row.addWidget(self.font_value)
        form.addRow("Title size", self.font_row)
        self.input = QtWidgets.QComboBox()
        form.addRow("Input Portal", self.input)
        self.rows = form
        self._pause = QtCore.QTimer(self)
        self._pause.setSingleShot(True)
        self._pause.setInterval(600)
        self._pause.timeout.connect(self._description_done)
        self.name.editingFinished.connect(self._name_done)
        self.description.textChanged.connect(self._pause.start)
        self.font_size.valueChanged.connect(self._size_changed)
        self.font_size.sliderReleased.connect(lambda: self._size_done(self.font_size.value()))
        self.input.activated.connect(self._input_done)
        self.hide()

    def _show_rows(self, backdrop):
        for widget in (self.description, self.font_row, self.colors):
            widget.setVisible(backdrop)
            self.rows.labelForField(widget).setVisible(backdrop)
        self.input.setVisible(not backdrop)
        self.rows.labelForField(self.input).setVisible(not backdrop)

    def show_item(self, item):
        """A BackdropItem, a PortalItem, or None (hidden)."""
        self.target = item
        if item is None:
            self.hide()
            return
        for widget in (self.name, self.description, self.font_size, self.input):
            widget.blockSignals(True)
        try:
            if isinstance(item, BackdropItem):
                self._show_rows(True)
                self.heading.setText("Backdrop")
                if not self.name.hasFocus():  # (the field being typed in keeps what is typed)
                    self.name.setText(item.data.get("title", "Backdrop"))
                if not self.description.hasFocus():
                    self.description.setPlainText(item.data.get("description", ""))
                if not self.font_size.isSliderDown():
                    self.font_size.setValue(int(item.data.get("fontSize") or 14))  # (none: the default, 14 pt)
                self.font_value.setText("%d pt" % self.font_size.value())
                color = list(item.data.get("color") or definitions.get("graph/backdropColors")[0]["rgb"])
                for button in self.swatches:
                    on = list(button.rgb) == color
                    button.setStyleSheet("QPushButton { background: rgb(%d, %d, %d); border: %dpx solid %s; border-radius: 4px; }"
                                         % (tuple(button.rgb) + (2 if on else 1, SELECTED.name() if on else MUTED.name())))
            else:
                self._show_rows(False)
                self.heading.setText("Dot" if item.exit is None else "Dot (an exit of %s)" % item.portal)
                if not self.name.hasFocus():
                    self.name.setText(item.portal)
                self.input.clear()
                self.input.addItem("Not connected to a dot", "")
                for p in self.editor.model.portals:
                    if p.get("from") and not (p["name"] == item.portal and item.exit is None):
                        self.input.addItem(p["name"], p["name"])
                self.input.setCurrentIndex(max(0, self.input.findData(item.portal)) if item.exit is not None else 0)
        finally:
            for widget in (self.name, self.description, self.font_size, self.input):
                widget.blockSignals(False)
        self.show()

    def _name_done(self):
        text = self.name.text().strip()
        item = self.target
        if not text or item is None:
            return
        if isinstance(item, BackdropItem):
            if text != item.data.get("title"):
                later(lambda: self.editor.set_backdrop_field(item.data, "title", text))
        elif text != item.portal:
            editor, portal, exit_index = self.editor, item.portal, item.exit

            def rename():
                try:
                    editor.model.rename_portal(portal, text)
                except gm.GraphError as e:
                    editor.report(str(e))
                    return
                editor._layout_dirty = True
                editor.commit_layout("rename dot")
                editor.rebuild()
                editor.reselect(dot=(text, exit_index))
            later(rename)

    def _description_done(self):
        item = self.target
        if isinstance(item, BackdropItem) and self.description.toPlainText() != item.data.get("description", ""):
            text = self.description.toPlainText()
            later(lambda: self.editor.set_backdrop_field(item.data, "description", text))

    def _size_changed(self, value):
        self.font_value.setText("%d pt" % value)
        item = self.target
        if not isinstance(item, BackdropItem):
            return
        if self.font_size.isSliderDown():  # shown live, set when let go
            item.prepareGeometryChange()
            item.data["fontSize"] = value
            item.update()
            self._live = True
            return
        self._size_done(value)

    def _size_done(self, value):
        item = self.target
        live, self._live = getattr(self, "_live", False), False
        if isinstance(item, BackdropItem) and (live or value != int(item.data.get("fontSize") or 0)):
            later(lambda: self.editor.set_backdrop_field(item.data, "fontSize", value))

    def _color_done(self, rgb):
        item = self.target
        if isinstance(item, BackdropItem) and list(rgb) != list(item.data.get("color") or []):
            later(lambda: self.editor.set_backdrop_field(item.data, "color", list(rgb)))

    def _pick_color(self):
        item = self.target
        if not isinstance(item, BackdropItem):
            return
        start = QtGui.QColor(*(item.data.get("color") or definitions.get("graph/backdropColors")[0]["rgb"]))
        color = QtWidgets.QColorDialog.getColor(start, self, "Backdrop Color")
        if color.isValid():
            self._color_done([color.red(), color.green(), color.blue()])

    def _input_done(self, row):
        item = self.target
        if isinstance(item, PortalItem):
            into = self.input.itemData(row) or None
            editor, portal, exit_index = self.editor, item.portal, item.exit
            later(lambda: editor.set_dot_input(portal, exit_index, into))


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
        self._carry = None         # nodes dragged on after their items were made again (a shake, Alt+drag copies)

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
        portal = next((i for i in self.items(event.scenePos()) if isinstance(i, PortalItem)), None)
        if portal is not None and portal.nub_at(event.scenePos()) and event.button() == QtCore.Qt.LeftButton:
            event.accept()  # a link out of the portal exit's nub, of the signal it carries
            self._detach = None
            self._begin(PortalPort(portal), event.scenePos())
            return
        pin = self.pin_at(event.scenePos())
        if pin is not None and pin.edges and pin.nub_at(event.scenePos()) and event.button() == QtCore.Qt.LeftButton:
            event.accept()  # a link out of the pin's nub (Blueprints'), of the signal it carries
            self._detach = None
            self._begin(PinPort(pin), event.scenePos())
            return
        port = self.port_at(event.scenePos())
        if (port is not None and port.is_output and event.button() == QtCore.Qt.LeftButton
                and event.modifiers() & QtCore.Qt.ShiftModifier and self.editor.model.output_links(port.node_item.name, port.name)):
            # Shift+drag from an output: all its links picked up, to drop on another output.
            event.accept()
            self._bundle = port
            self._drag_line = self.addPath(QtGui.QPainterPath(), QtGui.QPen(TYPE_COLORS.get(port.port_type, TEXT), 2.2, QtCore.Qt.DashLine))
            self._drag_line.setZValue(10)
            return
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

    def carry(self, names, scene_pos):
        """Go on dragging these nodes until the button is released: their items were made again
        under the drag (a shake pulled them loose; Alt+drag made them)."""
        self._carry = (list(names), scene_pos)

    def _carry_to(self, pos):
        names, last = self._carry
        delta = pos - last
        for name in names:
            item = self.editor.items.get(name)
            if item is not None:
                item.setPos(item.pos() + delta)
        self._carry = (names, pos)
        self.editor.grow_live(names)

    def mouseMoveEvent(self, event):
        if self._carry is not None:
            self._carry_to(event.scenePos())
            event.accept()
            return
        if self._cut_line is not None:
            self._move_cut(event.scenePos())
            event.accept()
            return
        if getattr(self, "_bundle", None) is not None:
            a = event.scenePos()
            path = QtGui.QPainterPath()
            for _node, input_name in self.editor.model.output_links(self._bundle.node_item.name, self._bundle.name):
                item = self.editor.items.get(_node)
                if item is not None and input_name in item.inputs:
                    b = item.inputs[input_name].scene_center()
                    path.moveTo(a)
                    route(path, a, b)
            self._drag_line.setPath(path)
            event.accept()
            return
        if self._drag_port is not None:
            self._update_line(event.scenePos())
            self.hover_reason(event.scenePos(), event.screenPos())
            event.accept()
            return
        super(GraphScene, self).mouseMoveEvent(event)
        self._hover_drop_wire(event.scenePos())

    def portal_entry_at(self, pos):
        """A dot's entry (or a free dot) at a scene point, or None."""
        return next((i for i in self.items(pos) if isinstance(i, PortalItem) and i.exit is None), None)

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

    def dragged_group(self):
        """Several selected nodes dragged with no links to the rest of the graph (a chain dropped on a
        wire goes in as one): (the grabbed node, the names), or None."""
        grabbed = self.mouseGrabberItem()
        selected = [i for i in self.selectedItems() if isinstance(i, NodeItem)]
        if not isinstance(grabbed, NodeItem) or grabbed not in selected or len(selected) < 2:
            return None
        for item in selected:
            for edge in item.edges:
                if isinstance(edge, EdgeItem) and (edge.src_item not in selected or edge.dst_item not in selected):
                    return None
        return grabbed, [i.name for i in selected]

    def _hover_drop_wire(self, pos):
        """While an unwired node (or chain) is dragged: the wire it lies over lights up (dropped: inserted)."""
        node = self.dragged_node()
        wire = self.edge_under_node(node) if node is not None else None
        group = self.dragged_group() if node is None else None
        if group is not None:
            grabbed, names = group
            wire = self.edge_under_node(grabbed)
            if wire is not None and (wire.src_item.name in names or wire.dst_item.name in names):
                wire = None
        if wire is not self._drop_wire:
            for edge in (self._drop_wire, wire):
                if edge is not None:
                    edge.hovered = edge is wire
                    edge.update()
            self._drop_wire = wire

    def mouseReleaseEvent(self, event):
        if self._carry is not None:
            self._carry_to(event.scenePos())
            self._carry = None
            event.accept()
            self.editor.commit_layout()
            self.editor.end_grow()
            return
        if getattr(self, "_bundle", None) is not None:
            start, self._bundle = self._bundle, None
            try:
                self.removeItem(self._drag_line)
            except RuntimeError:
                pass
            self._drag_line = None
            end = self.port_at(event.scenePos())
            event.accept()
            if end is not None and end.is_output and end is not start:
                editor = self.editor
                later(lambda: editor.move_output_links(start.node_item.name, start.name, end.node_item.name, end.name))
            elif end is not start:
                self.editor.report("Drop the links on another node's output (the ports on its right)")
            return
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
                if isinstance(out, PortalPort):
                    self.editor.link_from_portal(out.portal_item.portal, out.portal_item.exit, inp.node_item.name, inp.name)
                elif isinstance(out, PinPort):
                    self.editor.link_from_pin(out.pin_item.pin, inp.node_item.name, inp.name)
                else:
                    self.editor.link(out.node_item.name, inp.node_item.name, inp.name, detach=detach, output=out.name)
            elif self.pin_at(event.scenePos()) is not None and not isinstance(start, (PinPort, PortalPort)):
                # Dropped on a pin: an output feeds the pin's links from there; an input (or a link's
                # end picked up off one) takes the pin's signal.
                pin = self.pin_at(event.scenePos()).pin
                if detach:
                    self.editor.link_from_pin(pin, *detach)
                elif start.is_output:
                    self.editor.feed_pin(pin, start.node_item.name, start.name)
                else:
                    self.editor.link_from_pin(pin, start.node_item.name, start.name)
            elif (start.is_output and not detach and not isinstance(start, (PinPort, PortalPort))
                  and self.portal_entry_at(event.scenePos()) is not None):
                # An output's link dropped on a dot's entry: the dot (and its links) fed from there.
                self.editor.feed_dot(self.portal_entry_at(event.scenePos()).portal, start.node_item.name, start.name)
            elif (start.is_output and not detach and not isinstance(start, (PinPort, PortalPort))
                  and not any(isinstance(i, (NodeItem, PortItem)) for i in self.items(event.scenePos()))):
                # Dropped on the canvas: the add-node search there, what it adds fed from that output.
                editor, at, screen = self.editor, event.scenePos(), event.screenPos()
                source = (start.node_item.name, start.name)
                later(lambda: editor.show_search(at, screen, source=source))
            elif detach:
                self.editor.unlink(*detach)  # a picked-up link dropped on nothing: removed
            elif not start.is_output:
                self.editor.unlink(start.node_item.name, start.name)
            return
        node, wire, group = self.dragged_node(), self._drop_wire, self.dragged_group()
        self._drop_wire = None
        if wire is not None:
            wire.hovered = False
            wire.update()
        super(GraphScene, self).mouseReleaseEvent(event)
        self.editor.commit_layout()
        if node is not None and wire is not None:
            editor, name, link = self.editor, node.name, wire.link()
            later(lambda: editor.insert_on_wire(name, link))  # it rebuilds the scene: after this event
        elif group is not None and wire is not None:
            editor, names, link = self.editor, group[1], wire.link()
            later(lambda: editor.insert_group_on_wire(names, link))

    def keyPressEvent(self, event):
        ctrl = bool(event.modifiers() & QtCore.Qt.ControlModifier)
        key = event.key()
        selected = [i.name for i in self.selectedItems() if isinstance(i, NodeItem)]
        edges = [i for i in self.selectedItems() if isinstance(i, EdgeItem)]
        if key == QtCore.Qt.Key_Period and not event.modifiers() and edges:
            # . : a pin in the middle of the selected link.
            edge = edges[0]
            at = edge.path().pointAtPercent(0.5)
            editor, link, index = self.editor, edge.link(), edge.piece_at(at)
            later(lambda: editor.add_pin(link, at, index))
        elif key == QtCore.Qt.Key_O and event.modifiers() == QtCore.Qt.ShiftModifier:
            later(self.editor.add_backdrop)  # Shift+O: a backdrop round the selection (Houdini's key)
        elif key == QtCore.Qt.Key_L and not event.modifiers():
            later(self.editor.tidy_layout)  # the selected nodes, or all of them
        elif key == QtCore.Qt.Key_U and not event.modifiers() and self.editor.scope:
            self.editor.leave_compound()  # up out of the compound (Houdini's U)
        elif key == QtCore.Qt.Key_I and not event.modifiers() and len(selected) == 1 and self.editor.model.node_type(selected[0]) == "Compound":
            self.editor.open_compound(selected[0])  # into it (Houdini's I)
        elif undo_key(event):  # reached when the editor's shortcut did not take the key first
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
        elif ctrl and key == QtCore.Qt.Key_X and event.modifiers() & QtCore.Qt.ShiftModifier:
            self.editor.detach_selected()
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
        if any(isinstance(i, (NodeItem, PortItem, BackdropItem, PortalItem, EdgeItem)) for i in self.items(event.scenePos())):
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
        later(lambda: editor.import_nodes(files, at=(at.x(), at.y())))

    def wheelEvent(self, event):
        self._drop_stale_drag(QtWidgets.QApplication.mouseButtons())
        # The wheel's turn: Qt reports it on x with Alt held (Windows turns Alt+wheel horizontal),
        # so either axis zooms, in and out alike; a turn of nothing (a trackpad's end) does nothing.
        delta = event.angleDelta().y() or event.angleDelta().x()
        if not delta:
            event.ignore()
            return
        factor = 1.15 if delta > 0 else 1 / 1.15
        zoom = self.transform().m11() * factor
        if self.ZOOM_MIN * theme.scale() <= zoom <= self.ZOOM_MAX * theme.scale():
            self.scale(factor, factor)
        self.auto_frame = False
        event.accept()

    def _drop_stale_drag(self, buttons):
        """A pan or an Alt+right zoom whose button is up (its release went to Maya's Alt navigation,
        or outside the window): ended, its cursor given back, so it never sticks."""
        if self._pan is not None and not buttons & (QtCore.Qt.MiddleButton | QtCore.Qt.LeftButton):
            self._pan = None
            self.unsetCursor()
        if self._zoom is not None and not buttons & QtCore.Qt.RightButton:
            self._zoom = None
            self._snapshot = None
            self.unsetCursor()
            self.viewport().update()

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
        if event.key() == QtCore.Qt.Key_A and not qt.to_int(event.modifiers()) & ~qt.to_int(QtCore.Qt.ShiftModifier | QtCore.Qt.AltModifier):
            # Held, a drag from a node aligns (Houdini's gestures); let go without one: Frame All.
            if not event.isAutoRepeat():
                self._a_held, self._a_gestured = True, False
            return
        if event.key() == QtCore.Qt.Key_Tab:
            cursor = self.mapFromGlobal(QtGui.QCursor.pos())
            self.scene().editor.show_search(self.mapToScene(cursor), QtGui.QCursor.pos())
            return
        super(GraphView, self).keyPressEvent(event)

    def keyReleaseEvent(self, event):
        if event.key() == QtCore.Qt.Key_A and not event.isAutoRepeat():
            frame = getattr(self, "_a_held", False) and not getattr(self, "_a_gestured", False)
            self._a_held = False
            if frame:
                self.frame_all()
            return
        super(GraphView, self).keyReleaseEvent(event)

    def focusOutEvent(self, event):
        self._a_held = False  # (A let go elsewhere)
        super(GraphView, self).focusOutEvent(event)

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
        if event.button() == QtCore.Qt.LeftButton and getattr(self, "_a_held", False):
            # A held: a drag from a node lines nodes up with it (A alone its chain, Shift+A the
            # selection, Alt+A the selection spread evenly).
            at = self.mapToScene(qt.event_pos(event))
            node = next((i for i in self.scene().items(at) if isinstance(i, NodeItem)), None)
            if node is not None:
                mods = event.modifiers()
                mode = "distribute" if mods & QtCore.Qt.AltModifier else "selected" if mods & QtCore.Qt.ShiftModifier else "chain"
                self._a_gestured = True
                self.scene().editor.begin_align_gesture(node.name, mode, at)
                self._gesture = True
                event.accept()
                return
        if (event.button() == QtCore.Qt.LeftButton and event.modifiers() & QtCore.Qt.AltModifier
                and not event.modifiers() & QtCore.Qt.ShiftModifier):  # (Alt+Shift: the scene cuts links)
            pos = qt.event_pos(event)
            at = self.mapToScene(pos)
            under = self.scene().items(at)
            node = next((i for i in under if isinstance(i, NodeItem)), None)
            event.accept()
            if node is not None and not any(isinstance(i, PortItem) for i in under):
                # Alt+drag on a node: a copy of it (of the selection), dragged on.
                self.scene().editor.duplicate_drag(node.name, at)
                return
            # Alt+click (no drag) on a link puts a pin there: decided on the release.
            self._alt_click = pos
            return
        if event.button() == QtCore.Qt.MiddleButton:  # pan: the middle button
            self._pan = qt.event_pos(event)
            self._alt_click = None
            self.setCursor(QtCore.Qt.ClosedHandCursor)
            return
        super(GraphView, self).mousePressEvent(event)

    def mouseMoveEvent(self, event):
        self._drop_stale_drag(event.buttons())
        if getattr(self, "_gesture", False):
            self.scene().editor.align_gesture_to(self.mapToScene(qt.event_pos(event)), self.transform().m11())
            return
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
        if getattr(self, "_gesture", False) and event.button() == QtCore.Qt.LeftButton:
            self._gesture = False
            self.scene().editor.end_align_gesture()
            event.accept()
            return
        click = getattr(self, "_alt_click", None)
        if click is not None and event.button() == QtCore.Qt.LeftButton:
            # Alt+click on a link (no drag): a pin there.
            self._alt_click = None
            pos = qt.event_pos(event)
            if (pos - click).manhattanLength() <= 4:
                at = self.mapToScene(pos)
                edge = self.scene().edge_at(at)
                if edge is not None and not any(isinstance(i, (NodeItem, PinItem)) for i in self.scene().items(at)):
                    editor, link, index = self.scene().editor, edge.link(), edge.piece_at(at)
                    later(lambda: editor.add_pin(link, at, index))
            event.accept()
            return
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
            click, self._alt_click = getattr(self, "_alt_click", None), None
            pos = qt.event_pos(event)
            if click is not None and (pos - click).manhattanLength() <= 4:
                at = self.mapToScene(pos)
                edge = self.scene().edge_at(at)
                if edge is not None and not any(isinstance(i, (NodeItem, PinItem)) for i in self.scene().items(at)):
                    editor, link, index = self.scene().editor, edge.link(), edge.piece_at(at)
                    later(lambda: editor.add_pin(link, at, index))
            return
        super(GraphView, self).mouseReleaseEvent(event)


# ---------------------------------------------------------------------------
# Parameter panel


class GraphEditor(QtWidgets.QWidget):
    """The editor widget; `show()` below wraps it in a dockable Maya panel."""

    # What the Edit, Add Node and Presets menus run (engine/ui/graph_ui.json's editMenu, addMenu,
    # presetMenu, presetItem): method names, or functions of the editor.
    ACTIONS = {
        "graph.undo": "undo",
        "graph.redo": "redo",
        "graph.frame": lambda self: self.view.frame(),
        "graph.frameAll": lambda self: self.view.frame_all(),
        "graph.tidy": "tidy_layout",
        "graph.up": "leave_compound",
        "align.left": lambda self: self.align_selected("left"),
        "align.centerX": lambda self: self.align_selected("centerX"),
        "align.right": lambda self: self.align_selected("right"),
        "align.top": lambda self: self.align_selected("top"),
        "align.centerY": lambda self: self.align_selected("centerY"),
        "align.bottom": lambda self: self.align_selected("bottom"),
        "align.row": lambda self: self.align_selected("row"),
        "align.column": lambda self: self.align_selected("column"),
        "align.distributeX": lambda self: self.align_selected("distributeX"),
        "align.distributeY": lambda self: self.align_selected("distributeY"),
        "align.tidy": "tidy_selection",
        "graph.wireStyle": lambda self, style, _on=True: self.set_wire_style(style),
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
        self.scope = []            # the compounds opened, outermost first (the model is their graph)
        self.node_errors = {}      # node -> error text from the last evaluation
        self.node_warnings = {}    # node -> a warning (a Cards node whose maps want a rebake)
        self.node_notes = {}       # node -> a note: it does nothing as set (a Clump with no centers)
        self.node_times = {}       # node -> ms of the last evaluation
        self.node_channels = {}    # node -> strand channels in its last result
        self.node_color_channels = {}  # node -> [[channel, kind]] Color by Channel lists (the stats')

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
        # The wire style (its menu, its icon the style's) and the Align buttons, as the app's bar.
        toolbar.addSeparator()
        self.wire_menu = QtWidgets.QMenu(self)
        for choice in definitions.choices("wireStyles"):
            action = self.wire_menu.addAction(widgets.glyph("wire_" + choice["value"]), choice["label"])
            action.setToolTip(choice.get("tip", ""))
            action.triggered.connect(lambda _=False, v=choice["value"]: self.set_wire_style(v))
        self.wire_button = widgets.glyph_button("wire_" + WIRE_STYLE[0], "Wire style: Smooth, Square or Rounded", menu=self.wire_menu)
        toolbar.addWidget(self.wire_button)
        toolbar.addSeparator()
        self.align_buttons = {}
        for mode, tip in (("left", "Align left edges"), ("centerX", "Align centers (a vertical line)"),
                          ("right", "Align right edges"), ("distributeY", "Space evenly down"), (None, None),
                          ("top", "Align top edges"), ("centerY", "Align middles (a horizontal line)"),
                          ("bottom", "Align bottom edges"), (None, None), ("distributeX", "Space evenly across")):
            if mode is None:
                toolbar.addSeparator()
                continue
            button = widgets.glyph_button("align_" + mode, tip, slot=lambda m=mode: self.align_selected(m))
            self.align_buttons[mode] = button
            toolbar.addWidget(button)
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
        # The wire style, remembered between sessions; its Edit menu items checked as it is.
        try:
            from maya import cmds
            if cmds.optionVar(exists=WIRE_STYLE_VAR):
                WIRE_STYLE[0] = cmds.optionVar(query=WIRE_STYLE_VAR) or "smooth"
                self.wire_button.glyph = "wire_" + WIRE_STYLE[0]
                self.wire_button.setIcon(widgets.glyph(self.wire_button.glyph))
        except (ImportError, RuntimeError):
            pass
        self.edit_menu.aboutToShow.connect(self.sync_wire_style_menu)
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
        # Detach (Ctrl+Shift+X, issue #80): only while the graph view has the focus, and taken there
        # so no Maya hotkey runs on it.
        shortcut = qt.QShortcut(QtGui.QKeySequence("Ctrl+Shift+X"), self.view)
        shortcut.setContext(QtCore.Qt.WidgetWithChildrenShortcut)
        shortcut.activated.connect(self.detach_selected)
        shortcut.activatedAmbiguously.connect(self.detach_selected)
        self._shortcuts.append(shortcut)
        # S with the mouse over a param keys it, as over a channel in Maya's Channel Box; elsewhere in
        # the editor it is Maya's Set Key. Text fields keep S for typing (they take it first).
        shortcut = qt.QShortcut(QtGui.QKeySequence("S"), self)
        shortcut.setContext(QtCore.Qt.WidgetWithChildrenShortcut)
        shortcut.activated.connect(self.key_hovered)
        shortcut.activatedAmbiguously.connect(self.key_hovered)
        self._shortcuts.append(shortcut)

        splitter = QtWidgets.QSplitter()
        # The view under a path bar shown inside a compound ("Graph > compound"): each step goes back up.
        graph_side = QtWidgets.QWidget()
        side = QtWidgets.QVBoxLayout(graph_side)
        side.setContentsMargins(0, 0, 0, 0)
        side.setSpacing(0)
        self.scope_bar = QtWidgets.QWidget()
        self.scope_bar.setObjectName("graphScopeBar")
        QtWidgets.QHBoxLayout(self.scope_bar).setContentsMargins(6, 3, 6, 3)
        self.scope_bar.hide()
        side.addWidget(self.scope_bar)
        side.addWidget(self.view, 1)
        splitter.addWidget(graph_side)
        # The selected backdrop's or dot's card over the param panel.
        self.item_props = ItemProps(self)
        param_side = QtWidgets.QWidget()
        param_column = QtWidgets.QVBoxLayout(param_side)
        param_column.setContentsMargins(0, 0, 0, 0)
        param_column.setSpacing(0)
        param_column.addWidget(self.item_props)
        param_column.addWidget(self.params, 1)
        splitter.addWidget(param_side)
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
        if self.previewing() or text == self.model.top().to_json() or text == layout:
            return  # our own edit, our own layout write (edits not applied yet stay), or a live slider drag
        self.model = load_model(self.groom, self.catalog)
        self._restore_scope()  # back inside the compounds that were open
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
        self.node_color_channels = {}
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
        with no_undo():
            return fn()

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
        if text == self.model.top().to_json():
            return False
        self.scene.cancel_drag()
        self.model = load_model(self.groom, self.catalog)
        self._restore_scope()
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
        self.scope = []  # another groom: its whole graph
        self.model = load_model(groom, self.catalog)
        self._watch_graph()
        self.groom_box.setCurrentText(groom)
        self.rebuild()
        self.refresh_status()
        self.view.auto_frame = True
        view = self.view  # once the view has its size, if it is still there (an editor closed at once)
        QtCore.QTimer.singleShot(0, lambda: widgets.alive(view) and view.frame_readable())
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
            self._backdrop_items = []
            for backdrop in self.model.backdrops:
                item = BackdropItem(self, backdrop)
                self.scene.addItem(item)
                self._backdrop_items.append(item)
            for name in self.model.order:
                item = NodeItem(self, name)
                self.scene.addItem(item)
                self.items[name] = item
            self._linked_inputs = {(c["to"], c["input"]) for c in self.model.connections}
            self._linked_outputs = set()
            self.model.prune_pins()
            self.pin_items = {}
            self._edges = {}
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
                    self._edges[(c["to"], c["input"])] = edge
            # The portals: each entry by its source output (a short wire to it), its exits where
            # the teleported links come out.
            self.portal_items = []
            for p in self.model.portals:
                if not p.get("from"):  # a free dot: nothing feeds it yet
                    free = PortalItem(self, p["name"], None, (p["x"], p["y"]), None, None)
                    self.scene.addItem(free)
                    self.portal_items.append(free)
                    continue
                src = self.items.get(p["from"])
                if src is None:
                    continue
                port = src.outputs.get(p.get("output") or "", src.output) if p.get("output") else src.output
                entry = PortalItem(self, p["name"], None, (p["x"], p["y"]), src, port)
                self.scene.addItem(entry)
                stub = PortalStub(entry)
                self.scene.addItem(stub)
                entry.edges.append(stub)
                src.edges.append(stub)
                self.portal_items.append(entry)
                for k, e in enumerate(p.get("exits") or []):
                    exit_item = PortalItem(self, p["name"], k, (e["x"], e["y"]), src, port)
                    self.scene.addItem(exit_item)
                    self.portal_items.append(exit_item)
                    for link in e.get("links") or []:
                        edge = self._edges.get((link["to"], link["input"]))
                        if edge is not None:
                            edge.portal_item = exit_item
                            exit_item.edges.append(edge)
                            edge.update_path()
                for link in p.get("links") or []:  # links drawn straight out of the entry dot
                    edge = self._edges.get((link["to"], link["input"]))
                    if edge is not None:
                        edge.portal_item = entry
                        entry.edges.append(edge)
                        edge.update_path()
            # Folded backdrops hide their nodes, pins and dots (their links meet the backdrop's
            # edges instead).
            for backdrop in self._backdrop_items:
                if not backdrop.folded():
                    continue
                area = backdrop.full_rect()
                for item in self.items.values():
                    if area.contains(QtCore.QRectF(item.pos(), QtCore.QSizeF(NODE_W, item.height)).center()):
                        item.folded_in = backdrop
                        item.setVisible(False)
                for item in list(self.pin_items.values()) + list(self.portal_items):
                    if area.contains(item.pos()):
                        item.folded_in = backdrop
                        item.setVisible(False)
                        for stub in item.edges:
                            if isinstance(stub, PortalStub):
                                stub.setVisible(False)
            for edge in self._edges.values():
                start = edge.portal_item if edge.portal_item is not None else edge.src_item
                hidden_in = getattr(start, "folded_in", None)
                if hidden_in is not None and hidden_in is getattr(edge.dst_item, "folded_in", None):
                    edge.setVisible(False)
                elif (hidden_in is not None or getattr(edge.dst_item, "folded_in", None) is not None
                      or any(getattr(p, "folded_in", None) is not None for p in edge.pins)):
                    edge.update_path()
            for name in selected:
                if name in self.items:
                    self.items[name].setSelected(True)
        finally:
            self.scene.blockSignals(False)
        self.update_attribute_notes()
        self.selection_changed()
        self._fill_scope_bar()

    def is_linked(self, node, port, is_output):
        """Whether a port has a link (from the sets the last rebuild made: the scene is rebuilt on
        every link change, so no scan of the connections per repaint)."""
        if is_output:
            return (node, port) in self._linked_outputs
        return (node, port) in self._linked_inputs

    # -- Search, view, bypass, copy and paste, backdrops ------------------------

    def show_search(self, scene_pos, screen_pos, source=None):
        """Type to find a node type; Enter adds it where you clicked (Tab or right-click the canvas)."""
        if not self.model:
            text = "Select a groom first: its graph opens here, then Tab adds a node"
            self.report(text)
            QtWidgets.QToolTip.showText(screen_pos, text, self.view)
            return None
        self.scene.cancel_drag()  # the popup takes the mouse: a link drag would never end
        wire = self.scene.edge_at(scene_pos)
        if wire is None:  # a link selected first: the node goes into it
            wire = next((i for i in self.scene.selectedItems() if isinstance(i, EdgeItem)), None)
        popup = NodeSearch(self, (scene_pos.x() - NODE_W / 2, scene_pos.y() - HEADER_H / 2),
                           on_link=wire.link() if wire is not None and source is None else None, source=source)
        popup.dot_at = (scene_pos.x(), scene_pos.y())
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

    # -- Clump Colors: a Clump header's button (the groom's strandColorMode clump, from its channel) --

    def clump_channel(self, name):
        """A Clump's clumpChannel ("" none), None for another node."""
        try:
            if self.model.node_type(name) != "Clump":
                return None
            return self.model.param(name, "clumpChannel") or ""
        except Exception:  # noqa: BLE001 - a node gone
            return None

    def clump_colored(self, name):
        """Whether the strands show this Clump's clumps: colored by clump from its channel as this
        node wrote it ("<channel>@<node>"; a bare channel, from scenes before, lights the last Clump
        writing it: the core's GraphEdit.clump_colored)."""
        if not self.groom:
            return False
        from maya import cmds
        from groomlab import review
        try:
            if cmds.getAttr(self.groom + ".strandColorMode") != review.CLUMP_MODE:
                return False
            source = cmds.getAttr(self.groom + ".strandColorSource") or ""
        except (RuntimeError, ValueError):
            return False
        try:
            return self.model.clump_colored(name, source)
        except Exception:  # noqa: BLE001 - a node gone
            return False

    def channels_of(self, name):
        """[(channel, "strand" or "point")] its last result carries (the stats' colorChannels, the
        core's list: none before an evaluation reached it)."""
        return [tuple(c) for c in self.node_color_channels.get(name, [])]

    def channel_colored(self, name):
        """The channel Color by Channel shows of this node ("" for none): the Attribute colors read
        from it ("<channel>@<node>")."""
        if not self.groom:
            return ""
        import groomlab_core
        from maya import cmds
        from groomlab import review
        try:
            if cmds.getAttr(self.groom + ".strandColorMode") != review.ATTRIBUTE_MODE:
                return ""
            source = cmds.getAttr(self.groom + ".strandColorSource") or ""
        except (RuntimeError, ValueError):
            return ""
        return groomlab_core.channel_colored_at(source, name)

    def color_by_channel(self, name, channel, on):
        """Color by Channel: the hair colored by `channel` as node `name` leaves it, point by point
        (the Attribute colors from "<channel>@<node>", min and max equal: the values' own range);
        off, the color mode from before."""
        if not self.groom:
            return
        import groomlab_core
        from maya import cmds
        from groomlab import review
        groom = self.groom
        if not on:
            before = getattr(self, "_color_mode_before_channel", 0)
            self.silently(lambda: cmds.setAttr(groom + ".strandColorMode", 0 if before == review.ATTRIBUTE_MODE else before))
            return
        mode = cmds.getAttr(groom + ".strandColorMode")
        if mode != review.ATTRIBUTE_MODE:
            self._color_mode_before_channel = mode

        def colored():
            cmds.setAttr(groom + ".strandColorSource", groomlab_core.node_color_source(channel, name), type="string")
            cmds.setAttr(groom + ".strandColorMin", 0.0)
            cmds.setAttr(groom + ".strandColorMax", 0.0)
            cmds.setAttr(groom + ".strandColorMode", review.ATTRIBUTE_MODE)
        self.silently(colored)
        self.report("strands colored by %s at %s, blue to red over its range" % (channel, name))

    def toggle_clump_colors(self, name):
        """A Clump's Clump Colors: the strands colored by its clumps (its clumpChannel), the focus left
        as it is; on a lit one, the color mode from before."""
        if not self.groom:
            return
        from maya import cmds
        from groomlab import review
        if self.clump_colored(name):
            before = getattr(self, "_color_mode_before_clump", 0)
            self.silently(lambda: cmds.setAttr(self.groom + ".strandColorMode", 0 if before == review.CLUMP_MODE else before))
            self._repaint_toggles()
            return
        channel = self.clump_channel(name)
        if not channel:
            self.report("%s writes no clump channel (its clumpChannel is empty)" % name)
            return
        mode = cmds.getAttr(self.groom + ".strandColorMode")
        if mode != review.CLUMP_MODE:
            self._color_mode_before_clump = mode
        groom = self.groom

        def colored():
            cmds.setAttr(groom + ".strandColorSource", self.model.clump_color_source(name), type="string")
            cmds.setAttr(groom + ".strandColorMode", review.CLUMP_MODE)
        self.silently(colored)
        self._repaint_toggles()
        self.report("strands colored by %s's clumps (%s)" % (name, channel))

    def _repaint_toggles(self):
        for item in self.items.values():
            item.update()

    # -- Show Tubes: a Braid header's button (the groom's braidTubes: the nodes whose tubes draw) --

    def braid_tubes(self):
        """The Braid nodes showing their section tubes, as the groom keeps them (names, one a line)."""
        if not self.groom:
            return ""
        from maya import cmds
        try:
            return cmds.getAttr(self.groom + ".braidTubes") or ""
        except (RuntimeError, ValueError):
            return ""

    def _set_braid_tubes(self, shown):
        if not self.groom or shown == self.braid_tubes():
            return
        from maya import cmds
        groom = self.groom
        self.silently(lambda: cmds.setAttr(groom + ".braidTubes", shown, type="string"))
        self._repaint_toggles()

    def braid_tubes_shown(self, name):
        """Whether the viewport draws this Braid's section tubes (its Show Tubes lit: the core's
        GraphEdit.braid_tubes_shown)."""
        shown = self.braid_tubes()
        if not shown:
            return False
        try:
            return self.model.braid_tubes_shown(name, shown)
        except Exception:  # noqa: BLE001 - a node gone
            return False

    def toggle_braid_tubes(self, name):
        """A Braid's Show Tubes: its section tubes drawn in the viewport (the hair as it is); on a lit
        one, no more. Display only: nothing evaluates again."""
        if not self.groom:
            return
        try:
            shown = self.model.braid_tubes_toggled(name, self.braid_tubes())
        except Exception as e:  # noqa: BLE001 - a node gone
            self.report(str(e))
            return
        self._set_braid_tubes(shown)
        self.report("%s's tubes %s" % (name, "shown" if self.braid_tubes_shown(name) else "hidden"))

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
        text = self.model.copy_nodes(names) if names else ""
        if text:
            count = len(json.loads(text)["nodes"])
            QtWidgets.QApplication.clipboard().setText(text)
            self.report("copied %d node%s" % (count, "s" if count > 1 else ""))
        elif names:
            self.report("the output node is one of a kind")

    def paste(self, offset=None):
        """Ctrl+V: the copied nodes where the cursor is (their top-left there) when it is over the
        graph, else beside where they were copied from; links only among them."""
        text = QtWidgets.QApplication.clipboard().text()
        if offset is None:
            offset = (40.0, 40.0)
            cursor = self.view.mapFromGlobal(QtGui.QCursor.pos())
            try:
                data = json.loads(text)
            except ValueError:
                data = None
            if self.view.underMouse() and self.view.viewport().rect().contains(cursor) and isinstance(data, dict):
                corners = [(n["ui"]["x"], n["ui"]["y"]) for n in data.get("nodes") or []
                           if isinstance(n, dict) and isinstance(n.get("ui"), dict) and "x" in n["ui"] and "y" in n["ui"]]
                if corners:
                    at = self.view.mapToScene(cursor)
                    if self.snapping():
                        at = snapped(at)
                    offset = (at.x() - min(x for x, _ in corners), at.y() - min(y for _, y in corners))
        try:
            names = self.model.paste_nodes(text, offset)
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
        """Ctrl+D: copies beside the nodes, each reading what its node reads (the core's
        duplicate_nodes, the app's too); one undo step."""
        if not names:
            return []
        try:
            made = self.model.duplicate_nodes(names, (40.0, 60.0))
        except (ValueError, gm.GraphError) as e:
            self.report(str(e))
            return []
        self.rebuild()
        self.scene.clearSelection()
        for name in made:
            self.items[name].setSelected(True)
        self.changed()
        return made

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
            self.node_color_channels = stats.get("colorChannels", {})
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
        items = [i for i in self.scene.selectedItems() if isinstance(i, (BackdropItem, PortalItem))]
        if getattr(self, "item_props", None) is not None:
            self.item_props.show_item(items[0] if len(items) == 1 and not nodes else None)
        self.update_dot_preview()
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

    def sculpt_braid_sections(self, name):
        """Sculpt Sections on Braid node `name` (GraphModel.insert_braid_sculpt): its sections become a
        Sculpt node's guides that the brushes shape, its hair following them; the graph is applied
        and the brushes pick that Sculpt node. Returns the Sculpt's name, or None."""
        try:
            made = self.model.insert_braid_sculpt(name)
        except gm.GraphError as e:
            self.report(str(e))
            return None
        if made["made"]:
            self.rebuild()
            self.changed()
        if self.groom and not self.live and not self.apply():
            return None
        self.target_sculpt(made["sculpt"])
        self.report("brush %s's sections: %s keeps the strokes, %s carries the hair along them"
                    % (name, made["sculpt"], made["follow"]))
        return made["sculpt"]

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
            self._moved_names = getattr(self, "_moved_names", set()) | {name}

    def commit_layout(self, label="graph layout"):
        """The layout written to the groom as one undo step named `label` (nothing evaluates for it
        or for its undo); the backdrops holding moved nodes grown round them first."""
        if getattr(self, "_layout_dirty", False) and self.groom:
            self._layout_dirty = False
            moved, self._moved_names = getattr(self, "_moved_names", set()), set()
            grew = bool(moved) and self.grow_backdrops(moved)
            self._save_layout(label)
            if grew:
                later(self.rebuild)  # (not mid-event: the scene's items are rebuilt)

    def _save_layout(self, label=None):
        """Write the layout: one undo step named `label` (None: outside undo, a layout the editor
        made itself). The text written is remembered for the one sync its attribute change sets
        off (none when the graph's text did not change: nothing to skip)."""
        from maya import cmds
        import groomlab
        before = cmds.getAttr(self.groom + ".graph")
        if label is None:
            text = self.silently(lambda: self.model.save_layout(self.groom))
        else:
            with groomlab.undo_chunk(label):
                text = self.model.save_layout(self.groom)
        self._layout_text = text if text != before else None

    def layout_nodes(self):
        self.model.auto_layout()
        self.rebuild()
        self.view.frame_all()
        if self.groom:
            self._save_layout("tidy layout")

    # -- Arranging by hand, portals, pass-through, compounds (the core's: graph_arrange.h,
    # graph_portals.h, graph_edit.h; the app's too) ----------------------------------------------

    def selected_names(self):
        return [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]

    def upstream_of(self, names):
        """The nodes a Shift+drag of `names` carries: those and everything feeding them (not
        through a portal)."""
        import groomlab_core
        try:
            return list(groomlab_core.nodes_with_upstream(self.model._query(), list(names)))
        except (RuntimeError, ValueError):
            return list(names)

    def downstream_of(self, names):
        """The nodes a Ctrl+drag of `names` carries: those and everything they feed (not through a
        portal)."""
        import groomlab_core
        try:
            return list(groomlab_core.nodes_with_downstream(self.model._query(), list(names)))
        except (RuntimeError, ValueError):
            return list(names)

    def _layout_items(self, pins, dots):
        """The PinItems and PortalItems of pin ids and (portal, exit) dots (exit -1: the entry)."""
        items = [self.pin_items[p] for p in pins if p in getattr(self, "pin_items", {})]
        wanted = {(portal, None if exit_index < 0 else exit_index) for portal, exit_index in dots}
        items += [i for i in getattr(self, "portal_items", []) if (i.portal, i.exit) in wanted]
        return items

    def carried_layout(self, names):
        """The pins and dots that go with nodes moved together (the core's carried_with)."""
        import groomlab_core
        try:
            pins, dots = groomlab_core.carried_with(json.dumps(self.model.to_dict()), list(names))
        except (RuntimeError, ValueError):
            return []
        return self._layout_items(pins, dots)

    def layout_on_backdrop(self, area):
        """The pins and dots lying on a backdrop's frame (moved with it, hidden when it folds)."""
        import groomlab_core
        try:
            pins, dots = groomlab_core.layout_on_backdrop(json.dumps(self.model.to_dict()),
                                                          (area.x(), area.y(), area.width(), area.height()))
        except (RuntimeError, ValueError):
            return []
        return self._layout_items(pins, dots)

    # -- Align gestures (Houdini's A+drag): the core's alignGesture, live, one undo step ----------

    def begin_align_gesture(self, anchor, mode, at):
        self._gesture = {"anchor": anchor, "mode": mode, "at": at,
                         "start": {n: QtCore.QPointF(i.pos()) for n, i in self.items.items()}}
        if mode != "chain" and anchor in self.items:
            self.items[anchor].setSelected(True)
        self.report({"chain": "Align: drag left or up for the chain upstream, right or down for downstream",
                     "selected": "Align the selection: across for a row, up or down for a column",
                     "distribute": "Spread the selection evenly: across or up and down"}[mode])

    def align_gesture_to(self, at, zoom=1.0):
        import groomlab_core
        g = getattr(self, "_gesture", None)
        if not g:
            return
        for name, start in g["start"].items():
            item = self.items.get(name)
            if item is not None and item.pos() != start:
                item.setPos(start)
        boxes = [(n, i.pos().x(), i.pos().y(), NODE_W, i.height) for n, i in self.items.items() if i.isVisible()]
        d = (at - g["at"]) * zoom  # screen pixels decide the axis
        try:
            placed = groomlab_core.align_gesture(self.model._query(), boxes, g["anchor"], self.selected_names(),
                                                 g["mode"], d.x(), d.y())
        except (RuntimeError, ValueError):
            return
        for name, (x, y) in placed.items():
            if name in self.items:
                self.items[name].setPos(x, y)

    def end_align_gesture(self):
        g, self._gesture = getattr(self, "_gesture", None), None
        if not g:
            return
        moved = [n for n, start in g["start"].items() if n in self.items and self.items[n].pos() != start]
        if moved:
            self._layout_dirty = True
            self.commit_layout("align a node" if len(moved) == 1 else "align %d nodes" % len(moved))

    def rename_pass_through(self, node, input_name):
        """Rename Output: the name a pass-through output shows (empty, or thru_<input>: its own).
        One undo step; links still read thru_<input>."""
        current = self.model.pass_through_name(node, input_name) or "thru_" + input_name
        text, ok = QtWidgets.QInputDialog.getText(self, "Rename Output", "Name:", text=current)
        if not ok:
            return
        self.set_pass_through_name(node, input_name, text)

    def move_pass_through(self, node, input_name, by):
        """Move Output Up (-1) / Down (1): the pass-through output's place among the node's
        outputs (its links stay). One undo step."""
        try:
            changed = self.model.move_pass_through(node, input_name, by)
        except gm.GraphError as e:
            self.report(str(e))
            return False
        if changed:
            self.rebuild()
            self.changed()
        return changed

    def set_pass_through_name(self, node, input_name, text):
        label = (text or "").strip()
        if label == "thru_" + input_name:
            label = ""
        try:
            changed = self.model.set_pass_through_name(node, input_name, label)
        except gm.GraphError as e:
            self.report(str(e))
            return
        if changed:
            self.rebuild()
            self.changed()

    def detach_shaken(self, names, at):
        """A shake while dragged (Houdini's): the nodes out of their links, left where they were
        dropped (one undo step)."""
        self.detach_nodes(names)
        moved = False
        for name, pos in at.items():
            if name in self.model.nodes:
                self.model.set_position(name, pos.x(), pos.y())
                moved = True
        if moved:
            self._layout_dirty = True
            self.commit_layout()
            self.rebuild()

    def link_first_input(self, source, node):
        """`node` fed from source (node, output) on its first input that takes it (one undo step)."""
        src, output = source
        primary = self.model.output_names(src)[0]
        for port in self.model.node_spec(node)["inputs"]:
            try:
                self.model.connect(src, node, port["name"], output=None if output == primary else output, by_hand=True)
            except (gm.GraphError, ValueError):
                continue
            self.rebuild()
            self.changed()
            return True
        self.report("%s has no input that takes %s's output" % (node, src))
        return False

    def reselect(self, backdrop=None, dot=None):
        """After a rebuild: the backdrop (its dict) or dot ((portal, exit)) selected again, so its card
        stays up while it is edited."""
        self.scene.clearSelection()
        for item in getattr(self, "_backdrop_items", []):
            if backdrop is not None and item.data is backdrop:
                item.setSelected(True)
        for item in getattr(self, "portal_items", []):
            if dot is not None and (item.portal, item.exit) == tuple(dot):
                item.setSelected(True)

    def update_dot_preview(self):
        """A selected dot shows where its signal comes from: a dotted line from its portal's entry to
        it (an entry: to each of its exits)."""
        old = getattr(self, "_dot_preview", None)
        if old is not None:
            try:
                self.scene.removeItem(old)
            except RuntimeError:
                pass
        self._dot_preview = None
        dots = [i for i in self.scene.selectedItems() if isinstance(i, PortalItem)]
        if len(dots) != 1 or dots[0].src_item is None:
            return
        dot = dots[0]
        same = [i for i in getattr(self, "portal_items", []) if i.portal == dot.portal]
        entry = next((i for i in same if i.exit is None), None)
        if entry is None:
            return
        path = QtGui.QPainterPath()
        for exit_item in ([dot] if dot.exit is not None else [i for i in same if i.exit is not None]):
            a, b = entry.scenePos(), exit_item.scenePos()
            path.moveTo(a)
            path.lineTo(b)
        pen = QtGui.QPen(dot.color, 1.8, QtCore.Qt.DashLine)
        pen.setCosmetic(True)
        self._dot_preview = self.scene.addPath(path, pen)
        self._dot_preview.setZValue(-0.8)

    def shake_loose(self, names, at):
        """A shake while dragging: the nodes out of their links at once (one: what fed it feeds what
        it fed; several: the links among them kept), left under the cursor, the drag going on."""
        positions = dict((n, self.items[n].pos()) for n in names if n in self.items)
        try:
            done = self.model.detach(names[0]) if len(names) == 1 else self.model.detach_group(names)
        except (gm.GraphError, ValueError) as e:
            self.report(str(e))
            return
        if not done["changed"]:
            return
        for name, pos in positions.items():
            self.model.set_position(name, pos.x(), pos.y())
        self.end_grow()
        self.rebuild()
        for name in names:
            if name in self.items:
                self.items[name].setSelected(True)
        self.changed()
        self.report("shaken loose: %s out of %s links (Ctrl+Z puts them back)" % (
            ", ".join(names), "its" if len(names) == 1 else "their"))
        if QtWidgets.QApplication.mouseButtons() & QtCore.Qt.LeftButton:
            self.scene.carry(names, at)

    def duplicate_drag(self, name, at):
        """Alt+drag on a node: it (the selection, when it is one of it) duplicated where it is, the
        copies (reading what the nodes read) dragged on."""
        selected = self.selected_names()
        names = selected if name in selected else [name]
        try:
            made = self.model.duplicate_nodes(names, (0.0, 0.0))
        except (gm.GraphError, ValueError) as e:
            self.report(str(e))
            return
        self.rebuild()
        self.scene.clearSelection()
        for copy in made:
            self.items[copy].setSelected(True)
        self.changed()
        self.scene.carry(made, at)
        self.report("duplicated %d node%s: drag them where they go" % (len(made), "" if len(made) == 1 else "s"))

    def begin_grow(self, names):
        """A drag of these nodes begins: the open backdrops holding them, to grow with them."""
        import time
        self._grow = []
        for item in getattr(self, "_backdrop_items", []):
            if item.folded():
                continue
            area = item.full_rect()
            held = [n for n in names if n in self.items and
                    area.contains(QtCore.QRectF(self.items[n].pos(), QtCore.QSizeF(NODE_W, self.items[n].height)).center())]
            if held:
                self._grow.append({"item": item, "start": QtCore.QRectF(area), "nodes": held, "left": set()})
        self._grow_speed = [time.perf_counter(), QtGui.QCursor.pos(), 0.0]

    def grow_live(self, names, screen_pos=None):
        """While nodes are dragged: each backdrop holding them grows round them (graph_arrange.h): a
        node pushed past an edge slowly pushes it out; one dragged off fast leaves it."""
        import time
        import groomlab_core
        if getattr(self, "_grow", None) is None:
            self.begin_grow(names)
        now, cursor = time.perf_counter(), QtGui.QCursor.pos()
        last_time, last_pos, speed = self._grow_speed
        dt = max(now - last_time, 1e-3)
        moved = (cursor - last_pos).manhattanLength()
        speed = 0.6 * speed + 0.4 * (moved / dt)
        self._grow_speed = [now, cursor, speed]
        for g in self._grow:
            item, start = g["item"], g["start"]
            held = []
            for name in g["nodes"]:
                node = self.items.get(name)
                if node is None:
                    continue
                card = QtCore.QRectF(node.pos(), QtCore.QSizeF(NODE_W, node.height))
                over = item.full_rect().contains(card.center())
                if not groomlab_core.backdrop_keeps(over, speed, name in g["left"]):
                    g["left"].add(name)
                    continue
                held.append((name, card.x(), card.y(), float(NODE_W), float(node.height)))
            x, y, w, h = groomlab_core.grown_backdrop((start.x(), start.y(), start.width(), start.height()), held, 24.0,
                                                       item.header())
            data = item.data
            if (x, y, w, h) != (data["x"], data["y"], data["w"], data["h"]):
                item.prepareGeometryChange()
                data["w"], data["h"] = w, h
                item._carried = []
                item.setPos(x, y)
                item.update()
                self._layout_dirty = True

    def end_grow(self):
        self._grow = None

    def insert_group_on_wire(self, names, link):
        """A chain dropped on a wire as one: in it, between the link's ends (one undo step)."""
        src, output, dst, input_name = link
        try:
            self.model.insert_nodes_on_link(src, dst, input_name, names, output)
        except (gm.GraphError, ValueError) as e:
            self.report("Not inserted: %s" % e)
            return
        self.rebuild()
        self.changed()
        self.report("inserted %s between %s and %s" % (", ".join(names), src, dst))

    def add_dot(self, at, source=None):
        """A Dot where the search was opened (fed from `source`, (node, output), when a link was
        dropped there): name it, feed it, or pick its Input Portal."""
        try:
            name = self.model.add_dot(at[0], at[1], *(source or (None, None)))
        except (gm.GraphError, ValueError) as e:
            self.report(str(e))
            return None
        self._layout_dirty = True
        if source:
            self.rebuild()
            self.changed()
        else:
            self.commit_layout("add dot")
            self.rebuild()
        self.report("%s: double-click to name it; its Input Portal (its card) takes another dot's signal" % name)
        return name

    def feed_dot(self, portal, src, output):
        """A dot fed from an output (a link dropped on it): its links read it."""
        try:
            self.model.feed_dot(portal, src, output)
        except (gm.GraphError, ValueError) as e:
            self.report(str(e))
            return
        self.rebuild()
        self.changed()
        self.report("%s fed from %s" % (portal, src))

    def set_dot_input(self, portal, exit_index, into):
        """A dot's Input Portal: the dot whose signal it takes (None: a free dot)."""
        try:
            at = self.model.set_dot_input(portal, exit_index, into or None)
        except (gm.GraphError, ValueError) as e:
            self.report(str(e))
            return
        self.rebuild()
        self.changed()
        self.reselect(dot=at)

    def set_backdrop_field(self, data, key, value):
        """A backdrop's title, description or title size (0: the default): one undo step."""
        if data not in self.model.backdrops:
            return
        if value in ("", 0, None):
            data.pop(key, None)
            if key == "title":
                data["title"] = "Backdrop"
        else:
            data[key] = value
        if key in ("fontSize", "description", "title"):
            # Its nodes pushed down clear of a bigger title (or a longer description), the backdrop
            # as wide as its title (the core's clearBackdropTitle, the app's too): in the same step.
            import groomlab_core
            index = next(i for i, b in enumerate(self.model.backdrops) if b is data)
            try:
                self.model.load_dict(json.loads(groomlab_core.clear_backdrop_title(
                    json.dumps(self.model.to_dict()), index, self._node_boxes())))
                data = self.model.backdrops[index]
            except (RuntimeError, ValueError, IndexError, StopIteration):
                pass
        self._layout_dirty = True
        self.commit_layout("backdrop " + key)
        self.rebuild()
        self.reselect(backdrop=data)

    def _node_boxes(self):
        """Every node's card as drawn: (name, x, y, w, h), the core's boxes."""
        return [(name, item.pos().x(), item.pos().y(), float(NODE_W), float(item.height))
                for name, item in self.items.items() if isinstance(item, NodeItem)]

    def align_selected(self, mode):
        """Align the selected nodes (the core's align_boxes: left, centerX, right, top, centerY,
        bottom, row, column, distributeX, distributeY), one undo step."""
        import groomlab_core
        items = [self.items[n] for n in self.selected_names() if n in self.items]
        if len(items) < 2:
            self.report("select two nodes or more to align")
            return
        boxes = [(i.name, i.pos().x(), i.pos().y(), float(NODE_W), float(i.height)) for i in items]
        for name, (x, y) in groomlab_core.align_boxes(boxes, mode).items():
            self.model.set_position(name, x, y)
        self._layout_dirty = True
        self.commit_layout("align " + mode)
        self.rebuild()

    def tidy_selection(self):
        """Tidy Selection: Tidy Layout (the selected nodes; fewer than two: all)."""
        self.tidy_layout()

    def tidy_layout(self):
        """Tidy Layout (L): the selected nodes (fewer than two: the whole graph, then framed) laid
        out by their links, each backdrop a group laid out on it, pins and dots going with what they
        link (the core's graph_arrange.h tidyGraph, the app's too). One undo step."""
        import groomlab_core
        if not self.model:
            return
        selected = self.selected_names()
        chosen = selected if len(selected) >= 2 else []
        boxes = self._node_boxes()
        try:
            data = json.loads(groomlab_core.tidy_graph(json.dumps(self.model.to_dict()), boxes, chosen,
                                                       60.0, 40.0, 24.0, 28.0))
        except (RuntimeError, ValueError) as e:
            self.report("Tidy Layout: %s" % e, trouble=True)
            return
        self.model.load_dict(data)
        self.rebuild()
        if not chosen:
            self.view.frame_all()
        if self.groom:
            self._save_layout("tidy selection" if chosen else "tidy layout")

    def set_wire_style(self, style):
        """How links are drawn: smooth, square or rounded (remembered between sessions)."""
        import groomlab_core
        WIRE_STYLE[0] = style if style in groomlab_core.wire_styles() else "smooth"
        button = getattr(self, "wire_button", None)
        if button is not None:
            button.glyph = "wire_" + WIRE_STYLE[0]
            button.setIcon(widgets.glyph(button.glyph))
        try:
            from maya import cmds
            cmds.optionVar(stringValue=(WIRE_STYLE_VAR, WIRE_STYLE[0]))
        except (ImportError, RuntimeError):
            pass
        for item in self.scene.items():
            if isinstance(item, (EdgeItem, PortalStub)):
                item.update_path()
        self.sync_wire_style_menu()

    def sync_wire_style_menu(self):
        """The Edit menu's Wire Style items checked as the style is."""
        labels = dict((c["label"], c["value"]) for c in definitions.choices("wireStyles"))
        for sub in getattr(self.edit_menu, "submenus", []):
            for action in sub.actions():
                if action.text() in labels and action.isCheckable():
                    action.setChecked(labels[action.text()] == WIRE_STYLE[0])

    def grow_backdrops(self, names):
        """The open backdrops holding moved nodes grow to hold them (Houdini's network boxes): a card
        pushed to an edge pushes the edge out. Whether one grew."""
        import groomlab_core
        grew = False
        for data in self.model.backdrops:
            if data.get("collapsed"):
                continue
            rect = QtCore.QRectF(data["x"], data["y"], data["w"], data["h"])
            held = []
            for name in names:
                item = self.items.get(name)
                if item is not None and rect.contains(QtCore.QRectF(item.pos(), QtCore.QSizeF(NODE_W, item.height)).center()):
                    held.append((name, item.pos().x(), item.pos().y(), float(NODE_W), float(item.height)))
            if not held:
                continue
            x, y, w, h = groomlab_core.grown_backdrop((data["x"], data["y"], data["w"], data["h"]), held, 24.0, 28.0)
            if (x, y, w, h) != (data["x"], data["y"], data["w"], data["h"]):
                data.update({"x": x, "y": y, "w": w, "h": h})
                grew = True
        return grew

    def fold_backdrop(self, data, folded):
        """A backdrop folded to its title bar (its nodes hidden, their links meeting its edges) or
        open again: one undo step, nothing evaluates."""
        if data not in self.model.backdrops:
            return
        if folded:
            data["collapsed"] = True
        else:
            data.pop("collapsed", None)
        self._layout_dirty = True
        self.commit_layout("fold backdrop" if folded else "open backdrop")
        self.rebuild()

    def teleport_link(self, link):
        """Teleport Link (a link's menu): drawn through its source output's portal, out of a new
        exit by the input; the link stays as it is."""
        src, output, dst, input_name = link
        out = self.items[src].outputs.get(output, self.items[src].output).scene_center()
        into = self.items[dst].inputs[input_name].scene_center()
        try:
            portal, _exit = self.model.teleport(dst, input_name, (into.x() - 56, into.y()), (out.x() + 40, out.y()))
        except gm.GraphError as e:
            self.report(str(e))
            return
        self._layout_dirty = True
        self.commit_layout("teleport " + dst + "." + input_name)
        self.rebuild()
        self.report("%s teleported through the portal %s: drag its dots to place them, drag from an exit's nub for "
                    "another link, double-click one to rename it" % (src, portal))

    def link_from_portal(self, portal, exit_index, dst, input_name):
        """A link out of a portal's exit: from the portal's source (one undo step, as any link)."""
        source = next((p for p in self.model.portals if p["name"] == portal), None)
        if source is None:
            return
        self.link(source["from"], dst, input_name, output=source.get("output"))
        if self.model.source_of(dst, input_name) == source["from"]:
            self.model.add_portal_link(portal, -1 if exit_index is None else exit_index, dst, input_name)
            self._layout_dirty = True
            self.commit_layout()
            self.rebuild()

    def add_portal_exit(self, portal, at):
        try:
            self.model.add_portal_exit(portal, at[0], at[1])
        except gm.GraphError as e:
            self.report(str(e))
            return
        self._layout_dirty = True
        self.commit_layout("portal exit")
        self.rebuild()

    def portal_moved(self, portal, exit_index, pos):
        if self.model is not None:
            self.model.move_portal(portal, exit_index, pos.x(), pos.y())
            self._layout_dirty = True

    def rename_portal(self, portal):
        name, ok = QtWidgets.QInputDialog.getText(self, "Portal", "Name:", text=portal)
        if ok and name.strip():
            try:
                self.model.rename_portal(portal, name.strip())
            except gm.GraphError as e:
                self.report(str(e))
                return
            self._layout_dirty = True
            self.commit_layout("rename portal")
            self.rebuild()

    def remove_portal(self, portal, exit_index=None):
        """A portal gone (its links drawn as wires again), or one of its exits."""
        if self.model.remove_portal(portal, exit_index):
            self._layout_dirty = True
            self.commit_layout("remove portal")
            self.rebuild()

    def set_pass_through(self, node, input_name, on):
        """An input's pass-through output (thru_<input>): whatever comes in also comes out, to feed
        the next node. One undo step."""
        try:
            self.model.set_pass_through(node, input_name, on)
        except gm.GraphError as e:
            self.report(str(e))
            return
        self.rebuild()
        self.changed()
        if on:
            self.report("%s hands its %s on: the thru_%s output" % (node, input_name, input_name))

    def move_output_links(self, src, output, dst, dst_output):
        """Shift+drag from an output dropped on another: its links read that one where they fit."""
        try:
            moved = self.model.move_output_links(src, output, dst, dst_output)
        except gm.GraphError as e:
            self.report(str(e))
            return
        if not moved:
            self.report("none of %s's links can take %s's output" % (src, dst))
            return
        self.rebuild()
        self.changed()
        self.report("%d link%s moved to %s" % (len(moved), "" if len(moved) == 1 else "s", dst))

    def open_compound(self, name):
        """Into a Compound: its graph shown and edited (each edit written back into it, one undo
        step); U, Up or the path bar comes back out."""
        try:
            self.model = gm.ScopedModel(self.model, name)
        except (gm.GraphError, ValueError) as e:
            self.report("%s: %s" % (name, e))
            return
        self.scope.append(name)
        self.scene.clearSelection()
        self.rebuild()
        self.view.frame_all()
        self.report("inside %s: edit its nodes; U or the path bar goes back out" % name)

    def leave_compound(self, levels=1):
        """Back out of the compound(s) opened, the last one left selected."""
        left = None
        for _ in range(min(levels, len(self.scope))):
            self.model.write_back()
            self.model = self.model.outer
            left = self.scope.pop()
        if left is None:
            return
        self.rebuild()
        self.view.frame_all()
        if left in self.items:
            self.scene.clearSelection()
            self.items[left].setSelected(True)

    def _restore_scope(self):
        """After the graph was read again (an undo, a reload): back into the compounds that were open
        where they still are."""
        names, self.scope = list(self.scope), []
        for name in names:
            try:
                self.model = gm.ScopedModel(self.model, name)
            except (gm.GraphError, ValueError):
                break
            self.scope.append(name)

    def _fill_scope_bar(self):
        layout = self.scope_bar.layout()
        while layout.count():
            item = layout.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        self.scope_bar.setVisible(bool(self.scope))
        if not self.scope:
            return
        for k, name in enumerate(["Graph"] + self.scope):
            button = QtWidgets.QToolButton()
            button.setText(name)
            button.setAutoRaise(True)
            last = k == len(self.scope)
            button.setEnabled(not last)
            button.setToolTip("Back up to %s" % name if not last else "The compound you are in")
            button.clicked.connect(lambda _=False, up=len(self.scope) - k: self.leave_compound(up))
            layout.addWidget(button)
            if not last:
                layout.addWidget(QtWidgets.QLabel(u"›"))
        hint = QtWidgets.QLabel("U: up a level")
        hint.setEnabled(False)
        layout.addWidget(hint)
        layout.addStretch(1)

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
                self.model.expose(compound, "", target)  # (named as every host names it: the core's)
            else:
                self.model.unexpose(compound, target)
        except gm.GraphError as e:
            self.report(str(e), trouble=True)
            return False
        self.rebuild()
        self.changed()
        return True

    def param_promoted(self, node, param):
        """Inside a compound: whether node.param shows on it (Promote to Compound)."""
        import groomlab_core
        if not self.scope or not isinstance(self.model, gm.ScopedModel):
            return False
        return groomlab_core.param_promoted(json.dumps(self.model.outer.to_dict()), self.model.name, node, param)

    def param_promotable(self, node, param):
        """Inside a compound: whether node.param can show on it."""
        import groomlab_core
        if not self.scope or not isinstance(self.model, gm.ScopedModel):
            return False
        return groomlab_core.param_promotable(json.dumps(self.model.outer.to_dict()), self.model.name, node, param)

    def promote_param(self, node, param, on=True):
        """Promote to Compound (a param's menu inside a compound): node.param shown on the compound
        you are in, to set it from outside without going in and out, or no longer (the core's
        graph_controls.h promoteParam, every host's). The compound stays open; one undo step when Live."""
        import groomlab_core
        if not self.scope or not isinstance(self.model, gm.ScopedModel):
            self.report("Promote to Compound works inside a compound", trouble=True)
            return False
        self.model.write_back()
        outer, name = self.model.outer, self.model.name
        try:
            outer.load_dict(json.loads(groomlab_core.promote_param(json.dumps(outer.to_dict()), name, node, param, bool(on))))
            self.model = gm.ScopedModel(outer, name)
        except (gm.GraphError, RuntimeError, ValueError) as e:
            self.report("Promote to Compound: %s" % e, trouble=True)
            return False
        self.rebuild()
        self.changed()
        self.report(("%s.%s promoted to %s" if on else "%s.%s no longer promoted to %s") % (node, param, name))
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
                # Every host's rule (the editor's wireAfterNode): into src's first link the node
                # fits in, else fed by src on its first input that takes it.
                if self.model.wire_after_node(src, name):
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
            missing = [r for r in info.get("reads", []) if not r["reaches"] and not r.get("optional")]
            if missing:
                notes[name] = "; ".join("reads the attribute %s (%s), which no node upstream writes: it is ignored"
                                        % (r["name"], r["param"]) for r in missing)
        changed = notes != getattr(self, "attribute_notes", {})
        self.attribute_notes = notes
        if changed:
            for item in self.items.values():
                item.update()

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
            # (by hand: a Value node no link leaves takes the param's kind, GraphEdit.connect_by_hand)
            self.model.connect(src, dst, input_name, output=output, by_hand=True)
        except gm.GraphError as e:
            self.report(str(e))
            self.rebuild()
            return
        self.rebuild()
        self.changed()

    def value_kind_for_input(self, src, dst, input_name):
        """A kind an unconnected Value node adopts for a parameter input, else None (the core's)."""
        return self.model.value_kind_for_input(src, dst, input_name)

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
            self.model.remove_node(name, keep_flow=True)  # the flow passes through (issue #80)
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

    def menu_targets(self, name):
        """The nodes a node menu's entry acts on: the selection when the node is in it, else the node."""
        selected = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        return selected if name in selected else [name]

    def detach_selected(self):
        names = [i.name for i in self.scene.selectedItems() if isinstance(i, NodeItem)]
        if not names:
            self.report("select the nodes to detach (Ctrl+Shift+X)")
            return
        self.detach_nodes(names)

    def detach_nodes(self, names):
        """Detach (Ctrl+Shift+X, the node menu; the core's GraphEdit.detach, issue #80): each node
        taken out of the flow as if it were not there, what fed it feeding what it fed, the node kept,
        unlinked, below the new link. One undo step."""
        done = []
        for name in names:
            try:
                if self.model.detach(name)["changed"]:
                    done.append(name)
            except (gm.GraphError, ValueError) as e:
                self.report("%s: %s" % (name, e))
        if not done:
            if names:
                self.report("%s: not linked to anything" % ", ".join(names))
            return
        self.rebuild()
        for name in done:
            item = self.items.get(name)
            if item is not None:
                item.setSelected(True)
        self.changed()
        self.report("detached %s" % ", ".join(done))

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
        portals = [i for i in self.scene.selectedItems() if isinstance(i, PortalItem)]
        if portals:  # a portal (its entry), or an exit: the links drawn as wires again
            for item in portals:
                self.model.remove_portal(item.portal, item.exit)
            self._layout_dirty = True
            self.commit_layout("remove portal")
            if not (nodes or edges or backdrops or pins):
                self.rebuild()
                return
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
            self.model.remove_node(name, keep_flow=True)  # the flow passes through (issue #80)
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
            self._rename_color_source(name, new)
            self.rebuild()
            self.changed()

    def _rename_color_source(self, old, new):
        """A strand color source naming the renamed node ("<channel>@<node>", Clump Colors) follows it."""
        if not self.groom:
            return
        from maya import cmds
        try:
            source = cmds.getAttr(self.groom + ".strandColorSource") or ""
        except (RuntimeError, ValueError):
            return
        import groomlab_core
        renamed = groomlab_core.renamed_color_source(source, old, new)
        if renamed != source:
            groom = self.groom
            self.silently(lambda: cmds.setAttr(groom + ".strandColorSource", renamed, type="string"))

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
            self._cpu_drag(True)
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
            self._cpu_drag(True)
        values = list(value) if len(self._key_preview["plugs"]) == 3 else [value]
        self._key_preview["values"] = [float(v) for v in values]
        if not getattr(self, "_preview_pending", False):
            self._preview_pending = True
            QtCore.QTimer.singleShot(40, self._write_preview)

    def _set_silently(self, plugs, values):
        """Set attributes outside undo (a drag's preview, or putting it back)."""
        from maya import cmds
        with no_undo():
            try:
                for plug, v in zip(plugs, values):
                    cmds.setAttr(plug, v)
            except RuntimeError:  # the attribute went (undo, another groom)
                pass

    def _write_preview(self):
        self._preview_pending = False
        if self._key_preview is not None:
            if self._key_preview["values"] is not None and self.same_groom():
                from maya import cmds
                self._set_silently(self._key_preview["plugs"], self._key_preview["values"])
                cmds.refresh(currentView=True, force=False)
                if sys.platform == "darwin":
                    # (There that refresh draws nothing after a keyed plug's set: the redraw asked
                    # for as Maya's Attribute Editor asks for it.)
                    import maya.OpenMayaUI as omui
                    omui.M3dView.scheduleRefreshAllViews()
            return
        if not (self.live and self.groom) or self.model.validate() or not self.same_groom():
            return
        from maya import cmds
        if self._preview_base is None:
            self._preview_base = cmds.getAttr(self.groom + ".graph") or ""
        with no_undo():
            cmds.setAttr(self.groom + ".graph", self.model.top().to_json(), type="string")
        cmds.refresh(currentView=True, force=False)

    def _cpu_drag(self, on):
        """A slider drag the CPU evaluates (the GPU preview did not take it): a big groom shows thinned
        while it goes on, whole again at the release (groomLabGpuPreview -cpuDrag)."""
        if on == getattr(self, "_cpu_dragging", False) or not self.groom:
            return
        self._cpu_dragging = on
        try:
            from maya import cmds
            cmds.groomLabGpuPreview(self.groom, cpuDrag=on)
        except (RuntimeError, TypeError, AttributeError):  # no such command or flag: the drag shows every strand
            pass

    def end_preview(self):
        """Put back the graph (or a keyed param's attribute) from before a slider drag, silently, so
        the commit is one undo step from it."""
        self._preview_pending = False  # a write still waiting is superseded by the commit
        self._cpu_drag(False)
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
        with no_undo():
            cmds.setAttr(self.groom + ".graph", self._preview_base, type="string")
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


# The parameter panel lives in its own module (param_panel.py): imported last, it reads the
# helpers above.
from groomlab.ui.param_panel import MappableEditor, ParamPanel  # noqa: E402,F401
