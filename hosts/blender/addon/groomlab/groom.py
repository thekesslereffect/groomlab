# SPDX-License-Identifier: GPL-3.0-or-later
"""A GroomLab groom in Blender: the scalp mesh (the groom's owner), a Guides Curves object the user
sculpts with Blender's Curves Sculpt brushes, and a locked Hair Curves object the add-on fills
from the helper's evaluation. Each groom has a session in the helper; this module keeps the
session fed (scalp, guides, time, space) and writes its strands into Blender with foreach_set.

Spaces: everything crosses the pipe in the scalp object's local space (Z up, scene units); the
guides and hair objects are parented to the scalp with identity transforms, so the scalp's object
transform carries the whole groom. The helper turns that into groom space (Y up, centimeters)."""

import hashlib
import json
import os
import tempfile
import threading
import time
import uuid

import bpy
import numpy as np
from mathutils import Matrix, Vector

from . import process, shading, starter, units
from .helper_client import HelperError

GUIDE_ID = "groomlab_id"             # guide curves: the helper's stable guide ID (0: drawn just now)
STRAND_ID = "groomlab_strand_id"     # hair curves: the strand's 64-bit ID as two ints (low, high)
COLOR = shading.STRAND_COLOR         # hair curves: GroomLab's per-strand linear RGB
POINT_COLOR = shading.POINT_COLOR    # hair curves: its per-point RGB, when the color varies along the strands
ROOT_UV = "surface_uv_coordinate"    # hair curves: the root's scalp UV (Blender's own name for it)
ROOT_NORMAL = shading.ROOT_NORMAL    # hair curves: the root's normal (with iridescence)
# The helper's attributes the hair gets: {name: (Blender attribute, data type)}; the domain is the
# attribute's scope (strand: CURVE, point: POINT). What Arnold's look reads (shading.py).
HAIR_ATTRIBUTES = {"color": (COLOR, "FLOAT_COLOR"), "pointColor": (POINT_COLOR, "FLOAT_COLOR"),
                   "melanin": ("melanin", "FLOAT"), "melaninRedness": ("melaninRedness", "FLOAT"),
                   "iridescence": ("iridescence", "FLOAT"),
                   "rootNormal": (ROOT_NORMAL, "FLOAT_VECTOR"), "rootUV": (ROOT_UV, "FLOAT2"),
                   # RenderData's opacity and HairColor's highlight (shading.py)
                   "opacity": ("opacity", "FLOAT"), "roughness": ("roughness", "FLOAT"),
                   "specular": ("specular", "FLOAT")}
ATTRIBUTES = list(HAIR_ATTRIBUTES)
OWN_ATTRIBUTES = {STRAND_ID} | {name for name, _ in HAIR_ATTRIBUTES.values()}
# Graph nodes Blender cannot feed yet (they need the scalp's earlier frames): the panel says so; the
# groom evaluates as the core does without that input. (Simulate steps frame by frame as the timeline
# plays, and Simulate Range hands it the scalp's frames: pipeline.py.)
UNSUPPORTED = {"Jiggle": "Jiggle is not supported in Blender yet: the strands pass through it (Blender does not "
                         "hand GroomLab the scalp's earlier frames)"}
DEBOUNCE = 0.04   # seconds after the last change before evaluating
MIN_WAIT = 0.06   # while changes keep coming (a slider drag): an evaluation at least this far apart,
WAIT_PER_COST = 1.5  # and this many times the last one's own time apart (Blender stays responsive between)
MAX_WAIT = 0.30   # but never further apart than this


class Runtime(object):
    """What the add-on knows of one groom's helper session (not saved: rebuilt from the objects)."""

    def __init__(self, uid):
        self.uid = uid
        self.session = None
        self.generation = -1
        self.reset()

    def reset(self):
        self.keys = {}
        self.target = "auto"
        self.serial = None
        self.mode = None
        self.frame = None
        self.fps = None
        self.space = None
        self.follow = None
        self.last = {}
        self.notes = {}
        self.errors = []
        self.messages = []
        self.error = ""
        self.attributes = []
        self.look = {}
        self.scalp_note = ""


_runtimes = {}
_pending = {}
_last_change = 0.0
_last_flush = 0.0
_last_cost = 0.0   # seconds the last flush took (evaluation, the hair written)
_timer_on = False
_rendering = False
_exporting = False
_restore_after_render = False
_main_thread = threading.main_thread()
_cache = {"count": -1, "names": []}
before_save = []  # callables run before a groom is written (.groom, the .blend's copy): the Graph Editor's positions
# callables (scalp, rt, scene) run when a session is (re)built: True when one gave it the groom (GroomLab's tools:
# the groom's state kept in the .blend), else B2's snapshot is loaded
build_hooks = []
# callables (scalp, rt, depsgraph, force, read_pose) run by sync() after the scalp and guides: the groom's other
# inputs (pipeline.py: its colliders as they are posed now); True when one sent something
input_hooks = []


# ---- Grooms -----------------------------------------------------------------------------------

def is_groom(obj):
    return obj is not None and obj.type == "MESH" and obj.groomlab.role == "SCALP"


def scalps():
    """Every groom's scalp object in the file (cached by name; rebuilt when objects come or go)."""
    objects = bpy.data.objects
    if _cache["count"] != len(objects):
        _cache["names"] = [o.name for o in objects if is_groom(o)]
        _cache["count"] = len(objects)
    found = []
    for name in _cache["names"]:
        o = objects.get(name)
        if o is None or not is_groom(o):
            _cache["count"] = -1
            return scalps()
        found.append(o)
    return found


