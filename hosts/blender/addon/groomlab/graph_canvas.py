# SPDX-License-Identifier: GPL-3.0-or-later
"""The GroomLab Graph Editor drawn in the app's and Maya's look (uikit.py) over Blender's node editor.

Blender's nodes take no styling, so the GroomLab Graph's node editor is drawn over in full: the
canvas (the theme's graph colors, its dot grid), backdrops, wires and nodes as the app draws them
(hosts/app/src/node_graph.cpp drawNode: a card with a tinted header, the name, the cook time and the
header toggles, ports down the sides), and the selected node's params docked on the right (the
sidebar's GroomLab Node panel run through drawn.py).

Editing as the app's: drag from a port to wire it (the ports a wire can go to light up; a wire
picked up from an input moves or, dropped on the canvas, goes); a wire dropped on the canvas opens
the search to add a node wired to it; Tab searches the node types at the pointer; a right-click
opens the node's menu, or on the canvas Add Node, Edit and Presets (the app's drawn menus, from the
same Blender menus); Ctrl+right-drag cuts the wires it crosses. A double-click opens a Compound
(its nodes back in the graph) or a Script node's editor, renames a node by its header or a
backdrop by its title, and on a wire adds a pin (a reroute) to route it; a backdrop's corner grip
resizes it, its title bar moves it with its nodes; F frames the selection (or all) as the cards
are drawn.

The graph is still Blender's node tree (graph_tree.py keeps it in step with the groom's graph):
the canvas draws the tree's nodes where they are, the selection is the tree's, and Blender's own
view (the wheel, the middle button, Home) pans and zooms it. A press on the canvas is the canvas'
(a gizmo over the whole region, as the GroomLab window's): a node picked and dragged, a box drawn
around nodes, a wire from a port, a header toggle, the panel's widgets; a right press is the
canvas' too (its menus, a cut). Keys stay Blender's and act on the selection (F, V, B, Delete,
Ctrl+D, Ctrl+G; graph_editor.py; Tab opens the canvas' search), and Blender Nodes, the classic
view, is one click away.

Sizes are the app's in its scene units (a node 176 wide, the header 28, a port row 18), which are
the graph's own positions ("ui": {"x", "y"}), so a graph lays out alike in every host."""

import json
import math
import time

import bpy

from . import drawn, graph_arrange, graph_tree, uikit
from .helper_client import HelperError

NODE_W = 176.0
CLUMP_W = 23.0      # a Clump's header holds Clump Colors too (a Braid's, Show Tubes)
HEADER = 28.0
ROW = 18.0
PAD = 8.0
PORT_R = 5.0
BACKDROP_HEADER = 28.0
GRIP = 14.0         # a backdrop's corner grip (graph units)
PANEL_W = 300.0     # the params' dock (unscaled pixels, the UI scale's)
TOP_BAR = 40.0
DRAG_START = 3.0    # pixels a press moves before it drags
CLASSIC_PROP = "groomlab_graph_classic"

panel_state = uikit.WindowState()
_draw_handle = [None]
_keymaps = []
_press = {}          # the press going on: {"kind", ...}
_band = [None]       # the box select's rect (region pixels)
_wire = {}           # the wire being dragged: {"from": socket, "out": bool, "at": (x, y), "to": (x, y)}
_cut = []            # the cut's path (region pixels)
_search = {}         # the node search open: {"at": (x, y), "text", "hover", "items", "link": socket or None}


# ---- Where it draws -------------------------------------------------------------------------------

def classic(context):
    wm = getattr(context, "window_manager", None)
    return bool(getattr(wm, CLASSIC_PROP, False))


def tree_here(context):
    """The GroomLab graph this node editor shows (None: another editor, no groom yet)."""
    space = getattr(context, "space_data", None)
    if space is None or getattr(space, "type", "") != "NODE_EDITOR" or space.tree_type != graph_tree.TREE:
        return None
    tree = space.edit_tree or space.node_tree
    if tree is None or not graph_tree.is_graph_tree(tree):
        return None
    return tree


def shows_here(context):
    return not classic(context) and tree_here(context) is not None and context.region is not None \
        and context.region.type == "WINDOW"


# ---- The theme ------------------------------------------------------------------------------------

_theme_cache = {}


def theme():
    """The graph's colors (linear RGBA, as uikit draws) from the theme file's "graph" section."""
    if "t" in _theme_cache:
        return _theme_cache["t"]
    from . import blender_theme
    g = blender_theme.graph_colors()

    def color(value, fallback):
        rgb = uikit._srgb(value, {}) if isinstance(value, str) else None
        return uikit._linear_rgba(rgb or uikit._srgb(fallback, {}))

    t = {
        "canvas": color(g.get("canvas"), "#111215"), "grid": color(g.get("grid"), "#26282E"),
        "node": color(g.get("node"), "#1F2025"), "border": color(g.get("nodeBorder"), "#33353C"),
        "text": color(g.get("text"), "#EDEDF0"), "textDim": color(g.get("textDim"), "#B4B5BC"),
        "selected": color(g.get("selected"), "#8B7DFF"), "error": color(g.get("error"), "#E5534B"),
        "warning": color(g.get("warning"), "#D4B26A"), "accent": color(g.get("accent"), "#8B7DFF"),
        "muted": color(g.get("uiTextDim"), "#737373"), "focus": color(g.get("blue"), "#5AD2F0"),
        "radius": float(g.get("cornerRadius", 10)), "header": g.get("headerStyle", "tinted"),
        "dots": g.get("canvasStyle", "dots") == "dots", "shadow": bool(g.get("shadow", True)),
        "categories": {k: color(v, "#9095A0") for k, v in (g.get("categories") or {}).items()},
        "ports": {k: color(v, "#C8C8C8") for k, v in (g.get("ports") or {}).items()},
    }
    # An exposed param's input is named in the Field port's color, lighter (Maya's lighter(120)).
    t["param"] = _lighter(_port_color(t, "Field"), 1.2)
    _theme_cache["t"] = t
    return t


def _category_color(t, node):
    category = "Output" if node.gl_is_output and node.gl_type == "Output" else graph_tree.category_of(node.gl_type)
    if category in t["categories"]:
        return t["categories"][category]
    rgb = graph_tree.CATEGORY_COLORS.get(category, (142, 150, 158))
    return uikit._linear_rgba(tuple(c / 255.0 for c in rgb))


def _port_type(socket):
    name = socket.bl_idname
    return name[len("GroomLabSocket"):] if name.startswith("GroomLabSocket") else "Any"


def _port_color(t, port_type):
    for key in (port_type, port_type.lower(), port_type.capitalize()):
        if key in t["ports"]:
            return t["ports"][key]
    rgb = graph_tree.TYPE_COLORS.get(port_type, (200, 200, 200))
    return uikit._linear_rgba(tuple(c / 255.0 for c in rgb))


def _srgb_of(c):
    """A linear color's sRGB components (0-1), as Qt holds colors."""
    return tuple(12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055 for v in c[:3])


def _readable_on(c):
    """Dark or light text, whichever reads on `c` (Maya's graph_editor.readable_on)."""
    r, g, b = _srgb_of(c)
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    return uikit._linear_rgba((28 / 255.0, 29 / 255.0, 33 / 255.0) if luma > 0.55 else (1.0, 1.0, 1.0))


def _lighter(c, factor):
    """QColor.lighter in sRGB: the value scaled, past full taking the excess off the saturation."""
    import colorsys
    h, s, v = colorsys.rgb_to_hsv(*_srgb_of(c))
    v *= factor
    if v > 1.0:
        s = max(0.0, s - (v - 1.0))
        v = 1.0
    return uikit._linear_rgba(colorsys.hsv_to_rgb(h, s, v), c[3] if len(c) > 3 else 1.0)


def with_alpha(c, a):
    return (c[0], c[1], c[2], a)


# ---- The graph laid out on the region -------------------------------------------------------------

class Box(object):
    """A node's card on the region: its rect (x, y bottom, w, h, in region pixels), ports and toggles."""

    def __init__(self, node, rect, k):
        self.node, self.rect, self.k = node, rect, k
        self.inputs, self.outputs = [], []  # (socket, (x, y)) in region pixels
        self.toggles = []                   # (id, rect): the header's toggles, left to right
        self.header_ink = None              # on a filled header: the ink that reads on its color


