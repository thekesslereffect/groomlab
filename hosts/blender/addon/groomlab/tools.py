# SPDX-License-Identifier: GPL-3.0-or-later
"""GroomLab's own tools in Blender, as Maya's Groom Tools drive them: the helper runs the core's
brushes, selection, guide hierarchy edits, painted maps and undo (docs/BLENDER.md, "Tools"); this
module keeps the helper's tool in step with the palette's settings, turns Blender's view and mouse
into the helper's view and pixels, and after every edit evaluates the groom, refreshes the palette's
lists and keeps the groom's state for the .blend and a restarted helper.

No operators here (tool_ops.py has them and the modal tool): everything is plain functions over a
groom's scalp object, so the tests drive the same code with a synthetic view."""

import base64  # noqa: F401  (the state's textures are base64 in the helper's answer, kept as they come)
import json
import os
import time

import bpy
import numpy as np

from . import definitions as defs
from . import groom, process, props, tool_props
from .helper_client import HelperError

ALL_GROUPS = "__all__"          # the Group choice for every group (Maya's "All groups")
FIRST_GROUP = "primary"         # what Place starts on a groom without guides
LIVE_DENSITY = 0.35             # strands while a stroke is live (Maya's sculptDensity)
LIVE_INTERVAL = 0.04            # seconds between live evaluations during a stroke, at least,
LIVE_PER_COST = 1.5             # and this many times the last one's own time (the drags keep coming in between)
STATE_DELAY = 1.0               # seconds after the last edit before the groom's state is kept (get_state)
DEFER = 0.35                    # seconds after a slider's last change before its edit commits (one undo step)
# The Playful theme's viewport colors (hosts/maya/themes/playful.json "viewport"), as Maya hands
# them to its display (groomLabDisplayTheme). Blender's overlays are not color managed: sRGB as is.
THEME_HEX = {"selected": "#F5F5F7",
             "softSelected": "#FFC53D", "frozen": "#5EC8FF", "parts": "#FF4D4F", "flow": "#FFC53D",
             "flowField": "#8FA3BF", "tubes": "#8A93A0", "clumpCenters": "#FF4FD8"}
POINT_SIZE = 6.0

_catalog = {}


def _rgb(h):
    h = h.lstrip("#")
    return [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]


def theme():
    t = {"pointSize": POINT_SIZE}
    for key in ("selected", "softSelected", "parts", "flow", "flowField", "tubes"):
        t[key] = _rgb(THEME_HEX[key]) + [1.0]
    t["frozen"] = _rgb(THEME_HEX["frozen"])
    t["clumpCenters"] = _rgb(THEME_HEX["clumpCenters"])
    return t


# ---- The catalog (brush_catalog.h, every host's palette) ----------------------------------------

def available():
    """Whether the helper runs GroomLab's tools (hello's features.tools)."""
    try:
        process.client()
    except HelperError:
        return False
    return int((process.hello().get("features") or {}).get("tools", 0)) >= 1


def catalog():
    """{"tools": [...], "categories": [...], "settings": {id: {label, hint}}, "paintable": [{id, label,
    type, param, hint}] (what New Map > For offers first)} (cached)."""
    if not _catalog:
        r = process.call("tool_catalog")
        _catalog.update(tools=r["tools"], categories=r["categories"], settings={s["id"]: s for s in r["settings"]},
                        paintable=r.get("paintable") or [])
    return _catalog


def cached_catalog():
    return _catalog


def tool_info(tool):
    for t in (_catalog.get("tools") or []):
        if t["id"] == tool:
            return t
    return None


def sections():
    """[(category, [tool, ...])] in the catalog's order: the palette's tile groups."""
    cat = _catalog
    if not cat:
        return []
    return [(c, sorted((t for t in cat["tools"] if t["category"] == c), key=lambda t: t["order"]))
            for c in cat["categories"]]


def setting_label(sid, fallback=""):
    s = (_catalog.get("settings") or {}).get(sid)
    return s["label"] if s else fallback


def setting_hint(sid):
    s = (_catalog.get("settings") or {}).get(sid)
    return s["hint"] if s else ""


def shows(tool):
    info = tool_info(tool)
    return set(info["settings"]) if info else set()


# ---- Per groom: what the helper's tool was last given -------------------------------------------

class GroomTools(object):
    def __init__(self):
        self.pushed = None        # (session, generation) the settings were sent to
        self.pushed_version = -1  # the palette's settings version they were
        self.info = {}            # guides_info
        self.maps = []            # maps_list
        self.undo = {}            # undo_state
        self.last_live = 0.0
        self.live_cost = 0.0      # seconds the last live evaluation took (the hair written)
        self.state_dirty = False
        self.last_end = {}
        self.tube_guides = []     # the guides the Tubes section edits (the selection, else the soloed ones)
        self.tube_key = None      # (targets, their shapes) as last read
        self.pushed_keys = {}     # corrective -> the keys last sent from its Blender fcurve


_groom_tools = {}
_settings_version = [0]  # rises with every palette change: each groom's helper tool catches up on its next use
_states = {}   # uid -> the last get_state (a restarted helper gets it back; fresher than the .blend's)


def groom_tools(scalp):
    uid = groom.runtime(scalp).uid
    gt = _groom_tools.get(uid)
    if gt is None:
        gt = _groom_tools[uid] = GroomTools()
    return gt


def forget_all():
    _groom_tools.clear()
    _states.clear()


def is_hierarchy(scalp):
    return scalp is not None and scalp.groomlab.guide_target == "hierarchy"


def session(scalp, scene=None):
    """The groom's runtime with its session live and the tool's settings sent to it."""
    scene = scene or groom.scene_of(scalp)
    rt = groom.ensure(scalp, scene)
    gt = groom_tools(scalp)
    key = (rt.session, rt.generation)
    if gt.pushed != key:
        gt.pushed = key
        gt.info, gt.maps, gt.undo = {}, [], {}
        push(scalp, scene)
        try:
            restore_tools(scalp, rt)
        except HelperError:
            pass
    elif gt.pushed_version != _settings_version[0]:
        push(scalp, scene)
    return rt


def call(scalp, cmd, **args):
    rt = session(scalp)
    return process.call(cmd, session=rt.session, **args)


# ---- Settings: the palette's (Scene.groomlab_tools) as the helper's tool_set ---------------------

def strengths(t):
    try:
        return json.loads(t.strengths or "{}")
    except ValueError:
        return {}


def strength_for(t, tool):
    s = strengths(t)
    if tool in s:
        return float(s[tool])
    info = tool_info(tool)
    return float(info["defaultStrength"]) if info else 1.0


def group_target(scene):
    """(level, group) the brushes edit and new guides go into: the Group choice."""
    t = scene.groomlab_tools
    name = t.group
    if not name or name == ALL_GROUPS:
        return -1, ""
    for g in t.groups:
        if g.name == name:
            return int(g.level), g.name
    return 0, ""  # the placeholder of a groom without groups: Place starts "primary" at level 0


def current_map(scene):
    t = scene.groomlab_tools
    for m in t.maps:
        if m.name == t.paint_map:
            return m
    return None


