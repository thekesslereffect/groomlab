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

from . import drawn, graph_tree, groom, uikit

NODE_W = 176.0
CLUMP_W = 23.0      # a Clump's header holds Clump Colors too
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
        "muted": color(g.get("uiTextDim"), "#737373"),
        "radius": float(g.get("cornerRadius", 10)), "header": g.get("headerStyle", "tinted"),
        "dots": g.get("canvasStyle", "dots") == "dots", "shadow": bool(g.get("shadow", True)),
        "categories": {k: color(v, "#9095A0") for k, v in (g.get("categories") or {}).items()},
        "ports": {k: color(v, "#C8C8C8") for k, v in (g.get("ports") or {}).items()},
    }
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


def with_alpha(c, a):
    return (c[0], c[1], c[2], a)


# ---- The graph laid out on the region -------------------------------------------------------------

class Box(object):
    """A node's card on the region: its rect (x, y bottom, w, h, in region pixels), ports and toggles."""

    def __init__(self, node, rect, k):
        self.node, self.rect, self.k = node, rect, k
        self.inputs, self.outputs = [], []  # (socket, (x, y)) in region pixels
        self.toggles = []                   # (id, rect): the header's toggles, left to right


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
        for node in tree.nodes:
            if node.bl_idname == "NodeFrame":
                self.frames.append((node, self._frame_rect(node)))
            elif node.bl_idname == "NodeReroute":
                gx, gy = graph_tree.get_location(node)
                self.pins.append((node, self.at(gx, gy)))
            elif graph_tree.ours(node) and not node.hide:
                box = self._box(node)
                self.boxes.append(box)
                self.by_name[node.name] = box

    def at(self, gx, gy):
        return self.ox + gx * self.k, self.oy + gy * self.k

    def to_graph(self, x, y):
        return (x - self.ox) / self.k, (y - self.oy) / self.k

    def _frame_rect(self, frame):
        gx, gy = graph_tree.get_location(frame)
        w, h = frame.width, frame.height
        x, top = self.at(gx, gy)
        return (x, top - h * self.k, w * self.k, h * self.k)

    def _box(self, node):
        ins = [s for s in node.inputs if s.enabled and not s.hide]
        outs = [s for s in node.outputs if s.enabled and not s.hide]
        clump = node.gl_type == "Clump"
        label = graph_tree.label_of(node.gl_type)
        type_row = node.name != label and node.name != node.gl_type and len(ins) <= 1 and len(outs) <= 1
        rows = max(len(ins), len(outs), 1) + (1 if type_row else 0)
        w, h = NODE_W + (CLUMP_W if clump else 0.0), HEADER + PAD * 2 + rows * ROW
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
        """A backdrop whose title bar is under (x, y): it drags with its nodes."""
        for frame, r in reversed(self.frames):
            if uikit.contains((r[0], r[1] + r[3] - BACKDROP_HEADER * self.k, r[2], BACKDROP_HEADER * self.k), x, y):
                return frame
        return None

    def grip_at(self, x, y):
        """A backdrop whose corner grip (bottom right) is under (x, y): it resizes."""
        g = GRIP * self.k
        for frame, r in reversed(self.frames):
            if uikit.contains((r[0] + r[2] - g, r[1], g, g), x, y):
                return frame
        return None

    def wire_at(self, x, y, reach=7.0):
        """The link whose wire passes within `reach` pixels of (x, y), else None."""
        best, best_d = None, reach * reach
        for link in self.tree.links:
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
            return None
        for s, point in (box.outputs if output else box.inputs):
            if s == socket:
                return point
        return None


def _segment_distance2(pt, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((pt[0] - a[0]) * dx + (pt[1] - a[1]) * dy) / (dx * dx + dy * dy)))
    cx, cy = a[0] + t * dx, a[1] + t * dy
    return (pt[0] - cx) ** 2 + (pt[1] - cy) ** 2