class Graph(object):
    """The tree's nodes, wires, pins and backdrops where they show in `region` (Blender's view)."""

    def __init__(self, context, region, tree):
        self.tree, self.region = tree, region
        sysp = context.preferences.system
        pixel = float(getattr(sysp, "pixel_size", 1.0) or 0.0)
        self.ui = float(sysp.ui_scale or 1.0) * (pixel if pixel > 0 else 1.0)  # (0 without a window: headless)
        v2d = region.view2d
        self.ox, self.oy = v2d.view_to_region(0.0, 0.0, clip=False)
        x1, _ = v2d.view_to_region(1000.0, 0.0, clip=False)
        self.k = (x1 - self.ox) / 1000.0 * self.ui  # region pixels per graph unit (the app's px)
        self.boxes, self.by_name, self.pins, self.frames = [], {}, [], []
        # Folded backdrops (graph_arrange.h foldedBackdrop): drawn as their title bar and a strip,
        # their nodes, pins and dots hidden, links into or out of them meeting the strip's edges.
        self.folded = []     # (frame, its unfolded rect in graph units: x, y down, w, h)
        self.hidden = {}     # node name -> the folded frame hiding it
        for node in tree.nodes:
            if node.bl_idname == "NodeFrame":
                rect = self._frame_rect(node)
                if node.get("gl_collapsed"):
                    gx, gy = graph_tree.get_location(node)
                    w, h = graph_tree.frame_size(node)
                    self.folded.append((node, (gx, -gy, w, h)))
                    rect = self._folded_rect(node)
                self.frames.append((node, rect))
        for node in tree.nodes:
            if node.bl_idname in ("NodeFrame",):
                continue
            fold = self._folded_holding(node)
            if node.bl_idname == "NodeReroute":
                if fold is None:
                    gx, gy = graph_tree.get_location(node)
                    self.pins.append((node, self.at(gx, gy)))
            elif graph_tree.ours(node) and not node.hide:
                if fold is not None:
                    self.hidden[node.name] = fold
                    continue
                box = self._box(node)
                self.boxes.append(box)
                self.by_name[node.name] = box
        # The portals' dots (graph_portals.h), and the links drawn out of them instead of as wires.
        self.dots = []
        for d in graph_arrange.dots(tree):
            if any(_inside(r, d.x, d.y) for _f, r in self.folded):
                continue
            self.dots.append((d, self.at(d.x, -d.y)))
        self.teleported = {link: d for d, _at in self.dots for link in d.links}
        self.teleported.update({link: d for d in graph_arrange.dots(tree) for link in d.links if link not in self.teleported})

    def at(self, gx, gy):
        return self.ox + gx * self.k, self.oy + gy * self.k

    def to_graph(self, x, y):
        return (x - self.ox) / self.k, (y - self.oy) / self.k

    def _frame_rect(self, frame):
        gx, gy = graph_tree.get_location(frame)
        w, h = frame.width, frame.height
        x, top = self.at(gx, gy)
        return (x, top - h * self.k, w * self.k, h * self.k)

    def _folded_rect(self, frame):
        """A folded backdrop's title bar and strip (the core's foldedBackdrop), region pixels."""
        gx, gy = graph_tree.get_location(frame)
        b = graph_tree.backdrop_of(frame)
        w = graph_arrange.folded_width(b)
        h = graph_arrange.backdrop_header(b) + 16.0
        x, top = self.at(gx, gy)
        return (x, top - h * self.k, w * self.k, h * self.k)

    def _folded_holding(self, node):
        """The folded backdrop `node` lies on (a child of it, or its card's center on it), else None."""
        if not self.folded:
            return None
        p = node.parent
        while p is not None:
            for f, _r in self.folded:
                if f == p:
                    return f
            p = p.parent
        gx, gy = graph_tree.get_location(node)
        if node.bl_idname == "NodeReroute":
            cx, cy = gx, -gy
        else:
            w, h = graph_arrange.card_size(node)
            cx, cy = gx + w * 0.5, -gy + h * 0.5
        for f, r in self.folded:
            if _inside(r, cx, cy):
                return f
        return None

    def _box(self, node):
        ins = [s for s in node.inputs if s.enabled and not s.hide]
        outs = [s for s in node.outputs if s.enabled and not s.hide]
        clump = node.gl_type == "Clump"
        braid = not clump and graph_tree.shows_tubes(node)
        label = graph_tree.label_of(node.gl_type)
        type_row = node.name != label and node.name != node.gl_type and len(ins) <= 1 and len(outs) <= 1
        w, h = graph_arrange.card_size(node)  # (the size the core lays out: Tidy, Align)
        gx, gy = graph_tree.get_location(node)
        x, top = self.at(gx, gy)
        k = self.k
        box = Box(node, (x, top - h * k, w * k, h * k), k)
        box.type_row = type_row
        for i, s in enumerate(ins):
            box.inputs.append((s, (x, top - (HEADER + PAD + ROW * (i + 0.5)) * k)))
        for i, s in enumerate(outs):
            box.outputs.append((s, (x + w * k, top - (HEADER + PAD + ROW * (i + 0.5)) * k)))
        # The header's toggles, right to left from the corner: Bypass, View (strands out), Expand
        # Inputs (linkable params), Clump Colors (the app's bypassButtonRect and its neighbours).
        size, step = 18.0 * k, 23.0 * k
        right = x + w * k - 26.0 * k
        y = top - 5.0 * k - size
        toggles = [("bypass", (right, y, size, size))]
        if viewable(box):
            right -= step
            toggles.append(("view", (right, y, size, size)))
        if linkable(node):
            right -= step
            toggles.append(("inputs", (right, y, size, size)))
        if clump:
            right -= step
            toggles.append(("clump", (right, y, size, size)))
        elif braid:  # Show Tubes, where a Clump has Clump Colors
            right -= step
            toggles.append(("tubes", (right, y, size, size)))
        box.toggles = list(reversed(toggles))
        return box

    def box_at(self, x, y):
        for box in reversed(self.boxes):
            if uikit.contains(box.rect, x, y):
                return box
        return None

    def toggle_at(self, x, y):
        for box in reversed(self.boxes):
            for ident, r in box.toggles:
                if uikit.contains(r, x, y):
                    return box, ident
        return None

    def port_at(self, x, y, outputs=True, inputs=True):
        """(box, socket, is_output, point) of the port nearest (x, y) within reach, else None."""
        reach = max(10.0 * self.k, 8.0)
        best, best_d = None, reach * reach
        for box in self.boxes:
            r = box.rect
            if not (r[0] - reach <= x <= r[0] + r[2] + reach and r[1] - reach <= y <= r[1] + r[3] + reach):
                continue
            for out, ports in ((False, box.inputs if inputs else []), (True, box.outputs if outputs else [])):
                for s, (px, py) in ports:
                    d = (px - x) ** 2 + (py - y) ** 2
                    if d < best_d:
                        best, best_d = (box, s, out, (px, py)), d
        return best

    def pin_at(self, x, y):
        reach = 9.0 * self.k
        for node, (px, py) in reversed(self.pins):
            if (px - x) ** 2 + (py - y) ** 2 <= reach * reach:
                return node
        return None

    def frame_at(self, x, y):
        """A backdrop whose title bar is under (x, y): it drags with its nodes (folded: all of it)."""
        for frame, r in reversed(self.frames):
            hh = header_of(frame) * self.k
            if uikit.contains((r[0], r[1] + r[3] - hh, r[2], hh), x, y) or (frame.get("gl_collapsed") and uikit.contains(r, x, y)):
                return frame
        return None

    def chevron_at(self, x, y):
        """A backdrop whose fold chevron (its title bar's right end) is under (x, y)."""
        for frame, r in reversed(self.frames):
            c = chevron_rect(frame, r, self.k)
            if uikit.contains(c, x, y):
                return frame
        return None

    def grip_at(self, x, y):
        """A backdrop whose corner grip (bottom right) is under (x, y): it resizes (not folded)."""
        g = GRIP * self.k
        for frame, r in reversed(self.frames):
            if not frame.get("gl_collapsed") and uikit.contains((r[0] + r[2] - g, r[1], g, g), x, y):
                return frame
        return None

    def dot_at(self, x, y):
        """(dot, on its nub) of the portal dot under (x, y), else None."""
        reach = (graph_arrange.DOT_R + 3.0) * self.k
        nub = graph_arrange.DOT_NUB_REACH * self.k
        for d, (px, py) in reversed(self.dots):
            if d.source is not None and (px + graph_arrange.DOT_NUB * self.k - x) ** 2 + (py - y) ** 2 <= nub * nub \
                    and x > px + graph_arrange.DOT_R * 0.4 * self.k:
                return d, True
            if (px - x) ** 2 + (py - y) ** 2 <= reach * reach:
                return d, False
        return None

    def dot_point(self, d, nub=False):
        x, y = self.at(d.x, -d.y)
        return (x + graph_arrange.DOT_NUB * self.k, y) if nub else (x, y)

    def link_key(self, link):
        """(to, input) of a link into a GroomLab node, else None."""
        if graph_tree.ours(link.to_node) and link.to_node.gl_name:
            return link.to_node.gl_name, link.to_socket.identifier
        return None

    def drawn_as_wire(self, link):
        """Whether the link is drawn as a wire: not teleported (a dot draws it), not inside one folded backdrop."""
        if self.link_key(link) in self.teleported:
            return False
        a, b = self.hidden.get(link.from_node.name), self.hidden.get(link.to_node.name)
        if a is not None and a == b:
            return False
        if link.from_node.bl_idname == "NodeReroute" and self._folded_holding(link.from_node) is not None:
            return False
        if link.to_node.bl_idname == "NodeReroute" and self._folded_holding(link.to_node) is not None:
            return False
        return True

    def wire_at(self, x, y, reach=7.0):
        """The link whose wire passes within `reach` pixels of (x, y), else None."""
        best, best_d = None, reach * reach
        for link in self.tree.links:
            if not self.drawn_as_wire(link):
                continue
            a = self.socket_point(link.from_node, link.from_socket, True)
            b = self.socket_point(link.to_node, link.to_socket, False)
            if a is None or b is None:
                continue
            pts = _wire_points(a, b, self.k)
            for p, q in zip(pts, pts[1:]):
                d = _segment_distance2((x, y), p, q)
                if d < best_d:
                    best, best_d = link, d
        return best

    def socket_point(self, node, socket, output):
        if node.bl_idname == "NodeReroute":
            gx, gy = graph_tree.get_location(node)
            return self.at(gx, gy)
        box = self.by_name.get(node.name)
        if box is None:
            fold = self.hidden.get(node.name)
            if fold is not None:  # meets the folded backdrop's edge: the left for links in, the right out
                r = next(rr for f, rr in self.frames if f == fold)
                return (r[0] + r[2], r[1] + r[3] * 0.4) if output else (r[0], r[1] + r[3] * 0.4)
            return None
        for s, point in (box.outputs if output else box.inputs):
            if s == socket:
                return point
        return None


def _inside(r, x, y):
    """(x, y) in graph units on the rect (x, y down, w, h)."""
    return r[0] <= x <= r[0] + r[2] and r[1] <= y <= r[1] + r[3]


def header_of(frame):
    """A backdrop's title bar height (the core's backdropHeader: taller for a bigger title)."""
    return graph_arrange.backdrop_header(graph_tree.backdrop_of(frame))


def chevron_rect(frame, r, k):
    hh = header_of(frame) * k
    top = r[1] + r[3]
    return (r[0] + r[2] - 26.0 * k, top - hh, 22.0 * k, hh)


def _segment_distance2(pt, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((pt[0] - a[0]) * dx + (pt[1] - a[1]) * dy) / (dx * dx + dy * dy)))
    cx, cy = a[0] + t * dx, a[1] + t * dy
    return (pt[0] - cx) ** 2 + (pt[1] - cy) ** 2


def compatible(a, b, out=True):
    """Whether a wire can join ports of these types: `a` the wire's own end (an output when `out`,
    else an input), `b` the other (the core's portTypesMatch through the helper, which has the last
    word: a wire it refuses is taken back and the panel says why)."""
    return graph_tree.port_types_match(a, b) if out else graph_tree.port_types_match(b, a)


def viewable(box):
    return graph_tree.viewable(box.node)


def linkable(node):
    return any(p.get("port") for p in graph_tree.node_param_specs(node))


# ---- Drawing -------------------------------------------------------------------------------------

def _wire_points(a, b, k, below=None, through=False):
    """A wire from the output at `a` to the input at `b` (region pixels) in the user's wire style
    (the core's wireRoute, graph_arrange.WIRE_STYLES): forward a level S-curve or square runs;
    backwards, down under the cards and back along under them. `through`: a piece between pins or
    dots (no loop)."""
    k = max(k, 1e-6)
    # In graph units, y down (the route is the same wherever it starts; `below` is region pixels).
    steps = graph_arrange.wire_route(graph_arrange.wire_style(), a[0] / k, -a[1] / k, b[0] / k, -b[1] / k,
                                     None if below is None else -below / k, through)
    pts = graph_arrange.route_points((a[0] / k, -a[1] / k), steps, 20)
    return [(x * k, -y * k) for x, y in pts]


def _eye(p, c, rx, color, width, pupil):
    pts = [(c[0] + rx * (2 * t - 1), c[1] + 0.95 * rx * 4 * t * (1 - t) * 0.5) for t in (i / 12.0 for i in range(13))]
    p.lines(pts + [(x, 2 * c[1] - y) for x, y in reversed(pts)], color, width)
    p.rect((c[0] - pupil, c[1] - pupil, 2 * pupil, 2 * pupil), color, pupil)


def draw():
    context = bpy.context
    if not shows_here(context):
        return
    try:
        _draw(context)
    except Exception as e:  # noqa: BLE001 - a draw callback that raises runs again every redraw: say it once
        if getattr(draw, "_last", None) != str(e):
            draw._last = str(e)
            import traceback
            print("GroomLab graph:", e)
            traceback.print_exc()


def _draw(context):
    import gpu
    region = context.region
    tree = tree_here(context)
    t = theme()
    ui = uikit._theme()
    g = Graph(context, region, tree)
    k = g.k
    p = uikit.Painter(ui, k)
    W, H = region.width, region.height
    p.rect((0, 0, W, H), t["canvas"])
    _draw_grid(p, g, t, W, H)
    scalp = getattr(tree, "gl_scalp", None)
    from . import graph_editor
    errors = graph_editor.node_errors(scalp) if scalp is not None else {}
    notes = graph_editor.node_notes(scalp) if scalp is not None else {}
    times = graph_editor._node_times(scalp) if scalp is not None else {}
    for frame, r in g.frames:
        if not frame.get("gl_collapsed"):
            _draw_backdrop(p, t, frame, r, k)
    _draw_wires(p, g, t)
    _draw_dots(p, g, t)
    for frame, r in g.frames:  # folded: over the links meeting its edges
        if frame.get("gl_collapsed"):
            _draw_backdrop(p, t, frame, r, k)
    for node, (px, py) in g.pins:
        _draw_pin(p, t, node, px, py, k)
    active = tree.nodes.active
    order = [b for b in g.boxes if not b.node.select] + [b for b in g.boxes if b.node.select and b.node != active] + \
        [b for b in g.boxes if b.node == active and b.node.select]
    viewed = next((b.node.gl_name for b in g.boxes if b.node.gl_viewed), "")
    for box in order:
        _draw_node(p, t, box, errors, notes, times, viewed, box.node == active)
    if _wire:
        _draw_wire_drag(p, g, t)
    if len(_cut) > 1:
        p.lines(_cut, with_alpha(t["error"], 0.9), 1.5 / max(k, 0.01))
    if _band[0] is not None:
        x0, y0, x1, y1 = _band[0]
        r = (min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0))
        p.rect(r, with_alpha(t["selected"], 0.11), 0.0, t["selected"], 1.0)
    # The chrome at the UI's scale, not the view's.
    chrome = uikit.Painter(ui, g.ui)
    _draw_top_bar(chrome, context, region, tree, g.ui)
    _draw_panel(context, region)
    if _search:
        _draw_search(chrome, region, g.ui)
    gpu.state.blend_set("NONE")


def _dashed(p, points, color, width, dash, gap):
    """A polyline drawn in dashes `dash` long, `gap` apart (region pixels)."""
    on, left, seg = True, dash, [points[0]]
    for a, b in zip(points, points[1:]):
        length = math.hypot(b[0] - a[0], b[1] - a[1])
        pos = 0.0
        while length - pos > 1e-6:
            step = min(left, length - pos)
            pos += step
            left -= step
            q = (a[0] + (b[0] - a[0]) * pos / length, a[1] + (b[1] - a[1]) * pos / length)
            if on:
                seg.append(q)
            if left <= 1e-6:
                if on and len(seg) > 1:
                    p.lines(seg, color, width)
                on = not on
                left = dash if on else gap
                seg = [q]
    if on and len(seg) > 1:
        p.lines(seg, color, width)


def _draw_wire_drag(p, g, t):
    """The wire being dragged (dashed, the app's) and the ports it can go to, ringed."""
    k = g.k
    kind = _port_type(_wire["from"])
    color = _port_color(t, kind)
    a, b = (_wire["at"], _wire["to"]) if _wire["out"] else (_wire["to"], _wire["at"])
    _dashed(p, _wire_points(a, b, k), color, 2.0, 7.0 * k, 4.0 * k)
    for box in g.boxes:
        if box.node == _wire["from"].node:
            continue
        for s, (px, py) in (box.inputs if _wire["out"] else box.outputs):
            if compatible(kind, _port_type(s), _wire["out"]):
                r = (PORT_R + 3.0) * k
                p.rect((px - r, py - r, 2 * r, 2 * r), (0, 0, 0, 0), r, with_alpha(t["selected"], 0.9), 1.5 * k)


def _draw_grid(p, g, t, W, H):
    if not t["dots"]:
        return
    step = 20.0 * g.k
    while step < 12.0:
        step *= 2.0
    r = min(max(g.k, 0.75), 1.5)
    x0 = g.ox - math.floor(g.ox / step) * step
    y0 = g.oy - math.floor(g.oy / step) * step
    points = []
    y = y0
    while y < H:
        x = x0
        while x < W:
            points.append((x, y))
            x += step
        y += step
    if len(points) < 40000:
        p.dots(points, r, t["grid"])


