# SPDX-License-Identifier: GPL-3.0-or-later
"""The GroomLab Graph Editor in Blender's node editor, after Maya's GroomLab Graph Editor: the
groom's graph as nodes with typed, colored ports (graph_tree.py keeps it in step with the helper),
the selected node's params in the sidebar's Node tab (collapsible groups, Advanced folded,
showWhen, units, Upgrade, notes and errors, each param's menu), presets (apply, or insert as a
Compound), View (a node's strands in the viewport instead of the output's), Maya's menus from the
shared UI definitions (graph_ui.py: node, backdrop, Edit, Add, Presets, param), and Maya's keys:
Tab adds, F frames, V views, B bypasses, Ctrl+D duplicates, Ctrl+G groups, Ctrl+Shift+G ungroups,
Delete deletes. The authoring itself is graph_authoring.py's."""

import json
import re

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty

from . import graph, graph_authoring, graph_tree, graph_ui, groom, process, units
from .graph_tree import TREE, ours
from .graph_ui import ActionTip, MenuContext, action_prop, bind
from .helper_client import HelperError, sentence

_folded = {}       # (operator type, section) -> folded, for the session (Maya: unset = Advanced folded)
_deferred = {}     # key -> (fn, args): run once from a timer (not while Blender draws)
_pinned_spaces = set()  # the node editors (as_pointer) pinned when last drawn: unpinned, they follow again
_status = {}       # scalp uid -> {"serial", "times"}: the node timings the overlay shows
_channels = {}     # scalp uid -> {"graph", "channels"}: the attributes per node (graph_channels)
_catalog = {"presets": [], "categories": []}


# ---- Helpers ------------------------------------------------------------------------------------

def defer(key, fn, *args):
    """Runs fn(*args) once soon, outside drawing (a panel may not change data while it draws)."""
    if key in _deferred:
        return
    _deferred[key] = (fn, args)
    if bpy.app.background:
        return
    if not bpy.app.timers.is_registered(_run_deferred):
        bpy.app.timers.register(_run_deferred, first_interval=0.0)


def _run_deferred():
    jobs = list(_deferred.values())
    _deferred.clear()
    for fn, args in jobs:
        try:
            fn(*args)
        except HelperError as e:
            print("GroomLab: %s" % e)
        except Exception as e:  # noqa: BLE001 - a timer must not raise
            print("GroomLab: %r" % e)
    graph_tree._redraw()
    return None


def editor_tree(context):
    """The GroomLab graph the node editor shows (None elsewhere)."""
    space = getattr(context, "space_data", None)
    if space is None or space.type != "NODE_EDITOR" or space.tree_type != TREE:
        return None
    tree = space.edit_tree
    return tree if graph_tree.is_graph_tree(tree) else None


def context_groom(context):
    """The groom an operator works on: the node editor's, else the scene's (the panels')."""
    tree = editor_tree(context)
    if tree is not None and groom.is_groom(tree.gl_scalp):
        return tree.gl_scalp
    from .ops import active_groom
    return active_groom(context)


def context_tree(context):
    tree = editor_tree(context)
    if tree is not None:
        return tree
    return graph_tree.tree_of(context_groom(context))


def active_node(context):
    tree = context_tree(context)
    node = tree.nodes.active if tree is not None else None
    return node if node is not None and ours(node) and node.gl_name else None


def selected_nodes(context):
    tree = context_tree(context)
    return [n for n in tree.nodes if ours(n) and n.select and n.gl_name] if tree is not None else []


def wrap(layout, text, icon="NONE", width=44):
    words, line, first = text.split(), "", True
    for w in words:
        if line and len(line) + len(w) + 1 > width:
            layout.label(text=line, icon=icon if first else "BLANK1")
            line, first = w, False
        else:
            line = (line + " " + w).strip()
    if line:
        layout.label(text=line, icon=icon if first else "BLANK1")


def node_errors(scalp):
    """{node: error} from the last evaluation (Maya's: "node 'x': ...")."""
    rt = groom.runtime_if_any(scalp)
    out = {}
    for e in (rt.errors if rt is not None else []):
        m = re.match(r"node '([^']+)': (.*)", e, re.S)
        if m:
            out.setdefault(m.group(1), m.group(2))
    return out


def node_channels(scalp):
    """The attributes per node as the graph is set (the helper's graph_channels): {node: {"incoming",
    "writes", "overwrites", "reads"}}, fetched again (outside drawing) when the graph changed."""
    rt = groom.runtime_if_any(scalp)
    if rt is None or rt.session is None:
        return {}
    uid = scalp.groomlab.uid
    text = scalp.groomlab.graph_json
    cached = _channels.get(uid)
    if cached is None or cached["graph"] != text:
        def fetch():
            if rt.session is None or rt.generation != process.generation():
                return
            _channels[uid] = {"graph": text, "channels": process.call("graph_channels", session=rt.session).get("channels", {})}
        defer(("channels", uid), fetch)
        return cached["channels"] if cached else {}
    return cached["channels"]


def attribute_notes(scalp):
    """{node: note} for each node reading an attribute no node upstream writes (not an optional default)."""
    notes = {}
    for name, info in node_channels(scalp).items():
        missing = [r for r in info.get("reads", []) if not r.get("reaches") and not r.get("optional")]
        if missing:
            notes[name] = "; ".join("reads the attribute %s (%s), which no node upstream writes: it is ignored"
                                    % (r["name"], r["param"]) for r in missing)
    return notes


def node_notes(scalp):
    rt = groom.runtime_if_any(scalp)
    notes = dict(rt.notes) if rt is not None else {}
    for name, note in attribute_notes(scalp).items():
        notes[name] = (notes[name] + "; " + note) if notes.get(name) else note
    tree = graph_tree.tree_of(scalp)
    if tree is not None:
        for n in tree.nodes:
            if ours(n) and n.gl_type in groom.UNSUPPORTED:
                notes[n.gl_name] = (notes.get(n.gl_name, "") + " " + groom.UNSUPPORTED[n.gl_type]).strip()
    return notes


def show_groom(context, scalp):
    """The scene's groom changed: the node editors showing a GroomLab graph show this groom's
    (a pinned one stays on its own)."""
    if not groom.is_groom(scalp):
        return
    wm = getattr(context, "window_manager", None) or bpy.context.window_manager
    spaces = []
    for window in (wm.windows if wm is not None else []):
        for area in window.screen.areas:
            if area.type == "NODE_EDITOR":
                space = area.spaces.active
                if space.tree_type == TREE and not space.pin:
                    spaces.append((area, space))
    if not spaces:
        return
    tree = graph_tree.tree_of(scalp)
    if tree is None or not len(tree.nodes):
        try:
            tree = graph_tree.rebuild(scalp)
        except HelperError as e:
            print("GroomLab: %s" % e)
            return
    for area, space in spaces:
        if space.node_tree != tree:
            space.node_tree = tree
        area.tag_redraw()


def _follow_editor(tree):
    """The node editor shows another groom's graph (picked in its tree list): the scene follows."""
    scalp = tree.gl_scalp
    scene = bpy.context.scene
    if groom.is_groom(scalp) and scene.groomlab.groom != scalp:
        from . import props
        with props.quiet():
            scene.groomlab.groom = scalp


def _refresh_stale(tree):
    """A tree from a .blend just opened (or the helper restarted): made again from the groom."""
    scalp = tree.gl_scalp
    if groom.is_groom(scalp):
        graph_tree.rebuild(scalp)


def check_tree(tree):
    """While drawing: the tree is current, else it is rebuilt soon (a file just opened, nodes of
    types not registered yet, another helper)."""
    if tree is None or not groom.is_groom(tree.gl_scalp):
        return False
    stale = tree.get("gl_generation") != process.generation() or any(
        n.bl_idname == "NodeUndefined" for n in tree.nodes)
    if stale:
        defer(("stale", tree.name), _refresh_stale, tree)
    return not stale


# ---- Operators ----------------------------------------------------------------------------------

def foreign_editor(context):
    """Whether the context is a node editor showing another kind of tree (its keys are Blender's)."""
    space = getattr(context, "space_data", None)
    return space is not None and getattr(space, "type", "") == "NODE_EDITOR" and getattr(space, "tree_type", "") != TREE


class _GraphOperator(ActionTip, bpy.types.Operator):
    action: action_prop()  # the menu entry it runs (its tooltip: graph_ui.ActionTip)

    @classmethod
    def poll(cls, context):
        return not foreign_editor(context) and context_groom(context) is not None