def forget_cache():
    _cache["count"] = -1


def scalp_of(obj):
    if obj is None:
        return None
    role = obj.groomlab.role
    if role == "SCALP":
        return obj
    if role in ("GUIDES", "HAIR"):
        return obj.groomlab.scalp
    return None


def find(uid):
    for s in scalps():
        if s.groomlab.uid == uid:
            return s
    return None


def runtime(scalp):
    g = scalp.groomlab
    if not g.uid or any(o is not scalp and o.groomlab.uid == g.uid for o in scalps()):
        g.uid = uuid.uuid4().hex  # a duplicated scalp object is a groom of its own
    rt = _runtimes.get(g.uid)
    if rt is None:
        rt = _runtimes[g.uid] = Runtime(g.uid)
    return rt


def runtime_if_any(scalp):
    return _runtimes.get(scalp.groomlab.uid) if scalp is not None else None


def scene_of(scalp):
    scene = getattr(bpy.context, "scene", None)
    if scene is not None and scalp.name in scene.objects:
        return scene
    users = scalp.users_scene
    return users[0] if users else scene


# ---- The helper session -----------------------------------------------------------------------

def _call(cmd, **args):
    return process.call(cmd, **args)


def _put(**arrays):
    return process.client().put(**arrays)


def space_of(scene):
    return {"upAxis": "Z", "metersPerUnit": units.meters_per_unit(scene)}


def fps_of(scene):
    return float(scene.render.fps) / float(scene.render.fps_base or 1.0)


def ensure(scalp, scene=None):
    """The groom's runtime with a live session (the helper started, the session rebuilt from the
    objects when the helper is new: first use, a crash, a file just opened)."""
    scene = scene or scene_of(scalp)
    rt = runtime(scalp)
    process.client()
    if rt.session is None or rt.generation != process.generation():
        _build(scalp, rt, scene)
    return rt


def _build(scalp, rt, scene):
    g = scalp.groomlab
    r = _call("new_session", space=space_of(scene), fps=fps_of(scene))
    rt.session = r["session"]
    rt.generation = process.generation()
    rt.reset()
    rt.space = space_of(scene)
    rt.target = g.guide_target or "auto"
    restored = False
    for fn in build_hooks:
        try:
            restored = fn(scalp, rt, scene) or restored
        except HelperError as e:
            rt.error = "the groom saved in this file did not load: %s" % e.message
    if g.snapshot and not restored:
        path = os.path.join(tempfile.gettempdir(), "groomlab_%s_%d.groom" % (rt.uid, os.getpid()))
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(g.snapshot)
            _call("load_groom", session=rt.session, path=path)
        except HelperError as e:
            rt.error = "the groom saved in this file did not load: %s" % e.message
        finally:
            try:
                os.remove(path)
            except OSError:
                pass
    if g.graph_json:
        try:
            _call("set_graph", session=rt.session, graph=g.graph_json)
        except HelperError as e:
            rt.error = "the groom's graph did not load: %s" % e.message
    rt.follow = bool(g.follow_scalp)
    _call("set_options", session=rt.session, followScalp=rt.follow)


def close(scalp):
    rt = _runtimes.pop(scalp.groomlab.uid, None)
    if rt and rt.session is not None and rt.generation == process.generation() and process.running():
        try:
            _call("close_session", session=rt.session)
        except HelperError:
            pass


def forget_all():
    """A file was opened: the sessions belong to grooms that are gone."""
    for rt in list(_runtimes.values()):
        if rt.session is not None and rt.generation == process.generation() and process.running():
            try:
                _call("close_session", session=rt.session)
            except HelperError:
                pass
    _runtimes.clear()
    _pending.clear()
    forget_cache()


def invalidate_all():
    """Undo or redo restored the objects: send everything again, write the hair again."""
    for rt in _runtimes.values():
        rt.keys.clear()
        rt.serial = None
    for s in scalps():
        mark(s, "scalp", "guides", "eval")


# ---- Scalp ------------------------------------------------------------------------------------

def _key(*parts):
    h = hashlib.blake2b(digest_size=16)
    for p in parts:
        if p is None:
            h.update(b"-")
        elif isinstance(p, np.ndarray):
            h.update(str(p.shape).encode())
            h.update(np.ascontiguousarray(p).tobytes())
        else:
            h.update(repr(p).encode())
    return h.digest()


def uv_layer(mesh, name=""):
    layer = mesh.uv_layers.get(name) if name else None
    return layer or mesh.uv_layers.active


def read_scalp(scalp):
    """The mesh before modifiers: points (the rest pose), face counts, corner indices and corner UVs."""
    mesh = scalp.data
    n = len(mesh.vertices)
    rest = np.empty(n * 3, np.float32)
    mesh.vertices.foreach_get("co", rest)
    counts = np.empty(len(mesh.polygons), np.int32)
    mesh.polygons.foreach_get("loop_total", counts)
    indices = np.empty(len(mesh.loops), np.int32)
    mesh.loops.foreach_get("vertex_index", indices)
    uvs = None
    layer = uv_layer(mesh, scalp.groomlab.uv_map)
    if layer is not None and len(mesh.loops):
        uvs = np.empty(len(mesh.loops) * 2, np.float32)
        layer.uv.foreach_get("vector", uvs)
        uvs = uvs.reshape(-1, 2)
    return rest.reshape(-1, 3), counts, indices, uvs


def deforms(scalp):
    return bool(scalp.modifiers) or bool(scalp.data.shape_keys) or scalp.find_armature() is not None


