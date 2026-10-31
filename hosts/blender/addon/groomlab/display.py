# SPDX-License-Identifier: GPL-3.0-or-later
"""The groom in the 3D viewport as Maya draws it: the helper's display buffers (docs/BLENDER.md,
"Display buffers": Maya's Viewport 2.0 geometry from the core's DisplayBuilder) drawn with Blender's
gpu module. Guides colored by group (frozen ones blue), the selection white and the soft selection
orange, tubes (shaded, and as wires while the Shape brush is on, the selected ones warm), part
lines, flow arrows and field, a painted map on the scalp, root and tip dots, and the brush's ring
with the guides it would touch. Strand color modes (random, clump, density,
a color set or channel) go to the hair object as a color attribute the viewport shows.

Each item comes only when its version changed (`have`); its arrays are copied out of the shared
memory at once (they live until the next answer) and made into a gpu batch when first drawn. The
batch building is plain numpy (tests check it without drawing); the draw handler runs in POST_VIEW
with the scalp's matrix, overlays pulled toward the camera by the hair's depth as Maya pulls them."""

import time

import bpy
import numpy as np

from . import groom, process
from .helper_client import HelperError

ORDER = ("scalpMap", "tubes", "guides", "tubeWires", "flowField", "selectedGuides", "softSelectedGuides",
         "selectedTubeWires", "parts", "flowArrows")
# Pulled toward the camera by the hair's depth, so they show through the strands round them (the
# tubes too: a guide's tube sits inside its clump).
PULLED = {"tubes", "guides", "tubeWires", "flowField", "selectedGuides", "softSelectedGuides", "selectedTubeWires", "parts",
          "flowArrows"}
DISPLAY_COLORS = "groomlab_display"   # the hair's color attribute for the strand color modes
LIGHT = np.array([0.35, -0.55, 0.75], np.float32) / np.linalg.norm([0.35, -0.55, 0.75])


class Item(object):
    """One display item as the draw handler needs it (host space: the scalp's local space)."""

    def __init__(self, name):
        self.name = name
        self.version = 0
        self.shown = False
        self.primitive = "lines"
        self.positions = np.zeros((0, 3), np.float32)
        self.indices = np.zeros((0, 2), np.uint32)
        self.colors = None       # (n, 4) per vertex, or None: `color`
        self.normals = None
        self.color = (1.0, 1.0, 1.0, 1.0)
        self.width = 1.0
        self.layer = "dormantWire"
        self.batch = None        # (batch, shader) once drawn

    def empty(self):
        return not self.shown or len(self.positions) == 0 or len(self.indices) == 0


class GroomDisplay(object):
    def __init__(self):
        self.reset()

    def reset(self):
        self.key = None
        self.generation = None
        self.items = {}
        self.dots = Item("dots")
        self.dots.primitive = "points"
        self.dots_version = 0
        self.point_size = 6.0
        self.brush = Item("brush")
        self.brush_version = 0
        self.ring = None
        self.overlay_pull = 0.0
        self.strand_version = 0
        self.strand_mode = "solid"
        # From the colors last taken: the Source's choices and the source's range (Auto Range).
        self.strand_channels, self.strand_color_sets, self.strand_range = [], [], None
        self.pending_colors = None   # (colors, perPoint) for the hair, written outside drawing
        self.serial = None
        self.dirty = True
        self.refreshed = 0.0
        self.stroking = False


_displays = {}
_handle = [None]


def state(scalp):
    rt = groom.runtime_if_any(scalp)
    uid = rt.uid if rt is not None else groom.runtime(scalp).uid
    d = _displays.get(uid)
    if d is None:
        d = _displays[uid] = GroomDisplay()
    return d


def _get(scalp):
    rt = groom.runtime_if_any(scalp)
    return _displays.get(rt.uid) if rt is not None else None


def forget_all():
    _displays.clear()


def mark(scalp, viewport_only=False):
    if scalp is None:
        return
    state(scalp).dirty = True
    redraw(viewport_only)


def mark_all():
    for d in _displays.values():
        d.dirty = True


def redraw(viewport_only=False):
    groom._redraw(viewport_only)


# ---- What the display asks for: Maya's display settings of the groom ----------------------------