class _EditorOperator(ActionTip, bpy.types.Operator):
    """Keys of the GroomLab Graph Editor: only there (elsewhere Blender's own keys act)."""

    action: action_prop()

    @classmethod
    def poll(cls, context):
        return editor_tree(context) is not None


def _fail(op, e):
    op.report({"ERROR"}, sentence(e.message if isinstance(e, HelperError) else str(e)))
    return {"CANCELLED"}


class GROOMLAB_OT_graph_open(_GraphOperator):
    """Open the groom's graph in the GroomLab Graph Editor (a node editor)"""
    bl_idname = "groomlab.graph_open"
    bl_label = "Graph Editor"

    new_window: BoolProperty(name="New Window", default=True, options={"SKIP_SAVE"})

    def execute(self, context):
        scalp = context_groom(context)
        try:
            tree = graph_tree.rebuild(scalp)
        except HelperError as e:
            return _fail(self, e)
        screen = getattr(context, "screen", None)
        if screen is None or bpy.app.background:
            return {"FINISHED"}  # no interface: the tree is made
        area = next((a for a in screen.areas if a.type == "NODE_EDITOR" and a.spaces.active.tree_type == TREE), None)
        if area is None and self.new_window:
            before = set(context.window_manager.windows[:])
            try:
                bpy.ops.wm.window_new()
            except RuntimeError:
                pass
            window = next((w for w in context.window_manager.windows if w not in before), None)
            if window is not None:
                area = window.screen.areas[0]
        if area is None:
            area = context.area
        if area is None:
            return {"FINISHED"}
        area.type = "NODE_EDITOR"
        area.ui_type = TREE
        space = area.spaces.active
        space.node_tree = tree
        space.show_region_ui = True
        return {"FINISHED"}


class GROOMLAB_OT_graph_add_search(_EditorOperator):
    """Find a node type and add it (Tab, as in Maya's Graph Editor)"""
    bl_idname = "groomlab.graph_add_search"
    bl_label = "Add Node (Search)"

    def invoke(self, context, event):
        bpy.ops.wm.search_single_menu("INVOKE_DEFAULT", menu_idname="NODE_MT_add")
        return {"FINISHED"}


class GROOMLAB_OT_graph_frame(_EditorOperator):
    """Fit the selected nodes in the view, or the whole graph when none are selected (F)"""
    bl_idname = "groomlab.graph_frame"
    bl_label = "Frame"

    def execute(self, context):
        if any(n.select for n in editor_tree(context).nodes):
            return bpy.ops.node.view_selected()
        return bpy.ops.node.view_all()


class GROOMLAB_OT_graph_duplicate(_EditorOperator):
    """Duplicate the selected nodes (Ctrl+D): each a node of its own in the groom, with the same params"""
    bl_idname = "groomlab.graph_duplicate"
    bl_label = "Duplicate"

    def invoke(self, context, event):
        bpy.ops.node.duplicate_move("INVOKE_DEFAULT")
        return {"FINISHED"}


def _strands_output(node):
    return len(node.outputs) and node.outputs[0].bl_idname in ("GroomLabSocketStrands", "GroomLabSocketCurves")


def _viewable(node):
    """Focus shows a node giving strands (a Clump's centers as dots)."""
    return bool(_strands_output(node))


class GROOMLAB_OT_graph_view_node(_GraphOperator):
    """Focus: show the groom up to this node in the viewport (V); renders keep the output. Again: back to the output"""
    bl_idname = "groomlab.graph_view_node"
    bl_label = "Focus: View Up to Here"

    node: StringProperty(options={"SKIP_SAVE"}, description="The node (empty: the active one)")
    clear: BoolProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        scalp = context_groom(context)
        if self.clear:
            graph_tree.view_node(scalp, "")
            return {"FINISHED"}
        name = self.node or (active_node(context).gl_name if active_node(context) else "")
        tree = graph_tree.tree_of(scalp)
        node = next((n for n in tree.nodes if ours(n) and n.gl_name == name), None) if tree else None
        if node is None:
            self.report({"INFO"}, "No node selected: select the node to view, then press V")
            return {"CANCELLED"}
        if not _viewable(node):
            self.report({"INFO"}, "Nothing to view: %s gives no strands. Pick a node that outputs strands or curves" % name)
            return {"CANCELLED"}
        graph_tree.view_node(scalp, "" if scalp.groomlab.view_node == name else name)
        if scalp.groomlab.view_node:
            self.report({"INFO"}, "Viewing %s (renders use the output)" % name)
        return {"FINISHED"}


class GROOMLAB_OT_graph_bypass(_GraphOperator):
    """Bypass: pass the selected nodes' input straight through (B; Blender's M mutes the same way)"""
    bl_idname = "groomlab.graph_bypass"
    bl_label = "Bypass"
    bl_options = {"REGISTER", "UNDO"}

    node: StringProperty(options={"SKIP_SAVE"}, description="The node (empty: the selected ones)")

    def execute(self, context):
        scalp = context_groom(context)
        names = [self.node] if self.node else [n.gl_name for n in selected_nodes(context)]
        if not names:
            return {"CANCELLED"}
        tree = graph_tree.tree_of(scalp)
        bypassed = {n.gl_name for n in tree.nodes if ours(n) and n.mute} if tree else set()
        try:
            graph_tree.set_bypass(scalp, names, not all(n in bypassed for n in names))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_evaluation(_GraphOperator):
    """When the node runs: always, only in the viewport (renders and exports skip it) or only in
    renders (the viewport skips it)"""
    bl_idname = "groomlab.graph_evaluation"
    bl_label = "Evaluate"
    bl_options = {"REGISTER", "UNDO"}

    node: StringProperty(options={"SKIP_SAVE"}, description="The node (empty: the selected ones)")
    mode: EnumProperty(items=(("always", "Always", "In the viewport and in renders"),
                              ("viewportOnly", "Viewport Only", "Renders, exports and the CLI skip it"),
                              ("renderOnly", "Render Only", "The viewport skips it")),
                       default="always")

    def execute(self, context):
        scalp = context_groom(context)
        names = [self.node] if self.node else [n.gl_name for n in selected_nodes(context)]
        if not names:
            return {"CANCELLED"}
        try:
            graph_tree.set_evaluation(scalp, names, self.mode)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_set_output(_GraphOperator):
    """Make this node the groom's output: what it shows and renders"""
    bl_idname = "groomlab.graph_set_output"
    bl_label = "Set as Output"
    bl_options = {"REGISTER", "UNDO"}

    node: StringProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        name = self.node or (active_node(context).gl_name if active_node(context) else "")
        if not name:
            return {"CANCELLED"}
        try:
            graph_tree.set_output(context_groom(context), name)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_upgrade_node(_GraphOperator):
    """Upgrade: the node evaluates as new nodes of its type do (its result may change)"""
    bl_idname = "groomlab.graph_upgrade_node"
    bl_label = "Upgrade"
    bl_options = {"REGISTER", "UNDO"}

    node: StringProperty(options={"SKIP_SAVE"})

    @classmethod
    def description(cls, context, properties):
        tree = context_tree(context) if context is not None else None
        node = next((n for n in tree.nodes if ours(n) and n.gl_name == properties.node), None) if tree else None
        if node is None:
            return cls.__doc__
        newest = node.gl_spec.get("version", 1)
        return ("This %s keeps the behaviour of version %d, as it was saved; %s nodes are at version %d now. "
                "Upgrade: it evaluates as new %s nodes do (its result may change)"
                % (node.gl_type, node.gl_version, node.gl_type, newest, node.gl_type))

    def execute(self, context):
        name = self.node or (active_node(context).gl_name if active_node(context) else "")
        try:
            done = graph_tree.upgrade_node(context_groom(context), name)
        except HelperError as e:
            return _fail(self, e)
        if not done:
            self.report({"INFO"}, "Already the newest version: %s" % name)
        return {"FINISHED"}


class GROOMLAB_OT_graph_fold(bpy.types.Operator):
    """Fold or open this section of every node of this type (for the session)"""
    bl_idname = "groomlab.graph_fold"
    bl_label = "Fold Section"

    op_type: StringProperty()
    section: StringProperty()

    def execute(self, context):
        _folded[(self.op_type, self.section)] = not is_folded(self.op_type, self.section)
        return {"FINISHED"}


def is_folded(op_type, section):
    return _folded.get((op_type, section), section == graph_tree.ADVANCED)


