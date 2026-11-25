# SPDX-License-Identifier: GPL-3.0-or-later
"""The Graph Editor arranged by hand, as Maya's and the app's (graph_arrange.h, graph_portals.h and
GraphEdit's pass-through outputs, through the helper's arrange.cpp): Align and Tidy, wire styles,
portals and dots, the link, portal and port menus, Pass Through as Output, folded backdrops.

The rules are the core's; what is here is the canvas' side: the cards' boxes the core lays out
(graph units, y down, as every host's "ui"), the operators the shared menus bind (graph_ui.bind),
and a small port drawn every frame that the helper checks against the core (wire_route:
tests/hosts/blender/test_helper.py)."""

import math

import bpy
from bpy.props import BoolProperty, IntProperty, StringProperty

from . import graph_tree, graph_ui
from .helper_client import HelperError

# The cards' sizes (the app's node_graph.cpp, graph_canvas.py draws them so).
NODE_W = 176.0
TOGGLE_W = 23.0     # a Clump's (or a Braid's) extra header toggle
HEADER = 28.0
ROW = 18.0
PAD = 8.0
DOT_R = 7.0         # a portal's dot (Maya's PortalItem.RADIUS)
DOT_NUB = 13.0      # its nub, where links drag out
DOT_NUB_REACH = 10.0


# ---- The cards as the core lays them out -------------------------------------------------------------

def card_size(node):
    """(w, h) of a node's card in graph units (graph_canvas.Graph._box draws it so)."""
    ins = [s for s in node.inputs if s.enabled and not s.hide]
    outs = [s for s in node.outputs if s.enabled and not s.hide]
    extra = node.gl_type == "Clump" or graph_tree.shows_tubes(node)
    label = graph_tree.label_of(node.gl_type)
    type_row = node.name != label and node.name != node.gl_type and len(ins) <= 1 and len(outs) <= 1
    rows = max(len(ins), len(outs), 1) + (1 if type_row else 0)
    return NODE_W + (TOGGLE_W if extra else 0.0), HEADER + PAD * 2 + rows * ROW


def backdrop_header(backdrop):
    """Its title bar's height (the core's backdropHeader): taller for a bigger title."""
    size = float(backdrop.get("fontSize") or 0)
    return max(28.0, size * 1.9) if size > 0 else 28.0


def backdrop_title_width(backdrop):
    """About how wide its title is drawn with the chevron (the core's backdropTitleWidth)."""
    size = float(backdrop.get("fontSize") or 0)
    return len(str(backdrop.get("title") or "")) * (size if size > 0 else 14.0) * 0.62 + 56.0


def folded_width(backdrop, widest=260.0):
    """A folded backdrop's width (the core's foldedBackdrop, its title always showing)."""
    least = max(160.0, backdrop_title_width(backdrop))
    return min(max(float(backdrop.get("w", 300.0)), least), max(least, widest))


def grown_backdrop(backdrop, boxes, margin=24.0, header=28.0):
    """(x, y, w, h) grown to hold `boxes` (graph units, y down), never smaller (the core's grownBackdrop)."""
    x, y, w, h = backdrop
    right, bottom = x + w, y + h
    for b in boxes:
        x = min(x, b["x"] - margin)
        y = min(y, b["y"] - margin - header)
        right = max(right, b["x"] + b["w"] + margin)
        bottom = max(bottom, b["y"] + b["h"] + margin)
    return x, y, right - x, bottom - y


BACKDROP_ESCAPE_SPEED = 1100.0  # screen pixels a second (the core's kBackdropEscapeSpeed)


def backdrop_keeps(over_it, speed, left):
    """While a node on a backdrop is dragged: whether the backdrop keeps it (the core's backdropKeeps)."""
    if left:
        return False
    return over_it or speed < BACKDROP_ESCAPE_SPEED