def _draw_backdrop(p, t, frame, r, k):
    rgb = tuple(frame.color) if frame.use_custom_color else tuple(c / 255.0 for c in graph_tree.BACKDROP_COLOR)
    base = (rgb[0], rgb[1], rgb[2], 1.0)
    radius = t["radius"] * k
    folded = bool(frame.get("gl_collapsed"))
    p.rect(r, with_alpha(base, (200 if folded else 46) / 255.0), radius, with_alpha(base, 120 / 255.0), 1.5 * k)
    top = r[1] + r[3]
    hh = header_of(frame) * k
    p.band(r, with_alpha(base, 120 / 255.0), with_alpha(base, 120 / 255.0), radius, top - hh, top)
    size = int(round(float(frame.get("gl_font_size") or 0))) or 14
    chev = chevron_rect(frame, r, k)
    if size * k >= 6:
        p.text(frame.label or frame.name, (r[0] + 12 * k, top - hh, chev[0] - r[0] - 14 * k, hh),
               t["textDim"], size=size, bold=True)
        # The fold chevron: down open, right folded.
        cx, cy, a = chev[0] + chev[2] * 0.5, chev[1] + chev[3] * 0.5, 4.5 * k
        pts = [(cx - a * 0.6, cy + a), (cx + a * 0.6, cy), (cx - a * 0.6, cy - a)] if folded else \
            [(cx - a, cy + a * 0.6), (cx, cy - a * 0.6), (cx + a, cy + a * 0.6)]
        p.lines(pts, t["textDim"], 1.6)
    text = str(frame.get("gl_description") or "")
    if text and not folded and 12 * k >= 6:
        per_line = max(8, int((r[2] / k - 24.0) / 6.5))
        lines = []
        for para in text.split("\n"):
            while len(para) > per_line:
                cut = para.rfind(" ", 0, per_line)
                cut = per_line if cut <= 0 else cut
                lines.append(para[:cut])
                para = para[cut:].lstrip()
            lines.append(para)
        y = top - hh - 4 * k
        for line in lines:
            if y - 16 * k < r[1]:
                break
            p.text(line, (r[0] + 12 * k, y - 16 * k, r[2] - 24 * k, 16 * k), t["textDim"], size=12)
            y -= 16 * k
    if not folded:
        light = tuple(min(1.0, c * 1.3) for c in base[:3]) + (0.9,)
        for d in (4.0, 8.0, 12.0):  # the corner grip
            p.lines([(r[0] + r[2] - d * k, r[1] + 2 * k), (r[0] + r[2] - 2 * k, r[1] + d * k)], light, 1.5)
    if frame.select:
        p.rect(r, (0, 0, 0, 0), radius, t["selected"], 2.0 * k)


def _draw_wires(p, g, t):
    picked = _wire.get("picked")
    for link in g.tree.links:
        if not link.is_valid or link.is_hidden or (picked is not None and link == picked) or not g.drawn_as_wire(link):
            continue
        a = g.socket_point(link.from_node, link.from_socket, True)
        b = g.socket_point(link.to_node, link.to_socket, False)
        if a is None or b is None:
            continue
        kind = _port_type(link.from_socket)
        into = _port_type(link.to_socket)
        if kind in ("Any", "Curves") and into not in ("Any", "Curves"):
            kind = into
        color = with_alpha(_port_color(t, kind), 220 / 255.0)
        if link.is_muted:
            color = with_alpha(color, 0.45)
        p.lines(_wire_points(a, b, g.k), color, 2.0)


def _dot_kind(g, d):
    """The port type a dot carries (its source output's), "" for a free dot."""
    if d.source is None:
        return ""
    node = graph_tree.node_named(g.tree, d.source[0])
    if node is None:
        return "Any"
    out = next((o for o in node.outputs if o.identifier == d.source[1]), None) if d.source[1] else \
        (node.outputs[0] if len(node.outputs) else None)
    return _port_type(out) if out is not None else "Any"


def _dot_source_point(g, d):
    node = graph_tree.node_named(g.tree, d.source[0]) if d.source else None
    if node is None:
        return None
    out = next((o for o in node.outputs if o.identifier == d.source[1]), None) if d.source[1] else \
        (node.outputs[0] if len(node.outputs) else None)
    return g.socket_point(node, out, True) if out is not None else None


def _draw_dots(p, g, t):
    """The portals (graph_portals.h): each entry's short wire from its source output, the links out
    of each dot on to their inputs, the dots, their names (Maya's PortalItem)."""
    k = g.k
    for d, (x, y) in g.dots:
        kind = _dot_kind(g, d)
        color = _port_color(t, kind) if kind else t["textDim"]
        wire = with_alpha(color, 220 / 255.0)
        if d.exit is None:
            src = _dot_source_point(g, d)
            if src is not None:
                p.lines(_wire_points(src, (x - graph_arrange.DOT_R * k, y), k, through=True), wire, 2.0)
        nub = g.dot_point(d, nub=True)
        for to, input_name in d.links:
            node = graph_tree.node_named(g.tree, to)
            sock = next((s for s in node.inputs if s.identifier == input_name), None) if node is not None else None
            b = g.socket_point(node, sock, False) if sock is not None else None
            if b is not None:
                p.lines(_wire_points(nub, b, k, through=True), wire, 2.0)
    for d, (x, y) in g.dots:
        kind = _dot_kind(g, d)
        color = _port_color(t, kind) if kind else t["textDim"]
        r = graph_arrange.DOT_R * k
        picked = _press.get("dot") == (d.portal, d.exit)
        fill = tuple(c * 0.55 for c in color[:3]) + (1.0,) if d.exit is None else color
        p.rect((x - r, y - r, 2 * r, 2 * r), fill, r, t["selected"] if picked else color, 2.5 * k)
        if d.source is not None:
            nx = x + graph_arrange.DOT_NUB * k
            p.lines([(nx - 4.5 * k, y + 6 * k), (nx + 4.5 * k, y), (nx - 4.5 * k, y - 6 * k), (nx - 4.5 * k, y + 6 * k)],
                    color, 2.0)
        if 11 * k >= 6:
            tw = p.text_size(d.portal, 11)
            p.text(d.portal, (x - tw / 2.0, y + r + 3 * k, tw + 4, 18 * k), t["text"], size=11, bold=True, clip=False)


def _draw_pin(p, t, node, x, y, k):
    kind = _port_type(node.inputs[0]) if node.inputs else "Any"
    links = [l for l in node.id_data.links if l.to_node == node]
    if links:
        kind = _port_type(links[0].from_socket)
    color = _port_color(t, kind)
    r = 6.0 * k
    p.rect((x - r, y - r, 2 * r, 2 * r), color, r, t["selected"] if node.select else with_alpha(color, 0.6), 2.0 * k)


def _draw_node(p, t, box, errors, notes, times, viewed, active):
    from . import graph_editor
    node, (x, y, w, h), k = box.node, box.rect, box.k
    top = y + h
    radius = t["radius"] * k
    color = _category_color(t, node)
    fade = 0.5 if node.mute else 1.0
    if t["shadow"]:
        for i in (3, 2, 1):
            grow = 1.5 * i * k
            p.rect((x - grow, y - grow - 3.0 * k, w + 2 * grow, h + 2 * grow), (0, 0, 0, 22 / 255.0 * fade), radius + grow)
    p.fade = fade
    p.rect(box.rect, t["node"], radius, t["border"], 1.0)
    if t["header"] == "filled":
        p.band(box.rect, color, color, radius, top - HEADER * k, top)
    elif t["header"] != "minimal":  # tinted: the category's color washing down from the top edge
        p.band(box.rect, with_alpha(color, 30 / 255.0), with_alpha(color, (112 if node.select else 96) / 255.0),
               radius, top - HEADER * k, top)
        p.band(box.rect, color, color, radius, top - 2.0 * k, top)
    readable = 14 * k >= 6
    filled = t["header"] == "filled"
    # The header's text as Maya's: on a filled header the ink that reads on its color, else the text.
    ink = _readable_on(color) if filled else t["text"]
    box.header_ink = ink if filled else None
    # The toggles, then the badges right to left before them, then the name in what is left.
    left_of_toggles = box.toggles[0][1][0] - 4.0 * k if box.toggles else x + w - 8.0 * k
    for ident, r in box.toggles:
        _draw_toggle(p, t, box, ident, r, viewed)
    bx = left_of_toggles
    mid = top - HEADER * k * 0.5
    name = node.gl_name
    stamp = graph_editor.time_text(times.get(name))
    if stamp and readable:
        size = 11
        tw = p.text_size(stamp, size)
        ms = times.get(name)
        # As Maya's: on a filled header its ink; else the warning color when slow, otherwise the text
        # faded (Maya's muted gray is too dark on this header's brighter wash).
        stamp_color = with_alpha(ink, 190 / 255.0) if filled else (
            t["warning"] if ms is not None and ms >= graph_editor.SLOW_MS else with_alpha(ink, 0.75))
        p.text(stamp, (bx - tw, top - HEADER * k, tw, HEADER * k), stamp_color, size=size, clip=False)
        bx -= tw + 6.0 * k
    if node.gl_viewed and not viewable(box):
        _eye(p, (bx - 7.0 * k, mid), 6.5 * k, t["accent"], 1.4, 2.0 * k)
        bx -= 18.0 * k
    if notes.get(name) and name not in errors:
        c, s = (bx - 6.0 * k, mid), 12.0 * k
        hh = s * 0.9
        p.lines([(c[0], c[1] + hh / 2), (c[0] + s / 2, c[1] - hh / 2), (c[0] - s / 2, c[1] - hh / 2), (c[0], c[1] + hh / 2)],
                ink if filled else t["warning"], 1.4)
        p.lines([(c[0], c[1] + hh * 0.12), (c[0], c[1] - hh * 0.18)], ink if filled else t["warning"], 1.4)
        bx -= 17.0 * k
    if readable:
        tx = x + 12.0 * k
        if t["header"] == "minimal":
            dot = 4.0 * k
            p.rect((x + 14 * k - dot, mid - dot, 2 * dot, 2 * dot), color, dot)
            tx = x + 24.0 * k
        p.text(node.name, (tx, top - HEADER * k, max(24.0 * k, bx - tx - 2.0 * k), HEADER * k), ink, size=14, bold=True)
    # The ports: inputs down the left edge, outputs down the right, named inside.
    for s, (px, py) in box.inputs:
        pc = _port_color(t, _port_type(s))
        r = PORT_R * k
        if s.display_shape.startswith("DIAMOND"):
            p.lines([(px - r, py), (px, py + r), (px + r, py), (px, py - r), (px - r, py)], pc, 2.0)
        else:
            p.rect((px - r, py - r, 2 * r, 2 * r), pc, r)
        if readable:
            param = s.display_shape.startswith("DIAMOND")  # an exposed param: the Field color, lighter
            p.text(s.name, (px + 10 * k, py - ROW * k / 2, w / 2, ROW * k), t["param"] if param else t["text"], size=13)
    for s, (px, py) in box.outputs:
        pc = _port_color(t, _port_type(s))
        r = PORT_R * k
        p.rect((px - r, py - r, 2 * r, 2 * r), pc, r)
        if readable:
            p.text(s.name, (px - 10 * k - w / 2, py - ROW * k / 2, w / 2, ROW * k), t["text"], size=13, align="right")
    if box.type_row and readable:
        info = graph_editor.graph_authoring.library_info(node) if node.gl_library_info else {}
        # The type: under a tinted header the category's color, lighter, as Maya's; under a filled one
        # (Maya writes it on the header, here it sits on the card) or a minimal one, muted.
        kind = _lighter(color, 1.15) if t["header"] == "tinted" else t["textDim"]
        p.text(info.get("label") or graph_tree.label_of(node.gl_type), (x, y + 2 * k, w, 14 * k), kind, size=11,
               align="center")
    p.fade = 1.0
    if node.gl_evaluation in graph_tree.EVALUATION_LABELS and readable:
        p.text(graph_tree.EVALUATION_LABELS[node.gl_evaluation].lower(), (x + 4 * k, y - 18 * k, w - 8 * k, 16 * k), t["textDim"],
               size=11)
    if name in errors:
        p.rect((x - 3 * k, y - 3 * k, w + 6 * k, h + 6 * k), (0, 0, 0, 0), radius + 3 * k, t["error"], 2.0 * k)
        r = 9.0 * k
        p.rect((x + w - r, top - r, 2 * r, 2 * r), t["error"], r)
        if readable:
            p.text("!", (x + w - r, top - r, 2 * r, 2 * r), (1, 1, 1, 1), size=12, align="center", bold=True, clip=False)
    if node.select:
        width = (2.5 if active else 1.5) * k
        p.rect((x - k, y - k, w + 2 * k, h + 2 * k), (0, 0, 0, 0), radius + k, t["selected"], width)