def deformed_points(scalp, depsgraph, n):
    """The scalp's points after its modifiers and shape keys (the frame's pose), or None when its
    modifiers change the point count (then the groom stays on the mesh as it is)."""
    if depsgraph is None:
        depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = scalp.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        if len(mesh.vertices) != n:
            return None
        out = np.empty(n * 3, np.float32)
        mesh.vertices.foreach_get("co", out)
        return out.reshape(-1, 3)
    finally:
        evaluated.to_mesh_clear()


def _sizes(scalp):
    mesh = scalp.data
    layer = uv_layer(mesh, scalp.groomlab.uv_map)
    return len(mesh.vertices), len(mesh.polygons), len(mesh.loops), layer.name if layer is not None else None


def rest_points(scalp):
    mesh = scalp.data
    rest = np.empty(len(mesh.vertices) * 3, np.float32)
    mesh.vertices.foreach_get("co", rest)
    return rest.reshape(-1, 3)


def sync_scalp(scalp, rt, depsgraph=None, force=False, read_pose=True, topology=True):
    """Sends the scalp when it changed: set_scalp for another mesh (topology, UVs or rest pose),
    update_scalp for another pose of the same mesh (armature, shape keys). True when sent.
    `topology=False`: the mesh itself was not edited (a frame, a pose, a param): its faces and UVs
    are read again only when their counts changed, the points always (a dense scalp's corners and
    UVs are most of the reading)."""
    sizes = _sizes(scalp)
    full = force or topology or "topology" not in rt.keys or sizes != rt.keys.get("sizes")
    if full:
        rest, counts, indices, uvs = read_scalp(scalp)
        if len(counts) == 0:
            raise HelperError("state", "the scalp mesh %s has no faces" % scalp.name)
    else:
        rest = rest_points(scalp)
    now = rest
    rt.scalp_note = ""
    if read_pose and deforms(scalp):
        posed = deformed_points(scalp, depsgraph, len(rest))
        if posed is None:
            rt.scalp_note = "the scalp's modifiers change its point count: GroomLab uses the mesh before modifiers"
        else:
            now = posed
    rest_key = _key(rest)
    now_key = rest_key if now is rest else _key(now)
    if not full and rest_key != rt.keys.get("rest"):
        return sync_scalp(scalp, rt, depsgraph, force, read_pose, topology=True)  # another rest pose: set_scalp
    topology = _key(counts, indices, uvs) if full else rt.keys.get("topology")
    if full:
        rt.keys["sizes"] = sizes
    if force or topology != rt.keys.get("topology") or rest_key != rt.keys.get("rest"):
        arrays = dict(points=now, restPoints=rest, faceCounts=counts, faceIndices=indices)
        if uvs is not None:
            arrays["uvs"] = uvs
        rt.last_scalp = _call("set_scalp", session=rt.session, **_put(**arrays))
        rt.keys.update(topology=topology, rest=rest_key, now=now_key, sizes=sizes)
        return True
    if now_key != rt.keys.get("now"):
        _call("update_scalp", session=rt.session, **_put(points=now, restPoints=rest))
        rt.keys["now"] = now_key
        return True
    return False


# ---- Guides -----------------------------------------------------------------------------------

def _guide_matrix(scalp, guides):
    """Guides object space to scalp object space (identity for a groom as the add-on made it)."""
    m = scalp.matrix_world.inverted_safe() @ guides.matrix_world
    ident = Matrix.Identity(4)
    if all(abs(m[i][j] - ident[i][j]) < 1e-7 for i in range(4) for j in range(4)):
        return None
    return np.array(m, np.float64)


def _ids_attribute(curves):
    a = curves.attributes.get(GUIDE_ID)
    if a is not None and (a.domain != "CURVE" or a.data_type != "INT"):
        curves.attributes.remove(a)
        a = None
    if a is None:
        a = curves.attributes.new(GUIDE_ID, "INT", "CURVE")
    return a


def read_guides(scalp, guides):
    """(points (P, 3) in scalp space, counts (C,), ids (C,) int32) of the guide curves."""
    curves = guides.data
    c, p = len(curves.curves), len(curves.points)
    pos = np.empty(p * 3, np.float32)
    if p:
        curves.attributes["position"].data.foreach_get("vector", pos)
    pos = pos.reshape(-1, 3)
    offsets = np.empty(c + 1, np.int32)
    curves.curve_offset_data.foreach_get("value", offsets)
    counts = np.diff(offsets).astype(np.int32)
    ids = np.zeros(c, np.int32)
    if c:
        _ids_attribute(curves).data.foreach_get("value", ids)
    m = _guide_matrix(scalp, guides)
    if m is not None and p:
        pos = (pos.astype(np.float64) @ m[:3, :3].T + m[:3, 3]).astype(np.float32)
    return pos, counts, ids


def write_guides(scalp, guides, points, counts, ids):
    """Replaces the guide curves (a groom loaded from a file)."""
    curves = guides.data
    if len(curves.curves):
        curves.remove_curves()
    if len(counts):
        curves.add_curves([int(n) for n in counts])
        m = _guide_matrix(scalp, guides)
        pts = np.asarray(points, np.float32)
        if m is not None:
            inv = np.linalg.inv(m)
            pts = (pts.astype(np.float64) @ inv[:3, :3].T + inv[:3, 3]).astype(np.float32)
        curves.attributes["position"].data.foreach_set("vector", pts.ravel())
        _ids_attribute(curves).data.foreach_set("value", np.asarray(ids, np.int64).astype(np.int32))
    curves.update_tag()