class GROOMLAB_OT_graph_tidy(_GraphOperator):
    """Lay the graph out again: columns from the sources to the output"""
    bl_idname = "groomlab.graph_tidy"
    bl_label = "Tidy Layout"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            graph_tree.tidy(context_groom(context))
        except HelperError as e:
            return _fail(self, e)
        if editor_tree(context) is not None:
            bpy.ops.node.view_all()
        return {"FINISHED"}


class GROOMLAB_OT_graph_reload(_GraphOperator):
    """Make the editor's graph again from the groom (its layout too)"""
    bl_idname = "groomlab.graph_reload"
    bl_label = "Reload from Groom"

    def execute(self, context):
        scalp = context_groom(context)
        try:
            graph.refresh(scalp)
            graph_tree.rebuild(scalp, replaced=True)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


def preset_catalog(refresh=False):
    """{"presets": [{name, title, description, category}], "categories": [...]} (cached)."""
    if refresh or not _catalog["presets"]:
        r = process.call("list_presets")
        _catalog["presets"] = r.get("presets", [])
        _catalog["categories"] = r.get("categories", [])
        graph.presets(refresh=True)  # the 3D view's browser lists them too
    return _catalog


def preset_info(name):
    return next((p for p in _catalog["presets"] if p.get("name") == name), None)


def preset_categories():
    """The categories in the core's order, then the others; "" (your own presets) last."""
    found = []
    for p in _catalog["presets"]:
        c = p.get("category") or ""
        if c not in found:
            found.append(c)
    order = [c for c in _catalog["categories"] if c in found]
    order += sorted(c for c in found if c and c not in order)
    if "" in found:
        order.append("")
    return order


class GROOMLAB_OT_graph_preset(_GraphOperator):
    """Apply a preset, or insert its look after the selected node as one Compound"""
    bl_idname = "groomlab.graph_preset"
    bl_label = "Preset"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty()
    mode: EnumProperty(items=[("APPLY", "Apply", "Replace this groom's graph with the preset, sizes fitted to this scalp"),
                              ("INSERT", "Insert after selected node",
                               "Keep this graph and add the preset's look to it as one Compound after the selected node "
                               "(before the Output when none is selected), fitted to this scalp")])

    @classmethod
    def description(cls, context, properties):
        info = preset_info(properties.name) or {}
        what = info.get("description") or info.get("title") or properties.name
        how = ("Insert after the selected node as one Compound (before the Output when none is selected), fitted to "
               "this scalp" if properties.mode == "INSERT" else
               "Apply: replace this groom's graph with the preset, sizes fitted to this scalp")
        return "%s.\n%s" % (what.rstrip("."), how)

    def execute(self, context):
        scalp = context_groom(context)
        try:
            if self.mode == "APPLY":
                r = graph.apply_preset(scalp, self.name)
                note = graph.preset_maps_note(r)
                if note:
                    self.report(*note)
                if r.get("drawGroup"):
                    graph.draw_into(context, scalp, r["drawGroup"])  # a hand-placed look: draw its guides
            else:
                node = active_node(context)
                after = node.gl_name if node is not None and node.select and not node.gl_is_output and _strands_output(node) else None
                r = graph.insert_preset(scalp, self.name, after=after)
                made = r.get("node")
                tree = graph_tree.tree_of(scalp)
                if made and tree is not None:
                    for n in tree.nodes:
                        n.select = ours(n) and n.gl_name == made
                        if n.select:
                            tree.nodes.active = n
                self.report({"INFO"}, "Inserted %s as %s after %s" % (self.name, made, after or "the last node"))
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


class GROOMLAB_OT_graph_presets_refresh(bpy.types.Operator):
    """List the shipped and user presets again"""
    bl_idname = "groomlab.graph_presets_refresh"
    bl_label = "Refresh Presets"

    def execute(self, context):
        try:
            preset_catalog(refresh=True)
        except HelperError as e:
            return _fail(self, e)
        return {"FINISHED"}


# ---- Menus: the UI definitions' (engine/ui/graph_ui.json), as Maya's Graph Editor builds them -----

def menu_context(context, node=None, param=None, frame=None):
    return MenuContext(scalp=context_groom(context), tree=context_tree(context),
                       node=node.gl_name if node is not None else None, param=param,
                       frame=frame.name if frame is not None else None)


def node_state(node):
    """The node menu's conditions (Maya's NodeItem.menu_state) and the values its labels name."""
    state = set()
    if _viewable(node):
        state.add("node.viewable")
    if node.gl_type == "Sculpt":
        state.add("node.sculpt")
    if len(node.outputs) and node.outputs[0].bl_idname in ("GroomLabSocketStrands", "GroomLabSocketGuides") \
            and node.gl_type != "Output":
        state.add("node.bakeable")
    if graph_authoring.linkable(node):
        state.add("node.mappable")
    if node.gl_type in ("Script", "Compound"):
        state.add("node." + node.gl_type.lower())
    info = graph_authoring.library_info(node)
    if info:
        state.add("node.library")
        if info.get("updateAvailable"):
            state.add("node.updateAvailable")
    return state, {"latest": info.get("latest", "")}


def node_generators(node):
    """The node menu's generated entries: the params it can expose, a compound's inner params."""
    def expose(entry):
        return [{"id": entry["action"], "value": p, "label": p, "tip": ""} for p in graph_authoring.linkable(node)]

    def inner(entry):
        return [{"submenu": name, "items": [{"id": entry["action"], "value": "%s.%s" % (name, p["name"]),
                                              "label": graph_tree.pretty(p["name"]), "tip": p.get("description", "")}
                                             for p in specs]}
                for name, specs in graph_authoring.inner_params(node)]
    return {"exposeParams": expose, "innerParams": inner}


def draw_node_menu(layout, context, node):
    """The node's right-click menu (graph_ui.json's nodeMenu): in the context menu and the sidebar."""
    state, values = node_state(node)
    return graph_ui.draw(layout, "graph/nodeMenu", menu_context(context, node), "node", state, values,
                         node_generators(node))


def draw_backdrop_menu(layout, context, frame):
    return graph_ui.draw(layout, "graph/backdropMenu", menu_context(context, frame=frame), "backdrop")


def draw_node_actions(layout, context):
    node = active_node(context)
    if node is not None:
        draw_node_menu(layout, context, node)


def draw_pinned_actions(layout, context, node):
    """The node's actions by its type (graph_ui.json's nodeActions: the Cards exports, Simulate's
    range, bake and clear, Bake Node to Guides, a Sculpt node's Clear, a compound's), two to a row
    at the top of its panel, as Maya and the app pin them. Returns the ids drawn."""
    from . import actions, menu
    state, values = node_state(node)
    state.add("type." + node.gl_type)
    container = graph_ui.get("graph/nodeActions", {}) or {}
    items = [i for i in graph_ui.resolve(container.get("items", []), state, values) if i["kind"] == "action"]
    items = [i for i in items if i["id"] in graph_ui.ACTIONS or i["id"] in actions.BINDINGS]
    if not items:
        return []
    col = layout.column(align=True)
    ctx = menu_context(context, node)
    row = None
    for i, item in enumerate(items):
        if i % 2 == 0:
            row = col.row(align=True)
        if item["id"] in graph_ui.ACTIONS:
            graph_ui.draw_prepared(row, graph_ui.prepare([item], ctx, "nodeActions"))
        else:
            menu.draw_action(row, item, context)
    col.separator()
    return [i["id"] for i in items]


def _preset_entries(entry):
    """The Presets menu's presets (its "presets" generator): a submenu per category (your own last,
    not in one), each preset a submenu of Apply and Insert After Selected Node (presetItem)."""
    items = graph_ui.get("graph/presetItem", [])

    def preset(p):
        return {"submenu": p.get("title") or p["name"],
                "items": [e if e == "-" else {"id": e, "value": p["name"]} for e in items]}

    out = []
    for category in preset_categories():
        presets = sorted((p for p in _catalog["presets"] if (p.get("category") or "") == category),
                         key=lambda p: (p.get("title") or p["name"]).lower())
        if category:
            out.append({"submenu": category, "items": [preset(p) for p in presets]})
        else:
            out.append("-")
            out += [preset(p) for p in presets]
    return out