def _draw_toggle(p, t, box, ident, r, viewed):
    """A header toggle in Maya's colors (graph_editor.paint_toggles): idle, the muted text (on a filled
    header its ink, fainter); lit, View, Expand Inputs and Clump Colors in the blue, Bypass in the
    warning color (on a filled header its ink), on a tile of that color."""
    node, k = box.node, box.k
    c = (r[0] + r[2] / 2, r[1] + r[3] / 2)
    if ident == "bypass":
        on = node.mute
    elif ident == "view":
        on = node.gl_viewed
    elif ident == "inputs":
        exposed = set(json.loads(node.gl_inputs or "[]"))
        names = [s["name"] for s in graph_tree.node_param_specs(node) if s.get("port")]
        on = bool(names) and all(n in exposed for n in names)
    elif ident == "tubes":
        scalp = getattr(node.id_data, "gl_scalp", None)
        on = bool(scalp) and graph_tree.braid_tubes_shown(scalp, node)
    else:  # clump
        scalp = getattr(node.id_data, "gl_scalp", None)
        on = bool(scalp) and graph_tree.clump_colored(scalp, node)
    ink = getattr(box, "header_ink", None)
    idle = with_alpha(ink, 150 / 255.0) if ink is not None else t["textDim"]
    active = t["warning"] if ident == "bypass" else t["focus"]
    col = (ink if ink is not None else active) if on else idle
    if on:
        p.rect(r, with_alpha(col, 70 / 255.0), 4 * k)
    width = 1.8 if on else 1.6
    if ident == "bypass":
        rad = r[2] / 2 - 4 * k
        ring = [(c[0] + rad * math.cos(a * math.pi / 12), c[1] + rad * math.sin(a * math.pi / 12)) for a in range(25)]
        p.lines(ring, col, width)
        d = rad * 0.7071
        p.lines([(c[0] - d, c[1] - d), (c[0] + d, c[1] + d)], col, width)
    elif ident == "view":
        _eye(p, c, 6.5 * k, col, width, 2.2 * k)
    elif ident == "inputs":
        for dy in (5.0, 9.0, 13.0):
            yy = r[1] + r[3] - dy * k
            p.lines([(r[0] + 4 * k, yy), (r[0] + r[2] - 4 * k, yy)], col, width)
    elif ident == "tubes":  # a short tube: its far end's half, its two sides, its open near end
        top, bottom = c[1] + 4.0 * k, c[1] - 4.0 * k
        far, near = c[0] - 4.4 * k, c[0] + 4.4 * k
        half = [(far - 2.2 * k * math.sin(a * math.pi / 8), c[1] + 4.0 * k * math.cos(a * math.pi / 8)) for a in range(9)]
        p.lines(half, col, 1.4)
        p.lines([(far, top), (near, top)], col, 1.4)
        p.lines([(far, bottom), (near, bottom)], col, 1.4)
        ring = [(near + 2.2 * k * math.cos(a * math.pi / 8), c[1] + 4.0 * k * math.sin(a * math.pi / 8)) for a in range(17)]
        p.lines(ring, col, 1.4)
    else:
        hues = ((0.87, 0.16, 0.1, 1.0), (0.1, 0.58, 0.19, 1.0), (0.13, 0.3, 0.91, 1.0))
        for (dx, dy), hue in zip(((0.0, 3.4), (-3.6, -2.6), (3.6, -2.6)), hues):
            rr = 2.6 * k
            q = (c[0] + dx * k - rr, c[1] + dy * k - rr, 2 * rr, 2 * rr)
            if on:
                p.rect(q, hue, rr)
            else:
                p.rect(q, (0, 0, 0, 0), rr, col, 1.3)


# ---- The top bar: the groom, and the way back to Blender's nodes ------------------------------------

def _top_bar_rects(region, s):
    h = TOP_BAR * s
    bar = (0, region.height - h, region.width, h)
    button = (region.width - (PANEL_W + 12) * s - 120 * s, region.height - h + 8 * s, 120 * s, h - 16 * s)
    return bar, button


def path_crumbs(p, tree, bar, s):
    """The path bar: the groom's graph, then each compound the editor is inside, as (text, path,
    rect) left to right; a click on one goes to that graph (U goes up one)."""
    scalp = getattr(tree, "gl_scalp", None)
    title = scalp.name if scalp is not None else tree.name
    here = graph_tree.scope_of(scalp) if scalp is not None else ""
    parts = [(title, "")]
    if here:
        names = here.split("/")
        parts += [(n, "/".join(names[:i + 1])) for i, n in enumerate(names)]
    out, x = [], 12 * s
    for i, (text, path) in enumerate(parts):
        w = p.text_size(text, 13) + 10 * s
        out.append((text, path, (x, bar[1] + 6 * s, w, bar[3] - 12 * s)))
        x += w + (p.text_size(" > ", 13) if i + 1 < len(parts) else 0)
    return out


def _draw_top_bar(p, context, region, tree, s):
    ui = uikit._theme()
    bar, button = _top_bar_rects(region, s)
    crumbs = path_crumbs(p, tree, bar, s)
    for i, (text, path, r) in enumerate(crumbs):
        last = i + 1 == len(crumbs)
        p.text(text, r, ui.text if last else ui.textDim, size=ui.font_size + (2 if i == 0 else 1), bold=last)
        if not last:
            p.text(">", (r[0] + r[2], r[1], p.text_size(" > ", 13), r[3]), ui.textMuted, size=ui.font_size, align="center")
    hover = _hover[0] == "classic"
    p.rect(button, ui.hover if hover else ui.raised, ui.radius * s, ui.outline, 1.0 * s)
    p.text("Blender Nodes", button, ui.textDim if not hover else ui.text, align="center")


_hover = [None]


# ---- The params' dock ------------------------------------------------------------------------------

def _actions():
    from . import window
    return {
        "color": window._open_color_picker,
        "pointer": window._pointer_choice,
        "list_scroll": lambda list_id: panel_state.lists.get(list_id, 0),
        "list_area": lambda list_id, rect, overflow: None,
        "preview": window._preview_of,
    }


class Dock(uikit.Window):
    """The selected node's params docked on the canvas' right, full height (the app's Graph Editor
    panel): the sidebar's GroomLab Node panel run through drawn.py, scrolled with the wheel."""

    def __init__(self, context, region):
        state = panel_state
        self.state, self.theme = state, uikit._theme()
        self.scale = s = context.preferences.system.ui_scale
        self.interaction = state.interaction
        self.hover, self.mouse = state.hover, state.mouse
        self.region_w = region.width
        self.rail_h = 0.0
        w = PANEL_W * s
        x = region.width - w
        top = region.height
        self.title_h = TOP_BAR * s
        pad = 12 * s
        layout = uikit.Layout(self, x + pad, top - self.title_h - 6 * s + state.scroll * s, w - 2 * pad)
        content(layout, context)
        content_h = (top - self.title_h - 6 * s + state.scroll * s) - layout.y + pad
        h = region.height
        self.rect = (x, 0, w, h)
        self.body = (x, 0, w, h - self.title_h)
        self.content = (x, 0, w, h - self.title_h)
        self.overflow = max(0.0, (self.title_h + content_h - h) / s)
        self.widgets = [uikit.Label("title", (x + pad, top - self.title_h, w - 2 * pad, self.title_h), "Parameters",
                                    dim=False, bright=True)]
        self.chrome = {"title"}
        c = self.content
        self.widgets += [wd for wd in layout.widgets if wd.rect[1] + wd.rect[3] > c[1] and wd.rect[1] < c[1] + c[3]]

    def draw(self):
        t, s = self.theme, self.scale
        p = uikit.Painter(t, s)
        x, y, w, h = self.rect
        p.rect(self.rect, t.panel)
        p.rect((x, y, max(1.0, s), h), t.outline)
        p.rect((x, y + h - self.title_h, w, max(1.0, s)), t.cardRule)
        import gpu
        cx, cy, cw, ch = self.content
        gpu.state.scissor_test_set(True)
        try:
            for wd in self.widgets:
                if wd.id in self.chrome:
                    gpu.state.scissor_set(int(x), int(y), int(w), int(h))
                else:
                    gpu.state.scissor_set(int(cx), int(cy), int(cw), int(ch))
                p.fade = 0.5 if wd.dim or (wd.disabled and not wd.own_disabled) else 1.0
                wd.draw(p, self)
            p.fade = 1.0
        finally:
            gpu.state.scissor_test_set(False)
        if self.overflow > 0:
            track_h = ch - 12 * s
            frac = ch / (ch + self.overflow * s)
            pos = self.state.scroll / self.overflow if self.overflow else 0.0
            th = max(24 * s, track_h * frac)
            ty = cy + 6 * s + (track_h - th) * (1.0 - pos)
            p.rect((x + w - 6 * s, ty, 4 * s, th), t.scroll, 2 * s)
        if self.interaction is not None:
            self.interaction.draw(p, self)
        elif self.hover and self.state.hover_since and time.time() - self.state.hover_since > uikit.TOOLTIP_DELAY:
            hovered = next((wd for wd in self.widgets if wd.id == self.hover), None)
            text = hovered.tooltip_text() if hovered is not None else ""
            if text:
                uikit.draw_tooltip(p, self, text)


def content(layout, context):
    from . import graph_editor
    rec = drawn.DrawnLayout("column")
    drawn.set_folds(panel_state.folds)
    try:
        drawn.draw_panel(graph_editor.GROOMLAB_PT_graph_node, rec, context)
    except Exception as e:  # noqa: BLE001 - the panel says why rather than leaving the dock empty
        rec.label(text=str(e), icon="ERROR")
    drawn.place(rec, layout, context, _actions())


_dock_cache = {}


def dock(context, region=None, reuse=False):
    region = region or context.region
    key = region.as_pointer()
    if reuse:
        hit = _dock_cache.get(key)
        if hit is not None and time.time() - hit[0] < 0.1 and panel_state.interaction is None:
            return hit[1]
    d = Dock(context, region)
    _dock_cache[key] = (time.time(), d)
    return d


def _draw_panel(context, region):
    dock(context, region).draw()


def over_panel(region, x, s):
    return x >= region.width - PANEL_W * s


# ---- Input ----------------------------------------------------------------------------------------

def _redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "NODE_EDITOR":
                area.tag_redraw()


def _select_only(tree, nodes):
    for n in tree.nodes:
        n.select = n in nodes
    if nodes:
        tree.nodes.active = nodes[-1]


class GROOMLAB_GT_graph_canvas(bpy.types.Gizmo):
    """The whole canvas as one gizmo: a press anywhere on it is the canvas'."""
    bl_idname = "GROOMLAB_GT_graph_canvas"

    def draw(self, context):
        pass

    def test_select(self, context, location):
        if not shows_here(context):
            return -1
        x, y = location
        s = context.preferences.system.ui_scale
        region = context.region
        hover = None
        if over_panel(region, x, s):
            d = dock(context, reuse=True)
            widget = d.widget_at(x, y)
            hover = widget.id if widget is not None else None
            if hover != panel_state.hover:
                panel_state.hover_since = time.time()
            panel_state.hover, panel_state.mouse = hover, (x, y)
        else:
            if panel_state.hover is not None:
                panel_state.hover = None
            bar, button = _top_bar_rects(region, s)
            over = "classic" if uikit.contains(button, x, y) else None
            if over != _hover[0]:
                _hover[0] = over
                region.tag_redraw()
        return 0


class GROOMLAB_GGT_graph_canvas(bpy.types.GizmoGroup):
    bl_idname = "GROOMLAB_GGT_graph_canvas"
    bl_label = "GroomLab Graph"
    bl_space_type = "NODE_EDITOR"
    bl_region_type = "WINDOW"
    bl_options = {"PERSISTENT"}

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    @classmethod
    def setup_keymap(cls, keyconfig):
        km = keyconfig.keymaps.new(name="GroomLab Graph Canvas", space_type="NODE_EDITOR", region_type="WINDOW")
        km.keymap_items.new("gizmogroup.gizmo_tweak", "LEFTMOUSE", "PRESS", any=True)
        km.keymap_items.new("groomlab.graph_canvas_context", "RIGHTMOUSE", "PRESS", any=True)
        km.keymap_items.new("groomlab.graph_canvas_double", "LEFTMOUSE", "DOUBLE_CLICK", any=True)
        for key, delta in (("WHEELUPMOUSE", -1), ("WHEELDOWNMOUSE", 1)):
            km.keymap_items.new("groomlab.graph_canvas_scroll", key, "PRESS", any=True).properties.delta = delta
        return km

    def setup(self, context):
        gz = self.gizmos.new(GROOMLAB_GT_graph_canvas.bl_idname)
        gz.use_tooltip = False
        gz.target_set_operator("groomlab.graph_canvas_press")