def card_box(node):
    """{name, x, y, w, h}: a node's card, its top-left in graph units, y down (its "ui")."""
    x, y = graph_tree.get_location(node)
    w, h = card_size(node)
    return {"name": node.gl_name, "x": float(x), "y": float(-y), "w": float(w), "h": float(h)}


def card_boxes(tree, only=None):
    """Every GroomLab node's card (hidden ones too), or those named in `only`."""
    out = []
    for n in tree.nodes:
        if graph_tree.ours(n) and n.gl_name and (only is None or n.gl_name in only):
            out.append(card_box(n))
    return out


def selected_names(tree):
    return [n.gl_name for n in tree.nodes if graph_tree.ours(n) and n.gl_name and n.select]


# ---- Wire styles (graph_arrange.h wireRoute; the helper's wire_route checks this port) --------------

WIRE_STYLES = ("smooth", "square", "rounded")


def wire_style():
    """The style the user picked (the add-on's preferences: a user's, as Maya's optionVar)."""
    try:
        prefs = bpy.context.preferences.addons[__package__].preferences
        return getattr(prefs, "wire_style", "smooth") or "smooth"
    except (KeyError, AttributeError):
        return _style[0]


_style = ["smooth"]  # (without the add-on's preferences: a test, a source tree)


def set_wire_style(style):
    _style[0] = style
    try:
        prefs = bpy.context.preferences.addons[__package__].preferences
        prefs.wire_style = style
    except (KeyError, AttributeError, TypeError):
        pass


def _polyline_steps(points, radius):
    p = []
    for q in points:
        if p and abs(q[0] - p[-1][0]) < 1e-6 and abs(q[1] - p[-1][1]) < 1e-6:
            continue
        if len(p) >= 2:
            a, b = p[-2], p[-1]
            cross = (b[0] - a[0]) * (q[1] - b[1]) - (b[1] - a[1]) * (q[0] - b[0])
            dot = (b[0] - a[0]) * (q[0] - b[0]) + (b[1] - a[1]) * (q[1] - b[1])
            if abs(cross) < 1e-6 and dot > 0.0:
                p[-1] = q
            else:
                p.append(q)
        else:
            p.append(q)
    steps = []
    if len(p) < 2:
        return steps
    kappa = 0.5523
    for i in range(1, len(p)):
        a, b = p[i - 1], p[i]
        if i + 1 == len(p) or radius <= 0.0:
            steps.append((b[0], b[1]))
            continue
        c = p[i + 1]
        l1, l2 = math.hypot(b[0] - a[0], b[1] - a[1]), math.hypot(c[0] - b[0], c[1] - b[1])
        r = min(radius, l1 * 0.5, l2 * 0.5)
        d1x, d1y, d2x, d2y = (b[0] - a[0]) / l1, (b[1] - a[1]) / l1, (c[0] - b[0]) / l2, (c[1] - b[1]) / l2
        start, end = (b[0] - d1x * r, b[1] - d1y * r), (b[0] + d2x * r, b[1] + d2y * r)
        steps.append(start)
        steps.append((start[0] + d1x * r * kappa, start[1] + d1y * r * kappa, end[0] - d2x * r * kappa,
                      end[1] - d2y * r * kappa, end[0], end[1]))
        p[i] = end
    return steps


def wire_route(style, ax, ay, bx, by, below=None, through=False):
    """A wire from (ax, ay) to (bx, by) in graph units, y down, as steps: (x, y) a line, (c1x, c1y,
    c2x, c2y, x, y) a cubic (the core's wireRoute)."""
    forward = through or bx >= ax + 30.0
    if style in ("square", "rounded"):
        radius = 14.0 if style == "rounded" else 0.0
        if forward:
            mid = (ax + bx) * 0.5
            return _polyline_steps([(ax, ay), (mid, ay), (mid, by), (bx, by)], radius)
        y = max(ay, by) + 60.0 if below is None else below + 24.0
        out = 24.0
        return _polyline_steps([(ax, ay), (ax + out, ay), (ax + out, y), (bx - out, y), (bx - out, by), (bx, by)], radius)
    if forward:
        dx = max(50.0 if through else 60.0, abs(bx - ax) * 0.5)
        return [(ax + dx, ay, bx - dx, by, bx, by)]
    y = max(ay, by) + 60.0 if below is None else below + 24.0
    r = 40.0
    return [(ax + r, ay, ax + r, y, ax, y), (bx, y), (bx - r, y, bx - r, by, bx, by)]