def settings_of(scene, tool=None):
    """The helper's settings for `tool` (host units: lengths in scene units, as the properties)."""
    t = scene.groomlab_tools
    tool = tool if tool is not None else t.tool
    info = tool_info(tool)
    strength = strength_for(t, tool)
    if info is not None and not info.get("signed"):
        strength = max(0.0, strength)
    level, group = group_target(scene)
    targets = [n for n, on in (("guides", t.target_guides), ("parts", t.target_parts), ("flow", t.target_flow)) if on]
    s = {"radius": float(t.radius), "strength": strength, "falloff": float(t.falloff),
         "symmetry": t.symmetry_mode if t.symmetry else "none",
         "selectedOnly": bool(t.selected_only),
         "pressure": bool(t.pressure), "collide": bool(t.collide), "frontFacingOnly": bool(t.front_facing),
         "stretch": bool(t.stretch), "straighten": bool(t.straighten), "targets": ",".join(targets) or "none",
         "eraseGuides": bool(t.erase_guides), "eraseParts": bool(t.erase_parts), "eraseFlow": bool(t.erase_flow),
         "matchNeighbors": bool(t.match_neighbors), "soloNewGuides": bool(t.solo_new_guides),
         "length": float(t.length), "segments": int(t.segments),
         "spacing": float(t.spacing), "partName": t.part_name or "part", "flowSink": t.flow_sink,
         "smoothStrength": float(t.smooth_strength), "level": level, "group": group,
         "falloffProfile": tool_props.curve_of(tool_props.FALLOFF_CURVES, t.falloff_curve),
         "strengthProfile": tool_props.curve_of(tool_props.STRENGTH_PROFILES, t.strength_profile),
         "tubeChannel": t.tube_channel, "tube": placement_tube(scene)}
    if tool == "paint":
        m = current_map(scene)
        channels = "".join(c for c, on in zip("rgba", (t.channel_r, t.channel_g, t.channel_b, t.channel_a)) if on)
        s.update(value=float(t.value), paintColor=[float(c) for c in t.paint_color], paintChannels=channels or "rgba",
                 paintMode=t.paint_mode)
        if m is not None:
            s.update(attribute=m.name, paintKind=m.kind)
            if m.kind == "texture" and m.texture:
                s["paintTexture"] = m.texture
        s.update(alpha=t.alpha if t.alpha and os.path.isfile(t.alpha) else "", alphaRotation=t.alpha_rotation.lower(),
                 alphaAngle=float(t.alpha_angle), alphaSpacing=t.alpha_spacing / 100.0, alphaInvert=bool(t.alpha_invert))
    return s


def placement_tube(scene):
    """The tube Place and Draw give new guides (the Tubes section's shape, Width, Flatness and
    Twist as shown: Maya's placement tube)."""
    t = scene.groomlab_tools
    return {"shape": t.tube_choice or "round", "width": float(t.tube_width),
            "aspect": max(1.0 - float(t.tube_flatness), 0.01), "twist": float(t.tube_twist)}


def push(scalp, scene=None, **only):
    """Sends the tool and its settings (or just `only`) to the groom's helper tool."""
    scene = scene or groom.scene_of(scalp)
    t = scene.groomlab_tools
    rt = groom.ensure(scalp, scene)
    tool = t.tool or "comb"
    settings = settings_of(scene, tool)
    if only:
        settings = {k: v for k, v in settings.items() if k in only}
    r = process.call("tool_set", session=rt.session, tool=tool, settings=settings)
    if not only:
        groom_tools(scalp).pushed_version = _settings_version[0]
    return r


def push_safely(context, **only):
    _settings_version[0] += 1
    scalp = active(context)
    if scalp is None or not is_hierarchy(scalp):
        return
    try:
        push(scalp, context.scene, **only)
        status(context.scene, "")
    except HelperError as e:
        status(context.scene, e.message)


def status(scene, text):
    with props.quiet():
        scene.groomlab_tools.status = text


def active(context):
    from .ops import active_groom
    return active_groom(context)


def pick(context, tool):
    """Picks a tool as Maya's palette does: the tool's own strength back (the catalog's default the
    first time), its settings sent. Returns the helper's tool_get answer."""
    scene = context.scene
    t = scene.groomlab_tools
    if tool not in [x["id"] for x in catalog()["tools"]]:
        raise HelperError("bad_request", "no GroomLab tool %r" % tool)
    with props.quiet():
        t.tool = tool
        t.strength = strength_for(t, tool)
        if tool == "paint":
            t.tab = "PAINT"
    scalp = active(context)
    if scalp is None:
        status(scene, "Select a groom first; tools work on the selected groom.")
        return None
    if tool == "paint" and current_map(scene) is None:
        refresh_info(scalp, scene)
        if current_map(scene) is None:
            status(scene, "Make a map to paint (New Map).")
    session(scalp, scene)
    r = push(scalp, scene)
    status(scene, "")
    from . import display
    display.mark(scalp)
    return r


def set_strength(scene, value):
    t = scene.groomlab_tools
    s = strengths(t)
    s[t.tool or "comb"] = float(value)
    with props.quiet():
        t.strengths = json.dumps(s, sort_keys=True)


# ---- The view: Blender's region and region_3d as the helper's -----------------------------------

def _rows(m):
    return [[float(x) for x in row] for row in m]


def view_of(region, rv3d, scalp):
    """The helper's `view` for a 3D view region (docs/BLENDER.md "Views"): its matrices, the scalp's
    matrix_world (points cross in its local space), the region's size and the pivot."""
    target = scalp.matrix_world.inverted_safe() @ rv3d.view_location
    return {"viewMatrix": _rows(rv3d.view_matrix), "projectionMatrix": _rows(rv3d.window_matrix),
            "objectMatrix": _rows(scalp.matrix_world), "width": int(region.width), "height": int(region.height),
            "target": [float(v) for v in target]}


# ---- Hover, strokes, selection ------------------------------------------------------------------

def hover(scalp, view, x, y):
    from . import display
    r = call(scalp, "hover", view=view, x=float(x), y=float(y))
    display.take_overlay(scalp, r.get("overlay"), r.get("ring"))
    return r


def hover_clear(scalp):
    from . import display
    try:
        r = call(scalp, "hover_clear")
    except HelperError:
        return None
    display.take_overlay(scalp, r.get("overlay"), None)
    return r


def follow_graph(scalp, r):
    """The helper's graph changed under a tool (map_for_param links a setting's map into it; undo
    takes it out again): the node list, the node tree and the kept graph follow."""
    if not r or not r.get("graphChanged"):
        return
    from . import graph, graph_tree
    try:
        groom.remember_graph(scalp)
        graph.refresh(scalp)
        graph_tree.graph_changed(scalp)
    except Exception as e:  # noqa: BLE001 - the stroke stands; the tree catches up on its next refresh
        status(groom.scene_of(scalp), str(e))


def stroke_begin(scalp, view, x, y, pressure=1.0, ctrl=False, shift=False, adjust=False):
    """`adjust`: the adjusting button (the Length Keys tool's right-drag: a key's scale)."""
    from . import display
    r = call(scalp, "stroke_begin", view=view, x=float(x), y=float(y), pressure=float(pressure), ctrl=bool(ctrl),
             shift=bool(shift), adjust=bool(adjust))
    follow_graph(scalp, r)
    display.take_overlay(scalp, r.get("overlay"), r.get("ring"))
    display.mark(scalp)
    groom_tools(scalp).last_live = time.monotonic()
    return r


def stroke_drag(scalp, points, live=True):
    """Points [[x, y, pressure], ...] since the last drag; a live evaluation now and then."""
    from . import display
    if not points:
        return None
    r = call(scalp, "stroke_drag", points=[[float(p[0]), float(p[1]), float(p[2] if len(p) > 2 else 1.0)]
                                           for p in points])
    display.take_overlay(scalp, r.get("overlay"), r.get("ring"))
    display.mark(scalp, viewport_only=True)
    gt = groom_tools(scalp)
    now = time.monotonic()
    if live and r.get("changed") and now - gt.last_live >= max(LIVE_INTERVAL, LIVE_PER_COST * gt.live_cost):
        evaluate_live(scalp)
        gt.last_live = time.monotonic()
        gt.live_cost = gt.last_live - now
    return r


def evaluate_live(scalp):
    """The strands from the live stroke's guides at a fraction of the density (Maya's sculpt density)."""
    scene = groom.scene_of(scalp)
    rt = groom.ensure(scalp, scene)
    density = 1.0 if scene.groomlab_tools.full_density else LIVE_DENSITY
    try:
        groom.evaluate(scalp, rt, "preview", sculptDensity=density)
    except HelperError as e:
        rt.error = e.message


def stroke_end(scalp):
    """Commits the stroke (one undo step in the helper), evaluates every strand again, refreshes
    the palette."""
    scene = groom.scene_of(scalp)
    r = call(scalp, "stroke_end")
    groom_tools(scalp).last_end = r
    after_edit(scalp, scene, r.get("undo"), refresh=r.get("committed", False))
    warn = r.get("warnings") or []
    status(scene, "; ".join(warn) if warn else "")
    return r


def push_quiet(scalp, scene):
    try:
        push(scalp, scene)
    except HelperError as e:
        status(scene, e.message)


def stroke_cancel(scalp):
    from . import display
    r = call(scalp, "stroke_cancel")
    after_edit(scalp, groom.scene_of(scalp), None, refresh=False)
    display.mark(scalp)
    return r