def settings_for(scalp, scene):
    from . import tools
    d = scalp.groomlab_tools
    t = scene.groomlab_tools
    s = {"guides": bool(d.show_guides), "tubes": bool(d.show_tubes), "flow": bool(d.show_flow),
         "guidePoints": d.guide_points, "guideColorMode": d.guide_color_mode,
         # The automatic guide colors start from the groom's own hue (its name, as Maya's transform).
         "guideColorSeed": scalp.name, "theme": tools.theme()}
    if d.guide_color_mode == "layer" and d.guide_color_layer:
        s["guideColorLayer"] = d.guide_color_layer
    m = tools.current_map(scene)
    painting = t.tool == "paint" or t.tab == "PAINT"
    if painting and t.show_on_guides and m is not None and m.kind == "guides":
        s["guideColorMode"] = "layer"
        s["guideColorLayer"] = m.name
    if t.tool == "shape":
        s["tubeWires"] = True  # while the Shape brush is on: every tube it can shape as a wire (Maya's)
    if t.isolate:
        _level, group = tools.group_target(scene)
        s["isolateGroup"] = group or ""
    return s


def map_for(scene):
    from . import tools
    t = scene.groomlab_tools
    m = tools.current_map(scene)
    if m is None or not t.show_on_scalp or not (t.tool == "paint" or t.tab == "PAINT"):
        return None
    return {"name": m.name, "channel": t.scalp_channel}


def strand_colors_for(scalp):
    d = scalp.groomlab_tools
    return {"mode": d.strand_color, "source": d.strand_color_source or "clumpId",
            "min": float(d.strand_color_min), "max": float(d.strand_color_max)}


# ---- The helper's answer into items ---------------------------------------------------------------

def _copy(client, ref, dtype=None):
    a = client.array(ref)
    return a.astype(dtype) if dtype is not None and a.dtype != dtype else a


def refresh(scalp, scene=None, apply_colors=True):
    """Asks the helper for what changed and keeps it (arrays copied). `apply_colors`: the strand
    colors written into the hair now (not while drawing: Blender's data is read-only there)."""
    from . import tools
    scene = scene or groom.scene_of(scalp)
    # While drawing, Blender's data is read-only: the live session as it is (draw() checks it is up).
    rt = tools.session(scalp, scene) if apply_colors else groom.runtime_if_any(scalp)
    gd = state(scalp)
    key = (rt.session, rt.generation)
    if gd.key != key:
        gd.reset()
        gd.key = key
    have = {name: it.version for name, it in gd.items.items() if it.version}
    if gd.dots_version:
        have["dots"] = gd.dots_version
    if gd.brush_version:
        have["brush"] = gd.brush_version
    if gd.strand_version:
        have["strandColors"] = gd.strand_version
    args = {"settings": settings_for(scalp, scene), "have": have, "strandColors": strand_colors_for(scalp)}
    if gd.generation:
        args["generation"] = gd.generation
    m = map_for(scene)
    if m is not None:
        args["map"] = m
    r = process.call("display", session=rt.session, **args)
    take_display(scalp, r)
    gd.serial = rt.serial
    gd.dirty = False
    gd.refreshed = time.monotonic()
    if apply_colors:
        apply_pending(scalp)
    return r