def route_points(start, steps, n=20):
    """The steps from `start` as a polyline (cubics sampled `n` times)."""
    pts = [start]
    for s in steps:
        if len(s) == 2:
            pts.append(s)
            continue
        p0 = pts[-1]
        for i in range(1, n + 1):
            t = i / float(n)
            u = 1.0 - t
            pts.append((u * u * u * p0[0] + 3 * u * u * t * s[0] + 3 * u * t * t * s[2] + t * t * t * s[4],
                        u * u * u * p0[1] + 3 * u * u * t * s[1] + 3 * u * t * t * s[3] + t * t * t * s[5]))
    return pts


# ---- Portals: where the dots are --------------------------------------------------------------------

class Dot(object):
    """A portal's dot: its entry (exit None) or one of its exits, at (x, y) in graph units, y down;
    `links` the (to, input) drawn out of it; `source` (node, output) feeding it, None for a free dot."""

    def __init__(self, portal, exit_index, x, y, links, source):
        self.portal, self.exit, self.x, self.y = portal, exit_index, float(x), float(y)
        self.links, self.source = links, source


def dots(tree):
    """Every dot of the graph's portals (graph_tree.portals_of)."""
    out = []
    for p in graph_tree.portals_of(tree):
        name = p.get("name") or ""
        source = (p["from"], p.get("output") or "") if p.get("from") else None

        def links_of(entry):
            return [(l.get("to"), l.get("input")) for l in entry.get("links") or [] if isinstance(l, dict)]
        out.append(Dot(name, None, p.get("x", 0.0), p.get("y", 0.0), links_of(p), source))
        for i, e in enumerate(p.get("exits") or []):
            out.append(Dot(name, i, e.get("x", 0.0), e.get("y", 0.0), links_of(e), source))
    return out


def teleported(tree):
    """{(to, input): Dot}: the links drawn out of a dot instead of as a wire from their source."""
    return {link: d for d in dots(tree) for link in d.links}


# ---- What a menu acts on (the link, the dot or the port right-clicked) -------------------------------

target = {}


def _scalp(context):
    from . import graph_editor
    return graph_editor.context_groom(context)


def _tree(context):
    from . import graph_editor
    return graph_editor.editor_tree(context)


def _fail(op, e):
    op.report({"ERROR"}, str(e))
    return {"CANCELLED"}


class _Op(graph_ui.ActionTip, bpy.types.Operator):
    bl_options = {"REGISTER", "UNDO", "INTERNAL"}
    action: graph_ui.action_prop()

    @classmethod
    def poll(cls, context):
        return _tree(context) is not None


# Align, Tidy, wire styles.

ALIGN_MODES = ("row", "column", "left", "centerX", "right", "top", "centerY", "bottom", "distributeX", "distributeY")


class GROOMLAB_OT_graph_align(_Op):
    """Line up the selected nodes"""
    bl_idname = "groomlab.graph_align"
    bl_label = "Align"

    mode: StringProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        tree = _tree(context)
        names = selected_names(tree)
        if len(names) < 2:
            self.report({"INFO"}, "Select two nodes or more to align")
            return {"CANCELLED"}
        try:
            graph_tree.align(_scalp(context), self.mode, card_boxes(tree, set(names)))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


def tidy(context):
    """Tidy Layout (L) and Tidy Selection: the selected nodes (fewer than two: the whole graph)."""
    tree = _tree(context)
    graph_tree.tidy(_scalp(context), card_boxes(tree), selected_names(tree))