def select(scalp, view, points, mode="replace", solo=True):
    """Selects the guides in a lasso (3+ points), a box (2 corners) or under a click (1 point);
    `solo`: the guides not selected frozen (an undo step when that changes anything)."""
    from . import display
    r = call(scalp, "select", view=view, lasso=[[float(p[0]), float(p[1])] for p in points], mode=mode, solo=solo)
    if r.get("froze"):  # the guides not selected frozen (or all unfrozen): an undo step
        after_edit(scalp, groom.scene_of(scalp), call(scalp, "undo_state"))
    display.mark(scalp)
    refresh_info(scalp)
    return r


def selection_set(scalp, **args):
    from . import display
    r = call(scalp, "selection_set", **args)
    if r.get("froze"):  # solo: the guides not selected frozen, an undo step
        after_edit(scalp, groom.scene_of(scalp), call(scalp, "undo_state"))
    display.mark(scalp)
    refresh_info(scalp)
    return r


def selected_guides(scalp):
    """The selected guides' IDs (as guides_info last said)."""
    info = groom_tools(scalp).info or call(scalp, "guides_info")
    return [int(g) for g in (info.get("selection") or {}).get("selection") or []]


def pick_level(scalp, view, x, y):
    return call(scalp, "pick_level", view=view, x=float(x), y=float(y))


# ---- Edits that are one undo step each ----------------------------------------------------------

def undo(scalp, redo=False):
    r = call(scalp, "redo" if redo else "undo")
    # A transfer taken back or done again (the step undone is now the one to redo, and the other way
    # round; scalpChanged misses a scalp of the same shape): its owner follows (pipeline.py).
    if r.get("done") and (r.get("scalpChanged") or r.get("undoLabel" if redo else "redoLabel") == "transfer"):
        from . import pipeline
        scalp = pipeline.follow_helper_undo(scalp, redo)
    follow_graph(scalp, r)
    after_edit(scalp, groom.scene_of(scalp), r, refresh=bool(r.get("done")))
    return r


def guides_edit(scalp, op, **args):
    r = call(scalp, "guides_edit", op=op, **args)
    after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    if r.get("guides"):
        _fill_groups(groom.scene_of(scalp), r["guides"], groom_tools(scalp))
    return r


def map_create(scalp, name, kind="guides", color=False, value=0.0, fill=None, resolution=1024):
    args = {"name": name, "kind": kind, "value": float(value), "resolution": int(resolution)}
    if color:
        args["color"] = True
        args["fill"] = [float(c) for c in (fill or (value, value, value, 1.0))]
    r = call(scalp, "map_create", **args)
    scene = groom.scene_of(scalp)
    after_edit(scalp, scene, r.get("undo"), refresh=True)
    with props.quiet():
        scene.groomlab_tools.paint_map = name
    refresh_info(scalp, scene)
    return r


def setting_targets(scalp):
    """What New Map > For offers on this groom: the catalog's common settings (Density, Width,
    Length...), each {id, label, type, param, hint, node} with node "" while the graph has no node of
    its type; then every mappable param of the graph (Other) as {id: "node.param", label, node,
    param, hint}. Returns (settings, others)."""
    from . import graph
    rt = groom.ensure(scalp)
    g = process.call("get_graph", session=rt.session)["graph"]
    nodes = g.get("nodes") or []
    settings = []
    for s in catalog().get("paintable") or []:
        node = next((n["name"] for n in nodes if n.get("type") == s["type"]), "")
        settings.append(dict(s, node=node))
    specs = graph.operator_specs()
    others = []
    for n in nodes:
        for p in (specs.get(n.get("type")) or {}).get("params", []):
            if p.get("mappable"):
                key = "%s.%s" % (n["name"], p["name"])
                others.append({"id": key, "label": key, "node": n["name"], "param": p["name"],
                               "hint": p.get("description", "")})
    return settings, others


def map_for_param(scalp, node, param, kind="vertices", resolution=1024):
    """New Map > For a setting: the map node.param reads, or a white one (`kind`: vertices or
    texture) made and linked to it, one undo step (helper map_for_param); picked in the Paint tab,
    painted toward 1 (Ctrl: toward 0; on Density, no hair). The caller picks the Paint tool."""
    r = call(scalp, "map_for_param", node=node, param=param, kind=kind, resolution=int(resolution))
    scene = groom.scene_of(scalp)
    follow_graph(scalp, r)
    after_edit(scalp, scene, r.get("undo"), refresh=True)
    with props.quiet():
        scene.groomlab_tools.paint_map = r["name"]
        scene.groomlab_tools.value = 1.0
    refresh_info(scalp, scene)
    return r


def map_remove(scalp, name, delete_file=False):
    """Remove a map (Maya's remove_map): what reads it lets go of it (its Map nodes removed, map
    params naming it back to their defaults); a texture map's file is deleted too with `delete_file`
    when GroomLab made it (it is in the maps folder) and nothing else reads it. One undo step."""
    r = call(scalp, "map_remove", name=name, deleteFile=bool(delete_file))
    follow_graph(scalp, r)
    after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r


# ---- Map management (the Paint tab's list menu; Maya's painting.py) --------------------------------

def _map_edit(scalp, cmd, **args):
    """A map command that is one undo step: the graph, palette and groom follow."""
    r = call(scalp, cmd, **args)
    follow_graph(scalp, r)
    after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r


def map_rename(scalp, name, new):
    """Rename a map; the Map nodes and map params reading it follow, and a texture map's file named
    after it (in the maps folder) too. Returns the helper's answer (name, renamed, graphChanged, file?)."""
    r = _map_edit(scalp, "map_rename", name=name, to=new)
    scene = groom.scene_of(scalp)
    if scene.groomlab_tools.paint_map == name:
        with props.quiet():
            scene.groomlab_tools.paint_map = new
    return r


def map_convert(scalp, name, kind, resolution=1024, texture=None):
    """Resample a map into another kind (guides, vertices, texture), its name kept."""
    args = {"name": name, "kind": kind, "resolution": int(resolution)}
    if texture:
        args["texture"] = texture
    return _map_edit(scalp, "map_convert", **args)


def map_set_channels(scalp, name, channels):
    """Make a map gray (1) or color (4: r g b a); a guide layer stays gray."""
    return _map_edit(scalp, "map_set_channels", name=name, channels=int(channels))


def map_fill(scalp, name, value=None, color=None, channels="rgba", selected=False, mode="replace"):
    """Set a map to `value` everywhere (any kind), or a guide layer on the selected guides only; a
    color map takes `color` (r, g, b[, a]) in the channels of `channels`."""
    args = {"name": name, "channels": channels or "rgba", "selected": bool(selected), "paintMode": mode}
    if value is not None:
        args["value"] = float(value)
    if color is not None:
        args["color"] = [float(c) for c in color]
    return _map_edit(scalp, "map_fill", **args)


def map_import(scalp, path, name=None, color=None, copy=None):
    """A new texture map reading the image at `path`: a PNG in place, another format (or a PNG
    with `copy`) copied into the maps folder as a PNG. Named after the file unless `name`; color
    when the image has color unless `color` says. Returns the helper's answer (name, file...)."""
    args = {"path": path}
    if name:
        args["name"] = name
    if color is not None:
        args["color"] = bool(color)
    if copy is not None:
        args["copy"] = bool(copy)
    return _map_edit(scalp, "map_import", **args)


def map_relink(scalp, name, path, copy=None):
    """Map `name` reads the image at `path` as a texture map (the file rules of map_import)."""
    args = {"name": name, "path": path}
    if copy is not None:
        args["copy"] = bool(copy)
    return _map_edit(scalp, "map_relink", **args)


def map_reload(scalp, name=None):
    """Texture map `name`'s image (every texture map's when None) read again after an edit outside
    Blender; the groom evaluates again. Not an undo step. Returns the files."""
    r = call(scalp, "map_reload", **({"name": name} if name else {}))
    after_edit(scalp, groom.scene_of(scalp), refresh=True)
    return r.get("files") or []


def map_users(scalp, name):
    """(node, param) pairs map `name` feeds (through its Map nodes, and map params naming it)."""
    return [tuple(u) for u in call(scalp, "map_users", name=name).get("users") or []]