def sync_guides(scalp, rt, force=False):
    """Sends the guide curves when they changed (set_guides by stable ID, kept at the scalp's rest
    pose), writes the IDs of new curves back, and the helper's rooting of new or turned curves.
    True when the groom's guides changed."""
    guides = scalp.groomlab.guides
    if guides is None or guides.type != "CURVES":
        return False
    pos, counts, ids = read_guides(scalp, guides)
    key = _key(pos, counts, ids)
    if not force and key == rt.keys.get("guides"):
        return False
    # Curves of one point cannot be guides; a duplicated curve's ID belongs to its first copy.
    keep = counts >= 2
    sent_ids = ids.astype(np.int64)
    seen = set()
    for i, v in enumerate(sent_ids.tolist()):
        if v <= 0:
            sent_ids[i] = 0
        elif v in seen:
            sent_ids[i] = 0
        else:
            seen.add(v)
    point_keep = np.repeat(keep, counts)
    send_pos = np.ascontiguousarray(pos[point_keep])
    send_counts = np.ascontiguousarray(counts[keep])
    send_ids = sent_ids[keep].astype(np.uint64)
    if len(send_counts):
        r = _call("set_guides", session=rt.session, target=rt.target, pose="rest",
                  **_put(points=send_pos, counts=send_counts, ids=send_ids))
    else:
        r = _call("set_guides", session=rt.session, target=rt.target, pose="rest", points=[], counts=[])
    kept = np.nonzero(keep)[0]
    new_ids = sent_ids.copy()
    for a in r.get("added", []):
        new_ids[kept[a["index"]]] = a["id"]
    curves = guides.data
    wrote = False
    if not np.array_equal(new_ids.astype(np.int32), ids):
        _ids_attribute(curves).data.foreach_set("value", new_ids.astype(np.int32))
        wrote = True
    if r.get("updated") and len(send_counts):
        # The helper roots new and changed curves as Maya roots its guide curves (moved onto the
        # scalp, tip-first ones turned): give Blender the same shapes.
        g = _call("get_guides", session=rt.session, source=r.get("target", "flat"), pose="rest")
        client = process.client()
        back_ids = client.array(g["arrays"]["ids"]).astype(np.int64)
        back_counts = client.array(g["arrays"]["counts"])
        if np.array_equal(back_ids, new_ids[keep]) and np.array_equal(back_counts, send_counts):
            back = client.array(g["arrays"]["points"])
            if back.shape == send_pos.shape and np.abs(back - send_pos).max(initial=0.0) > 1e-6:
                full = pos.copy()
                full[point_keep] = back
                m = _guide_matrix(scalp, guides)
                if m is not None:
                    inv = np.linalg.inv(m)
                    full = (full.astype(np.float64) @ inv[:3, :3].T + inv[:3, 3]).astype(np.float32)
                curves.attributes["position"].data.foreach_set("vector", full.ravel())
                wrote = True
    if wrote:
        curves.update_tag()
        pos, counts, ids = read_guides(scalp, guides)
        key = _key(pos, counts, ids)
    rt.keys["guides"] = key
    rt.target = r.get("target", rt.target)
    return bool(r.get("updated")) or force


# ---- Strands into Blender ---------------------------------------------------------------------

def _attribute(curves, name, data_type, domain):
    a = curves.attributes.get(name)
    if a is not None and (a.data_type != data_type or a.domain != domain):
        curves.attributes.remove(a)
        a = None
    if a is None:
        a = curves.attributes.new(name, data_type, domain)
    return a


def write_hair(scalp, r):
    """The evaluation's strands into the hair Curves object: the curves rebuilt only when the
    strands are others (count or sizes), positions, radii and attributes set with foreach_set."""
    hair = ensure_hair_object(scalp)
    client = process.client()
    a = r["arrays"]
    pos = client.view(a["positions"])
    counts = client.view(a["pointCounts"])
    s, p = len(counts), len(pos)
    curves = hair.data
    same = len(curves.curves) == s and len(curves.points) == p
    if same and s:
        offsets = np.empty(s + 1, np.int32)
        curves.curve_offset_data.foreach_get("value", offsets)
        same = np.array_equal(np.diff(offsets), counts)
    if not same:
        if len(curves.curves):
            curves.remove_curves()
        if s:
            curves.add_curves(counts.tolist())
    if p:
        curves.attributes["position"].data.foreach_set("vector", np.ascontiguousarray(pos).ravel())
        if "widths" in a:
            radius = client.view(a["widths"]) * np.float32(0.5)
            _attribute(curves, "radius", "FLOAT", "POINT").data.foreach_set("value", radius)
    present = set()
    base = None  # the strands' base colors (the look's RGB, or its pigment's): the viewport color
    if s:
        ids = np.ascontiguousarray(client.view(a["ids"])).view(np.int32)
        _attribute(curves, STRAND_ID, "INT32_2D", "CURVE").data.foreach_set("value", ids)
        present.add(STRAND_ID)
        for name, ref in a.get("attributes", {}).items():
            if name not in HAIR_ATTRIBUTES:
                continue
            target, kind = HAIR_ATTRIBUTES[name]
            per_point = ref.get("scope") == "point"
            n = p if per_point else s
            values = np.ascontiguousarray(client.view(ref), np.float32).reshape(n, -1)
            attr = _attribute(curves, target, kind, "POINT" if per_point else "CURVE")
            if kind == "FLOAT_COLOR":
                rgba = np.ones((n, 4), np.float32)
                rgba[:, :3] = values[:, :3]
                attr.data.foreach_set("color", rgba.ravel())
                if target == COLOR:
                    base = values[:, :3]
            elif kind == "FLOAT":
                attr.data.foreach_set("value", values.ravel())
            else:
                attr.data.foreach_set("vector", values.ravel())
            present.add(target)
    for name in OWN_ATTRIBUTES - present:
        if curves.attributes.get(name) is not None:
            curves.attributes.remove(curves.attributes[name])
    if COLOR in present:
        try:
            curves.color_attributes.active_color = curves.color_attributes[COLOR]
        except Exception:
            pass
    curves.update_tag()
    mat = shading.ensure_material(scalp, hair, present, r.get("look") or {})
    shading.set_viewport_color(mat, base)  # Solid shading: the hair's look, not the default gray