class GROOMLAB_MT_graph_presets(bpy.types.Menu):
    """Start from a look (hair, fur, feathers), or a technique to learn from"""
    bl_label = "Presets"

    def draw(self, context):
        layout = self.layout
        if not _catalog["presets"]:
            layout.operator("groomlab.graph_presets_refresh", text="List Presets", icon="FILE_REFRESH")
        graph_ui.draw(layout, "graph/presetMenu", menu_context(context, active_node(context)), "presets",
                      {"groom"}, generators={"presets": _preset_entries})
        if _catalog["presets"]:
            layout.separator()
            layout.operator("groomlab.graph_presets_refresh", icon="FILE_REFRESH")


class GROOMLAB_MT_graph_edit(bpy.types.Menu):
    """Undo, framing and layout, group into or out of compounds, copy and paste the graph"""
    bl_label = "Edit"

    def draw(self, context):
        # (Units and themes are Blender's own: their generators give nothing here.)
        graph_ui.draw(self.layout, "graph/editMenu", menu_context(context, active_node(context)), "edit",
                      {"groom"}, generators={"units": lambda e: [], "themeMenu": lambda e: []})


def draw_context_menu(self, context):
    tree = editor_tree(context)
    if tree is None:
        return
    layout = self.layout
    layout.separator()
    active = tree.nodes.active
    if active is not None and active.bl_idname == "NodeFrame" and active.select:
        draw_backdrop_menu(layout, context, active)
    else:
        draw_node_actions(layout, context)


def draw_header(self, context):
    tree = editor_tree(context)
    if tree is None:
        return
    layout = self.layout
    scene = context.scene
    space = context.space_data
    pinned = bool(getattr(space, "pin", False))
    # Unpinned, the editor shows the current groom (the Groom Tools'): another tree picked here makes
    # its groom current. Pinned, it stays on its groom while the Groom Tools move on.
    key = space.as_pointer() if hasattr(space, "as_pointer") else id(space)
    if pinned:
        _pinned_spaces.add(key)
    elif key in _pinned_spaces:  # just unpinned: back to the current groom
        _pinned_spaces.discard(key)
        defer(("unpinned", key), lambda: show_groom(bpy.context, bpy.context.scene.groomlab.groom))
    elif tree.gl_scalp is not None and scene.groomlab.groom != tree.gl_scalp:
        defer(("follow", tree.name), _follow_editor, tree)
    check_tree(tree)
    row = layout.row(align=True)
    if pinned:
        row.label(text=tree.gl_scalp.name if tree.gl_scalp is not None else tree.name, icon="CURVES")
    else:
        row.prop(scene.groomlab, "groom", text="", icon="CURVES")
    if hasattr(space, "pin"):
        row.prop(space, "pin", text="", icon="PINNED" if pinned else "UNPINNED", emboss=False)
    layout.menu("GROOMLAB_MT_graph_edit")
    layout.menu("GROOMLAB_MT_graph_presets")
    scalp = tree.gl_scalp
    if scalp is not None and scalp.groomlab.view_node:
        op = layout.operator("groomlab.graph_view_node", text="Viewing %s" % scalp.groomlab.view_node,
                             icon="HIDE_OFF", depress=True)
        op.clear = True


def _n(ctx, _item):
    return {"node": ctx.node or ""}


def _tree_node(ctx):
    tree = ctx.tree
    return next((n for n in tree.nodes if ours(n) and n.gl_name == ctx.node), None) if tree is not None and ctx.node else None


def _bind_editor():
    """The node, Edit and Presets menus' actions this module runs (graph_authoring binds the rest)."""
    bind("node.setOutput", "groomlab.graph_set_output", _n)
    bind("node.view", "groomlab.graph_view_node", _n,
         lambda ctx, item: ctx.scalp is not None and ctx.scalp.groomlab.view_node == ctx.node)
    bind("node.bypass", "groomlab.graph_bypass", _n,
         lambda ctx, item: bool(_tree_node(ctx) and _tree_node(ctx).mute))
    bind("node.evaluate", "groomlab.graph_evaluation", lambda ctx, item: {"node": ctx.node or "", "mode": item["value"]},
         lambda ctx, item: bool(_tree_node(ctx)) and _tree_node(ctx).gl_evaluation == item["value"])
    bind("node.duplicate", "groomlab.graph_duplicate")
    bind("graph.undo", "ed.undo")
    bind("graph.redo", "ed.redo")
    bind("graph.frame", "groomlab.graph_frame")
    bind("graph.frameAll", "node.view_all")
    bind("graph.tidy", "groomlab.graph_tidy")
    bind("graph.delete", "groomlab.graph_delete")
    bind("graph.reload", "groomlab.graph_reload")
    bind("preset.apply", "groomlab.graph_preset", lambda ctx, item: {"name": item["value"], "mode": "APPLY"})
    bind("preset.insert", "groomlab.graph_preset", lambda ctx, item: {"name": item["value"], "mode": "INSERT"})


# ---- The Add menu: Maya's categories ------------------------------------------------------------

def _add_categories():
    import nodeitems_utils

    class GroomLabCategory(nodeitems_utils.NodeCategory):
        @classmethod
        def poll(cls, context):
            space = getattr(context, "space_data", None)
            return space is not None and getattr(space, "tree_type", "") == TREE

    def items_of(category):
        def items(context):
            for cat, types in graph_tree.categories() if graph_tree._node_classes else ():
                if cat != category:
                    continue
                for t in types:
                    cls = graph_tree.node_class(t)
                    if cls is not None and t not in graph_tree.HIDDEN_TYPES:
                        yield nodeitems_utils.NodeItem(cls.bl_idname, label=graph_tree.label_of(t))
        return items

    def library_items(kind):
        # The node library's nodes of a kind: compound nodes (saved compounds: Maya's and the app's
        # Save as Node) and script nodes (Maya's and the app's script editor), each a Compound or a
        # Script holding the library's copy once added (sync_from_tree sends its type).
        def items(context):
            cls = graph_tree.node_class("Script" if kind == "script" else "Compound") if graph_tree._node_classes else None
            if cls is None:
                return
            for entry in graph.library_nodes():
                if entry.get("kind", "compound") != kind:
                    continue
                yield nodeitems_utils.NodeItem(cls.bl_idname, label=entry.get("label") or entry["id"],
                                               settings={"gl_library": repr(entry["type"])})
        return items

    def map_items(context):
        # The groom's painted maps, each a Map node reading it, named after it (Maya's Tab search
        # lists them the same way: MAP_ENTRY).
        cls = graph_tree.node_class("Map") if graph_tree._node_classes else None
        if cls is None:
            return
        for m in painted_maps(context_groom(context or bpy.context)):
            init = json.dumps({"name": m["name"], "params": {"name": m["name"]}})
            yield nodeitems_utils.NodeItem(cls.bl_idname, label="Map: %s" % m["name"], settings={"gl_init": repr(init)})

    names = list(graph_tree.CATEGORY_TITLES)
    categories = [GroomLabCategory("GROOMLAB_" + re.sub(r"\W", "_", c).upper(), c, items=items_of(c)) for c in names]
    categories.append(GroomLabCategory("GROOMLAB_COMPOUND_NODES", "Compound Nodes", items=library_items("compound")))
    categories.append(GroomLabCategory("GROOMLAB_SCRIPT_NODES", "Script Nodes", items=library_items("script")))
    categories.append(GroomLabCategory("GROOMLAB_PAINTED_MAPS", PAINTED_MAPS, items=map_items))
    return categories


PAINTED_MAPS = "Painted Maps"
CATEGORY_OF_MENU = {"Compound Nodes": "Compound", "Script Nodes": "Compound", PAINTED_MAPS: "Color"}
_icons = {"previews": None, "ids": {}}


def painted_maps(scalp):
    """The groom's painted maps ({name, kind}), Freeze's own mask left out; [] without a session."""
    rt = groom.runtime_if_any(scalp) if scalp is not None else None
    if rt is None or rt.session is None or not process.running():
        return []
    try:
        maps = process.call("maps_list", session=rt.session).get("maps", [])
    except HelperError:
        return []
    return [m for m in maps if not m.get("freezeMask")]


def _make_icons():
    """A swatch per category color (the Add menu's icons, Maya's category colors)."""
    try:
        from bpy.utils import previews as preview_utils
        previews = preview_utils.new()
    except Exception:  # noqa: BLE001 - no icons, the menu still works
        return
    for category, rgb in graph_tree.CATEGORY_COLORS.items():
        try:
            p = previews.new("category_" + category)
            p.image_size = (16, 16)
            pixels = []
            for y in range(16):
                for x in range(16):
                    edge = x in (0, 15) or y in (0, 15)
                    pixels += [c / 255.0 * (0.6 if edge else 1.0) for c in rgb] + [1.0]
            p.image_pixels_float = pixels
            _icons["ids"][category] = p.icon_id
        except Exception:  # noqa: BLE001
            pass
    _icons["previews"] = previews