# ---- Guide edits: Segments, Mirror Selection, group colors -------------------------------------------

def _targets(guides=None, group=None, level=None):
    if guides:
        return {"ids": [int(g) for g in guides]}
    if group:
        return {"group": group}
    if level is not None:
        return {"level": int(level)}
    return {}


def guide_segments(scalp, guides=None, group=None, level=None):
    """The segment counts of guides (default: the selected ones, else every guide): {ids, segments}."""
    return call(scalp, "guide_segments", **_targets(guides, group, level))


def resample_guides(scalp, segments, guides=None, group=None, level=None):
    """Give guides (default: the selected ones, else every guide) `segments` segments. One undo step."""
    return guides_edit(scalp, "resample", segments=int(segments), **_targets(guides, group, level))


def _preview_shown(scalp):
    from . import display
    groom.sync_safely(scalp, scene=groom.scene_of(scalp), force_eval=True, topology=False)
    display.mark(scalp)
    groom._redraw()


def reset_group_segments(scalp, group):
    """Maya's reset_group_segments: every guide of the group back to its segments, those with their
    own too (frozen ones kept). Returns the segments."""
    return int(guides_edit(scalp, "reset_group_segments", group=group).get("segments") or 0)


def set_group_segments(scalp, group, segments):
    """Maya's set_group_segments: the segments a group's new guides get (0: the tool's own)."""
    return guides_edit(scalp, "set_group_segments", group=group, segments=int(segments))


def preview_resample(scalp, segments, guides=None, group=None, level=None):
    """The Segments slider while dragged: the guides shown resampled, no change, no undo step;
    resample_guides commits, end_preview drops it."""
    r = call(scalp, "guides_preview", segments=int(segments), **_targets(guides, group, level))
    _preview_shown(scalp)
    return r


def end_preview(scalp):
    """Drop a guide preview (Segments, a tube shape's points): the stored guides show again."""
    r = call(scalp, "guides_preview_end")
    if r.get("ended"):
        _preview_shown(scalp)
    return r


def mirror_selection(scalp, axis="x", center=0.0, add=False):
    """Select the mirror images of the selected guides (the nearest guide of the same group across
    the plane). A selection: not an undo step."""
    from . import display
    r = call(scalp, "guides_edit", op="mirror_selection", axis=axis, center=float(center), add=bool(add))
    display.mark(scalp)
    refresh_info(scalp)
    mark_state(scalp)
    return r


def set_group_color(scalp, group, color):
    """Give a guide group its own viewport color ("#rrggbb" or (r, g, b) in 0-1); None goes back to
    its automatic color. One undo step."""
    if isinstance(color, (tuple, list)):
        color = [float(c) for c in color[:3]]
    return guides_edit(scalp, "set_group_color", group=group, color=color)


# ---- Tubes (the Tubes section; Maya's guides.py tube functions) ------------------------------------

TUBE_CHANNELS = ("width", "flatness", "twist")


def tube_tool(scalp, channel=None, tube=None):
    """The Shape brush's channel (`channel`: width, flatness or twist) and the tube Place and Draw
    give new guides (`tube`: {shape, width (scene units), aspect, twist}): helper tool_set."""
    settings = {}
    if channel is not None:
        settings["tubeChannel"] = channel
    if tube is not None:
        settings["tube"] = dict(tube)
    return call(scalp, "tool_set", settings=settings)


def tube_targets(scalp):
    """The guides the Tubes section edits: {source (selection, solo, none), guides, shapes, tube}."""
    return call(scalp, "tube_targets")


def get_tube(scalp, guide):
    """A guide's tube: {shape, width (scene units), aspect, twist (radians)}, lists root to tip."""
    return call(scalp, "tube_get", guide=int(guide))["tube"]


def _tube_edit(scalp, cmd, **args):
    r = call(scalp, cmd, **args)
    after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r


def set_tube(scalp, guides=None, group=None, level=None, shape=None, width=None, aspect=None, twist=None, samples=None):
    """Edit guides' tubes (default: the selected ones): a number or a list root to tip for each of
    width (scene units), aspect and twist (radians); a shape's name. One undo step."""
    args = _targets(guides, group, level)
    for key, value in (("shape", shape), ("width", width), ("aspect", aspect), ("twist", twist), ("samples", samples)):
        if value is not None:
            args[key] = list(value) if isinstance(value, (list, tuple)) else value
    return _tube_edit(scalp, "tube_set", **args)


def scale_tube(scalp, factor, guides=None, group=None, level=None):
    """Every width of guides' tubes times `factor` (their taper kept). One undo step."""
    return _tube_edit(scalp, "tube_scale", factor=float(factor), **_targets(guides, group, level))


def smooth_tube(scalp, channel="all", passes=3, guides=None, group=None, level=None):
    """Smooth guides' tubes along each guide: channel width, flatness, twist or all. One undo step."""
    return _tube_edit(scalp, "tube_smooth", channel=channel, passes=int(passes), **_targets(guides, group, level))


def tube_shapes(scalp):
    """The built-in shapes and the groom's library: [{name, builtin, points, smooth, users}]."""
    return call(scalp, "tube_shapes")["shapes"]


def add_tube_shape(scalp, name, points, smooth=True):
    return _tube_edit(scalp, "tube_shape_add", name=name, points=[[float(u), float(v)] for u, v in points], smooth=bool(smooth))


def set_tube_shape_points(scalp, name, points=None, smooth=None):
    args = {"name": name}
    if points is not None:
        args["points"] = [[float(u), float(v)] for u, v in points]
    if smooth is not None:
        args["smooth"] = bool(smooth)
    return _tube_edit(scalp, "tube_shape_set", **args)


def rename_tube_shape(scalp, name, new):
    return _tube_edit(scalp, "tube_shape_rename", name=name, to=new)


def remove_tube_shape(scalp, name):
    return _tube_edit(scalp, "tube_shape_remove", name=name)


def preview_tube_shape(scalp, name, points, smooth=True):
    """The shape editor's drag: the library shape shown with new points, no change, no undo step;
    set_tube_shape_points commits, end_preview drops it."""
    r = call(scalp, "tube_shape_preview", name=name, points=[[float(u), float(v)] for u, v in points], smooth=bool(smooth))
    _preview_shown(scalp)
    return r


# ---- Parts and flow ---------------------------------------------------------------------------------

def parts_list(scalp):
    """The groom's parts: [{name, points}]."""
    return call(scalp, "parts_list")["parts"]


def part_add(scalp, points, name="part"):
    """A part through points on or near the scalp (scene units, the scalp object's space), named
    `name` or name1, name2... One undo step. Returns its name."""
    pts = np.asarray(points, np.float64).reshape(-1, 3)
    r = call(scalp, "part_add", name=name, points=pts.tolist())
    after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r["name"]


def part_remove(scalp, name):
    return _tube_edit(scalp, "part_remove", name=name)


def parts_clear(scalp):
    return _tube_edit(scalp, "parts_clear")


def flow_clear(scalp):
    return _tube_edit(scalp, "flow_clear")


# ---- The Length Keys tool's keys (the Keys box) --------------------------------------------------

_length_syncing = [False]


def length_syncing():
    """The Keys box's fields are being filled from the helper (their updates send nothing)."""
    return _length_syncing[0]


def sync_lengths(scene, state):
    """The Keys box's fields from the helper's state (length_state): kept on the palette's
    properties, sending nothing back."""
    if scene is None or not state:
        return
    t = scene.groomlab_tools
    _length_syncing[0] = True
    try:
        t.length_state = json.dumps(state)
        scales = state.get("scales") or []
        key = state.get("activeKey", -1)
        if 0 <= key < len(scales):
            t.length_scale = float(scales[key])
            t.length_key = float(state["lengths"][key])
        t.length_change = (state.get("change") or "scale").upper()
    finally:
        _length_syncing[0] = False


def length_state(scalp):
    r = call(scalp, "length_state")
    sync_lengths(groom.scene_of(scalp), r)
    return r


def length_edit(scalp, edit, **args):
    """One edit of the keys ("scale", "length" (value), "change" (name), "select" (key),
    "deleteKey", "clear", "apply", "cancel"). Apply commits the guides (one undo step); the others
    preview."""
    from . import display
    scene = groom.scene_of(scalp)
    r = call(scalp, "length_edit", edit=edit, **args)
    if r.get("committed"):
        after_edit(scalp, scene, r.get("undo"))
    elif edit != "select":
        evaluate_live(scalp)
        display.mark(scalp)
    sync_lengths(scene, r)
    return r