class GROOMLAB_OT_graph_canvas_press(bpy.types.Operator):
    """A press on the GroomLab graph: pick and drag nodes, box select, a header toggle, the params"""
    bl_idname = "groomlab.graph_canvas_press"
    bl_label = "GroomLab Graph"
    bl_options = {"INTERNAL"}

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    def invoke(self, context, event):
        region = context.region
        s = context.preferences.system.ui_scale
        x, y = event.mouse_region_x, event.mouse_region_y
        if not (0 <= x < region.width and 0 <= y < region.height):
            return {"PASS_THROUGH"}
        _press.clear()
        if over_panel(region, x, s):
            return self._panel_press(context, event)
        bar, button = _top_bar_rects(region, s)
        if uikit.contains(button, x, y):
            setattr(context.window_manager, CLASSIC_PROP, True)
            _show_header(context, True)
            _redraw()
            return {"FINISHED"}
        if uikit.contains(bar, x, y):
            tree = tree_here(context)
            painter = uikit.Painter(uikit._theme(), s)
            for _text, path, r in path_crumbs(painter, tree, bar, s)[:-1]:
                if uikit.contains(r, x, y):  # a graph holding the one shown: back out to it
                    bpy.ops.groomlab.graph_up("EXEC_DEFAULT", path=path, to_path=True)
                    break
            return {"FINISHED"}
        tree = tree_here(context)
        g = Graph(context, region, tree)
        hit = g.toggle_at(x, y)
        if hit is not None:
            _toggle(context, tree, hit[0].node, hit[1])
            _redraw()
            return {"FINISHED"}
        chevron = g.chevron_at(x, y)
        if chevron is not None:  # a backdrop's fold chevron
            _select_only(tree, [chevron])
            graph_arrange.set_backdrop(context, chevron, collapsed=not bool(chevron.get("gl_collapsed")))
            _push_undo("Fold Backdrop")
            _redraw()
            return {"FINISHED"}
        grip = g.grip_at(x, y)
        if grip is not None:
            _select_only(tree, [grip])
            _press.update(kind="resize", frame=grip, start=(x, y), size=(grip.width, grip.height), k=g.k, moved=False)
            context.window_manager.modal_handler_add(self)
            return {"RUNNING_MODAL"}
        port = g.port_at(x, y)
        if port is not None:
            _start_wire(tree, g, port, x, y)
            if event.shift and port[2] and any(l.from_socket == port[1] for l in tree.links):
                _wire["bundle"] = True  # Shift+drag from an output: all its links to another output
            _press.update(kind="wire", start=(x, y))
            _redraw()
            context.window_manager.modal_handler_add(self)
            return {"RUNNING_MODAL"}
        dot = g.dot_at(x, y)
        if dot is not None:
            d, nub = dot
            if nub and _start_dot_wire(tree, g, d, x, y):  # another link of its signal, out of its nub
                _press.update(kind="wire", start=(x, y))
            else:
                _press.update(kind="dot", dot=(d.portal, d.exit), start=(x, y), last=(x, y), at=(d.x, d.y), k=g.k,
                              moved=False)
            _redraw()
            context.window_manager.modal_handler_add(self)
            return {"RUNNING_MODAL"}
        shift, ctrl, alt = event.shift, event.ctrl, event.alt
        node = g.pin_at(x, y)
        box = g.box_at(x, y) if node is None else None
        if box is not None:
            node = box.node
        if node is None:
            node = g.frame_at(x, y)
        if node is None and alt:
            link = g.wire_at(x, y)
            if link is not None:  # Alt+click a link: a pin there
                add_pin(tree, g, link, x, y)
                _redraw()
                return {"FINISHED"}
        if node is not None:
            # Shift+click adds to the selection and Ctrl+click toggles (on release, without a move);
            # Shift+drag carries what feeds it, Ctrl+drag what it feeds, Alt+drag a copy.
            if not node.select and not (shift or ctrl):
                _select_only(tree, [node])
            elif node.select:
                tree.nodes.active = node
            _press.update(kind="move", start=(x, y), last=(x, y), node=node, moved=False, shift=shift, ctrl=ctrl,
                          alt=alt, nodes=None, dots=[], k=g.k, shake=graph_arrange.ShakeDetector(), shaken=False,
                          left=set(), time=time.time())
        else:
            _press.update(kind="band", start=(x, y), shift=shift, before={n for n in tree.nodes if n.select})
            _band[0] = (x, y, x, y)
        _redraw()
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

    def _panel_press(self, context, event):
        d = dock(context)
        widget = d.widget_at(event.mouse_region_x, event.mouse_region_y)
        if widget is None or widget.disabled:
            return {"FINISHED"}
        panel_state.hover_since = time.time() + 3600.0
        del uikit.changed[:]
        interaction = widget.press(d, context, event)
        _redraw()
        if interaction is None:
            _push_panel_undo()
            return {"FINISHED"}
        panel_state.interaction = interaction
        _press.update(kind="panel")
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

    def modal(self, context, event):
        kind = _press.get("kind")
        if kind == "panel":
            return self._panel_modal(context, event)
        tree = tree_here(context)
        if tree is None:
            _band[0] = None
            return {"CANCELLED"}
        x, y = event.mouse_region_x, event.mouse_region_y
        if kind == "wire":
            return self._wire_modal(context, event, tree, x, y)
        if kind == "resize":
            return self._resize_modal(context, event, x, y)
        if event.type == "MOUSEMOVE":
            if kind == "dot":
                sx, sy = _press["start"]
                if not _press["moved"] and math.hypot(x - sx, y - sy) < DRAG_START:
                    return {"RUNNING_MODAL"}
                k = _press["k"]
                portal, exit_index = _press["dot"]
                ax, ay = _press["at"]
                _move_dots(tree, [(portal, exit_index, ax + (x - sx) / k, ay - (y - sy) / k)])
                _press.update(last=(x, y), moved=True)
            elif kind == "move":
                sx, sy = _press["start"]
                if not _press["moved"] and math.hypot(x - sx, y - sy) < DRAG_START:
                    return {"RUNNING_MODAL"}
                if _press["nodes"] is None:
                    try:
                        _begin_move(context, tree)
                    except HelperError as e:
                        self.report({"ERROR"}, str(e))
                        _press["nodes"] = _carried(tree)
                lx, ly = _press["last"]
                k = _press["k"]
                dx, dy = (x - lx) / k, (y - ly) / k
                for n in _press["nodes"]:
                    gx, gy = graph_tree.get_location(n)
                    graph_tree.set_location(n, (gx + dx, gy + dy))
                if _press["dots"]:
                    _press["dots"] = [(pp, e, dx0 + dx, dy0 - dy) for pp, e, dx0, dy0 in _press["dots"]]
                    _move_dots(tree, _press["dots"])
                now = time.time()
                speed = math.hypot(x - lx, y - ly) / max(now - _press["time"], 1e-3)
                _press.update(last=(x, y), moved=True, time=now)
                _hold_on_backdrops(tree, _press, speed)
                if not _press["shaken"] and _press["shake"].add(now, event.mouse_x, event.mouse_y):
                    _press["shaken"] = True
                    _shake_loose(context, tree)
            elif kind == "band":
                sx, sy = _press["start"]
                _band[0] = (sx, sy, x, y)
                g = Graph(context, context.region, tree)
                r = (min(sx, x), min(sy, y), abs(x - sx), abs(y - sy))
                inside = {b.node for b in g.boxes if _overlap(b.rect, r)}
                inside |= {n for n, (px, py) in g.pins if uikit.contains(r, px, py)}
                keep = _press["before"] if _press["shift"] else set()
                for n in tree.nodes:
                    n.select = n in inside or n in keep
            _redraw()
            return {"RUNNING_MODAL"}
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            if kind == "dot":
                if _press["moved"]:
                    portal, exit_index = _press["dot"]
                    d = next((dd for dd in graph_arrange.dots(tree) if (dd.portal, dd.exit) == (portal, exit_index)), None)
                    if d is not None:
                        try:
                            graph_tree.move_portal(tree.gl_scalp, portal, exit_index, (d.x, d.y))
                        except HelperError as e:
                            self.report({"ERROR"}, str(e))
                        _push_undo("Move Dot")
            elif kind == "move":
                node = _press["node"]
                if _press["moved"]:
                    _end_move(context, tree, x, y)
                elif _press["shift"]:
                    node.select = True  # Shift+click: added to the selection
                    tree.nodes.active = node
                elif _press["ctrl"]:
                    node.select = not node.select  # Ctrl+click: toggled
                    if node.select:
                        tree.nodes.active = node
                else:
                    _select_only(tree, [node])  # a click on one of several picked: that one alone
            elif kind == "band":
                sx, sy = _press["start"]
                if math.hypot(x - sx, y - sy) < DRAG_START and not _press["shift"]:
                    _select_only(tree, [])  # a click on the canvas: nothing picked
            _band[0] = None
            _press.clear()
            _redraw()
            return {"FINISHED"}
        if event.type in ("RIGHTMOUSE", "ESC") and event.value == "PRESS":
            if kind == "move" and _press["moved"] and _press["nodes"] is not None:
                k = _press["k"]
                sx, sy = _press["start"]
                lx, ly = _press["last"]
                for n in _press["nodes"]:
                    gx, gy = graph_tree.get_location(n)
                    graph_tree.set_location(n, (gx - (lx - sx) / k, gy - (ly - sy) / k))
                if _press["dots"]:
                    _move_dots(tree, [(pp, e, dx0 - (lx - sx) / k, dy0 + (ly - sy) / k) for pp, e, dx0, dy0 in _press["dots"]])
            elif kind == "dot" and _press["moved"]:
                portal, exit_index = _press["dot"]
                _move_dots(tree, [(portal, exit_index, _press["at"][0], _press["at"][1])])
            elif kind == "band":
                for n in tree.nodes:
                    n.select = n in _press["before"]
            _band[0] = None
            _press.clear()
            _redraw()
            return {"CANCELLED"}
        return {"RUNNING_MODAL"}

    def _panel_modal(self, context, event):
        return _feed_panel(context, event)

    def _resize_modal(self, context, event, x, y):
        frame, (w0, h0), k = _press["frame"], _press["size"], _press["k"]
        sx, sy = _press["start"]
        if event.type == "MOUSEMOVE":
            frame.shrink = False
            frame.width = max(80.0, w0 + (x - sx) / k)
            frame.height = max(60.0, h0 - (y - sy) / k)  # (dragged down: taller)
            _press["moved"] = True
            _redraw()
            return {"RUNNING_MODAL"}
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            if _press["moved"]:
                _push_undo("Resize Backdrop")
            _press.clear()
            return {"FINISHED"}
        if event.type in ("RIGHTMOUSE", "ESC") and event.value == "PRESS":
            frame.width, frame.height = w0, h0
            _press.clear()
            _redraw()
            return {"CANCELLED"}
        return {"RUNNING_MODAL"}

    def _wire_modal(self, context, event, tree, x, y):
        g = Graph(context, context.region, tree)
        if event.type == "MOUSEMOVE":
            snap = _wire_target(g, x, y)
            _wire["to"] = snap[3] if snap is not None and snap[3] is not None else (x, y)
            _redraw()
            return {"RUNNING_MODAL"}
        if event.type == "LEFTMOUSE" and event.value == "RELEASE":
            sx, sy = _press["start"]
            dropped = _drop_wire(context, tree, g, x, y, math.hypot(x - sx, y - sy) >= DRAG_START)
            _wire.clear()
            _press.clear()
            _redraw()
            if dropped == "search":
                bpy.ops.groomlab.graph_canvas_search("INVOKE_DEFAULT")
            return {"FINISHED"}
        if event.type in ("RIGHTMOUSE", "ESC") and event.value == "PRESS":
            _wire.clear()
            _press.clear()
            _redraw()
            return {"CANCELLED"}
        return {"RUNNING_MODAL"}

    def cancel(self, context):
        _band[0] = None
        _wire.clear()
        panel_state.interaction = None
        _press.clear()


def _feed_panel(context, event):
    """An event to the params' widget or the drawn menu open (panel_state.interaction)."""
    interaction = panel_state.interaction
    if interaction is None:
        _press.clear()
        return {"CANCELLED"}
    d = dock(context)
    panel_state.mouse = (event.mouse_region_x, event.mouse_region_y)
    result = interaction.event(d, context, event)
    if d.interaction is not interaction:
        panel_state.interaction = d.interaction
        result = "RUNNING"
    if result != "RUNNING":
        panel_state.interaction = None
        _press.clear()
        if result.startswith("FINISHED"):
            _push_panel_undo()
        else:
            del uikit.changed[:]
    _redraw()
    from . import window
    return window._RESULTS[result]


# ---- Wires -----------------------------------------------------------------------------------------

def _start_wire(tree, g, port, x, y):
    """A press on a port: an output starts a wire; an input's wire is picked up (from its output);
    a free input starts a wire back toward an output."""
    box, socket, out, point = port
    _wire.clear()
    if out:
        _wire.update({"from": socket, "out": True, "at": point, "to": (x, y), "picked": None})
        return
    linked = [l for l in tree.links if l.to_socket == socket]
    if linked:
        link = linked[0]
        at = g.socket_point(link.from_node, link.from_socket, True) or point
        _wire.update({"from": link.from_socket, "out": True, "at": at, "to": (x, y), "picked": link})
    else:
        _wire.update({"from": socket, "out": False, "at": point, "to": (x, y), "picked": None})


def _wire_target(g, x, y):
    """The port the dragged wire would go to at (x, y): the nearest of the other side's, a type
    it can take, not on its own node."""
    port = g.port_at(x, y, outputs=not _wire["out"], inputs=_wire["out"])
    if port is None or port[0].node == _wire["from"].node:
        return None
    return port if compatible(_port_type(_wire["from"]), _port_type(port[1]), _wire["out"]) else None


def _connect(tree, out_socket, in_socket):
    tree.links.new(out_socket, in_socket)  # (an input takes one wire: the one there goes)


def _start_dot_wire(tree, g, d, x, y):
    """A press on a dot's nub: a wire of the portal's signal (its source output), dropped on an input
    it is drawn out of this dot. False for a dot nothing feeds."""
    node = graph_tree.node_named(tree, d.source[0]) if d.source else None
    if node is None:
        return False
    out = next((o for o in node.outputs if o.identifier == d.source[1]), None) if d.source[1] else \
        (node.outputs[0] if len(node.outputs) else None)
    if out is None:
        return False
    _wire.clear()
    _wire.update({"from": out, "out": True, "at": g.dot_point(d, nub=True), "to": (x, y), "picked": None,
                  "portal": (d.portal, d.exit)})
    return True


def _source_name(socket):
    """(node, output) the graph names a GroomLab output by ("" its first)."""
    node = socket.node
    return node.gl_name, "" if len(node.outputs) and node.outputs[0] == socket else socket.identifier