def category_icon(title):
    return _icons["ids"].get(CATEGORY_OF_MENU.get(title, title), 0)


def _draw_add_menu(categories):
    """The Add menu (graph_ui.json's addMenu): the core's categories, each with its color, then the
    node library's New Script Node, Import Node..., Open Nodes Folder."""
    def draw(self, context):
        layout = self.layout
        shown = False
        for cat in categories:
            if cat.poll(context):
                layout.menu("NODE_MT_category_" + cat.identifier, icon_value=category_icon(cat.name))
                shown = True
        if not shown or editor_tree(context) is None:
            return
        layout.separator()
        graph_ui.draw(layout, "graph/addMenu", menu_context(context, active_node(context)), "add", {"catalog", "groom"},
                      generators={"nodeCategories": lambda e: []})
    return draw


def _register_add_menu():
    import nodeitems_utils
    categories = _add_categories()
    nodeitems_utils.register_node_categories("GROOMLAB_NODES", categories)
    registered = getattr(nodeitems_utils, "_node_categories", {}).get("GROOMLAB_NODES")
    if registered is not None and len(registered) == 3:  # (categories, draw, menu types): our draw instead
        nodeitems_utils._node_categories["GROOMLAB_NODES"] = (registered[0], _draw_add_menu(registered[0]), registered[2])


# ---- The sidebar: the selected node's params (Maya's param panel) -------------------------------

class GROOMLAB_PT_graph_node(bpy.types.Panel):
    bl_space_type = "NODE_EDITOR"
    bl_region_type = "UI"
    bl_category = "Node"
    bl_label = "GroomLab Node"

    @classmethod
    def poll(cls, context):
        return editor_tree(context) is not None

    def draw(self, context):
        tree = editor_tree(context)
        layout = self.layout
        if not check_tree(tree):
            layout.label(text="Reading the groom's graph...", icon="TIME")
            return
        node = tree.nodes.active
        if node is not None and node.bl_idname == "NodeFrame" and node.select:
            draw_backdrop_panel(layout, context, node)
            return
        if node is None or not ours(node) or not node.select:
            wrap(layout, "Select a node to edit it. Shift+A or Tab adds one; drag from a port to wire it.")
            return
        draw_node_panel(layout, context, node)


def draw_backdrop_panel(layout, context, frame):
    """A backdrop (a frame): its title, its color (Maya's), the backdrop menu."""
    layout.label(text="Backdrop", icon="SELECT_SET")
    layout.prop(frame, "label", text="Title")
    row = layout.row(align=True)
    for text, rgb in graph_ui.backdrop_colors():
        op = row.operator("groomlab.graph_backdrop_color", text=text)
        op.frame, op.color = frame.name, json.dumps(rgb)
    col = layout.column(align=True)
    col.separator()
    draw_backdrop_menu(col, context, frame)
    wrap(layout.column(), "Kept in the graph with its nodes' layout (Maya's backdrops): moving it moves the nodes in it.",
         icon="INFO")


def draw_node_panel(layout, context, node):
    tree = node.id_data
    scalp = tree.gl_scalp
    scene = context.scene
    # The header: the name (edit it to rename), the type, Upgrade when outdated, what it does.
    row = layout.row(align=True)
    row.prop(node, "name", text="")
    kind = "OUTPUT" if node.gl_is_output and node.gl_type == "Output" else node.gl_type
    row.label(text=kind, icon="NODE")
    if node.gl_outdated:
        row.operator("groomlab.graph_upgrade_node", text="Upgrade", icon="FILE_REFRESH").node = node.gl_name
    info = graph_authoring.library_info(node)
    if info.get("updateAvailable"):
        op = row.operator("groomlab.graph_update_node", text="Update", icon="SORT_DESC")
        op.node, op.action = node.gl_name, "node.update"
    # Its actions (bake, export, simulate...) at the top, in view before any param.
    draw_pinned_actions(layout.column(), context, node)
    about = (json.loads(node.gl_spec_json).get("description") if node.gl_spec_json else None) or node.gl_spec.get("description", "")
    if about:
        wrap(layout.column(), about)
    if info:
        line = "Library node %s, version %d" % (info.get("id", ""), int(info.get("version", 0)))
        if info.get("updateAvailable"):
            line += ": the library has version %d (Update Node)" % int(info.get("latest", 0))
        wrap(layout.column(), line, icon="ASSET_MANAGER")
    error = node_errors(scalp).get(node.gl_name) if scalp is not None else None
    if error:
        wrap(layout.box(), error, icon="ERROR")
    note = node_notes(scalp).get(node.gl_name) if scalp is not None else None
    if note:
        wrap(layout.column(), note, icon="INFO")
    if node.gl_type == "Script":
        draw_script_editor(layout, node)
    if node.gl_type == "Control" and scalp is not None:
        draw_control_slider(layout, node, scalp)
    draw_params(layout, context, node, scene)
    col = layout.column(align=True)
    col.separator()
    draw_node_actions(col, context)


def draw_script_editor(layout, node):
    """A script node being edited (its text blocks): whether it checks, its errors, Apply, Save to
    Library, Save as New, Done; else Edit Node..."""
    box = layout.box()
    if not graph_authoring.editing_script(node.gl_name):
        op = box.operator("groomlab.graph_script_edit", text=graph_ui.label("script.edit", "Edit Node..."), icon="TEXT")
        op.node, op.action = node.gl_name, "script.edit"
        return
    script_name, header_name = graph_authoring.script_text_names(node.gl_name)
    wrap(box, "Editing in the text blocks %s and %s (a Text Editor shows them)" % (script_name, header_name), icon="TEXT")
    errors = graph_authoring.script_errors_of(node.gl_name)
    if errors:
        for e in errors[:6]:
            where = " (line %d)" % e["line"] if e.get("line") else ""
            wrap(box, "%s%s: %s" % (e.get("part", "script"), where, e.get("message", "")), icon="ERROR")
    else:
        box.label(text="Checks: the node takes each change (live)", icon="CHECKMARK")
    row = box.row(align=True)
    row.operator("groomlab.graph_apply_text", text="Apply", icon="FILE_REFRESH").text = script_name
    op = row.operator("groomlab.graph_script_save", text="Save to Library")
    op.node, op.action = node.gl_name, "script.saveToLibrary"
    op = row.operator("groomlab.graph_script_save", text="Save as New")
    op.node, op.as_new = node.gl_name, True
    row.operator("groomlab.graph_script_done", text="Done").node = node.gl_name


def draw_control_slider(layout, node, scalp):
    """A Control node's slider: the groom's ctl_ property (keyable: I over it)."""
    name = graph_tree.values(node).get("name") or ""
    key = graph_authoring.CONTROL_PREFIX + name
    if name and key in scalp.keys():
        layout.prop(scalp, '["%s"]' % key, text="Slider (%s)" % name)


def param_state(node, spec, scalp):
    """A param menu's conditions (Maya's ParamPanel: keyable, linked, keyed, linkable, mappable,
    groom) and the values its texts name (why it cannot be keyed)."""
    name = spec["name"]
    state = {"groom"} if scalp is not None else set()
    values = {"why": ""}
    if spec.get("kind") in graph_authoring.KEYABLE_KINDS:
        state.add("param.keyable")
        why = graph_authoring.keyable_why(node, name, spec)
        linked = bool(graph_authoring.control_names(node, name))
        if linked or not why:
            state.add("param.canKey")
        if linked:
            state.add("param.linked")
            if scalp is not None and graph_authoring.key_state(scalp, node, name) == "keyed":
                state.add("param.keyed")
        values["why"] = why
    if spec.get("port"):
        state.add("param.linkable")
        if spec.get("mappable"):
            state.add("param.mappable")
    return state, values


def draw_param_menu(row, context, node, spec):
    """The param's menu (graph_ui.json's paramMenu: keys, Expose as Input, Paint, Drive with Slider)
    behind a small button at the end of its row."""
    scalp = node.id_data.gl_scalp
    state, values = param_state(node, spec, scalp)
    key = "param:%s:%s" % (node.gl_name, spec["name"])
    ctx = MenuContext(scalp=scalp, tree=node.id_data, node=node.gl_name, param=spec["name"])
    entries = graph_ui.prepare(graph_ui.resolve(graph_ui.get("graph/paramMenu", []), state, values), ctx, key)
    if not any(e[0] != "divider" for e in entries):
        return
    graph_ui._submenus[key] = (spec["name"], entries)
    row.context_string_set("groomlab_menu", key)
    row.menu(graph_ui.GROOMLAB_MT_graph_submenu.bl_idname, text="", icon="DOWNARROW_HLT")