# ---- Correctives and Sculpt nodes (Maya's shots.py and sculpts.py) -----------------------------

def correctives(scalp):
    """The corrective stack, bottom first ([{name, enabled, guides, weight, weightNow, keyed, keys
    [[frame, weight]], allFrames, frame, fadeIn, fadeOut}]), what brushes edit ({kind: base,
    corrective or node, name}) and the Sculpt nodes ([{name, editedGuides}])."""
    return call(scalp, "correctives_list")


def _corrective_edit(scalp, cmd, **args):
    r = call(scalp, cmd, **args)
    after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r


def corrective_new(scalp, name=None, frame=None, fade=None, fade_in=None, fade_out=None, all_frames=None):
    """A new corrective on top of the stack, what brushes sculpt into: on every frame, or with a fade
    (frames) a fix keyed around `frame` (the current one by default). Returns its name."""
    args = {}
    for key, value in (("name", name), ("frame", frame), ("fade", fade), ("fadeIn", fade_in), ("fadeOut", fade_out),
                       ("allFrames", all_frames)):
        if value is not None:
            args[key] = value
    return _corrective_edit(scalp, "corrective_new", **args)["name"]


def corrective_set(scalp, name, enabled=None, weight=None, keys=None, frame=None, fade=None, fade_in=None, fade_out=None,
                   all_frames=None):
    """Change a corrective: on or off, its weight (a keyed one keyed at the current frame), its keys
    ([(frame, weight)], the weight's fcurve sampled; [] drops them), every frame, or re-timed as a fix."""
    args = {"name": name}
    for key, value in (("enabled", enabled), ("weight", weight), ("frame", frame), ("fade", fade), ("fadeIn", fade_in),
                       ("fadeOut", fade_out), ("allFrames", all_frames)):
        if value is not None:
            args[key] = value
    if keys is not None:
        args["keys"] = [[float(f), float(w)] for f, w in keys]
    return _corrective_edit(scalp, "corrective_set", **args)


def corrective_rename(scalp, name, new):
    return _corrective_edit(scalp, "corrective_rename", name=name, to=new)


def corrective_move(scalp, name, step):
    """Up the stack (+1) or down (-1)."""
    return _corrective_edit(scalp, "corrective_move", name=name, step=int(step))


def corrective_solo(scalp, name):
    """Only this corrective on; when it already is, every one back on."""
    return _corrective_edit(scalp, "corrective_solo", name=name)


def corrective_delete(scalp, name):
    return _corrective_edit(scalp, "corrective_delete", name=name)


def corrective_clear_keys(scalp, name):
    """Drop a corrective's keys (or a fix's fades): on every frame at its weight now."""
    return _corrective_edit(scalp, "corrective_clear_keys", name=name)


def sculpt_target(scalp, node=None, corrective=None, base=False):
    """What brushes edit: Sculpt node `node`'s guides, corrective `corrective`, or the base groom
    (`base`); none given: as it is. Returns {target, sculptNodes}."""
    from . import display
    args = {}
    if node:
        args["node"] = node
    elif corrective:
        args["corrective"] = corrective
    elif base:
        args["base"] = True
    r = call(scalp, "sculpt_target", **args)
    if args:
        display.mark(scalp)
        groom._redraw()
    return r


def sculpt_clear(scalp, node):
    """Drop Sculpt node `node`'s edits: its guides as it is fed them. One undo step."""
    return _corrective_edit(scalp, "sculpt_clear", node=node)


def set_hidden_groups(scalp, names):
    from . import display
    r = call(scalp, "set_hidden_groups", groups=list(names))
    display.mark(scalp)
    refresh_info(scalp)
    mark_state(scalp)
    return r


def after_edit(scalp, scene, undo_state=None, refresh=True):
    """After a committed edit: every strand evaluated again, the display and palette refreshed, the
    groom's state kept (for the .blend and a restarted helper)."""
    from . import display
    gt = groom_tools(scalp)
    if undo_state:
        gt.undo = dict(undo_state)
    if refresh:
        groom.sync_safely(scalp, scene=scene, force_eval=True, topology=False)
        mark_state(scalp)
        refresh_info(scalp, scene)
    display.mark(scalp)
    groom._redraw()


# ---- The palette's lists ------------------------------------------------------------------------

def _fill_groups(scene, info, gt):
    gt.info = info
    t = scene.groomlab_tools
    groups = info.get("groups") or []
    by_parent = {}
    for g in groups:
        by_parent.setdefault(g.get("parent"), []).append(g)
    ordered = []

    def walk(parent, depth):
        for g in by_parent.get(parent, []):
            ordered.append((g, depth))
            walk(g["name"], depth + 1)
    walk(None, 0)
    names = [g["name"] for g in groups]
    with props.quiet():
        t.groups.clear()
        for g, depth in ordered:
            item = t.groups.add()
            item.name = g["name"]
            item.parent = g.get("parent") or ""
            item.level = int(g.get("level", 0))
            item.guides = int(g.get("guides", 0))
            item.hidden = bool(g.get("hidden"))
            item.depth = depth
            color = g.get("color")
            item.has_color = bool(color)
            shown = color or g.get("autoColor")
            if shown:
                item.color = _rgb(shown)
        if t.group not in names and t.group != ALL_GROUPS:
            t.group = names[0] if names else FIRST_GROUP
        t.group_index = next((i for i, item in enumerate(t.groups) if item.name == t.group), -1)
        t.guide_count = int(info.get("guides", 0))
        t.selected_count = int((info.get("selection") or {}).get("count", 0))
        t.frozen_count = int(info.get("frozen", 0))


def refresh_info(scalp, scene=None):
    """guides_info, maps_list and undo_state into the palette's lists."""
    scene = scene or groom.scene_of(scalp)
    if not is_hierarchy(scalp):
        return
    gt = groom_tools(scalp)
    try:
        info = call(scalp, "guides_info", guideColorSeed=scalp.name)  # the automatic colors as drawn
        maps = call(scalp, "maps_list")["maps"]
        gt.undo = call(scalp, "undo_state")
    except HelperError as e:
        status(scene, e.message)
        return
    _fill_groups(scene, info, gt)
    gt.maps = maps
    t = scene.groomlab_tools
    with props.quiet():
        t.maps.clear()
        for m in maps:
            if m.get("freezeMask"):
                continue  # Freeze's own mask (the Freeze tool paints it)
            item = t.maps.add()
            item.name = m["name"]
            item.kind = m.get("kind", "guides")
            item.channels = int(m.get("channels", 1))
            item.texture = m.get("texture") or ""
        names = [m.name for m in t.maps]
        if t.paint_map not in names:
            t.paint_map = names[0] if names else ""
        t.map_index = names.index(t.paint_map) if t.paint_map in names else -1
    try:
        refresh_map_users(scalp, scene)
        refresh_parts(scalp, scene)
        refresh_segments(scalp, scene)
        refresh_tubes(scalp, scene)
        refresh_correctives(scalp, scene)
    except HelperError as e:
        status(scene, e.message)


def refresh_map_users(scalp, scene=None):
    """The picked map's "Read by" line (Maya's map_users: node.param it feeds)."""
    scene = scene or groom.scene_of(scalp)
    t = scene.groomlab_tools
    users = map_users(scalp, t.paint_map) if t.paint_map else []
    with props.quiet():
        t.map_read_by = ", ".join("%s.%s" % (n, p) for n, p in users)


def refresh_parts(scalp, scene=None):
    """The Part card's list and the Flow card's count."""
    scene = scene or groom.scene_of(scalp)
    t = scene.groomlab_tools
    r = call(scalp, "parts_list")
    parts, flow = r["parts"], r.get("flow") or {}
    with props.quiet():
        t.flow_arrows = int(flow.get("arrows", 0))
        t.flow_sinks = int(flow.get("sinks", 0))
        picked = t.parts[t.part_index].name if 0 <= t.part_index < len(t.parts) else ""
        t.parts.clear()
        for part in parts:
            item = t.parts.add()
            item.name = part["name"]
            item.points = int(part.get("points") or 0)
        names = [p["name"] for p in parts]
        t.part_index = names.index(picked) if picked in names else (0 if names else -1)