# ---- Objects ----------------------------------------------------------------------------------

def _link_like(obj, scalp, scene):
    collections = list(scalp.users_collection) or [scene.collection]
    for c in collections:
        if obj.name not in c.objects:
            c.objects.link(obj)


def _parent_identity(obj, scalp):
    obj.parent = scalp
    obj.matrix_parent_inverse = Matrix.Identity(4)
    obj.matrix_basis = Matrix.Identity(4)


def make_guides_object(scalp, scene):
    curves = bpy.data.hair_curves.new("%s_guides" % scalp.name)
    obj = bpy.data.objects.new(curves.name, curves)
    _link_like(obj, scalp, scene)
    _parent_identity(obj, scalp)
    curves.surface = scalp
    layer = uv_layer(scalp.data, scalp.groomlab.uv_map)
    if layer is not None:
        curves.surface_uv_map = layer.name
    _ids_attribute(curves)
    obj.hide_render = True  # guides shape the hair; renders show the hair
    obj.groomlab.role = "GUIDES"
    obj.groomlab.scalp = scalp
    scalp.groomlab.guides = obj
    return obj


def ensure_hair_object(scalp):
    g = scalp.groomlab
    obj = g.hair
    if obj is not None and obj.type == "CURVES" and obj.name in bpy.data.objects:
        return obj
    scene = scene_of(scalp)
    curves = bpy.data.hair_curves.new("%s_hair" % scalp.name)
    obj = bpy.data.objects.new(curves.name, curves)
    _link_like(obj, scalp, scene)
    _parent_identity(obj, scalp)
    obj.hide_select = True  # the add-on's output: sculpt the guides, not this
    obj.groomlab.role = "HAIR"
    obj.groomlab.scalp = scalp
    g.hair = obj
    return obj


# ---- Evaluating -------------------------------------------------------------------------------

def _sync_settings(scalp, rt, scene):
    changed = False
    space = space_of(scene)
    if space != rt.space:
        _call("set_space", session=rt.session, **space)
        rt.space = space
        rt.keys.pop("topology", None)  # the scalp and guides again, in the new unit
        rt.keys.pop("guides", None)
        changed = True
    follow = bool(scalp.groomlab.follow_scalp)
    if follow != rt.follow:
        _call("set_options", session=rt.session, followScalp=follow)
        rt.follow = follow
        changed = True
    frame = float(scene.frame_current) + float(scene.frame_subframe)
    fps = fps_of(scene)
    if frame != rt.frame or fps != rt.fps:
        _call("set_time", session=rt.session, frame=frame, fps=fps)
        rt.frame, rt.fps = frame, fps
        changed = True
    return changed


def preview_args(scalp):
    """The viewport's thinning, as `evaluate` takes it: the preview density and the point budget
    (0: no limit), both a subset of the render's strands."""
    g = scalp.groomlab
    return {"density": float(g.preview_density), "pointBudget": 1000.0 * float(g.preview_point_budget)}


def evaluate(scalp, rt, mode="preview", **extra):
    """`extra`: more evaluate arguments (a live stroke's sculptDensity)."""
    args = {"session": rt.session, "attributes": ATTRIBUTES}
    args.update(extra)
    if mode == "render":
        args["mode"] = "render"
    else:
        args.update(preview_args(scalp))
        if scalp.groomlab.view_node:  # the Graph Editor's View: this node's strands (renders keep the output)
            args["viewNode"] = scalp.groomlab.view_node
    r = _call("evaluate", **args)
    rt.notes = dict(r.get("notes") or {})
    rt.errors = list(r.get("errors") or [])
    rt.messages = [m.get("text", "") for m in r.get("messages") or []]
    rt.attributes = [x["name"] for x in r.get("attributesAvailable") or []]
    rt.look = dict(r.get("look") or {})
    rt.last = {k: r.get(k) for k in ("strandCount", "pointCount", "milliseconds", "serial", "sameTopology",
                                     "nodesEvaluated", "nodesIncremental", "following")}
    rt.last["mode"] = mode
    hair = scalp.groomlab.hair
    if r.get("serial") != rt.serial or mode != rt.mode or hair is None:
        write_hair(scalp, r)
        rt.serial, rt.mode = r.get("serial"), mode
    return r


def sync(scalp, scene=None, depsgraph=None, mode=None, force_eval=False, evaluate_now=True, force_inputs=False,
         read_pose=True, topology=True):
    """Brings the groom's session up to date with Blender and evaluates when anything changed.
    Returns the evaluation (None when nothing needed one). `topology=False`: the scalp mesh was
    not edited since (sync_scalp)."""
    scene = scene or scene_of(scalp)
    rt = ensure(scalp, scene)
    fresh = rt.serial is None
    changed = _sync_settings(scalp, rt, scene)
    changed |= sync_scalp(scalp, rt, depsgraph, force=force_inputs or "topology" not in rt.keys, read_pose=read_pose,
                          topology=topology)
    changed |= sync_guides(scalp, rt, force=force_inputs or "guides" not in rt.keys)
    for fn in input_hooks:
        changed |= bool(fn(scalp, rt, depsgraph, force_inputs, read_pose))
    rt.error = ""
    mode = mode or ("render" if _rendering else "preview")
    if evaluate_now and (changed or force_eval or fresh or mode != rt.mode):
        return evaluate(scalp, rt, mode)
    return None