KEY_ICONS = {"keyed": "DECORATE_KEYFRAME", "animated": "DECORATE_ANIMATE", "linked": "DECORATE_LINKED",
             "driven": "DECORATE_DRIVER"}


def draw_params(layout, context, node, scene):
    op_type = node.gl_type
    specs = graph_tree.node_param_specs(node)
    params = graph_tree.values(node)
    mapped = json.loads(node.gl_mapped or "{}")
    inputs = set(json.loads(node.gl_inputs or "[]"))
    drivers = json.loads(node.gl_drivers or "{}")
    collection = {p.name: p for p in node.gl_params} if node.gl_spec_json else None

    def value_of(param):
        spec = node.gl_param_specs.get(param) if collection is None else None
        if spec is not None and hasattr(node, graph_tree.prop_name(param)) and param not in mapped:
            return graph_tree.property_value(node, spec, scene)
        return params.get(param)

    spec_of = {s["name"]: s for s in specs}.get  # what a showWhen names
    shown = []
    for spec in specs:
        name = spec["name"]
        if op_type in ("Compound", "Script") and name == "definition":
            continue  # (a script node's file: Edit Node..., its text blocks)
        if (op_type, name) in graph_tree.PLUMBING and params.get(name, spec.get("default")) == spec.get("default"):
            continue
        shown.append(spec)
    if not shown:
        layout.label(text="No parameters.")
        return
    for section, members in graph_tree.param_sections(shown):
        visible = [s for s in members if graph_tree.shown_when(s, value_of, spec_of)]
        if not visible:
            continue
        col = layout.column()
        if section:
            folded = is_folded(op_type, section)
            op = col.operator("groomlab.graph_fold", text=section.upper(), icon="TRIA_RIGHT" if folded else "TRIA_DOWN",
                              emboss=False)
            op.op_type, op.section = op_type, section
            if folded:
                continue
        for spec in visible:
            draw_param(col, node, spec, scene, mapped, inputs, drivers, collection, context)


def _map_label(value):
    source = next((k for k in ("texture", "colorSet", "attribute", "expression", "guide") if k in value), None)
    label = {"texture": "texture", "colorSet": "color set", "attribute": "attribute", "expression": "expression",
             "guide": "painted map"}.get(source, "map")
    name = value.get(source) if source else ""
    if source == "expression" and isinstance(name, str):
        name = name.strip().splitlines()[0][:32] if name.strip() else ""
    return "%s %s" % (label, name if isinstance(name, str) else "")


def draw_mapped(layout, context, node, spec, value, label):
    """A param read from a map: its source named, edited (Map Source), its expression's text block
    and errors, Paint (a painted map), Clear (back to a value); the param menu."""
    name = spec["name"]
    col = layout.column(align=True)
    row = col.row(align=True)
    row.label(text="%s: %s" % (label, _map_label(value)), icon="IMAGE_DATA")
    op = row.operator("groomlab.graph_map_source", text="", icon="PREFERENCES")
    op.node, op.param = node.gl_name, name
    if "expression" in value:
        op = row.operator("groomlab.graph_edit_expression", text="", icon="TEXT")
        op.node, op.param = node.gl_name, name
    else:
        op = row.operator("groomlab.graph_paint_param", text="", icon="BRUSH_DATA")
        op.node, op.param = node.gl_name, name
    op = row.operator("groomlab.graph_map_source", text="", icon="X")
    op.node, op.param, op.clear = node.gl_name, name, True
    draw_param_menu(row, context, node, spec)
    text = bpy.data.texts.get(graph_authoring.expression_text_name(node.gl_name, name))
    if text is not None:
        errors = graph_authoring.text_errors(text)
        sub = col.row(align=True)
        sub.label(text=("Error: " + errors[0]["message"]) if errors else "Editing in %s (live)" % text.name,
                  icon="ERROR" if errors else "TEXT")
        sub.operator("groomlab.graph_apply_text", text="Apply").text = text.name


def draw_param(layout, node, spec, scene, mapped, inputs, drivers, collection, context=None):
    context = context or bpy.context
    name = spec["name"]
    unit = graph_tree._unit_of(node, spec)  # (unitWhen: as the node's values have it)
    label = graph_tree.pretty(name) + (" ◆" if name in inputs else "")
    suffix = units.suffix(unit, scene) if unit not in units.SCENE_LENGTH else ""
    driver = drivers.get(name)
    scalp = node.id_data.gl_scalp
    if name in mapped:
        draw_mapped(layout, context, node, spec, mapped[name], label)
        return
    if spec.get("kind") == "ramp" and collection is None:
        if driver:
            layout.label(text="%s: driven by %s" % (label, driver), icon="LINKED")
            return
        row = layout.row(align=True)
        row.label(text=label)
        draw_param_menu(row, context, node, spec)
        curve = graph_tree.ramp_node(node, name)
        if curve is None:
            layout.label(text="(updating)", icon="TIME")
            defer(("rebuild", node.id_data.name), _refresh_stale, node.id_data)
            return
        if graph_tree.ramp_changed(curve):
            defer(("ramps", node.id_data.name), graph_tree.sync_ramps, node.id_data)
        layout.template_curve_mapping(curve, "mapping")
        return
    controls = graph_authoring.control_names(node, name)
    if controls and scalp is not None and all(graph_authoring.CONTROL_PREFIX + c in scalp.keys() for c in controls):
        # A keyed param: the groom's ctl_ properties, keyed in Blender (I over them; the Graph Editor
        # and Dope Sheet show their curves), given to the graph on each frame.
        state = graph_authoring.key_state(scalp, node, name)
        row = layout.row(align=True)
        row.label(text=label + suffix, icon=KEY_ICONS.get(state, "NONE"))
        for c in controls:
            row.prop(scalp, '["%s%s"]' % (graph_authoring.CONTROL_PREFIX, c), text="")
        draw_param_menu(row, context, node, spec)
        return
    if collection is not None:
        p = collection.get(name)
        if p is not None:
            from . import ui
            row = layout.row(align=True)
            ui.draw_param(row, p, scene)
            draw_param_menu(row, context, node, spec)
        return
    prop = graph_tree.prop_name(name)
    if not hasattr(node, prop):
        row = layout.row(align=True)
        row.label(text="%s: edit it in Maya or the GroomLab app" % label)
        draw_param_menu(row, context, node, spec)
        return
    if spec.get("channel") in ("writes", "reads") and spec.get("kind") == "string":
        draw_attribute_param(layout, node, spec, prop, label)
        return
    row = layout.row(align=True)
    if spec.get("kind") == "vec3" and unit != "color":
        col = layout.column(align=True)
        head = col.row(align=True)
        head.label(text=label + suffix)
        draw_param_menu(head, context, node, spec)
        col.row(align=True).prop(node, prop, text="")
    else:
        row.prop(node, prop, text=label + suffix, slider=fill_slider(node, prop, spec))
    if driver:
        src = next((n for n in node.id_data.nodes if ours(n) and n.gl_name == driver), None)
        row.label(text="x %s" % driver, icon=KEY_ICONS["driven"] if src is not None and src.gl_type == "Control" else "LINKED")
    if spec.get("mappable") and spec.get("port") and not driver:
        op = row.operator("groomlab.graph_map_source", text="", icon="IMAGE_DATA")
        op.node, op.param = node.gl_name, name
    if not (spec.get("kind") == "vec3" and unit != "color"):
        draw_param_menu(row, context, node, spec)


SOFT_LIMIT = 1e9  # past this a property's soft range is Blender's "none" (FLT_MAX, INT_MAX)


def fill_slider(node, prop, spec):
    """Whether a number param draws as a fill slider (the name inside, filled by the value): when its
    property has a soft range to fill (the spec's softMin/softMax or min/max, a fraction's 0-1). Blender's
    number fields drag and take typed values either way."""
    if spec.get("kind") not in ("float", "int"):
        return False
    rna = node.bl_rna.properties.get(prop)
    lo, hi = getattr(rna, "soft_min", None), getattr(rna, "soft_max", None)
    return lo is not None and hi is not None and -SOFT_LIMIT < lo < hi < SOFT_LIMIT