def segments_group(t):
    """The group Edit Guides' Segments works on: the picked one, with no guides selected (None: the
    selected guides, else every guide)."""
    if t.selected_count or not 0 <= t.group_index < len(t.groups):
        return None
    return t.groups[t.group_index].name


def refresh_segments(scalp, scene=None):
    """Edit Guides' Segments: the targets' count (the most common; the label says when they differ)."""
    scene = scene or groom.scene_of(scalp)
    t = scene.groomlab_tools
    if _deferred.get("segments") is not None:
        return  # a drag's commit pending: the slider stays as dragged
    group = segments_group(t)
    counts = [int(c) for c in guide_segments(scalp, group=group).get("segments") or []]
    with props.quiet():
        if counts:
            t.resample_segments = max(set(counts), key=counts.count)
            scope = ("%d selected" % len(counts) if t.selected_count else
                     "%s: %d" % (group, len(counts)) if group else "all %d" % len(counts))
            t.segments_scope = scope + (", mixed" if min(counts) != max(counts) else "")
        else:
            t.segments_scope = ""


# ---- Sliders that commit after the drag (Maya's release) ------------------------------------------

_deferred = {}


def defer(key, fn, delay=DEFER):
    """Run fn() once the slider `key` stops changing for `delay` seconds (Blender has no release
    event for a dragged property: each drag step would be an undo step). Headless: at once."""
    if bpy.app.background:
        _deferred.pop(key, None)
        fn()
        return

    def run():
        if _deferred.get(key) is not run:
            return None
        _deferred.pop(key, None)
        from . import tool_ops
        if tool_ops.stroking():
            _deferred[key] = run
            return delay
        try:
            fn()
        except HelperError as e:
            status(bpy.context.scene, e.message)
        groom._redraw()
        return None
    _deferred[key] = run
    bpy.app.timers.register(run, first_interval=delay)


def segments_changed(context, segments):
    """The Segments slider: the guides shown resampled at once (a preview, no undo step), committed
    as one undo step when the drag stops (Maya's segments_dragged / segments_committed)."""
    scalp = active(context)
    if scalp is None or not is_hierarchy(scalp):
        return
    scene = context.scene
    group = segments_group(scene.groomlab_tools)

    def commit():
        # Selected guides already at the count: nothing to do. A group (or every guide) commits
        # anyway: its new guides take the count on.
        counts = [int(c) for c in guide_segments(scalp, group=group).get("segments") or []]
        if counts and (not scene.groomlab_tools.selected_count or any(c != segments for c in counts)):
            r = resample_guides(scalp, segments, group=group)
            status(scene, "Resampled %d guides to %d segments." % (int(r.get("resampled") or 0), segments))
        end_preview(scalp)
        refresh_segments(scalp, scene)
    try:
        if not bpy.app.background:
            preview_resample(scalp, segments, group=group)
        defer("segments", commit)
    except HelperError as e:
        status(scene, e.message)


# ---- The Tubes section --------------------------------------------------------------------------

def refresh_tubes(scalp, scene=None):
    """The Tubes section from the groom: its targets (the selection, else the soloed guides) and
    their tube (the shape shown is theirs, Mixed when they differ; width, flatness and twist the
    first one's), the shape library, Fill Tubes (the Interpolate nodes')."""
    scene = scene or groom.scene_of(scalp)
    t = scene.groomlab_tools
    gt = groom_tools(scalp)
    info = tube_targets(scalp)
    library = [x for x in tube_shapes(scalp) if not x.get("builtin")]
    gt.tube_guides = [int(g) for g in info.get("guides") or []]
    tube, shapes = info.get("tube"), list(info.get("shapes") or [])
    with props.quiet():
        picked = t.tube_shapes[t.tube_shape_index].name if 0 <= t.tube_shape_index < len(t.tube_shapes) else ""
        t.tube_shapes.clear()
        for x in library:
            item = t.tube_shapes.add()
            item.name = x["name"]
            item.users = int(x.get("users") or 0)
            item.smooth = bool(x.get("smooth", True))
        names = [x["name"] for x in library]
        t.tube_shape_index = names.index(picked) if picked in names else (0 if names else -1)
        t.tube_source = info.get("source") or "none"
        t.tube_targets = len(gt.tube_guides)
        builtin = [c["value"] for c in defs.choices("tubeShape")]
        if t.tube_choice not in builtin and t.tube_choice not in names:
            t.tube_choice = "round"  # a library shape gone (renamed, deleted, another groom)
        t.tube_mixed = tube is not None and len(shapes) > 1
        if tube is not None and not any(k in _deferred for k in ("tube.width", "tube.flatness", "tube.twist")):
            key = (tuple(gt.tube_guides), tuple(shapes))
            if len(shapes) == 1 and key != gt.tube_key:
                t.tube_choice = shapes[0]
            gt.tube_key = key
            widths = tube.get("width") or [0.1]
            aspects = tube.get("aspect") or [1.0]
            twists = tube.get("twist") or [0.0]
            t.tube_reference = max(widths)
            t.tube_width = max(widths)
            t.tube_flatness = min(max(1.0 - sum(aspects) / len(aspects), 0.0), 0.99)
            t.tube_twist = min(max(sum(twists) / len(twists), -3.14159265), 3.14159265)
        if t.tube_choice in names:  # the library list follows the shape picked
            t.tube_shape_index = names.index(t.tube_choice)
        try:
            t.tube_shape = tool_props.MIXED if t.tube_mixed else t.tube_choice
        except TypeError:
            pass
    fill = fill_tubes(scalp)
    with props.quiet():
        t.tube_has_fill = fill is not None
        if fill is not None:
            t.tube_fill = bool(fill["fillTubes"])
            t.tube_blend = float(fill["tubeBlend"])


def _interpolate_nodes(scalp):
    return [n.name for n in scalp.groomlab.nodes if n.type == "Interpolate"]


def fill_tubes(scalp):
    """{"fillTubes", "tubeBlend"} of the graph's first Interpolate node, None without one."""
    nodes = _interpolate_nodes(scalp)
    if not nodes:
        return None
    rt = groom.ensure(scalp)
    on = process.call("get_param", session=rt.session, node=nodes[0], param="fillTubes").get("value")
    blend = process.call("get_param", session=rt.session, node=nodes[0], param="tubeBlend").get("value")
    return {"fillTubes": bool(on), "tubeBlend": 0.25 if blend is None else float(blend)}


def set_fill_tubes(scalp, on=None, blend=None):
    """Fill Tubes and Tube Blend on every Interpolate node of the graph (Maya's set_fill_tubes)."""
    from . import graph
    for node in _interpolate_nodes(scalp):
        if on is not None:
            graph.set_param(scalp, node, "fillTubes", bool(on))
        if blend is not None:
            graph.set_param(scalp, node, "tubeBlend", min(max(float(blend), 0.0), 1.0))


def tube_fill_changed(context):
    scalp = active(context)
    if scalp is None or not is_hierarchy(scalp):
        return
    t = context.scene.groomlab_tools
    try:
        set_fill_tubes(scalp, on=t.tube_fill, blend=t.tube_blend)
    except HelperError as e:
        status(context.scene, e.message)


def tube_changed(context, key):
    """A change in the Tubes section: the shape given to the targets at once; Width, Flatness and
    Twist once the drag stops (one undo step each, Maya's on release). Every change also goes to
    the guides Place and Draw make (the placement tube)."""
    scalp = active(context)
    if scalp is None or not is_hierarchy(scalp):
        return
    scene = context.scene
    t = scene.groomlab_tools
    gt = groom_tools(scalp)
    try:
        if key == "shape":
            name = t.tube_shape
            if name == tool_props.MIXED:
                return
            with props.quiet():
                t.tube_choice = name
            if gt.tube_guides:
                set_tube(scalp, guides=gt.tube_guides, shape=name)
        else:
            def commit():
                if not gt.tube_guides:
                    return
                if key == "width":
                    if t.tube_reference > 0.0:
                        scale_tube(scalp, float(t.tube_width) / t.tube_reference, guides=gt.tube_guides)
                    else:
                        set_tube(scalp, guides=gt.tube_guides, width=float(t.tube_width))
                elif key == "flatness":
                    set_tube(scalp, guides=gt.tube_guides, aspect=max(1.0 - float(t.tube_flatness), 0.01))
                elif key == "twist":
                    set_tube(scalp, guides=gt.tube_guides, twist=float(t.tube_twist))
            defer("tube." + key, commit)
        push_safely(context)
    except HelperError as e:
        status(scene, e.message)