def take_display(scalp, r):
    """A `display` answer into the groom's items."""
    client = process.client()
    gd = state(scalp)
    if r.get("generation") != gd.generation:
        gd.generation = r.get("generation")
    gd.overlay_pull = float(r.get("overlayPull") or 0.0)
    gd.stroking = bool(r.get("stroking"))
    for name, it in (r.get("items") or {}).items():
        if name == "dots":
            gd.point_size = float(it.get("pointSize") or 6.0)
            gd.dots.shown = it.get("count", 0) > 0
            if it.get("changed") and "arrays" in it:
                a = it["arrays"]
                gd.dots.positions = _copy(client, a["positions"], np.float32)
                c = _copy(client, a["colors"], np.float32)
                gd.dots.colors = np.concatenate([c, np.ones((len(c), 1), np.float32)], axis=1) if c.shape[1] == 3 else c
                gd.dots.indices = np.arange(len(gd.dots.positions), dtype=np.uint32).reshape(-1, 1)
                gd.dots.batch = None
            gd.dots_version = it.get("version", 0)
            continue
        item = gd.items.get(name)
        if item is None:
            item = gd.items[name] = Item(name)
        item.version = it.get("version", 0)
        item.shown = bool(it.get("shown"))
        item.primitive = it.get("primitive", "lines")
        item.color = tuple(it.get("color") or (1.0, 1.0, 1.0, 1.0))
        item.width = float(it.get("lineWidth") or 1.0)
        item.layer = it.get("layer", "dormantWire")
        if it.get("changed") and "arrays" in it:
            a = it["arrays"]
            item.positions = _copy(client, a["positions"], np.float32)
            item.indices = _copy(client, a["indices"], np.uint32)
            item.colors = _copy(client, a["colors"], np.float32) if "colors" in a else None
            if item.colors is not None and item.colors.shape[1] == 3:
                item.colors = np.concatenate([item.colors, np.ones((len(item.colors), 1), np.float32)], axis=1)
            item.normals = _copy(client, a["normals"], np.float32) if "normals" in a else None
            item.batch = None
    brush = r.get("brush") or {}
    take_overlay(scalp, brush.get("overlay"), brush.get("ring"))
    sc = r.get("strandColors")
    if sc:
        gd.strand_mode = sc.get("mode", "solid")
        if gd.strand_mode == "solid":
            if gd.strand_version != sc.get("version"):
                gd.pending_colors = (None, False)
            gd.strand_version = sc.get("version", 0)
        elif sc.get("changed") and "colors" in sc:
            gd.pending_colors = (_copy(client, sc["colors"], np.float32), bool(sc.get("perPoint")))
            gd.strand_version = sc.get("version", 0)
            gd.strand_channels = list(sc.get("channels") or [])
            gd.strand_color_sets = list(sc.get("colorSets") or [])
            gd.strand_range = tuple(sc["range"]) if sc.get("range") else None


def take_overlay(scalp, overlay, ring=None):
    """The brush overlay of a hover, a stroke or a display answer (its arrays when they came)."""
    gd = state(scalp)
    if ring is not None:
        gd.ring = ring
    if not overlay:
        return
    if "arrays" in overlay:
        client = process.client()
        a = overlay["arrays"]
        gd.brush.positions = _copy(client, a["positions"], np.float32)
        gd.brush.indices = _copy(client, a["indices"], np.uint32)
        gd.brush.colors = _copy(client, a["colors"], np.float32) if "colors" in a else None
        gd.brush.shown = True
        gd.brush.width = 2.0
        gd.brush.batch = None
    elif overlay.get("vertexCount", 1) == 0:
        gd.brush.positions = np.zeros((0, 3), np.float32)
        gd.brush.indices = np.zeros((0, 2), np.uint32)
        gd.brush.batch = None
    gd.brush_version = overlay.get("version", gd.brush_version)


# ---- Strand colors and visibility on the hair object --------------------------------------------

def apply_pending(scalp):
    gd = state(scalp)
    if gd.pending_colors is None:
        return False
    colors, per_point = gd.pending_colors
    gd.pending_colors = None
    return set_strand_colors(scalp, colors, per_point)


def set_strand_colors(scalp, colors, per_point=False):
    """The strand color mode's colors as the hair's active color attribute (None: the hair's own)."""
    hair = scalp.groomlab.hair
    if hair is None or hair.type != "CURVES":
        return False
    curves = hair.data
    attrs = curves.attributes
    if colors is None:
        if attrs.get(DISPLAY_COLORS) is not None:
            attrs.remove(attrs[DISPLAY_COLORS])
        if attrs.get(groom.COLOR) is not None:
            _make_active(curves, groom.COLOR)
        curves.update_tag()
        return True
    domain = "POINT" if per_point else "CURVE"
    count = len(curves.points) if per_point else len(curves.curves)
    if len(colors) != count:
        state(scalp).dirty = True  # the strands changed since: asked again
        return False
    a = attrs.get(DISPLAY_COLORS)
    if a is not None and (a.domain != domain or a.data_type != "FLOAT_COLOR"):
        attrs.remove(a)
        a = None
    if a is None:
        a = attrs.new(DISPLAY_COLORS, "FLOAT_COLOR", domain)
    rgba = np.ones((count, 4), np.float32)
    rgba[:, :3] = colors[:, :3]
    a.data.foreach_set("color", rgba.ravel())
    _make_active(curves, DISPLAY_COLORS)
    curves.update_tag()
    return True


def _make_active(curves, name):
    """The hair's active (color) attribute: what Solid shading's Attribute color shows."""
    attrs = curves.attributes
    a = attrs.get(name)
    if a is None:
        return
    try:
        attrs.active = a
    except Exception:
        pass
    try:
        curves.color_attributes.active_color = curves.color_attributes[name]  # where Blender has it
    except Exception:
        pass