def _drop_wire(context, tree, g, x, y, moved):
    """The dragged wire let go: onto a port (or a card: its first free port it can take), wired;
    a picked-up wire dropped on the canvas goes; a new one opens the search (returns "search")."""
    picked = _wire.get("picked")
    source, out = _wire["from"], _wire["out"]
    scalp = getattr(tree, "gl_scalp", None)
    if _wire.get("bundle"):
        # Shift+drag from an output: every link it feeds now read from the output it is dropped on.
        port = g.port_at(x, y, outputs=True, inputs=False)
        if port is not None and port[1] != source and graph_tree.ours(port[0].node):
            src, output = _source_name(source)
            new_src, new_output = _source_name(port[1])
            try:
                graph_tree.move_output_links(scalp, src, output, new_src, new_output)
            except HelperError as e:
                print("GroomLab:", e)
            _push_undo("Move Links")
        return None
    if _wire.get("portal") and out:
        # Out of a dot's nub onto an input: the link drawn out of that dot.
        port = _wire_target(g, x, y)
        if port is not None and graph_tree.ours(port[0].node):
            portal, exit_index = _wire["portal"]
            try:
                graph_tree.add_portal_link(scalp, portal, exit_index, port[0].node.gl_name, port[1].identifier)
            except HelperError as e:
                print("GroomLab:", e)
            _push_undo("Link from Portal")
        return None
    if out and moved and picked is None and graph_tree.ours(source.node):
        dot = g.dot_at(x, y)
        if dot is not None and dot[0].source is None and dot[0].exit is None:
            # A wire from an output dropped on a free dot: the dot fed from it.
            src, output = _source_name(source)
            try:
                graph_tree.feed_dot(scalp, dot[0].portal, src, output)
            except HelperError as e:
                print("GroomLab:", e)
            _push_undo("Feed Dot")
            return None
    target = _wire_target(g, x, y)
    if target is None:
        box = g.box_at(x, y)
        if box is not None and box.node != source.node:
            ports = box.inputs if out else box.outputs
            free = [s for s, _p in ports if compatible(_port_type(source), _port_type(s), out)
                    and not (out and s.is_linked)]
            if free:
                target = (box, free[0], not out, None)
    if target is not None:
        socket = target[1]
        if picked is not None and picked.to_socket == socket:
            return None
        if picked is not None:
            tree.links.remove(picked)
        if out:
            _connect(tree, source, socket)
        else:
            _connect(tree, socket, source)
        _push_undo("Connect")
        return None
    if not moved:
        return None
    if picked is not None:
        tree.links.remove(picked)
        _push_undo("Disconnect")
        return None
    _search.clear()
    _search.update({"at": (x, y), "link": source, "out": out, "pending": True})
    return "search"


def _move_dots(tree, moves):
    """Dots drawn where a drag has them ([(portal, exit, x, y)], graph units, y down): the tree's copy
    of the portals (the graph takes them on release)."""
    portals = graph_tree.portals_of(tree)
    for portal, exit_index, nx, ny in moves:
        for p in portals:
            if p.get("name") != portal:
                continue
            entry = p if exit_index is None else (p.get("exits") or [None] * (exit_index + 1))[exit_index]
            if isinstance(entry, dict):
                entry["x"], entry["y"] = float(nx), float(ny)
    with graph_tree.building():
        tree["gl_portals"] = json.dumps(portals)


def _begin_move(context, tree):
    """A drag's first move: what it carries. Shift: the nodes and what feeds them; Ctrl: and what they
    feed (never through a portal); Alt: copies of them, which move; else the selection (a backdrop's
    nodes with it). The pins and dots between moved nodes go too (graph_arrange.h carriedWith)."""
    node = _press["node"]
    scalp = tree.gl_scalp
    if not graph_tree.ours(node):
        if not node.select:
            _select_only(tree, [node])
        _press["nodes"] = _carried(tree)
        if node.bl_idname == "NodeFrame":  # the dots on the backdrop go with it
            b = graph_tree.backdrop_of(node)
            on = graph_tree.backdrop_layout(scalp, b)
            here = {(dd.portal, dd.exit): dd for dd in graph_arrange.dots(tree)}
            _press["dots"] = []
            for d in on.get("dots") or []:
                key = (d["portal"], None if d["exit"] < 0 else d["exit"])
                if key in here:
                    _press["dots"].append((key[0], key[1], here[key].x, here[key].y))
        return
    names = [n.gl_name for n in tree.nodes if graph_tree.ours(n) and n.select and n.gl_name]
    if node.gl_name not in names:
        names = [node.gl_name]
        _select_only(tree, [node])
    if _press["alt"]:
        made = graph_tree.duplicate_nodes(scalp, names, (0.0, 0.0))
        copies = [graph_tree.node_named(tree, m) for m in made]
        copies = [c for c in copies if c is not None]
        _select_only(tree, copies)
        _press["nodes"] = copies
        return
    mode = "up" if _press["shift"] else ("down" if _press["ctrl"] else "")
    carry = graph_tree.carried(scalp, names, mode)
    moving = {graph_tree.node_named(tree, n) for n in carry.get("nodes") or []}
    pins = set(carry.get("pins") or [])
    moving |= {r for r in graph_tree.reroutes(tree) if graph_tree.pin_of(r) in pins}
    if not mode:
        moving |= set(_carried(tree))
    moving.discard(None)
    _press["nodes"] = [n for n in tree.nodes if n in moving]
    here = {(dd.portal, dd.exit): dd for dd in graph_arrange.dots(tree)}
    _press["dots"] = []
    for d in carry.get("dots") or []:
        key = (d["portal"], None if d["exit"] < 0 else d["exit"])
        if key in here:
            _press["dots"].append((key[0], key[1], here[key].x, here[key].y))


def _hold_on_backdrops(tree, press, speed):
    """A node dragged on its backdrop: dragged slowly past an edge the backdrop grows round it; out
    fast it leaves (and stays out for the drag): graph_arrange.h backdropKeeps, grownBackdrop."""
    moving = set(press["nodes"] or [])
    for n in press["nodes"] or []:
        frame = n.parent
        if frame is None or frame in moving or not graph_tree.ours(n) or frame.get("gl_collapsed"):
            continue
        fx, fy = graph_tree.get_location(frame)
        fw, fh = graph_tree.frame_size(frame)
        box = graph_arrange.card_box(n)
        cx, cy = box["x"] + box["w"] * 0.5, box["y"] + box["h"] * 0.5
        over = fx <= cx <= fx + fw and -fy <= cy <= -fy + fh
        inside = fx <= box["x"] and box["x"] + box["w"] <= fx + fw and -fy <= box["y"] and box["y"] + box["h"] <= -fy + fh
        if inside:
            continue
        if graph_arrange.backdrop_keeps(over, speed, n.name in press["left"]):
            x, y, w, h = graph_arrange.grown_backdrop((fx, -fy, fw, fh), [box], 24.0, header_of(frame))
            at = graph_tree.get_location(n)
            frame.shrink = False
            graph_tree.set_location(frame, (x, -y))
            frame.width, frame.height = w, h
            graph_tree.set_location(n, at)
        else:
            press["left"].add(n.name)
            at = graph_tree.get_location(n)
            n.parent = None
            graph_tree.set_location(n, at)


def _shake_loose(context, tree):
    """A shake while dragging: the dragged nodes out of their links (several: their own links kept)."""
    names = [n.gl_name for n in _press["nodes"] or [] if graph_tree.ours(n) and n.gl_name]
    if not names:
        return
    at = {n.name: graph_tree.get_location(n) for n in _press["nodes"]}
    try:
        graph_tree.detach_group(tree.gl_scalp, names)
    except HelperError as e:
        print("GroomLab:", e)
        return
    for n in _press["nodes"]:  # (the tree made again from the graph: where the drag has them)
        try:
            if n.name in at:
                graph_tree.set_location(n, at[n.name])
        except ReferenceError:
            pass


def _end_move(context, tree, x, y):
    """A drag let go: nodes dropped on a wire go into it (a chain as one: the core's
    insertNodesOnLink); dots carried go into the graph with the nodes (one step)."""
    scalp = tree.gl_scalp
    group = [n for n in _press["nodes"] or [] if graph_tree.ours(n) and n.gl_name]
    names = {n.gl_name for n in group}
    if group and not _press["shift"] and not _press["ctrl"] and not _press["shaken"]:
        g = Graph(context, context.region, tree)
        outside = [l for l in tree.links if (l.from_node in group) != (l.to_node in group)]
        link = g.wire_at(x, y) if not outside else None
        if link is not None and link.from_node not in group and link.to_node not in group \
                and graph_tree.ours(link.from_node) and graph_tree.ours(link.to_node):
            src, output = _source_name(link.from_socket)
            try:
                graph_tree.insert_nodes_on_link(scalp, src, link.to_node.gl_name, link.to_socket.identifier,
                                                sorted(names), output)
                _push_undo("Insert Nodes")
                return
            except HelperError as e:
                print("GroomLab:", e)
    if _press["dots"]:
        try:
            graph_tree.place(scalp, dots=[{"portal": pp, "exit": e, "x": dx, "y": dy} for pp, e, dx, dy in _press["dots"]])
        except HelperError as e:
            print("GroomLab:", e)
    _push_undo("Move Nodes")


def _links_crossing(g, path):
    """The tree's wires the cut's path crosses."""
    def crosses(a, b, c, d):
        def side(p, q, r):
            return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
        s1, s2, s3, s4 = side(a, b, c), side(a, b, d), side(c, d, a), side(c, d, b)
        if s1 == s2 == 0:  # (along each other: not a crossing)
            return False
        return s1 * s2 <= 0 and s3 * s4 <= 0  # (touching a wire's sample point counts)
    hits = []
    for link in g.tree.links:
        a = g.socket_point(link.from_node, link.from_socket, True)
        b = g.socket_point(link.to_node, link.to_socket, False)
        if a is None or b is None:
            continue
        wire = _wire_points(a, b, g.k)
        if any(crosses(p, q, c, d) for p, q in zip(path, path[1:]) for c, d in zip(wire, wire[1:])):
            hits.append(link)
    return hits


# ---- The node search (Tab, a wire dropped on the canvas) ---------------------------------------------

SEARCH_W, SEARCH_ROW, SEARCH_FIELD, SEARCH_ROWS = 300.0, 24.0, 32.0, 12


def _node_items(context):
    """(label, category, node type, settings) of every node the Add menu offers."""
    import nodeitems_utils
    out = []
    for cat in nodeitems_utils.node_categories_iter(context):
        for item in cat.items(context):
            if isinstance(item, nodeitems_utils.NodeItem):
                out.append((item.label, cat.name, item.nodetype, dict(item.settings or {})))
    # Layout (every host's search): a Dot (a free one; fed from a dropped wire's output), a Backdrop,
    # and an exit of each portal.
    out.append(("Dot", "Layout", DOT_ITEM, {}))
    out.append(("Backdrop", "Layout", BACKDROP_ITEM, {}))
    tree = tree_here(context)
    for p in graph_tree.portals_of(tree):
        if p.get("from"):
            out.append(("Portal: %s" % p["name"], "Portals", PORTAL_ITEM + p["name"], {}))
    return out


DOT_ITEM, BACKDROP_ITEM, PORTAL_ITEM = "__dot__", "__backdrop__", "__portal__:"


def _matches(items, text):
    words = text.lower().split()
    found = [it for it in items if all(w in (it[0] + " " + it[1]).lower() for w in words)]
    first = text.lower().strip()
    return sorted(found, key=lambda it: (not it[0].lower().startswith(first), it[0].lower()))


def _search_rects(region, s):
    ax, ay = _search["at"]
    shown = _search.get("shown", [])[:SEARCH_ROWS]
    h = SEARCH_FIELD * s + max(1, len(shown)) * SEARCH_ROW * s + 12 * s
    w = SEARCH_W * s
    x = min(max(4 * s, ax - 20 * s), region.width - w - 4 * s)
    top = min(region.height - 4 * s, max(ay + 16 * s, h + 4 * s))
    box = (x, top - h, w, h)
    field = (x + 6 * s, top - 6 * s - (SEARCH_FIELD - 8) * s, w - 12 * s, (SEARCH_FIELD - 8) * s)
    rows = [(x + 4 * s, field[1] - 6 * s - (i + 1) * SEARCH_ROW * s, w - 8 * s, SEARCH_ROW * s) for i in range(len(shown))]
    return box, field, rows


def _draw_search(p, region, s):
    ui = uikit._theme()
    box, field, rows = _search_rects(region, s)
    uikit.draw_popup(p, box)
    p.rect(field, ui.sunken, ui.radius * s, ui.accent, 1.0 * s)
    text = _search.get("text", "")
    p.text(text + "|" if text else "Search nodes...", uikit.inset(field, 8 * s, 0), ui.text if text else ui.textMuted)
    shown = _search.get("shown", [])[:SEARCH_ROWS]
    if not shown:
        p.text("No node by that name", (box[0] + 12 * s, box[1] + 6 * s, box[2] - 24 * s, SEARCH_ROW * s), ui.textMuted)
    for i, (r, item) in enumerate(zip(rows, shown)):
        if i == _search.get("hover", 0):
            p.rect(r, ui.hover, (ui.radius - 2) * s)
        p.text(item[0], (r[0] + 8 * s, r[1], r[2] * 0.6, r[3]), ui.text)
        p.text(item[1], (r[0] + r[2] * 0.55, r[1], r[2] * 0.45 - 8 * s, r[3]), ui.textMuted, size=ui.font_size - 1,
               align="right")