def unique_shape_name(scene, base):
    """`base 1`, `base 2`...: a name no built-in or library shape has."""
    t = scene.groomlab_tools
    taken = set(c["value"] for c in defs.choices("tubeShape")) | set(x.name for x in t.tube_shapes)
    k = 1
    while "%s %d" % (base, k) in taken:
        k += 1
    return "%s %d" % (base, k)


def shape_points(scalp, name):
    """(points, smooth) of a shape (a built-in's from the helper's list)."""
    for x in tube_shapes(scalp):
        if x["name"] == name:
            return [list(p) for p in x["points"]], bool(x.get("smooth", True))
    raise HelperError("not_found", "no tube shape %r" % name)


# ---- The Correctives tab: the stack, its weights keyed with Blender's keyframes --------------------

def weight_path(name):
    """The data path of corrective `name`'s weight on its groom (the scalp object), by its name (an
    fcurve keeps to its corrective as others come and go)."""
    return 'groomlab_tools.corrective_weights["%s"].weight' % name


def insert_weight_key(scalp, name, frame, value):
    """A key on corrective `name`'s weight fcurve (made when it has none: Blender's keyframe_insert
    makes the action and its slot, its path then named instead of numbered)."""
    weights = scalp.groomlab_tools.corrective_weights
    fc = weight_fcurve(scalp, name)
    if fc is None:
        with props.quiet():
            weights[name].weight = float(value)
        path = "groomlab_tools.corrective_weights[%d].weight" % weights.find(name)
        scalp.keyframe_insert(data_path=path, frame=float(frame))
        bag = _channelbag(scalp)
        fc = bag.fcurves.find(path) if bag is not None else None
        if fc is None:
            raise HelperError("state", "the weight of %s could not be keyed" % name)
        fc.data_path = weight_path(name)
    else:
        fc.keyframe_points.insert(float(frame), float(value))
        fc.update()
    return fc


def _channelbag(obj):
    ad = obj.animation_data
    if ad is None or ad.action is None:
        return None
    try:
        from bpy_extras import anim_utils
        return anim_utils.action_get_channelbag_for_slot(ad.action, ad.action_slot)
    except (ImportError, AttributeError):
        return ad.action  # before slotted actions: the action holds the fcurves


def weight_fcurve(scalp, name):
    bag = _channelbag(scalp)
    return bag.fcurves.find(weight_path(name)) if bag is not None else None


def remove_weight_fcurve(scalp, name):
    bag = _channelbag(scalp)
    fc = bag.fcurves.find(weight_path(name)) if bag is not None else None
    if fc is not None:
        bag.fcurves.remove(fc)
    groom_tools(scalp).pushed_keys.pop(name, None)


def sampled_keys(fc):
    """A weight fcurve as the helper's keys: [frame, weight] on every frame from its first key to
    its last (Maya writes keyed weights into files the same way)."""
    frames = sorted(k.co[0] for k in fc.keyframe_points)
    if not frames:
        return []
    first, last = int(round(frames[0])), int(round(frames[-1]))
    if first == last:
        return [[float(first), float(fc.evaluate(first))]]
    return [[float(f), round(float(fc.evaluate(f)), 6)] for f in range(first, last + 1)]


def push_corrective_keys(scalp, name, force=False):
    """Corrective `name`'s Blender fcurve sampled into the helper's keys (none: its keys dropped)."""
    fc = weight_fcurve(scalp, name)
    keys = sampled_keys(fc) if fc is not None else []
    gt = groom_tools(scalp)
    if not force and gt.pushed_keys.get(name) == keys:
        return None
    gt.pushed_keys[name] = keys
    if keys:
        return corrective_set(scalp, name, keys=keys)
    return corrective_clear_keys(scalp, name)


def key_corrective(scalp, name, frame=None, weight=None):
    """Key corrective `name`'s weight on `frame` (the current one) with Blender's keyframes, the
    fcurve sent to the helper."""
    scene = groom.scene_of(scalp)
    w = scalp.groomlab_tools.corrective_weights.get(name)
    if w is None:
        raise HelperError("not_found", "no corrective %r" % name)
    insert_weight_key(scalp, name, scene.frame_current if frame is None else frame,
                      w.weight if weight is None else weight)
    return push_corrective_keys(scalp, name, force=True)


def corrective_weight_changed(scalp, name, weight):
    """The weight slider: keyed here when the weight has keys (Maya: a keyed weight is keyed as you
    change it), else the corrective's weight on every frame."""
    try:
        if weight_fcurve(scalp, name) is not None:
            key_corrective(scalp, name)
        else:
            corrective_set(scalp, name, weight=float(weight))
    except HelperError as e:
        status(groom.scene_of(scalp), e.message)


def _keys_from_layer(scalp, layer):
    """A layer keyed (or faded as a fix) in the helper but with no Blender fcurve yet (Fix Frame, a
    .groom opened): its keys as Blender keyframes, linear, then sent back as the fcurve says."""
    name = layer["name"]
    keys = [list(k) for k in layer.get("keys") or []]
    if not keys and layer.get("frame") is not None and (layer.get("fadeIn") or layer.get("fadeOut")):
        f = float(layer["frame"])
        keys = [[f - float(layer.get("fadeIn") or 0.0), 0.0], [f, float(layer.get("weight", 1.0))],
                [f + float(layer.get("fadeOut") or 0.0), 0.0]]
    if not keys:
        return False
    fc = None
    for frame, weight in keys:
        fc = insert_weight_key(scalp, name, frame, weight)
    for k in fc.keyframe_points:
        k.interpolation = "LINEAR"
    return True


def refresh_correctives(scalp, scene=None):
    """The Correctives list: the stack top first, the Sculpt nodes, the base groom; the row brushes
    edit picked. Each corrective's weight lives on the groom as a Blender property (keyed with
    Blender's keyframes): a layer the helper keys that has no fcurve gets one."""
    scene = scene or groom.scene_of(scalp)
    t = scene.groomlab_tools
    r = correctives(scalp)
    layers = r.get("layers") or []
    target = r.get("target") or {"kind": "base"}
    weights = scalp.groomlab_tools.corrective_weights
    names = [l["name"] for l in layers]
    made_keys = []
    with props.quiet():
        for gone in [w.name for w in weights if w.name not in names]:
            remove_weight_fcurve(scalp, gone)
            weights.remove(weights.find(gone))
        for l in layers:
            w = weights.get(l["name"])
            if w is None:
                w = weights.add()
                w.name = l["name"]
            if weight_fcurve(scalp, l["name"]) is None:
                if (l.get("keyed") or l.get("fadeIn") or l.get("fadeOut")) and _keys_from_layer(scalp, l):
                    made_keys.append(l["name"])
                else:
                    w.weight = float(l.get("weight", 1.0))
        t.correctives.clear()
        picked = 0
        for l in reversed(layers):  # the top of the stack first
            item = t.correctives.add()
            item.name, item.kind = l["name"], "layer"
            item.enabled = bool(l.get("enabled", True))
            item.weight_now = float(l.get("weightNow", 0.0))
            item.keyed = bool(l.get("keyed")) or weight_fcurve(scalp, l["name"]) is not None
            item.keys = len(l.get("keys") or [])
            item.has_frame = l.get("frame") is not None
            item.frame = float(l.get("frame") or 0.0)
            item.guides = int(l.get("guides") or 0)
            if target.get("kind") == "corrective" and target.get("name") == l["name"]:
                picked = len(t.correctives) - 1
        for n in r.get("sculptNodes") or []:
            item = t.correctives.add()
            item.name, item.kind = n["name"], "node"
            item.guides = int(n.get("editedGuides") or 0)
            if target.get("kind") == "node" and target.get("name") == n["name"]:
                picked = len(t.correctives) - 1
        item = t.correctives.add()
        item.name, item.kind = "Base groom", "base"
        if target.get("kind") not in ("corrective", "node"):
            picked = len(t.correctives) - 1
        t.corrective_index = picked
    for name in made_keys:
        push_corrective_keys(scalp, name, force=True)
    for name in names:  # Blender's keys are the weights' (after Blender's undo, a .blend opened): sent when they differ
        if name not in made_keys and weight_fcurve(scalp, name) is not None:
            push_corrective_keys(scalp, name)