def compatible(a, b):
    """Whether a wire can join ports of these types (the helper has the last word: a wire it
    refuses is taken back and the panel says why). Strands-or-guides and Any take either; a mesh
    goes only into a mesh."""
    if "Mesh" in (a, b):
        return a == b or "Any" in (a, b)
    return a == b or a in ("Any", "Curves") or b in ("Any", "Curves")


def viewable(box):
    return any(_port_type(s) in ("Strands", "Curves") for s, _p in box.outputs)


def linkable(node):
    return any(p.get("port") for p in graph_tree.node_param_specs(node))


# ---- Drawing -------------------------------------------------------------------------------------

def _wire_points(a, b, k, below=None):
    """The app's wire (node_graph.cpp wirePath): a level S-curve forward; backwards, down under the
    cards and back along under them."""
    def cubic(p0, p1, p2, p3, n=20):
        out = []
        for i in range(n + 1):
            t = i / float(n)
            u = 1.0 - t
            out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                        u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
        return out
    if b[0] >= a[0] + 30.0 * k:
        dx = max(60.0 * k, abs(b[0] - a[0]) * 0.5)
        return cubic(a, (a[0] + dx, a[1]), (b[0] - dx, b[1]), b)
    y = min(a[1], b[1]) - 60.0 * k if below is None else below - 24.0 * k
    r = 40.0 * k
    return cubic(a, (a[0] + r, a[1]), (a[0] + r, y), (a[0], y), 10) + cubic((b[0], y), (b[0] - r, y), (b[0] - r, b[1]), b, 10)


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
        _draw_backdrop(p, t, frame, r, k)
    _draw_wires(p, g, t)
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
            if compatible(kind, _port_type(s)):
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
    p.rect(r, with_alpha(base, 46 / 255.0), radius, with_alpha(base, 120 / 255.0), 1.5 * k)
    top = r[1] + r[3]
    p.band(r, with_alpha(base, 120 / 255.0), with_alpha(base, 120 / 255.0), radius, top - BACKDROP_HEADER * k, top)
    if 14 * k >= 6:
        p.text(frame.label or frame.name, (r[0] + 12 * k, top - BACKDROP_HEADER * k, r[2] - 24 * k, BACKDROP_HEADER * k),
               t["textDim"], size=14, bold=True)
    light = tuple(min(1.0, c * 1.3) for c in base[:3]) + (0.9,)
    for d in (4.0, 8.0, 12.0):  # the corner grip
        p.lines([(r[0] + r[2] - d * k, r[1] + 2 * k), (r[0] + r[2] - 2 * k, r[1] + d * k)], light, 1.5)
    if frame.select:
        p.rect(r, (0, 0, 0, 0), radius, t["selected"], 2.0 * k)