class GROOMLAB_OT_graph_wire_style(_Op):
    """How links are drawn: smooth curves, square runs or square runs with rounded corners"""
    bl_idname = "groomlab.graph_wire_style"
    bl_label = "Wire Style"
    bl_options = {"REGISTER", "INTERNAL"}

    style: StringProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        if self.style not in WIRE_STYLES:
            return {"CANCELLED"}
        set_wire_style(self.style)
        _redraw()
        return {"FINISHED"}


# Backdrops: fold, and the card's fields.

class GROOMLAB_OT_graph_backdrop_fold(_Op):
    """Fold the backdrop to its title bar, its nodes hidden and their links meeting its edges; again: open"""
    bl_idname = "groomlab.graph_backdrop_fold"
    bl_label = "Fold"

    frame: StringProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        tree = _tree(context)
        frame = tree.nodes.get(self.frame) if self.frame else tree.nodes.active
        if frame is None or frame.bl_idname != "NodeFrame":
            return {"CANCELLED"}
        set_backdrop(context, frame, collapsed=not bool(frame.get("gl_collapsed")))
        return {"FINISHED"}


class GROOMLAB_OT_graph_backdrop_edit(_Op):
    """The backdrop's description (under its title) and its title's size"""
    bl_idname = "groomlab.graph_backdrop_edit"
    bl_label = "Backdrop"

    frame: StringProperty(options={"SKIP_SAVE"})
    description: StringProperty(name="Description")
    font_size: IntProperty(name="Title Size", min=0, max=96, default=0, description="Points (0: the editors' default)")

    def invoke(self, context, event):
        frame = _tree(context).nodes.get(self.frame)
        if frame is None:
            return {"CANCELLED"}
        self.description = str(frame.get("gl_description") or "")
        self.font_size = int(round(float(frame.get("gl_font_size") or 0)))
        return context.window_manager.invoke_props_dialog(self, width=420)

    def execute(self, context):
        frame = _tree(context).nodes.get(self.frame)
        if frame is None:
            return {"CANCELLED"}
        try:
            set_backdrop(context, frame, description=self.description, fontSize=self.font_size)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


def set_backdrop(context, frame, **fields):
    """A backdrop's own fields (title, description, fontSize, collapsed) set and kept in the graph;
    a bigger title moves its nodes clear of it (the core's clearBackdropTitle)."""
    grew = False
    with graph_tree.building():
        if "title" in fields:
            frame.label = str(fields["title"])
        if "description" in fields:
            grew = len(str(fields["description"] or "")) > len(str(frame.get("gl_description") or ""))
            frame["gl_description"] = str(fields["description"] or "")
        if "fontSize" in fields:
            grew = grew or float(fields["fontSize"] or 0) > float(frame.get("gl_font_size") or 0)
            frame["gl_font_size"] = float(fields["fontSize"] or 0)
        if "collapsed" in fields:
            frame["gl_collapsed"] = bool(fields["collapsed"])
    scalp = _scalp(context)
    graph_tree.push_positions(scalp)
    if grew:
        tree = frame.id_data
        index = graph_tree.frames(tree).index(frame)
        graph_tree._arrange(scalp, "clear_backdrop_title", index=index, boxes=card_boxes(tree))
    _redraw()


# Links: Teleport, a pin, Delete.

class GROOMLAB_OT_graph_link_teleport(_Op):
    """Draw this link through a portal: a named dot by its output and another by its input"""
    bl_idname = "groomlab.graph_link_teleport"
    bl_label = "Teleport Link"

    def execute(self, context):
        link = target.get("link")
        if not link:
            return {"CANCELLED"}
        to, input_name, exit_at, entry_at = link["to"], link["input"], link["exit_at"], link["entry_at"]
        try:
            graph_tree.teleport_link(_scalp(context), to, input_name, exit_at, entry_at)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_link_pin(_Op):
    """A pin on the link where you clicked, to route it"""
    bl_idname = "groomlab.graph_link_pin"
    bl_label = "Add Pin Here"

    def execute(self, context):
        link = target.get("link")
        if not link or link.get("tree_link") is None:
            return {"CANCELLED"}
        from . import graph_canvas
        tree = _tree(context)
        g = graph_canvas.Graph(context, context.region, tree) if context.region is not None else None
        if g is None:
            return {"CANCELLED"}
        x, y = link["region_at"]
        graph_canvas.add_pin(tree, g, link["tree_link"], x, y)
        return {"FINISHED"}