def sync_safely(scalp, **kwargs):
    """sync() for handlers and timers: an error is kept for the panel, never raised into Blender."""
    try:
        return sync(scalp, **kwargs)
    except HelperError as e:
        rt = runtime(scalp)
        rt.error = e.message
        if e.code == "gone":
            rt.session = None  # started again on the next use
        print("GroomLab: %s" % e)
    except Exception as e:  # noqa: BLE001 - a handler must not take Blender down
        runtime(scalp).error = str(e)
        print("GroomLab: %r" % e)
    return None


# ---- Changes, debounced -------------------------------------------------------------------------

def mark(scalp, *what):
    global _last_change
    _pending.setdefault(runtime(scalp).uid, set()).update(what or ("eval",))
    _last_change = time.monotonic()
    _schedule()


def _schedule():
    global _timer_on
    if _timer_on or bpy.app.background:
        return  # no event loop in background mode: tests call flush()
    try:
        bpy.app.timers.register(_tick, first_interval=DEBOUNCE)
        _timer_on = True
    except Exception:
        pass


def flush_wait():
    """Seconds between evaluations while changes keep coming: a slow groom's evaluations are
    spaced by their own cost, so Blender takes the slider's or the stroke's events in between."""
    return min(MAX_WAIT, max(MIN_WAIT, WAIT_PER_COST * _last_cost))


def _tick():
    global _timer_on
    now = time.monotonic()
    if now - _last_change < DEBOUNCE and now - _last_flush < flush_wait():
        return 0.02
    _timer_on = False
    flush()
    return None


def flush():
    """Evaluates every groom with pending changes now (the timer's work; tests call it)."""
    global _last_flush, _last_cost
    start = time.monotonic()
    pending = dict(_pending)
    _pending.clear()
    for uid, what in pending.items():
        scalp = find(uid)
        if scalp is None:
            continue
        # The scalp's topology is read again only when its mesh was edited ("scalp"); a pose
        # ("pose": an armature, shape keys) sends its points alone.
        sync_safely(scalp, force_eval="eval" in what, topology="scalp" in what)
        _redraw()
    _last_flush = time.monotonic()
    if pending:
        _last_cost = _last_flush - start


def _redraw(viewport_only=False):
    """The 3D views and Properties redrawn; `viewport_only`: the 3D views' viewports alone (a
    hover's ring, a stroke's guides: the sidebar's panels are not drawn again on every mouse move)."""
    wm = getattr(bpy.context, "window_manager", None)
    if wm is None:
        return
    for window in wm.windows:
        for area in window.screen.areas:
            if viewport_only:
                if area.type == "VIEW_3D":
                    for region in area.regions:
                        if region.type == "WINDOW":
                            region.tag_redraw()
            elif area.type in ("VIEW_3D", "PROPERTIES"):
                area.tag_redraw()


# ---- Frames and renders -----------------------------------------------------------------------

def on_frame(scene, depsgraph):
    for scalp in scalps():
        if scalp.name not in scene.objects:
            continue
        if _exporting:
            sync_safely(scalp, scene=scene, depsgraph=depsgraph, evaluate_now=False, topology=False)
        else:
            sync_safely(scalp, scene=scene, depsgraph=depsgraph, mode="render" if _rendering else "preview",
                        topology=False)


def set_rendering(scene, on):
    """Final renders get every strand (mode "render"); the preview density comes back after."""
    global _rendering, _restore_after_render
    if on:
        _rendering = True
        for scalp in scalps():
            if scalp.name in scene.objects:
                sync_safely(scalp, scene=scene, mode="render", read_pose=threading.current_thread() is _main_thread)
        return
    _rendering = False
    if threading.current_thread() is _main_thread:
        restore_preview()
    else:
        _restore_after_render = True  # done by the poll timer, on Blender's main thread


def restore_preview():
    global _restore_after_render
    _restore_after_render = False
    for scalp in scalps():
        rt = runtime_if_any(scalp)
        if rt is not None and rt.mode == "render":
            sync_safely(scalp, mode="preview", topology=False)


def poll():
    """A slow persistent timer: the preview back after a render that ran on another thread."""
    if _restore_after_render and not _rendering:
        restore_preview()
    return 0.5


# ---- Files ------------------------------------------------------------------------------------

def snapshot_all():
    """Before the .blend is saved: each groom as a .groom text in its scalp object, so the file
    opens with the whole groom (graph, guides, maps) even without the helper running then."""
    for fn in before_save:
        fn()
    for scalp in scalps():
        rt = runtime_if_any(scalp)
        if rt is None or rt.session is None or rt.generation != process.generation() or not process.running():
            continue
        path = os.path.join(tempfile.gettempdir(), "groomlab_snapshot_%s_%d.groom" % (rt.uid, os.getpid()))
        try:
            _call("save_groom", session=rt.session, path=path)
            with open(path, encoding="utf-8") as f:
                scalp.groomlab.snapshot = f.read()
            scalp.groomlab.graph_json = json.dumps(_call("get_graph", session=rt.session)["graph"])
        except (HelperError, OSError) as e:
            print("GroomLab: the groom of %s was not kept in the file: %s" % (scalp.name, e))
        finally:
            try:
                os.remove(path)
            except OSError:
                pass