def _draw_wires(p, g, t):
    picked = _wire.get("picked")
    for link in g.tree.links:
        if not link.is_valid or link.is_hidden or (picked is not None and link == picked):
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
    ink = t["text"]
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
        p.text(stamp, (bx - tw, top - HEADER * k, tw, HEADER * k), with_alpha(ink, 0.75), size=size, clip=False)
        bx -= tw + 6.0 * k
    if node.gl_viewed and not viewable(box):
        _eye(p, (bx - 7.0 * k, mid), 6.5 * k, t["accent"], 1.4, 2.0 * k)
        bx -= 18.0 * k
    if notes.get(name) and name not in errors:
        c, s = (bx - 6.0 * k, mid), 12.0 * k
        hh = s * 0.9
        p.lines([(c[0], c[1] + hh / 2), (c[0] + s / 2, c[1] - hh / 2), (c[0] - s / 2, c[1] - hh / 2), (c[0], c[1] + hh / 2)],
                t["warning"], 1.4)
        p.lines([(c[0], c[1] + hh * 0.12), (c[0], c[1] - hh * 0.18)], t["warning"], 1.4)
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
            p.text(s.name, (px + 10 * k, py - ROW * k / 2, w / 2, ROW * k), t["textDim"], size=13)
    for s, (px, py) in box.outputs:
        pc = _port_color(t, _port_type(s))
        r = PORT_R * k
        p.rect((px - r, py - r, 2 * r, 2 * r), pc, r)
        if readable:
            p.text(s.name, (px - 10 * k - w / 2, py - ROW * k / 2, w / 2, ROW * k), t["textDim"], size=13, align="right")
    if box.type_row and readable:
        info = graph_editor.graph_authoring.library_info(node) if node.gl_library_info else {}
        p.text(info.get("label") or graph_tree.label_of(node.gl_type), (x, y + 2 * k, w, 14 * k), t["muted"], size=11,
               align="center")
    p.fade = 1.0
    if node.gl_evaluation in graph_tree.EVALUATION_LABELS and readable:
        p.text(graph_tree.EVALUATION_LABELS[node.gl_evaluation].lower(), (x + 4 * k, y - 18 * k, w - 8 * k, 16 * k), t["muted"],
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
    node, k = box.node, box.k
    ink = t["text"]
    c = (r[0] + r[2] / 2, r[1] + r[3] / 2)
    if ident == "bypass":
        on = node.mute
        if on:
            p.rect(r, with_alpha(ink, 46 / 255.0), 4 * k)
        rad = r[2] / 2 - 4 * k
        col = ink if on else with_alpha(ink, 120 / 255.0)
        ring = [(c[0] + rad * math.cos(a * math.pi / 12), c[1] + rad * math.sin(a * math.pi / 12)) for a in range(25)]
        p.lines(ring, col, 1.8 if on else 1.5)
        d = rad * 0.7071
        p.lines([(c[0] - d, c[1] - d), (c[0] + d, c[1] + d)], col, 1.8 if on else 1.5)
    elif ident == "view":
        on = node.gl_viewed
        col = t["accent"] if on else with_alpha(t["textDim"], 170 / 255.0)
        if on:
            p.rect(r, with_alpha(col, 60 / 255.0), 4 * k)
        _eye(p, c, 6.5 * k, col, 1.8 if on else 1.5, 2.2 * k)
    elif ident == "inputs":
        exposed = set(json.loads(node.gl_inputs or "[]"))
        names = [s["name"] for s in graph_tree.node_param_specs(node) if s.get("port")]
        on = bool(names) and all(n in exposed for n in names)
        col = ink if on else with_alpha(ink, 170 / 255.0)
        for dy in (5.0, 9.0, 13.0):
            yy = r[1] + r[3] - dy * k
            p.lines([(r[0] + 4 * k, yy), (r[0] + r[2] - 4 * k, yy)], col, 1.6)
    elif ident == "clump":
        scalp = getattr(node.id_data, "gl_scalp", None)
        on = bool(scalp) and graph_tree.clump_colored(scalp, node)
        col = t["accent"] if on else with_alpha(t["textDim"], 170 / 255.0)
        if on:
            p.rect(r, with_alpha(col, 60 / 255.0), 4 * k)
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


def _draw_top_bar(p, context, region, tree, s):
    ui = uikit._theme()
    bar, button = _top_bar_rects(region, s)
    scalp = getattr(tree, "gl_scalp", None)
    title = scalp.name if scalp is not None else tree.name
    p.text(title, (12 * s, bar[1], 300 * s, bar[3]), ui.text, size=ui.font_size + 2, bold=True)
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
        t = self.theme
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
            return {"FINISHED"}
        tree = tree_here(context)
        g = Graph(context, region, tree)
        hit = g.toggle_at(x, y)
        if hit is not None:
            _toggle(context, tree, hit[0].node, hit[1])
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
            _press.update(kind="wire", start=(x, y))
            _redraw()
            context.window_manager.modal_handler_add(self)
            return {"RUNNING_MODAL"}
        shift = event.shift
        node = g.pin_at(x, y)
        box = g.box_at(x, y) if node is None else None
        if box is not None:
            node = box.node
        if node is None:
            node = g.frame_at(x, y)
        if node is not None:
            if shift:
                node.select = not node.select
                if node.select:
                    tree.nodes.active = node
            elif not node.select:
                _select_only(tree, [node])
            else:
                tree.nodes.active = node
            moving = _carried(tree)
            _press.update(kind="move", start=(x, y), last=(x, y), node=node, moved=False, shift=shift,
                          nodes=moving, k=g.k)
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
            if kind == "move":
                sx, sy = _press["start"]
                if not _press["moved"] and math.hypot(x - sx, y - sy) < DRAG_START:
                    return {"RUNNING_MODAL"}
                lx, ly = _press["last"]
                k = _press["k"]
                dx, dy = (x - lx) / k, (y - ly) / k
                for n in _press["nodes"]:
                    gx, gy = graph_tree.get_location(n)
                    graph_tree.set_location(n, (gx + dx, gy + dy))
                _press.update(last=(x, y), moved=True)
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
            if kind == "move":
                if _press["moved"]:
                    _push_undo("Move Nodes")
                elif not _press["shift"]:
                    _select_only(tree, [_press["node"]])  # a click on one of several picked: that one alone
            elif kind == "band":
                sx, sy = _press["start"]
                if math.hypot(x - sx, y - sy) < DRAG_START and not _press["shift"]:
                    _select_only(tree, [])  # a click on the canvas: nothing picked
            _band[0] = None
            _press.clear()
            _redraw()
            return {"FINISHED"}
        if event.type in ("RIGHTMOUSE", "ESC") and event.value == "PRESS":
            if kind == "move" and _press["moved"]:
                k = _press["k"]
                sx, sy = _press["start"]
                lx, ly = _press["last"]
                for n in _press["nodes"]:
                    gx, gy = graph_tree.get_location(n)
                    graph_tree.set_location(n, (gx - (lx - sx) / k, gy - (ly - sy) / k))
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
    return port if compatible(_port_type(_wire["from"]), _port_type(port[1])) else None


def _connect(tree, out_socket, in_socket):
    tree.links.new(out_socket, in_socket)  # (an input takes one wire: the one there goes)


def _drop_wire(context, tree, g, x, y, moved):
    """The dragged wire let go: onto a port (or a card: its first free port it can take), wired;
    a picked-up wire dropped on the canvas goes; a new one opens the search (returns "search")."""
    picked = _wire.get("picked")
    source, out = _wire["from"], _wire["out"]
    target = _wire_target(g, x, y)
    if target is None:
        box = g.box_at(x, y)
        if box is not None and box.node != source.node:
            ports = box.inputs if out else box.outputs
            free = [s for s, _p in ports if compatible(_port_type(source), _port_type(s))
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
    return out


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
                into = [s for s in node.inputs if s.enabled and compatible(_port_type(source), _port_type(s))]
                if into:
                    _connect(tree, source, into[0])
            else:
                outs = [s for s in node.outputs if s.enabled and compatible(_port_type(source), _port_type(s))]
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
    box = g.box_at(x, y)
    if box is not None:
        node = box.node
        _select_only(tree, [node])
        if node.gl_type == "Compound":
            bpy.ops.groomlab.graph_ungroup("INVOKE_DEFAULT", node=node.name)
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


def frame_view(context, margin=48.0):
    """The view fitted to the selected cards (none: all of them) as the canvas draws them, in the
    canvas left of the params (F)."""
    region = context.region
    tree = tree_here(context)
    g = Graph(context, region, tree)
    rects = [b.rect for b in g.boxes if b.node.select] + [r for f, r in g.frames if f.select]
    rects += [(px - 6, py - 6, 12, 12) for n, (px, py) in g.pins if n.select]
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
           GROOMLAB_OT_graph_canvas_double, drawn.GROOMLAB_PT_curve_popup)


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