def draw_attribute_param(layout, node, spec, prop, label):
    """A param naming an attribute: "Writes" (a warning when the name overwrites an upstream node's) or
    "Reads" with a menu of the attributes reaching the node, each with the node that writes it (a note
    when the one named does not reach)."""
    name = spec["name"]
    writes = spec.get("channel") == "writes"
    info = node_channels(node.id_data.gl_scalp).get(node.gl_name, {}) if node.id_data.gl_scalp is not None else {}
    col = layout.column(align=True)
    row = col.row(align=True)
    row.label(text="Writes" if writes else "Reads", icon="EXPORT" if writes else "IMPORT")
    row.prop(node, prop, text=label)
    if not writes:
        op = row.operator("groomlab.graph_pick_attribute", text="", icon="DOWNARROW_HLT")
        op.node, op.param = node.gl_name, name
    if writes:
        over = [o for o in info.get("overwrites", []) if o.get("param") == name]
        if over:
            col.label(text="overwrites " + ", ".join("%s from %s" % (o["name"], o["node"]) for o in over), icon="ERROR")
    else:
        read = next((r for r in info.get("reads", []) if r.get("param") == name), None)
        if read is not None and not read.get("reaches"):
            col.label(text="no node upstream writes %s: %s" % (read["name"], "none applies" if read.get("optional")
                                                                else "this node ignores it"), icon="INFO")


class GROOMLAB_OT_graph_pick_attribute(bpy.types.Operator):
    """Pick an attribute reaching this node: each with the node that writes it last"""
    bl_idname = "groomlab.graph_pick_attribute"
    bl_label = "Attributes Reaching the Node"
    bl_options = {"INTERNAL"}

    node: StringProperty()
    param: StringProperty()
    value: StringProperty(options={"SKIP_SAVE"})
    pick: BoolProperty(options={"SKIP_SAVE"})

    def execute(self, context):
        tree = editor_tree(context)
        target = next((n for n in tree.nodes if ours(n) and n.gl_name == self.node), None) if tree else None
        if target is None:
            return {"CANCELLED"}
        if not self.pick:  # the menu of what reaches the node
            sources = node_channels(tree.gl_scalp).get(self.node, {}).get("incoming", [])
            node, param = self.node, self.param

            def draw(menu, _context):
                if not sources:
                    menu.layout.label(text="No attribute reaches this node")
                for source in sources:
                    text = "%s (from %s)" % (source["name"], source["node"]) if source.get("node") else source["name"]
                    op = menu.layout.operator(GROOMLAB_OT_graph_pick_attribute.bl_idname, text=text)
                    op.node, op.param, op.value, op.pick = node, param, source["name"], True
            context.window_manager.popup_menu(draw, title="Attributes reaching " + self.node)
            return {"FINISHED"}
        setattr(target, graph_tree.prop_name(self.param), self.value)
        return {"FINISHED"}


# ---- The sidebar's GroomLab tab: the groom, its status, the presets browser -----------------------

class GROOMLAB_PT_graph_groom(bpy.types.Panel):
    bl_space_type = "NODE_EDITOR"
    bl_region_type = "UI"
    bl_category = "GroomLab"
    bl_label = "Groom"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return space is not None and space.tree_type == TREE

    def draw(self, context):
        layout = self.layout
        layout.prop(context.scene.groomlab, "groom")
        tree = editor_tree(context)
        scalp = tree.gl_scalp if tree is not None else None
        if not groom.is_groom(scalp):
            layout.label(text="Add a groom in the 3D view's GroomLab tab")
            return
        rt = groom.runtime_if_any(scalp)
        if rt is not None and rt.last:
            layout.label(text="%s strands, %.0f ms" % ("{:,}".format(rt.last.get("strandCount") or 0),
                                                       rt.last.get("milliseconds") or 0.0), icon="INFO")
        text = graph_tree.message(scalp)
        if text:
            wrap(layout.box(), text, icon="ERROR")
        if rt is not None:
            for e in rt.errors + ([rt.error] if rt.error else []):
                wrap(layout.box(), e, icon="ERROR")
        row = layout.row(align=True)
        row.operator("groomlab.update", text="Evaluate", icon="FILE_REFRESH")
        row.operator("groomlab.graph_tidy", text="Tidy")


class GROOMLAB_PT_graph_presets(bpy.types.Panel):
    bl_space_type = "NODE_EDITOR"
    bl_region_type = "UI"
    bl_category = "GroomLab"
    bl_label = "Presets"

    @classmethod
    def poll(cls, context):
        return editor_tree(context) is not None

    def draw(self, context):
        layout = self.layout
        if not _catalog["presets"]:
            layout.operator("groomlab.graph_presets_refresh", text="List Presets", icon="FILE_REFRESH")
            return
        sg = context.scene.groomlab
        row = layout.row(align=True)
        row.prop(sg, "preset_category", text="")
        row.operator("groomlab.graph_presets_refresh", text="", icon="FILE_REFRESH")
        category = sg.preset_category
        col = layout.column(align=True)
        for p in sorted(_catalog["presets"], key=lambda p: (p.get("title") or p["name"]).lower()):
            if category not in ("ALL", "", p.get("category") or ""):
                continue
            row = col.row(align=True)
            op = row.operator("groomlab.graph_preset", text=p.get("title") or p["name"])
            op.name, op.mode = p["name"], "APPLY"
            op = row.operator("groomlab.graph_preset", text="", icon="NODE_INSERT_ON")
            op.name, op.mode = p["name"], "INSERT"
        layout.label(text="Click: apply. Insert: as a Compound after the selected node", icon="INFO")


# ---- The overlay: the cards' status, as Maya's and the app's --------------------------------------

_draw_handle = [None]

# The status on a node's header (the 2026-10-30 redesign's badges, Maya's and the app's): a dot in the
# category's color before the title (minimal headers: the label leaves room for it, graph_tree's
# draw_label), and right-aligned in the header row a warning triangle (a note: the node does nothing
# as set), the cook time (muted, the warning color from SLOW_MS) and the View eye in the blue. An
# error keeps its red "!" on the corner.
SLOW_MS = 10.0
MUTED = (0.62, 0.62, 0.62, 1.0)       # the cook time under SLOW_MS (Blender's headers are lighter than Maya's cards)
WARNING = (0.831, 0.698, 0.416, 1.0)  # Charcoal's yellow (#D4B26A), Maya's graph.warning
FOCUS = (0.486, 0.612, 0.831, 1.0)    # Charcoal's blue (#7C9CD4): View on
ERROR = (0.94, 0.31, 0.27, 1.0)
INK = (0.1, 0.1, 0.12, 1.0)           # glyphs on those colors


def time_text(ms):
    """A node's cook time as its header shows it ("0.4 ms", "12 ms"); None under a twentieth of a
    millisecond, as Maya's and the app's cards."""
    if ms is None or ms < 0.05:
        return None
    return ("%.1f ms" % ms) if ms < 10.0 else ("%d ms" % int(round(ms)))


def time_color(ms):
    return WARNING if ms >= SLOW_MS else MUTED


def header_badges(name, errors, notes, times, viewed):
    """The badges right-aligned in a node's header, left to right: ("warning", note), ("time", text,
    color), ("view",). An error's "!" is on the corner instead (and takes the warning's place)."""
    badges = []
    if notes.get(name) and name not in errors:
        badges.append(("warning", notes[name]))
    stamp = time_text(times.get(name))
    if stamp:
        badges.append(("time", stamp, time_color(times[name])))
    if viewed:
        badges.append(("view",))
    return badges


def _node_times(scalp):
    """{node: ms} of the last evaluation (node_status), fetched once per evaluation."""
    rt = groom.runtime_if_any(scalp)
    if rt is None or not rt.last:
        return {}
    uid = scalp.groomlab.uid
    cached = _status.get(uid)
    serial = (rt.last.get("serial"), rt.last.get("milliseconds"))
    if cached is None or cached["serial"] != serial:
        def fetch():
            if rt.session is None or rt.generation != process.generation():
                return
            _status[uid] = {"serial": serial, "times": process.call("node_status", session=rt.session).get("times", {})}
        defer(("times", uid), fetch)
        return cached["times"] if cached else {}
    return cached["times"]


def _draw_overlay():
    try:
        _draw_overlay_now()
    except Exception as e:  # noqa: BLE001 - never break the editor's drawing
        print("GroomLab overlay: %r" % e)