class GROOMLAB_OT_graph_link_delete(_Op):
    """Disconnect the link"""
    bl_idname = "groomlab.graph_link_delete"
    bl_label = "Delete Link"

    def execute(self, context):
        link = target.get("link")
        if not link:
            return {"CANCELLED"}
        tree = _tree(context)
        for l in list(tree.links):
            if graph_tree.ours(l.to_node) and l.to_node.gl_name == link["to"] and l.to_socket.identifier == link["input"]:
                tree.links.remove(l)
        from . import graph_canvas
        graph_canvas._push_undo("Delete Link")
        return {"FINISHED"}


# Portals.

class GROOMLAB_OT_graph_portal_rename(_Op):
    """The portal's name, shown by each of its dots"""
    bl_idname = "groomlab.graph_portal_rename"
    bl_label = "Rename Portal"

    portal: StringProperty(options={"SKIP_SAVE"})
    name: StringProperty(name="Name")

    def invoke(self, context, event):
        self.portal = self.portal or (target.get("dot") or {}).get("portal", "")
        self.name = self.portal
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        if not self.portal or not self.name.strip():
            return {"CANCELLED"}
        try:
            graph_tree.rename_portal(_scalp(context), self.portal, self.name.strip())
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_portal_remove(_Op):
    """The links out of this exit, or (the whole portal) every link through it, drawn as wires again"""
    bl_idname = "groomlab.graph_portal_remove"
    bl_label = "Remove Portal"

    whole: BoolProperty(default=True, options={"SKIP_SAVE"})

    def execute(self, context):
        dot = target.get("dot")
        if not dot:
            return {"CANCELLED"}
        try:
            graph_tree.remove_portal(_scalp(context), dot["portal"], None if self.whole else dot["exit"])
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# Inside a compound: in (double-click, I), out (U, the path bar), Promote to Compound.

class GROOMLAB_OT_graph_enter_compound(_Op):
    """Go into the compound to edit its nodes (double-click it); the path bar or U comes back out"""
    bl_idname = "groomlab.graph_enter_compound"
    bl_label = "Open Compound"

    node: StringProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        tree = _tree(context)
        name = self.node or (tree.nodes.active.gl_name if tree.nodes.active is not None and graph_tree.ours(tree.nodes.active)
                             else "")
        node = graph_tree.node_named(tree, name) if name else None
        if node is None or node.gl_type != "Compound":
            self.report({"INFO"}, "Select a Compound to go into it")
            return {"CANCELLED"}
        try:
            graph_tree.enter_compound(_scalp(context), name)
        except HelperError as e:
            return _fail(self, e)
        _frame_all(context)
        return {"FINISHED"}


class GROOMLAB_OT_graph_up(_Op):
    """Out of the compound you are in (U), or to the graph at `path` of the path bar"""
    bl_idname = "groomlab.graph_up"
    bl_label = "Up to the Parent Graph"

    path: StringProperty(options={"SKIP_SAVE"})
    to_path: BoolProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        scalp = _scalp(context)
        try:
            if self.to_path:
                graph_tree.set_scope(scalp, self.path)
            elif not graph_tree.leave_compound(scalp):
                return {"CANCELLED"}
        except HelperError as e:
            return _fail(self, e)
        _frame_all(context)
        return {"FINISHED"}


def _frame_all(context):
    from . import graph_canvas
    try:
        if graph_canvas.shows_here(context) and context.region is not None:
            graph_canvas.frame_view(context, everything=True)
    except (RuntimeError, AttributeError, TypeError):
        pass