def remember_graph(scalp, rt=None):
    rt = rt or ensure(scalp)
    scalp.groomlab.graph_json = json.dumps(_call("get_graph", session=rt.session)["graph"])


def scalp_size_cm(rest, scene):
    if len(rest) == 0:
        return 1.0
    return float((rest.max(axis=0) - rest.min(axis=0)).max()) * units.cm_per_unit(scene)


def _scatter_length_cm(rt):
    try:
        for n in _call("list_nodes", session=rt.session)["nodes"]:
            if n["type"] == "Scatter":
                v = _call("get_param", session=rt.session, node=n["name"], param="length")["value"]
                if isinstance(v, (int, float)) and v > 0:
                    return float(v)
    except HelperError:
        pass
    return None


def create_groom(context, scalp, preset="", guide_count=0, guide_points=8, guides="hierarchy"):
    """Makes `scalp` (a mesh object) a GroomLab groom as Maya's Create Groom does: new_groom in the
    helper (the default graph fitted to the scalp, or `preset`'s; guides in the hierarchy GroomLab's
    own tools edit: Place and Draw add them), its Hair Curves object. `guide_count` starter guides go into the hierarchy's first group.
    `guides="flat"`: B2's guides as a Blender Curves object sculpted with Blender's brushes (what
    grooms made before GroomLab's tools have; the tests of that path)."""
    scene = context.scene
    if scalp.type != "MESH" or not len(scalp.data.polygons):
        raise HelperError("state", "a groom grows on a mesh with faces")
    hierarchy = guides != "flat"
    g = scalp.groomlab
    g.role = "SCALP"
    g.uid = uuid.uuid4().hex
    layer = uv_layer(scalp.data)
    g.uv_map = layer.name if layer is not None else ""
    g.guide_target = "hierarchy" if hierarchy else "flat"
    g.snapshot = ""
    scalp.groomlab_tools.state = ""
    forget_cache()
    guides_obj = None
    if not hierarchy:
        guides_obj = g.guides if g.guides is not None and g.guides.type == "CURVES" else make_guides_object(scalp, scene)
    ensure_hair_object(scalp)
    rt = ensure(scalp, scene)
    _sync_settings(scalp, rt, scene)
    sync_scalp(scalp, rt, force=True)
    missing_maps = []  # what the preset reads that the new groom lacks: made below
    required_groups = []
    if hierarchy:
        from . import tools
        started = tools.start_groom(scalp, rt, preset=preset, follow=g.follow_scalp)
        missing_maps = (started.get("preset") or {}).get("missingMaps") or []
        required_groups = (started.get("preset") or {}).get("groups") or []
    elif preset:
        missing_maps = _call("apply_preset", session=rt.session, name=preset).get("missingMaps") or []
    rest, counts, indices, _ = read_scalp(scalp)
    if guide_count > 0 and (hierarchy or len(guides_obj.data.curves) == 0):
        length_cm = _scatter_length_cm(rt) or 0.5 * scalp_size_cm(rest, scene)
        normals = np.empty(len(scalp.data.vertices) * 3, np.float32)
        scalp.data.vertex_normals.foreach_get("vector", normals)
        down = scalp.matrix_world.inverted_safe().to_3x3() @ Vector((0.0, 0.0, -1.0))
        pts, cnts = starter.starter_guides(rest, counts, indices, normals.reshape(-1, 3), count=guide_count,
                                           length=length_cm / units.cm_per_unit(scene), segments=guide_points,
                                           down=tuple(down.normalized()))
        if hierarchy:
            tools.starter_guides(scalp, rt, pts, cnts)
        else:
            write_guides(scalp, guides_obj, pts, cnts, np.zeros(len(cnts), np.int32))
    if not hierarchy:
        sync_guides(scalp, rt, force=True)
    if missing_maps:
        from . import graph
        graph.make_required_maps(scalp, missing_maps)
    if required_groups:  # a hand-placed look: its guide groups, empty
        from . import graph
        graph.make_required_groups(scalp, required_groups)
    remember_graph(scalp, rt)
    r = evaluate(scalp, rt, "preview")
    if hierarchy:
        tools.keep_state(scalp)
    scene.render.use_lock_interface = True  # renders write the full-density hair from their handler
    shading.use_render_settings(scene)
    return r


def load_groom(context, path, scalp=None):
    """Opens a .groom: into `scalp` (made a groom if it is not one), or with the file's own scalp
    as a new mesh object when `scalp` is None. Flat guides (Maya's guide curves) become the Guides
    Curves object; a guide hierarchy stays in the helper, for GroomLab's tools."""
    scene = context.scene
    if scalp is None:
        scalp = _scalp_from_file(context, path)
    g = scalp.groomlab
    if g.role != "SCALP":
        g.role = "SCALP"
        g.uid = uuid.uuid4().hex
        layer = uv_layer(scalp.data)
        g.uv_map = layer.name if layer is not None else ""
        forget_cache()
    ensure_hair_object(scalp)
    rt = ensure(scalp, scene)
    info = _call("load_groom", session=rt.session, path=path)
    # A groom of flat guides (Maya's guide curves) keeps them as a Blender Curves object (B2); every
    # other groom's guides live in the helper's hierarchy, edited with GroomLab's own tools.
    hierarchy = info.get("flatGuides", 0) == 0
    rt.target = g.guide_target = "hierarchy" if hierarchy else "flat"
    guides = None
    if hierarchy:
        if g.guides is not None:
            remove_guides_object(scalp)
    else:
        guides = g.guides if g.guides is not None and g.guides.type == "CURVES" else make_guides_object(scalp, scene)
    rt.keys.clear()
    rt.serial = None
    rt.frame = rt.fps = None
    rt.space = space_of(scene)
    _call("set_space", session=rt.session, **rt.space)
    from . import props
    with props.quiet():
        g.follow_scalp = bool(info.get("followScalp"))
    rt.follow = g.follow_scalp
    _sync_settings(scalp, rt, scene)
    sync_scalp(scalp, rt, force=True)
    if guides is not None:
        gg = _call("get_guides", session=rt.session, source=rt.target, pose="rest")
        client = process.client()
        write_guides(scalp, guides, client.array(gg["arrays"]["points"]), client.array(gg["arrays"]["counts"]),
                     client.array(gg["arrays"]["ids"]))
        sync_guides(scalp, rt, force=True)
    remember_graph(scalp, rt)
    r = evaluate(scalp, rt, "preview")
    shading.use_render_settings(scene)
    if hierarchy:
        from . import tools
        tools.keep_state(scalp)
        tools.refresh_info(scalp, scene)
    return scalp, info, r