def _draw_overlay_now():
    import math

    import blf
    import gpu
    from gpu_extras.batch import batch_for_shader
    context = bpy.context
    tree = editor_tree(context)
    region = context.region
    if tree is None or region is None or not groom.is_groom(tree.gl_scalp):
        return
    scalp = tree.gl_scalp
    v2d = region.view2d
    sysp = context.preferences.system
    scale = float(sysp.ui_scale) * float(getattr(sysp, "pixel_size", 1.0))
    x0, y0 = v2d.view_to_region(0.0, 0.0, clip=False)
    x1, _ = v2d.view_to_region(100.0, 0.0, clip=False)
    zoom = (x1 - x0) / 100.0
    errors, notes, times = node_errors(scalp), node_notes(scalp), _node_times(scalp)
    shader = gpu.shader.from_builtin("UNIFORM_COLOR")
    gpu.state.blend_set("ALPHA")
    font = 0
    unit = 20.0 * scale * zoom  # Blender's widget unit on screen: the header is one tall

    def fan(points, color):
        """A convex shape filled: its outline, fanned from the first point."""
        batch = batch_for_shader(shader, "TRIS", {"pos": points},
                                 indices=[(0, i, i + 1) for i in range(1, len(points) - 1)])
        shader.uniform_float("color", color)
        batch.draw(shader)

    def disc(cx, cy, r, color):
        fan([(cx, cy)] + [(cx + r * math.cos(a * math.pi / 8), cy + r * math.sin(a * math.pi / 8))
                          for a in range(17)], color)

    def text(x, y, s, color, size):
        blf.size(font, max(6.0, size))
        blf.color(font, *color)
        blf.position(font, x, y, 0)
        blf.draw(font, s)

    def width(s, size):
        blf.size(font, max(6.0, size))
        return blf.dimensions(font, s)[0]

    def triangle(cx, cy, r):
        """The warning sign: a triangle in the warning color, a dark "!" on it."""
        fan([(cx - r, cy - 0.85 * r), (cx + r, cy - 0.85 * r), (cx, cy + r)], WARNING)
        size = 1.3 * r
        text(cx - 0.5 * width("!", size), cy - 0.7 * r, "!", INK, size)

    def eye(cx, cy, r):
        """View on: an almond in the blue with a dark pupil."""
        top = [(cx + r * (2 * t - 1), cy + 0.62 * r * 4 * t * (1 - t)) for t in (i / 10.0 for i in range(11))]
        fan([(cx, cy)] + top + [(x, 2 * cy - y) for x, y in reversed(top)], FOCUS)
        disc(cx, cy, 0.3 * r, INK)

    for node in tree.nodes:
        if not ours(node) or node.hide:
            continue
        loc = node.location_absolute if hasattr(node, "location_absolute") else node.location
        w, h = node.dimensions
        if w <= 0:
            continue
        ax, ay = v2d.view_to_region(loc[0] * scale, loc[1] * scale, clip=False)
        bx, by = ax + w * zoom, ay - h * zoom
        if bx < 0 or ax > region.width or ay < 0 or by > region.height:
            continue
        category = "Output" if node.gl_is_output else graph_tree.category_of(node.gl_type)
        accent = graph_tree._rgb(graph_tree.CATEGORY_COLORS.get(category, (142, 150, 158)))
        size = 11 * zoom * scale
        mid = ay - 0.5 * unit  # the header row's middle
        # The category's dot in the room the label leaves before the name (Blender's title starts
        # 1.6 units in: its margin and text padding).
        lead = width(graph_tree.LABEL_LEAD, size)
        disc(ax + 1.6 * unit + 0.5 * lead, mid, 0.18 * unit, accent)
        r = 8 * zoom * scale
        name = node.gl_name
        if name in errors:
            disc(bx, ay, r, ERROR)
            text(bx - 0.18 * size, ay - 0.36 * size, "!", (1, 1, 1, 1), size)
        if node.gl_library_info and graph_authoring.library_info(node).get("updateAvailable"):
            # The library has a newer copy of this node: a mark on its left corner (Update Node), as Maya's.
            disc(ax, ay, r, (0.96, 0.69, 0.26, 1.0))
            text(ax - 0.24 * size, ay - 0.36 * size, "↑", (0.1, 0.1, 0.12, 1), size)
        # The header badges, right to left from the header's right end.
        x = bx - 0.5 * unit
        small = 10 * zoom * scale
        for badge in reversed(header_badges(name, errors, notes, times, node.gl_viewed)):
            if badge[0] == "view":
                eye(x - 0.4 * unit, mid, 0.4 * unit)
                x -= 0.8 * unit + 0.3 * unit
            elif badge[0] == "time":
                tw = width(badge[1], small)
                text(x - tw, mid - 0.36 * small, badge[1], badge[2], small)
                x -= tw + 0.3 * unit
            else:
                triangle(x - 0.32 * unit, mid, 0.32 * unit)
                x -= 0.64 * unit + 0.3 * unit
    gpu.state.blend_set("NONE")


# ---- Registration -------------------------------------------------------------------------------

_keymaps = []

classes = (GROOMLAB_OT_graph_open, GROOMLAB_OT_graph_add_search, GROOMLAB_OT_graph_frame, GROOMLAB_OT_graph_duplicate,
           GROOMLAB_OT_graph_view_node, GROOMLAB_OT_graph_bypass, GROOMLAB_OT_graph_evaluation, GROOMLAB_OT_graph_set_output,
           GROOMLAB_OT_graph_upgrade_node, GROOMLAB_OT_graph_fold, GROOMLAB_OT_graph_pick_attribute, GROOMLAB_OT_graph_tidy, GROOMLAB_OT_graph_reload,
           GROOMLAB_OT_graph_preset, GROOMLAB_OT_graph_presets_refresh, GROOMLAB_MT_graph_presets,
           GROOMLAB_MT_graph_edit, GROOMLAB_PT_graph_node, GROOMLAB_PT_graph_groom, GROOMLAB_PT_graph_presets)

# Maya's keys (shortcuts.h): Tab adds, F frames, V views, B bypasses, Ctrl+D duplicates, Ctrl+G groups
# into a Compound, Ctrl+Shift+G ungroups (only in GroomLab graphs: each operator's poll).
KEYS = (("groomlab.graph_add_search", "TAB", {}), ("groomlab.graph_frame", "F", {}),
        ("groomlab.graph_view_node", "V", {}), ("groomlab.graph_bypass", "B", {}),
        ("groomlab.graph_duplicate", "D", {"ctrl": True}), ("groomlab.graph_group", "G", {"ctrl": True}),
        ("groomlab.graph_ungroup", "G", {"ctrl": True, "shift": True}))


def register():
    for c in classes:
        bpy.utils.register_class(c)
    _bind_editor()
    _make_icons()
    _register_add_menu()
    bpy.types.NODE_HT_header.append(draw_header)
    bpy.types.NODE_MT_context_menu.append(draw_context_menu)
    _draw_handle[0] = bpy.types.SpaceNodeEditor.draw_handler_add(_draw_overlay, (), "WINDOW", "POST_PIXEL")
    kc = bpy.context.window_manager.keyconfigs.addon if bpy.context.window_manager else None
    if kc is not None:
        km = kc.keymaps.new(name="Node Editor", space_type="NODE_EDITOR")
        for idname, key, mods in KEYS:
            _keymaps.append((km, km.keymap_items.new(idname, key, "PRESS", **mods)))


def unregister():
    import nodeitems_utils
    for km, kmi in _keymaps:
        try:
            km.keymap_items.remove(kmi)
        except (ReferenceError, RuntimeError):
            pass
    _keymaps.clear()
    if _draw_handle[0] is not None:
        bpy.types.SpaceNodeEditor.draw_handler_remove(_draw_handle[0], "WINDOW")
        _draw_handle[0] = None
    bpy.types.NODE_MT_context_menu.remove(draw_context_menu)
    bpy.types.NODE_HT_header.remove(draw_header)
    try:
        nodeitems_utils.unregister_node_categories("GROOMLAB_NODES")
    except KeyError:
        pass
    if _icons["previews"] is not None:
        from bpy.utils import previews
        previews.remove(_icons["previews"])
        _icons["previews"] = None
        _icons["ids"].clear()
    if bpy.app.timers.is_registered(_run_deferred):
        bpy.app.timers.unregister(_run_deferred)
    _deferred.clear()
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