def apply_strand_visibility(scalp, stroking=False):
    hair = scalp.groomlab.hair
    if hair is None:
        return
    hide = not scalp.groomlab_tools.show_strands
    if stroking and groom.scene_of(scalp).groomlab_tools.sculpt_display == "guidesOnly":
        hide = True
    if hair.hide_viewport != hide:
        hair.hide_viewport = hide


def show_attribute_colors(context):
    """The 3D views in Solid shading show the hair's active color attribute (the strand colors)."""
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                shading = area.spaces.active.shading
                if shading.type == "SOLID" and shading.color_type != "VERTEX":
                    shading.color_type = "VERTEX"


# ---- Draw lists: what the handler draws, in order (plain numpy) ---------------------------------

def lit_colors(item):
    """A tube's triangles shaded by a fixed light (its normals) in its color."""
    base = np.asarray(item.color, np.float32)
    n = item.normals
    if n is None or len(n) != len(item.positions):
        return np.tile(base, (len(item.positions), 1))
    shade = 0.6 + 0.4 * np.abs(n @ LIGHT)  # light, as Maya's headlight shows them (never near black)
    out = np.empty((len(n), 4), np.float32)
    out[:, :3] = base[:3] * shade[:, None]
    out[:, 3] = base[3]
    return out


def draw_list(scalp):
    """[(name, primitive, positions, indices, colors (n, 4) or None, uniform color, width, pulled,
    depth)] in Maya's layer order: filled, wires, selection, dots, then the brush (no depth test)."""
    gd = _get(scalp)
    if gd is None:
        return []
    out = []
    for name in ORDER:
        it = gd.items.get(name)
        if it is None or it.empty():
            continue
        colors = it.colors
        if it.primitive == "triangles" and colors is None:
            colors = lit_colors(it)
        out.append((name, it.primitive, it.positions, it.indices, colors, it.color, it.width, name in PULLED, True))
    if gd.dots.shown and len(gd.dots.positions):
        out.append(("dots", "points", gd.dots.positions, gd.dots.indices, gd.dots.colors, (1, 1, 1, 1), gd.point_size,
                     True, True))
    if gd.brush.shown and len(gd.brush.positions) and len(gd.brush.indices):
        out.append(("brush", "lines", gd.brush.positions, gd.brush.indices, gd.brush.colors, (1.0, 0.8, 0.2, 1.0),
                    gd.brush.width, False, False))
    return out


# ---- gpu ------------------------------------------------------------------------------------------

_shaders = {}


def _shader(name):
    import gpu
    s = _shaders.get(name)
    if s is None:
        s = _shaders[name] = gpu.shader.from_builtin(name)
    return s


def make_batch(primitive, positions, indices, colors, color):
    """(batch, shader name) for one draw list entry."""
    from gpu_extras.batch import batch_for_shader
    pos = np.ascontiguousarray(positions, np.float32)
    if primitive == "points":
        shader = "POINT_FLAT_COLOR"
        cols = colors if colors is not None else np.tile(np.asarray(color, np.float32), (len(pos), 1))
        return batch_for_shader(_shader(shader), "POINTS", {"pos": pos, "color": np.ascontiguousarray(cols, np.float32)}), shader
    idx = np.ascontiguousarray(indices, np.int32)
    if primitive == "triangles":
        if colors is None:
            shader = "UNIFORM_COLOR"
            return batch_for_shader(_shader(shader), "TRIS", {"pos": pos}, indices=idx), shader
        shader = "SMOOTH_COLOR"
        return batch_for_shader(_shader(shader), "TRIS", {"pos": pos, "color": np.ascontiguousarray(colors, np.float32)},
                                indices=idx), shader
    if colors is None:
        shader = "POLYLINE_UNIFORM_COLOR"
        return batch_for_shader(_shader(shader), "LINES", {"pos": pos}, indices=idx), shader
    shader = "POLYLINE_SMOOTH_COLOR"
    return batch_for_shader(_shader(shader), "LINES", {"pos": pos, "color": np.ascontiguousarray(colors, np.float32)},
                            indices=idx), shader


def _batch_of(gd, name, entry):
    it = gd.dots if name == "dots" else gd.brush if name == "brush" else gd.items.get(name)
    if it.batch is None:
        it.batch = make_batch(entry[1], entry[2], entry[3], entry[4], entry[5])
    return it.batch