class GROOMLAB_OT_graph_promote(_Op):
    """Show this param on the compound you are in, to set it from outside without going in and out (again: no longer)"""
    bl_idname = "groomlab.graph_promote"
    bl_label = "Promote to Compound"

    node: StringProperty(options={"SKIP_SAVE"})
    param: StringProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        scalp = _scalp(context)
        tree = graph_tree.tree_of(scalp)
        try:
            graph_tree.promote_param(scalp, self.node, self.param, not graph_tree.promoted(tree, self.node, self.param))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# Ports: Pass Through as Output, and a pass-through output's name and place.

class GROOMLAB_OT_graph_pass_through(_Op):
    """Whatever comes into this input also comes out of the node as an extra output (thru_<input>)"""
    bl_idname = "groomlab.graph_pass_through"
    bl_label = "Pass Through as Output"

    def execute(self, context):
        port = target.get("port")
        if not port:
            return {"CANCELLED"}
        tree = _tree(context)
        on = port["name"] not in graph_tree.pass_through(tree, port["node"])
        try:
            graph_tree.set_pass_through(_scalp(context), port["node"], port["name"], on)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_rename_output(_Op):
    """The name this pass-through output shows (empty: thru_<input>); links still read it as before"""
    bl_idname = "groomlab.graph_rename_output"
    bl_label = "Rename Output"

    label: StringProperty(name="Name")

    def invoke(self, context, event):
        port = target.get("port")
        if not port:
            return {"CANCELLED"}
        self.label = graph_tree.pass_through_name(_tree(context), port["node"], port["passes"]) or "thru_" + port["passes"]
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        port = target.get("port")
        if not port:
            return {"CANCELLED"}
        label = self.label.strip()
        if label == "thru_" + port["passes"]:
            label = ""
        try:
            graph_tree.set_pass_through_name(_scalp(context), port["node"], port["passes"], label)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_move_output(_Op):
    """This pass-through output one place up or down among the node's outputs (its links stay)"""
    bl_idname = "groomlab.graph_move_output"
    bl_label = "Move Output"

    by: IntProperty(default=1, options={"SKIP_SAVE"})

    def execute(self, context):
        port = target.get("port")
        if not port:
            return {"CANCELLED"}
        try:
            graph_tree.move_pass_through(_scalp(context), port["node"], port["passes"], self.by)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


def _redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "NODE_EDITOR":
                area.tag_redraw()


# ---- The menus the canvas opens (graph_ui.json's linkMenu, portalMenu, portMenu, outputPortMenu) -----

def port_state(tree):
    """A pass-through output's menu state: where it can move."""
    port = target.get("port") or {}
    passes = graph_tree.pass_through(tree, port.get("node", "")) if port else []
    state = set()
    if port.get("passes") in passes:
        at = passes.index(port["passes"])
        if at > 0:
            state.add("port.canMoveUp")
        if at + 1 < len(passes):
            state.add("port.canMoveDown")
    return state


class GROOMLAB_MT_graph_canvas_link(bpy.types.Menu):
    """A link's menu on the canvas"""
    bl_label = "Link"

    def draw(self, context):
        graph_ui.draw(self.layout, "graph/linkMenu", graph_ui.MenuContext(tree=_tree(context)), "link")


class GROOMLAB_MT_graph_canvas_portal(bpy.types.Menu):
    """A portal dot's menu on the canvas"""
    bl_label = "Portal"

    def draw(self, context):
        dot = target.get("dot") or {}
        graph_ui.draw(self.layout, "graph/portalMenu", graph_ui.MenuContext(tree=_tree(context)), "portal",
                      set() if dot.get("exit") is None else {"portal.exit"})