def picked_corrective(scene):
    """The picked row of the Correctives list: (kind, name), or (None, "")."""
    t = scene.groomlab_tools
    if 0 <= t.corrective_index < len(t.correctives):
        row = t.correctives[t.corrective_index]
        return row.kind, row.name
    return None, ""


@bpy.app.handlers.persistent
def _on_depsgraph(scene, depsgraph):
    """A weight's keys edited in Blender's timeline or Graph Editor: sent to the helper (the
    fcurve sampled again; nothing when it is as last sent)."""
    actions = set()
    for u in depsgraph.updates:
        if isinstance(u.id, bpy.types.Action):
            actions.add(u.id.original if hasattr(u.id, "original") else u.id)
    if not actions:
        return
    for scalp in groom.scalps():
        ad = scalp.animation_data
        if ad is None or ad.action not in actions or not is_hierarchy(scalp):
            continue
        rt = groom.runtime_if_any(scalp)
        if rt is None or rt.session is None or not process.running():
            continue
        for w in scalp.groomlab_tools.corrective_weights:
            try:
                if weight_fcurve(scalp, w.name) is not None or w.name in groom_tools(scalp).pushed_keys:
                    push_corrective_keys(scalp, w.name)
            except HelperError as e:
                print("GroomLab: %s's keys were not sent: %s" % (w.name, e.message))


def undo_label(scalp, redo=False):
    gt = _groom_tools.get(groom.runtime(scalp).uid) if scalp is not None else None
    if gt is None or not gt.undo:
        return None
    if redo:
        return gt.undo.get("redoLabel") if gt.undo.get("canRedo") else None
    return gt.undo.get("undoLabel") if gt.undo.get("canUndo") else None


# ---- The groom's state: kept in the .blend, given back to a new helper ---------------------------

def mark_state(scalp):
    """After an edit: the groom's state kept soon (a helper that ends gives nothing back), once
    the edits pause (STATE_DELAY): a stroke's release stays quick. Headless (no timers): now."""
    groom_tools(scalp).state_dirty = True
    if bpy.app.background:
        keep_state(scalp)
        return
    if bpy.app.timers.is_registered(_keep_dirty):
        bpy.app.timers.unregister(_keep_dirty)
    bpy.app.timers.register(_keep_dirty, first_interval=STATE_DELAY)


def _keep_dirty():
    from . import tool_ops
    if tool_ops.stroking():
        return STATE_DELAY  # not during a stroke
    for scalp in groom.scalps():
        rt = groom.runtime_if_any(scalp)
        gt = _groom_tools.get(rt.uid) if rt is not None else None
        if gt is not None and gt.state_dirty:
            keep_state(scalp)
    return None


def keep_state(scalp):
    """get_state into memory (a helper started again gets it back); the .blend's copy is written
    before it is saved (store_states). The scalp stays out of it unless maps painted on its
    vertices live in it (set_state keeps the session's own scalp)."""
    rt = groom.runtime_if_any(scalp)
    if rt is None or rt.session is None or rt.generation != process.generation() or not process.running():
        return None
    try:
        state = process.call("get_state", session=rt.session, scalp="maps")
    except HelperError as e:
        print("GroomLab: the groom of %s was not kept: %s" % (scalp.name, e))
        return None
    keep = {"groom": state["groom"], "textures": state.get("textures") or [], "tools": state.get("tools") or {}}
    _states[rt.uid] = keep
    gt = _groom_tools.get(rt.uid)
    if gt is not None:
        gt.state_dirty = False
    return keep


def store_states():
    """Before a save: each GroomLab-tools groom's state into its scalp object (the .blend)."""
    for scalp in groom.scalps():
        if not is_hierarchy(scalp):
            continue
        keep = keep_state(scalp)
        if keep is not None:
            scalp.groomlab_tools.state = json.dumps(keep, separators=(",", ":"))


def restore(scalp, rt, scene):
    """groom._build's hook: a GroomLab-tools groom's session from its state (memory, else the
    .blend), or a new groom (new_groom) when it has none. False: not ours (B2's snapshot path)."""
    if not is_hierarchy(scalp):
        return False
    state = _states.get(rt.uid)
    if state is None and scalp.groomlab_tools.state:
        try:
            state = json.loads(scalp.groomlab_tools.state)
        except ValueError:
            state = None
    if state is None and scalp.groomlab.snapshot:
        return False  # a hierarchy groom kept by B2's snapshot (a .groom loaded, saved before any edit)
    groom.sync_scalp(scalp, rt, force=True)
    if state is not None:
        process.call("set_state", session=rt.session, groom=state["groom"], textures=state.get("textures") or [],
                     tools=state.get("tools") or {}, keepScalp=True)
    else:
        process.call("new_groom", session=rt.session, followScalp=bool(scalp.groomlab.follow_scalp))
    return True


def restore_tools(scalp, rt):
    """The palette's lists for a (new) session."""
    refresh_info(scalp)


# ---- New grooms ---------------------------------------------------------------------------------

def start_groom(scalp, rt, preset=None, follow=True):
    """new_groom: Maya's create_groom (the default graph or the preset's, no guides)."""
    args = {"followScalp": bool(follow)}
    if preset:
        args["preset"] = preset
    return process.call("new_groom", session=rt.session, **args)


def starter_guides(scalp, rt, points, counts):
    """Starter guides into the hierarchy's first group ("primary", as Place makes it)."""
    if not len(counts):
        return None
    process.call("guides_edit", session=rt.session, op="add_group", name=FIRST_GROUP)
    client = process.client()
    return process.call("set_guides", session=rt.session, target="hierarchy",
                        **client.put(points=np.asarray(points, np.float32), counts=np.asarray(counts, np.int32),
                                     ids=np.zeros(len(counts), np.uint64)))


# ---- Handlers ------------------------------------------------------------------------------------

@bpy.app.handlers.persistent
def _on_load(*args):
    forget_all()
    from . import display
    display.forget_all()
    if not bpy.app.background and not bpy.app.timers.is_registered(_wake):
        bpy.app.timers.register(_wake, first_interval=0.5)


@bpy.app.handlers.persistent
def _on_undo(*args):
    """Blender's undo and redo put the palette's lists (Blender data: the maps, the groups) back as
    they were, while the groom in the helper keeps to GroomLab's own undo: the lists read again
    from the helper, so a map made or removed since still shows as it is."""
    for scalp in groom.scalps():
        rt = groom.runtime_if_any(scalp)
        if not is_hierarchy(scalp) or rt is None or rt.session is None or rt.generation != process.generation()                 or not process.running():
            continue
        try:
            refresh_info(scalp)
        except HelperError as e:
            print("GroomLab: the palette of %s was not read again: %s" % (scalp.name, e.message))


def _wake():
    """A file just opened: its GroomLab-tools grooms back in a helper (their state from the .blend),
    so their guides draw and the tools work at once."""
    for scalp in groom.scalps():
        if is_hierarchy(scalp):
            groom.mark(scalp, "eval")
    return None


def register():
    groom.before_save.append(store_states)
    groom.build_hooks.append(restore)
    if _on_load not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load)
    for handlers in (bpy.app.handlers.undo_post, bpy.app.handlers.redo_post):
        if _on_undo not in handlers:
            handlers.append(_on_undo)
    if _on_depsgraph not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph)


def unregister():
    if store_states in groom.before_save:
        groom.before_save.remove(store_states)
    if restore in groom.build_hooks:
        groom.build_hooks.remove(restore)
    if _on_load in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load)
    for handlers in (bpy.app.handlers.undo_post, bpy.app.handlers.redo_post):
        if _on_undo in handlers:
            handlers.remove(_on_undo)
    if _on_depsgraph in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph)
    _deferred.clear()
    if bpy.app.timers.is_registered(_wake):
        bpy.app.timers.unregister(_wake)
    if bpy.app.timers.is_registered(_keep_dirty):
        bpy.app.timers.unregister(_keep_dirty)
    forget_all()