def _add_node(context, item, at):
    """The node type `item` added with its header's left part under `at` (region pixels), wired to
    the search's wire if it came from one; one undo step."""
    tree = tree_here(context)
    _label, _cat, idname, settings = item
    if idname in (DOT_ITEM, BACKDROP_ITEM) or idname.startswith(PORTAL_ITEM):
        g = Graph(context, context.region, tree)
        gx, gy = g.to_graph(*at)
        scalp = tree.gl_scalp
        source = _search.get("link")
        try:
            if idname == DOT_ITEM:
                src = output = None
                if source is not None and _search.get("out") and graph_tree.ours(source.node):
                    src, output = _source_name(source)
                graph_tree.add_dot(scalp, (gx, -gy), src, output)
                _push_undo("Add Dot")
            elif idname == BACKDROP_ITEM:
                bpy.ops.groomlab.graph_backdrop_add("INVOKE_DEFAULT")
            else:
                graph_tree.add_portal_exit(scalp, idname[len(PORTAL_ITEM):], (gx, -gy))
                _push_undo("Add Portal Exit")
        except HelperError as e:
            print("GroomLab:", e)
        return None
    before = set(tree.nodes)
    bpy.ops.node.add_node("EXEC_DEFAULT", type=idname, use_transform=False,
                          settings=[{"name": k, "value": v} for k, v in settings.items()])
    made = [n for n in tree.nodes if n not in before]
    if not made:
        return None
    node = made[0]
    g = Graph(context, context.region, tree)
    gx, gy = g.to_graph(*at)
    graph_tree.set_location(node, (gx - 30.0, gy + HEADER * 0.5))
    _select_only(tree, [node])
    source = _search.get("link")
    if source is not None:
        try:
            if _search.get("out"):
                into = [s for s in node.inputs if s.enabled and compatible(_port_type(source), _port_type(s), True)]
                if into:
                    _connect(tree, source, into[0])
            else:
                outs = [s for s in node.outputs if s.enabled and compatible(_port_type(source), _port_type(s), False)]
                if outs:
                    _connect(tree, outs[0], source)
        except ReferenceError:  # (the wire's node went meanwhile)
            pass
    _push_undo("Add Node")
    return node


class GROOMLAB_OT_graph_canvas_search(bpy.types.Operator):
    """Find a node type and add it at the pointer (Tab, as in the app's and Maya's Graph Editor)"""
    bl_idname = "groomlab.graph_canvas_search"
    bl_label = "Search Nodes"
    bl_options = {"INTERNAL"}

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    def invoke(self, context, event):
        if not _search.get("pending"):
            _search.clear()
            _search["at"] = (event.mouse_region_x, event.mouse_region_y)
        _search.update({"pending": False, "text": "", "hover": 0, "items": _node_items(context)})
        _search["shown"] = _matches(_search["items"], "")
        context.window_manager.modal_handler_add(self)
        _redraw()
        return {"RUNNING_MODAL"}

    def modal(self, context, event):
        if not _search:
            return {"CANCELLED"}
        s = context.preferences.system.ui_scale
        box, field, rows = _search_rects(context.region, s)
        shown = _search["shown"][:SEARCH_ROWS]
        x, y = event.mouse_region_x, event.mouse_region_y

        def close(result):
            _search.clear()
            _redraw()
            return result

        if event.value == "PRESS" and event.type in ("RET", "NUMPAD_ENTER"):
            if shown:
                _add_node(context, shown[min(_search["hover"], len(shown) - 1)], _search["at"])
            return close({"FINISHED"})
        if event.value == "PRESS" and event.type == "ESC":
            return close({"CANCELLED"})
        if event.value == "PRESS" and event.type in ("UP_ARROW", "DOWN_ARROW"):
            step = -1 if event.type == "UP_ARROW" else 1
            _search["hover"] = min(max(_search["hover"] + step, 0), max(len(shown) - 1, 0))
        elif event.value == "PRESS" and event.type == "BACK_SPACE":
            _search["text"] = _search["text"][:-1]
            _search["shown"], _search["hover"] = _matches(_search["items"], _search["text"]), 0
        elif event.type == "MOUSEMOVE":
            for i, r in enumerate(rows):
                if uikit.contains(r, x, y):
                    _search["hover"] = i
        elif event.type == "LEFTMOUSE" and event.value == "PRESS":
            for i, r in enumerate(rows):
                if uikit.contains(r, x, y):
                    _add_node(context, shown[i], _search["at"])
                    return close({"FINISHED"})
            if not uikit.contains(box, x, y):
                return close({"CANCELLED"})
        elif event.type == "RIGHTMOUSE" and event.value == "PRESS":
            return close({"CANCELLED"})
        elif event.value == "PRESS" and event.unicode and event.unicode.isprintable() and not event.ctrl:
            _search["text"] += event.unicode
            _search["shown"], _search["hover"] = _matches(_search["items"], _search["text"]), 0
        _redraw()
        return {"RUNNING_MODAL"}

    def cancel(self, context):
        _search.clear()


# ---- The right-click: menus, and the cut (Ctrl) -----------------------------------------------------

class GROOMLAB_MT_graph_canvas_node(bpy.types.Menu):
    """A node's (or a backdrop's) menu on the canvas"""
    bl_label = "Node"

    def draw(self, context):
        from . import graph_editor
        tree = tree_here(context)
        active = tree.nodes.active if tree is not None else None
        if active is not None and active.bl_idname == "NodeFrame" and active.select:
            graph_editor.draw_backdrop_menu(self.layout, context, active)
        else:
            graph_editor.draw_node_actions(self.layout, context)


class GROOMLAB_MT_graph_canvas(bpy.types.Menu):
    """The canvas' menu: add a node, the graph's Edit and Presets menus"""
    bl_label = "Graph"

    def draw(self, context):
        layout = self.layout
        layout.menu("NODE_MT_add", text="Add Node")
        layout.operator(GROOMLAB_OT_graph_canvas_search.bl_idname, text="Search Nodes...")
        layout.separator()
        layout.menu("GROOMLAB_MT_graph_edit", text="Edit")
        layout.menu("GROOMLAB_MT_graph_presets", text="Presets")


def _menu_here(tree, g, x, y):
    """The menu of what is under a right-click besides nodes and backdrops (graph_ui.json): a portal
    dot's, a port's (an input's Pass Through as Output; a pass-through output's Rename and Move), a
    link's (Teleport, a pin, Delete). Its class name, what it acts on in graph_arrange.target; None."""
    graph_arrange.target.clear()
    dot = g.dot_at(x, y)
    if dot is not None:
        graph_arrange.target["dot"] = {"portal": dot[0].portal, "exit": dot[0].exit}
        return graph_arrange.GROOMLAB_MT_graph_canvas_portal.__name__
    port = g.port_at(x, y)
    if port is not None and graph_tree.ours(port[0].node):
        node, socket, out = port[0].node, port[1], port[2]
        passes = socket.identifier[len("thru_"):] if out and socket.identifier.startswith("thru_") else ""
        if out and not passes:
            return None
        _select_only(tree, [node])
        graph_arrange.target["port"] = {"node": node.gl_name, "name": socket.identifier, "passes": passes}
        return graph_arrange.GROOMLAB_MT_graph_canvas_port.__name__
    if g.box_at(x, y) is not None or g.frame_at(x, y) is not None or g.pin_at(x, y) is not None:
        return None
    link = g.wire_at(x, y)
    if link is not None and graph_tree.ours(link.to_node):
        a = g.socket_point(link.from_node, link.from_socket, True)
        b = g.socket_point(link.to_node, link.to_socket, False)
        ax, ay = g.to_graph(*a)
        bx, by = g.to_graph(*b)
        graph_arrange.target["link"] = {"to": link.to_node.gl_name, "input": link.to_socket.identifier,
                                        "exit_at": (bx - 60.0, -by), "entry_at": (ax + 40.0, -ay),
                                        "region_at": (x, y), "tree_link": link}
        return graph_arrange.GROOMLAB_MT_graph_canvas_link.__name__
    return None


class GROOMLAB_OT_graph_canvas_context(bpy.types.Operator):
    """A right press on the GroomLab graph: the node's menu, the canvas' menu, or (Ctrl) a cut"""
    bl_idname = "groomlab.graph_canvas_context"
    bl_label = "GroomLab Graph Menu"
    bl_options = {"INTERNAL"}

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    def invoke(self, context, event):
        region = context.region
        s = context.preferences.system.ui_scale
        x, y = event.mouse_region_x, event.mouse_region_y
        if over_panel(region, x, s):  # a param's row: its menu (Expose as Input, keys, Paint...)
            d = dock(context)
            widget = d.widget_at(x, y)
            open_menu = getattr(widget, "context_menu", None)
            if widget is None or widget.disabled or open_menu is None:
                return {"FINISHED"}
            interaction = open_menu(context, d, event)
            if interaction is None:
                _redraw()
                return {"FINISHED"}
            panel_state.interaction = interaction
            _press.clear()
            _press.update(kind="menu")
            context.window_manager.modal_handler_add(self)
            _redraw()
            return {"RUNNING_MODAL"}
        tree = tree_here(context)
        _press.clear()
        if event.ctrl:
            _cut[:] = [(x, y)]
            _press.update(kind="cut")
            context.window_manager.modal_handler_add(self)
            return {"RUNNING_MODAL"}
        g = Graph(context, region, tree)
        name = _menu_here(tree, g, x, y)
        if name is None:
            node = g.pin_at(x, y)
            if node is None:
                box = g.box_at(x, y)
                node = box.node if box is not None else g.frame_at(x, y)
            if node is not None:
                if not node.select:
                    _select_only(tree, [node])
                tree.nodes.active = node
                name = GROOMLAB_MT_graph_canvas_node.__name__
            else:
                name = GROOMLAB_MT_graph_canvas.__name__
        entries = drawn.menu_entries(name, context)
        if not entries:
            bpy.ops.wm.call_menu(name=name)
            return {"FINISHED"}
        from . import editors
        panel_state.interaction = editors.PopupMenu(dock(context), entries, (x, y, 0, 0), armed=True)
        _press.update(kind="menu")
        context.window_manager.modal_handler_add(self)
        _redraw()
        return {"RUNNING_MODAL"}

    def modal(self, context, event):
        if _press.get("kind") == "menu":
            return _feed_panel(context, event)
        x, y = event.mouse_region_x, event.mouse_region_y
        if event.type == "MOUSEMOVE":
            _cut.append((x, y))
            _redraw()
            return {"RUNNING_MODAL"}
        if event.type == "RIGHTMOUSE" and event.value == "RELEASE":
            tree = tree_here(context)
            if tree is not None and len(_cut) > 1:
                hits = _links_crossing(Graph(context, context.region, tree), _cut)
                for link in hits:
                    tree.links.remove(link)
                if hits:
                    _push_undo("Cut Wires")
            _cut.clear()
            _press.clear()
            _redraw()
            return {"FINISHED"}
        if event.type == "ESC" and event.value == "PRESS":
            _cut.clear()
            _press.clear()
            _redraw()
            return {"CANCELLED"}
        return {"RUNNING_MODAL"}

    def cancel(self, context):
        _cut.clear()
        _press.clear()
        panel_state.interaction = None


class GROOMLAB_OT_graph_canvas_double(bpy.types.Operator):
    """A double-click on the GroomLab graph: open a Compound or a Script node, rename a node or a
    backdrop, add a pin on a wire"""
    bl_idname = "groomlab.graph_canvas_double"
    bl_label = "GroomLab Graph Double-Click"
    bl_options = {"INTERNAL"}

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    def invoke(self, context, event):
        region = context.region
        s = context.preferences.system.ui_scale
        x, y = event.mouse_region_x, event.mouse_region_y
        if over_panel(region, x, s) or event.alt:
            return {"PASS_THROUGH"}
        double_click(context, Graph(context, region, tree_here(context)), x, y)
        _redraw()
        return {"FINISHED"}


def double_click(context, g, x, y):
    """What a double-click at (x, y) does (the app's): "open" a Compound (its nodes back in the
    graph) or a Script node's editor, "rename" a node (its header) or a backdrop (its title), "pin"
    a wire; None: nothing there."""
    tree = g.tree
    if g.toggle_at(x, y) is not None or g.port_at(x, y) is not None:
        return None
    dot = g.dot_at(x, y)
    if dot is not None:
        bpy.ops.groomlab.graph_portal_rename("INVOKE_DEFAULT", portal=dot[0].portal)
        return "rename"
    box = g.box_at(x, y)
    if box is not None:
        node = box.node
        _select_only(tree, [node])
        if node.gl_type == "Compound":  # into it, to edit its nodes (U or the path bar: back out)
            try:
                graph_tree.enter_compound(tree.gl_scalp, node.gl_name)
            except HelperError as e:
                print("GroomLab:", e)
            return "open"
        if node.gl_type == "Script":
            bpy.ops.groomlab.graph_script_edit("INVOKE_DEFAULT", node=node.name)
            return "open"
        if y >= box.rect[1] + box.rect[3] - HEADER * g.k:
            bpy.ops.groomlab.graph_rename("INVOKE_DEFAULT", node=node.name)
            return "rename"
        return None
    frame = g.frame_at(x, y)
    if frame is not None:
        _select_only(tree, [frame])
        bpy.ops.groomlab.graph_backdrop_rename("INVOKE_DEFAULT", frame=frame.name)
        return "rename"
    link = g.wire_at(x, y)
    if link is not None:
        add_pin(tree, g, link, x, y)
        return "pin"
    return None