def pull_matrix(gd, scalp_matrix):
    """A projection nudging overlays toward the camera by the hair's depth (Maya's overlay pull)."""
    import gpu
    from mathutils import Matrix
    proj = gpu.matrix.get_projection_matrix()
    view = gpu.matrix.get_model_view_matrix()
    pull = gd.overlay_pull if gd.overlay_pull > 0 else 0.002
    if abs(proj[3][2] + 1.0) < 1e-6:  # perspective: ndc depth changes by |P23| / z^2 per unit
        z = -(view @ scalp_matrix.translation).z
        z = max(z, 1e-4)
        eps = abs(proj[2][3]) * pull / (z * z)
    else:
        eps = abs(proj[2][2]) * pull
    eps = min(max(eps, 1e-6), 0.02)
    nudge = Matrix.Identity(4)
    nudge[2][3] = -eps
    return nudge @ proj


def draw_groom(scalp, region_size):
    import gpu
    gd = _get(scalp)
    if gd is None:
        return 0
    entries = draw_list(scalp)
    if not entries:
        return 0
    m = scalp.matrix_world
    pulled = pull_matrix(gd, m)
    drawn = 0
    gpu.state.blend_set("ALPHA")
    gpu.state.depth_mask_set(False)
    with gpu.matrix.push_pop():
        gpu.matrix.multiply_matrix(m)
        for entry in entries:
            name, primitive, _pos, _idx, colors, color, width, is_pulled, depth = entry
            batch, shader_name = _batch_of(gd, name, entry)
            shader = _shader(shader_name)
            gpu.state.depth_test_set("LESS_EQUAL" if depth else "NONE")
            with gpu.matrix.push_pop_projection():
                if is_pulled:
                    gpu.matrix.load_projection_matrix(pulled)
                shader.bind()
                if shader_name.startswith("POLYLINE"):
                    shader.uniform_float("viewportSize", region_size)
                    shader.uniform_float("lineWidth", float(width))
                if shader_name in ("POLYLINE_UNIFORM_COLOR", "UNIFORM_COLOR"):
                    shader.uniform_float("color", tuple(color))
                if primitive == "points":
                    gpu.state.point_size_set(float(width))
                batch.draw(shader)
                drawn += 1
    gpu.state.depth_test_set("NONE")
    gpu.state.blend_set("NONE")
    return drawn


def live(scalp):
    """Whether the groom's session is up (drawing never starts a helper)."""
    rt = groom.runtime_if_any(scalp)
    return rt is not None and rt.session is not None and rt.generation == process.generation() and process.running()


def draw():
    """POST_VIEW: every live groom in the view layer, its display refreshed when it changed."""
    context = bpy.context
    region = context.region
    if region is None:
        return
    view_layer = context.view_layer
    size = (float(region.width), float(region.height))
    for scalp in groom.scalps():
        if not live(scalp):
            continue
        try:
            if not scalp.visible_get(view_layer=view_layer):
                continue
        except Exception:
            continue
        gd = state(scalp)
        rt = groom.runtime_if_any(scalp)
        if gd.dirty or gd.serial != rt.serial:
            try:
                refresh(scalp, apply_colors=False)
            except HelperError as e:
                gd.dirty = False
                print("GroomLab: display: %s" % e)
                continue
            if gd.pending_colors is not None:
                _schedule_colors()
        try:
            draw_groom(scalp, size)
        except Exception as e:  # noqa: BLE001 - a draw handler must not raise every redraw
            print("GroomLab: drawing %s: %r" % (scalp.name, e))


def _schedule_colors():
    if not bpy.app.timers.is_registered(_apply_all):
        bpy.app.timers.register(_apply_all, first_interval=0.0)


def _apply_all():
    for scalp in groom.scalps():
        rt = groom.runtime_if_any(scalp)
        if rt is not None and rt.uid in _displays:
            try:
                apply_pending(scalp)
            except Exception as e:  # noqa: BLE001
                print("GroomLab: strand colors: %r" % e)
    return None


def register():
    if not bpy.app.background:
        _handle[0] = bpy.types.SpaceView3D.draw_handler_add(draw, (), "WINDOW", "POST_VIEW")


def unregister():
    if _handle[0] is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_handle[0], "WINDOW")
        _handle[0] = None
    if bpy.app.timers.is_registered(_apply_all):
        bpy.app.timers.unregister(_apply_all)
    _displays.clear()
    _shaders.clear()