class GROOMLAB_MT_graph_canvas_port(bpy.types.Menu):
    """A port's menu on the canvas"""
    bl_label = "Port"

    def draw(self, context):
        port = target.get("port") or {}
        tree = _tree(context)
        if port.get("passes"):
            graph_ui.draw(self.layout, "graph/outputPortMenu", graph_ui.MenuContext(tree=tree), "outputPort", port_state(tree))
        else:
            graph_ui.draw(self.layout, "graph/portMenu", graph_ui.MenuContext(tree=tree), "port")


def bind_actions():
    bind = graph_ui.bind
    for mode in ALIGN_MODES:
        bind("align." + mode, GROOMLAB_OT_graph_align.bl_idname, lambda ctx, item, m=mode: {"mode": m})
    bind("align.tidy", "groomlab.graph_tidy")
    bind("graph.wireStyle", GROOMLAB_OT_graph_wire_style.bl_idname, lambda ctx, item: {"style": item["value"]},
         lambda ctx, item: wire_style() == item["value"])
    bind("backdrop.fold", GROOMLAB_OT_graph_backdrop_fold.bl_idname, lambda ctx, item: {"frame": ctx.frame or ""},
         lambda ctx, item: bool(ctx.tree is not None and ctx.frame and ctx.tree.nodes.get(ctx.frame) is not None
                                and ctx.tree.nodes[ctx.frame].get("gl_collapsed")))
    bind("link.teleport", GROOMLAB_OT_graph_link_teleport.bl_idname)
    bind("link.pin", GROOMLAB_OT_graph_link_pin.bl_idname)
    bind("link.delete", GROOMLAB_OT_graph_link_delete.bl_idname)
    bind("portal.rename", GROOMLAB_OT_graph_portal_rename.bl_idname)
    bind("portal.removeExit", GROOMLAB_OT_graph_portal_remove.bl_idname, lambda ctx, item: {"whole": False})
    bind("portal.delete", GROOMLAB_OT_graph_portal_remove.bl_idname, lambda ctx, item: {"whole": True})
    bind("port.passThrough", GROOMLAB_OT_graph_pass_through.bl_idname, None,
         lambda ctx, item: bool(target.get("port")) and target["port"]["name"] in graph_tree.pass_through(
             ctx.tree, target["port"]["node"]))
    bind("port.renameOutput", GROOMLAB_OT_graph_rename_output.bl_idname)
    bind("node.openCompound", GROOMLAB_OT_graph_enter_compound.bl_idname, lambda ctx, item: {"node": ctx.node or ""})
    bind("graph.up", GROOMLAB_OT_graph_up.bl_idname)
    bind("param.promote", GROOMLAB_OT_graph_promote.bl_idname, lambda ctx, item: {"node": ctx.node or "", "param": ctx.param or ""},
         lambda ctx, item: graph_tree.promoted(ctx.tree, ctx.node, ctx.param))
    bind("port.outputUp", GROOMLAB_OT_graph_move_output.bl_idname, lambda ctx, item: {"by": -1})
    bind("port.outputDown", GROOMLAB_OT_graph_move_output.bl_idname, lambda ctx, item: {"by": 1})


CLASSES = (GROOMLAB_OT_graph_align, GROOMLAB_OT_graph_wire_style, GROOMLAB_OT_graph_backdrop_fold,
           GROOMLAB_OT_graph_backdrop_edit,
           GROOMLAB_OT_graph_link_teleport, GROOMLAB_OT_graph_link_pin, GROOMLAB_OT_graph_link_delete,
           GROOMLAB_OT_graph_portal_rename, GROOMLAB_OT_graph_portal_remove, GROOMLAB_OT_graph_pass_through,
           GROOMLAB_OT_graph_rename_output, GROOMLAB_OT_graph_move_output, GROOMLAB_OT_graph_enter_compound,
           GROOMLAB_OT_graph_up, GROOMLAB_OT_graph_promote, GROOMLAB_MT_graph_canvas_link,
           GROOMLAB_MT_graph_canvas_portal, GROOMLAB_MT_graph_canvas_port)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bind_actions()


def unregister():
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)
    target.clear()