def add_pin(tree, g, link, x, y):
    """A pin (a reroute) on `link` at (x, y): the wire runs through it (the app's and Unreal's)."""
    source, target = link.from_socket, link.to_socket
    pin = tree.nodes.new("NodeReroute")
    gx, gy = g.to_graph(x, y)
    graph_tree.set_location(pin, (gx, gy))
    tree.links.remove(link)
    tree.links.new(source, pin.inputs[0])
    tree.links.new(pin.outputs[0], target)
    _select_only(tree, [pin])
    _push_undo("Add Pin")
    return pin


class GROOMLAB_OT_graph_canvas_align_key(bpy.types.Operator):
    """A held: drag from a node to line things up with it (Houdini's: A its chain, Shift+A the
    selection, Alt+A the selection spread evenly); A tapped frames the whole graph"""
    bl_idname = "groomlab.graph_canvas_align_key"
    bl_label = "Align Gesture"
    bl_options = {"INTERNAL"}

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    def invoke(self, context, event):
        if over_panel(context.region, event.mouse_region_x, context.preferences.system.ui_scale):
            return {"PASS_THROUGH"}
        self.key = event.type
        self.mode = "selected" if event.shift else ("distribute" if event.alt else "chain")
        self.drag = None
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

    def _restore(self, tree):
        for n in tree.nodes:
            at = self.drag["was"].get(n.name)
            if at is not None:
                graph_tree.set_location(n, at)

    def modal(self, context, event):
        tree = tree_here(context)
        if tree is None:
            return {"CANCELLED"}
        x, y = event.mouse_region_x, event.mouse_region_y
        if event.type == "LEFTMOUSE" and event.value == "PRESS" and self.drag is None:
            g = Graph(context, context.region, tree)
            box = g.box_at(x, y)
            if box is None:
                return {"RUNNING_MODAL"}
            self.drag = {"anchor": box.node.gl_name, "start": (x, y), "k": g.k, "positions": {},
                         "was": {n.name: graph_tree.get_location(n) for n in tree.nodes if graph_tree.ours(n)},
                         "boxes": graph_arrange.card_boxes(tree), "selected": graph_arrange.selected_names(tree)}
            return {"RUNNING_MODAL"}
        if event.type == "MOUSEMOVE" and self.drag is not None and "was" in self.drag and "done" not in self.drag:
            sx, sy = self.drag["start"]
            k = self.drag["k"]
            try:
                r = graph_tree.align_gesture(tree.gl_scalp, self.drag["boxes"], self.drag["anchor"], self.drag["selected"],
                                             self.mode, (x - sx) / k, -(y - sy) / k)
            except HelperError as e:
                self.report({"ERROR"}, str(e))
                return {"RUNNING_MODAL"}
            self._restore(tree)
            self.drag["positions"] = r.get("positions") or {}
            for name, (px, py) in self.drag["positions"].items():
                node = graph_tree.node_named(tree, name)
                if node is not None:
                    graph_tree.set_location(node, (px, -py))
            _redraw()
            return {"RUNNING_MODAL"}
        if event.type == "LEFTMOUSE" and event.value == "RELEASE" and self.drag is not None and "was" in self.drag:
            positions = self.drag["positions"]
            if positions:
                self._restore(tree)
                try:
                    graph_tree.place(tree.gl_scalp, positions)
                except HelperError as e:
                    self.report({"ERROR"}, str(e))
                _push_undo("Align")
            self.drag = {"done": True}
            _redraw()
            return {"RUNNING_MODAL"}
        if event.type == self.key and event.value == "RELEASE":
            if self.drag is None:  # tapped: the whole graph framed
                frame_view(context, everything=True)
            _redraw()
            return {"FINISHED"}
        if event.type in ("RIGHTMOUSE", "ESC") and event.value == "PRESS":
            if self.drag is not None and "was" in self.drag:
                self._restore(tree)
            _redraw()
            return {"CANCELLED"}
        return {"RUNNING_MODAL"}


class GROOMLAB_OT_graph_canvas_pin_key(bpy.types.Operator):
    """A pin on the link under the pointer (.)"""
    bl_idname = "groomlab.graph_canvas_pin_key"
    bl_label = "Pin Link"
    bl_options = {"INTERNAL"}

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    def invoke(self, context, event):
        tree = tree_here(context)
        g = Graph(context, context.region, tree)
        x, y = event.mouse_region_x, event.mouse_region_y
        link = g.wire_at(x, y, reach=12.0)
        if link is None:
            return {"PASS_THROUGH"}
        add_pin(tree, g, link, x, y)
        _redraw()
        return {"FINISHED"}


def frame_view(context, margin=48.0, everything=False):
    """The view fitted to the selected cards (none, or `everything`: all of them) as the canvas draws
    them, in the canvas left of the params (F)."""
    region = context.region
    tree = tree_here(context)
    g = Graph(context, region, tree)
    rects = [] if everything else [b.rect for b in g.boxes if b.node.select] + [r for f, r in g.frames if f.select]
    rects += [] if everything else [(px - 6, py - 6, 12, 12) for n, (px, py) in g.pins if n.select]
    if not rects:
        rects = [b.rect for b in g.boxes] + [r for _f, r in g.frames] or [(0, 0, region.width, region.height)]
    x0 = min(r[0] for r in rects) - margin
    y0 = min(r[1] for r in rects) - margin
    x1 = max(r[0] + r[2] for r in rects) + margin
    y1 = max(r[1] + r[3] for r in rects) + margin
    s = context.preferences.system.ui_scale
    canvas_w = max(region.width - PANEL_W * s, 100.0)
    bar = TOP_BAR * s
    # zoom_border fits a box to the whole region: widened on the right by the params' share and on
    # top by the bar's, so the cards land in the canvas beside them.
    scale = max((x1 - x0) / canvas_w, (y1 - y0) / max(region.height - bar, 100.0))
    x1 += PANEL_W * s * scale
    y1 += bar * scale
    bpy.ops.view2d.zoom_border(xmin=int(x0), xmax=int(x1), ymin=int(y0), ymax=int(y1), wait_for_input=False,
                               zoom_out=False)


def _carried(tree):
    """What a drag moves: the selected nodes, and the nodes on a selected backdrop (Blender keeps
    every node's location in the editor's space, so a frame moved alone would leave its nodes)."""
    picked = {n for n in tree.nodes if n.select}

    def on_picked_frame(n):
        p = n.parent
        while p is not None:
            if p in picked:
                return True
            p = p.parent
        return False
    return [n for n in tree.nodes if n in picked or on_picked_frame(n)]


def _overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def _push_undo(label):
    from . import tools
    tools.push_undo_step(label)


def _push_panel_undo():
    if uikit.changed:
        label = uikit.changed[-1]
        del uikit.changed[:]
        _push_undo(label)


def _toggle(context, tree, node, ident):
    """A header toggle pressed: Bypass, View, Expand Inputs, Clump Colors (their operators)."""
    _select_only(tree, [node])
    if ident == "bypass":
        bpy.ops.groomlab.graph_bypass("INVOKE_DEFAULT")
    elif ident == "view":
        bpy.ops.groomlab.graph_view_node("INVOKE_DEFAULT")
    elif ident == "inputs":
        bpy.ops.groomlab.graph_toggle_inputs("INVOKE_DEFAULT", node=node.gl_name)
    elif ident == "clump":
        bpy.ops.groomlab.graph_clump_colors("INVOKE_DEFAULT", node=node.gl_name)
    elif ident == "tubes":
        bpy.ops.groomlab.graph_braid_tubes("INVOKE_DEFAULT", node=node.gl_name)


class GROOMLAB_OT_graph_canvas_scroll(bpy.types.Operator):
    """Scroll the params on the graph's right (elsewhere the wheel zooms the graph)"""
    bl_idname = "groomlab.graph_canvas_scroll"
    bl_label = "Scroll GroomLab Params"
    bl_options = {"INTERNAL"}

    delta: bpy.props.IntProperty()

    @classmethod
    def poll(cls, context):
        return shows_here(context)

    def invoke(self, context, event):
        s = context.preferences.system.ui_scale
        if not over_panel(context.region, event.mouse_region_x, s):
            return {"PASS_THROUGH"}
        d = dock(context, reuse=True)
        if d.overflow <= 0:
            return {"FINISHED"}
        panel_state.scroll = min(max(panel_state.scroll + self.delta * 40.0, 0.0), d.overflow)
        _redraw()
        return {"FINISHED"}


class GROOMLAB_OT_graph_canvas_toggle(bpy.types.Operator):
    """Draw the GroomLab Graph in GroomLab's look, or as Blender's own nodes (wiring by dragging ports)"""
    bl_idname = "groomlab.graph_canvas_toggle"
    bl_label = "GroomLab Look"

    def execute(self, context):
        on = classic(context)
        setattr(context.window_manager, CLASSIC_PROP, not on)
        _show_header(context, not on)
        _redraw()
        return {"FINISHED"}


def _show_header(context, show):
    """Blender's header in the graph's own window: shown with Blender's nodes (the way back), hidden
    over GroomLab's canvas (its own top bar)."""
    area = getattr(context, "area", None)
    if area is not None and area.type == "NODE_EDITOR" and area.as_pointer() in _own_windows:
        sp = area.spaces.active
        sp.show_region_header = show
        sp.show_region_ui = show


_own_windows = set()  # the node editors GroomLab opened as the graph's own window (as_pointer)


def set_up_own_window(area):
    """The graph's own window: the canvas alone (Blender's header and sidebar hidden). Set again a
    moment later too: a node editor just switched to sets up its regions on its first redraw."""
    key = area.as_pointer()
    _own_windows.add(key)
    _hide_regions(area)

    def later():
        for window in bpy.context.window_manager.windows:
            for a in window.screen.areas:
                if a.as_pointer() == key and a.type == "NODE_EDITOR":
                    _hide_regions(a)
                    a.tag_redraw()
        return None
    bpy.app.timers.register(later, first_interval=0.05)


def _hide_regions(area):
    sp = area.spaces.active
    show = classic(bpy.context)
    for attr in ("show_region_header", "show_region_toolbar", "show_region_ui"):
        try:
            setattr(sp, attr, show)
        except (AttributeError, TypeError):
            pass


def draw_header_toggle(layout, context):
    """In Blender's header, with Blender's nodes: the way back to GroomLab's look."""
    if tree_here(context) is not None and classic(context):
        layout.operator(GROOMLAB_OT_graph_canvas_toggle.bl_idname, text="GroomLab Look", icon="NODETREE")


# ---- Registration ---------------------------------------------------------------------------------

CLASSES = (GROOMLAB_GT_graph_canvas, GROOMLAB_GGT_graph_canvas, GROOMLAB_OT_graph_canvas_press,
           GROOMLAB_OT_graph_canvas_scroll, GROOMLAB_OT_graph_canvas_toggle, GROOMLAB_OT_graph_canvas_search,
           GROOMLAB_MT_graph_canvas_node, GROOMLAB_MT_graph_canvas, GROOMLAB_OT_graph_canvas_context,
           GROOMLAB_OT_graph_canvas_double, GROOMLAB_OT_graph_canvas_align_key, GROOMLAB_OT_graph_canvas_pin_key,
           drawn.GROOMLAB_PT_curve_popup)


def register():
    setattr(bpy.types.WindowManager, CLASSIC_PROP, bpy.props.BoolProperty(
        name="Blender Nodes", description="Show the GroomLab Graph as Blender's own nodes", default=False))
    for c in CLASSES:
        bpy.utils.register_class(c)
    _draw_handle[0] = bpy.types.SpaceNodeEditor.draw_handler_add(draw, (), "WINDOW", "POST_PIXEL")
    kc = bpy.context.window_manager.keyconfigs.addon if bpy.context.window_manager else None
    if kc is not None:
        # Without the gizmo (the editor's Gizmos off): the same press from the node editor's keymap.
        km = kc.keymaps.new(name="Node Editor", space_type="NODE_EDITOR")
        _keymaps.append((km, km.keymap_items.new(GROOMLAB_OT_graph_canvas_press.bl_idname, "LEFTMOUSE", "PRESS", any=True)))
        _keymaps.append((km, km.keymap_items.new(GROOMLAB_OT_graph_canvas_context.bl_idname, "RIGHTMOUSE", "PRESS", any=True)))
        _keymaps.append((km, km.keymap_items.new(GROOMLAB_OT_graph_canvas_double.bl_idname, "LEFTMOUSE", "DOUBLE_CLICK",
                                                 any=True)))
        # A held (A+drag aligns, Shift+A the selection, Alt+A spreads it; tapped frames all) and . (a pin).
        for mods in ({}, {"shift": True}, {"alt": True}):
            _keymaps.append((km, km.keymap_items.new(GROOMLAB_OT_graph_canvas_align_key.bl_idname, "A", "PRESS", **mods)))
        _keymaps.append((km, km.keymap_items.new(GROOMLAB_OT_graph_canvas_pin_key.bl_idname, "PERIOD", "PRESS")))


def unregister():
    if _draw_handle[0] is not None:
        bpy.types.SpaceNodeEditor.draw_handler_remove(_draw_handle[0], "WINDOW")
        _draw_handle[0] = None
    for km, kmi in _keymaps:
        try:
            km.keymap_items.remove(kmi)
        except (ReferenceError, RuntimeError):
            pass
    _keymaps.clear()
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
    if hasattr(bpy.types.WindowManager, CLASSIC_PROP):
        delattr(bpy.types.WindowManager, CLASSIC_PROP)
    _theme_cache.clear()
    _dock_cache.clear()