def remove_guides_object(scalp):
    """B2's guides object of a groom whose guides now live in the hierarchy."""
    g = scalp.groomlab
    obj = g.guides
    g.guides = None
    if obj is not None and obj.name in bpy.data.objects:
        data = obj.data
        bpy.data.objects.remove(obj)
        if data is not None and data.users == 0:
            bpy.data.hair_curves.remove(data)


def _scalp_from_file(context, path):
    """A mesh object of the file's scalp (its points in Blender's space and units)."""
    scene = context.scene
    process.client()
    s = _call("new_session", space=space_of(scene))["session"]
    try:
        _call("load_groom", session=s, path=path)
        got = _call("get_scalp", session=s)
        if not got.get("scalp"):
            raise HelperError("state", "%s has no scalp: select the mesh to grow it on" % os.path.basename(path))
        client = process.client()
        a = got["arrays"]
        pts = client.array(a["points"])
        counts = client.array(a["faceCounts"]).astype(int)
        idx = client.array(a["faceIndices"]).astype(int)
        uvs = client.array(a["uvs"]) if "uvs" in a else None
    finally:
        _call("close_session", session=s)
    starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
    faces = [idx[b:b + n].tolist() for b, n in zip(starts, counts)]
    name = os.path.splitext(os.path.basename(path))[0] + "_scalp"
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(pts.tolist(), [], faces)
    if uvs is not None:
        layer = mesh.uv_layers.new(name="UVMap")
        layer.uv.foreach_set("vector", uvs.ravel())
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    return obj


def save_groom(scalp, path, up_axis="Y"):
    rt = ensure(scalp)
    sync(scalp, evaluate_now=False)
    for fn in before_save:
        fn()
    r = _call("save_groom", session=rt.session, path=path, upAxis=up_axis)
    remember_graph(scalp, rt)
    return r


_card_presets = []


def card_map_presets(refresh=False):
    """The card map export presets every host lists (the helper's card_map_presets: the built-in
    ones, then the user's Documents/GroomLab/card_presets), as dicts with name, description,
    format, bitDepth, files. Asked once, then on refresh."""
    global _card_presets
    if refresh or not _card_presets:
        try:
            _card_presets = list(_call("card_map_presets")["presets"])
        except HelperError:
            _card_presets = []
    return _card_presets


def bake_card_maps(scalp, scene, preset, directory, name, resolution=0, fmt="", bit_depth=0, padding=-1, supersample=1,
                   name_template="{name}_{map}"):
    """The groom's cards (its Cards nodes) baked at the scene's frame and written as `preset` (a
    name or a preset dict) packs them into `directory`; the helper's answer ({paths, cards, size})."""
    rt = ensure(scalp, scene)
    sync(scalp, scene=scene, evaluate_now=False)
    return _call("bake_card_maps", session=rt.session, preset=preset, directory=directory, name=name,
                 resolution=int(resolution), format=fmt, bitDepth=int(bit_depth), padding=int(padding),
                 supersample=int(supersample), nameTemplate=name_template)


def export(scalp, scene, fmt, path, first, last, step=1, density=1.0, **options):
    """The groom's strands over frames: Alembic, USD, scalesUsd, vrscene, unrealGroom or unityStrands, with the
    format's options (scales, groups, lod, pointsPerStrand...: docs/BLENDER.md "Pipeline")."""
    global _exporting
    rt = ensure(scalp, scene)
    sync(scalp, scene=scene, evaluate_now=False)
    if not deforms(scalp):
        # The helper steps the frames itself (time-driven nodes such as Wind need nothing from Blender).
        return _call("export", session=rt.session, format=fmt, path=path, density=density,
                     frames={"first": first, "last": last, "step": step}, **options)
    # A deforming scalp: Blender plays each frame and hands the helper the scalp's pose.
    e = _call("export_open", session=rt.session, format=fmt, path=path, startFrame=first, step=step, **options)["export"]
    current = scene.frame_current
    _exporting = True
    done = False
    try:
        f = first
        while f <= last + 1e-6:
            scene.frame_set(int(f))
            sync(scalp, scene=scene, evaluate_now=False, topology=False)
            _call("export_frame", session=rt.session, export=e, frame=f, density=density)
            f += step
        done = True
    finally:
        _exporting = False
        try:
            r = _call("export_close", session=rt.session, export=e)
        except HelperError:
            if done:
                raise
        scene.frame_set(current)
    return r
