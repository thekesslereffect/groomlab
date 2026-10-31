# SPDX-License-Identifier: GPL-3.0-or-later
"""Groom Tools > Photos: Maya's reference cameras (hosts/maya/scripts/groomlab/reference.py and the
palette's ReferenceTab) in Blender. Each photo is a Blender camera with the photo as its background
image (shown while looking through it), under the "GroomLab References" collection. Look through
it, line it up with the mesh by eye or by pairs of points (a feature clicked on the photo in the
camera view, the same feature clicked on the mesh in any other view), Solve for the camera's
position, rotation and focal length (the helper's solve_camera, Maya's groomLabCameraSolve), lock
it, and set how the photo shows (its opacity, in front of the mesh or behind it).

Guides from Photos (experimental) grows guides from the lined-up photos into the fromReference guide
group (the helper's guides_from_photos: the core's guidesFromReference, Maya's from_reference).

The photos travel with the groom: after every change they are sent to the helper
(set_reference_photos), which keeps them in the groom's file ("referencePhotos", the standalone
app's Reference window's form, in groom space), so a .groom saved here opens with its photos in the
app; a groom file opened with photos and no cameras for them gets its cameras back
(restore_from_groom).

The camera model is Maya's and the core's: the photo fills the camera's frame (the sensor fit
horizontal, so the focal length in pixels is lens / sensor width x the photo's width); Look Through
sets the scene's render size to the photo's, so the frame has the photo's shape. Points on the mesh
are kept in the scalp object's space (they move with it), as everything crossing the pipe is."""

import math
import os

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatVectorProperty, FloatProperty,
                       IntProperty, PointerProperty, StringProperty)
from bpy_extras.io_utils import ImportHelper
from mathutils import Euler, Matrix, Vector

from . import actions, groom, process
from .helper_client import HelperError
from .ops import _fail, active_groom
from .ui import _wrap

COLLECTION = "GroomLab References"
SENSOR_WIDTH = 36.0     # mm: a full-frame film back, as Maya's references
DEFAULT_LENS = 35.0
DEFAULT_OPACITY = 0.6
MIN_PAIRS = 6           # pairs to solve the focal length too
MIN_PAIRS_FIXED = 4     # pairs with the focal length kept
GROUP = "fromReference"
IMAGES = "*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.tga;*.bmp;*.exr;*.webp"
SYMMETRIES = (("auto", "Auto", "The scalp's topology when it mirrors, else none"),
              ("x", "World X", "Mirrored across the world's X = 0 plane"),
              ("topology", "Topology", "Mirrored by the scalp's own symmetry"),
              ("none", "None", "Each guide grown on its own"))

_previous = {}   # scene name -> the camera it showed before looking through a photo
_picking = [False]
_draw_handle = [None]


# ---- Data ----------------------------------------------------------------------------------------

class GroomLabPhotoPair(bpy.types.PropertyGroup):
    image: FloatVectorProperty(size=2, description="The point on the photo: pixels from its top-left, v down")
    has_image: BoolProperty()
    mesh: FloatVectorProperty(size=3, description="The point on the mesh, in the scalp object's space")
    has_mesh: BoolProperty()
    error: FloatProperty(default=-1.0, description="The last solve's reprojection error, pixels (-1: none)")


def _lock_changed(self, context):
    obj = self.id_data
    obj.lock_location = (self.locked,) * 3
    obj.lock_rotation = (self.locked,) * 3
    obj.lock_scale = (self.locked,) * 3


class GroomLabPhoto(bpy.types.PropertyGroup):
    """A camera's photo (Maya's glRef* attributes)."""
    is_photo: BoolProperty()
    image_path: StringProperty(subtype="FILE_PATH")
    width: IntProperty()
    height: IntProperty()
    rms: FloatProperty(default=-1.0, description="The last solve's error in pixels (-1: none yet)")
    pairs: CollectionProperty(type=GroomLabPhotoPair)
    pair_index: IntProperty(default=-1)
    locked: BoolProperty(name="Lock", update=_lock_changed,
                         description="Lock the camera where it is: its position and rotation (a solve leaves it alone)")


def _photo_index_get(self):
    obj = self.photo
    if obj is None:
        return -1
    for i, o in enumerate(bpy.data.objects):
        if o == obj:
            return i
    return -1


def _photo_index_set(self, value):
    if 0 <= value < len(bpy.data.objects) and is_photo(bpy.data.objects[value]):
        was, self.photo = self.photo, bpy.data.objects[value]
        # Looking through a photo: the picked one is looked through instead (Look Through again
        # still returns to the view and the camera before).
        if was is not None and was != self.photo and looking_through(bpy.context, was):
            look_through(bpy.context, self.photo)


def _is_photo_poll(self, obj):
    return is_photo(obj)


class GroomLabPhotos(bpy.types.PropertyGroup):
    """The Photos tab's settings (the scene's)."""
    photo: PointerProperty(type=bpy.types.Object, poll=_is_photo_poll, name="Photo")
    photo_index: IntProperty(get=_photo_index_get, set=_photo_index_set)
    keep_focal: BoolProperty(name="Keep focal length", default=False,
                             description="Solve the position and rotation only, keeping the camera's focal length (4 pairs "
                                         "are enough; else 6, and the focal length is solved too)")
    guide_count: IntProperty(name="Guides", default=300, min=10, max=5000,
                             description="Roots spread over the scalp; fewer survive (bald, unseen)")
    symmetry: EnumProperty(name="Symmetry", items=SYMMETRIES, default="auto",
                           description="One view covers both sides: each guide comes with its mirror image")
    result: StringProperty()


# ---- Finding photos ------------------------------------------------------------------------------

def is_photo(obj):
    return obj is not None and obj.type == "CAMERA" and obj.groomlab_photo.is_photo


def list_photos(scene=None):
    """The scene's photo cameras, by name."""
    scene = scene or bpy.context.scene
    return sorted((o for o in scene.objects if is_photo(o)), key=lambda o: o.name)


def current(scene=None):
    """The picked photo camera, or None."""
    scene = scene or bpy.context.scene
    obj = scene.groomlab_photos.photo
    return obj if is_photo(obj) and obj.name in scene.objects else None


def image_path(cam):
    p = cam.groomlab_photo.image_path
    return bpy.path.abspath(p) if p else ""


def background(cam):
    """The camera's background image slot showing the photo, or None."""
    bgs = cam.data.background_images
    return bgs[0] if len(bgs) else None


def complete_pairs(cam):
    return [(i, p) for i, p in enumerate(cam.groomlab_photo.pairs) if p.has_image and p.has_mesh]


# ---- Making and removing ---------------------------------------------------------------------------

def _collection(scene):
    coll = bpy.data.collections.get(COLLECTION)
    if coll is None:
        coll = bpy.data.collections.new(COLLECTION)
    if coll.name not in scene.collection.children:
        try:
            scene.collection.children.link(coll)
        except RuntimeError:
            pass
    return coll


def _load_image(path):
    image = bpy.data.images.load(path, check_existing=True)
    if not image.size[0] or not image.size[1]:
        raise ValueError("cannot read the image %s" % path)
    return image


def _subject(scalp=None, scene=None):
    """(center, radius) of what the photos show, in world space: the groom's scalp, else every visible mesh."""
    scene = scene or bpy.context.scene
    objs = [scalp] if scalp is not None else [o for o in scene.objects if o.type == "MESH" and o.visible_get()]
    pts = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    if not pts:
        return Vector((0.0, 0.0, 0.0)), 1.0
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    return (lo + hi) * 0.5, max((hi - lo).length * 0.5, 1e-3)


def _place(cam, scalp=None):
    """In front (-Y, Blender's front view) of the subject, far enough for it to fill most of the
    photo's short side, aimed at it (Maya's references start in front of the face, +Z there)."""
    center, radius = _subject(scalp)
    d = cam.data
    p = cam.groomlab_photo
    aspect = p.height / float(p.width) if p.width else 1.0
    half = min(math.atan(0.5 * SENSOR_WIDTH / d.lens), math.atan(0.5 * SENSOR_WIDTH * aspect / d.lens))
    distance = radius / math.sin(half) * 1.05
    cam.rotation_mode = "XYZ"
    # The world matrix set (not only location and rotation): it is what the helper is sent, now.
    cam.matrix_world = Matrix.LocRotScale(center + Vector((0.0, -distance, 0.0)),
                                          Euler((math.pi / 2.0, 0.0, 0.0)).to_quaternion(), Vector((1.0, 1.0, 1.0)))
    d.clip_start = max(0.001, 0.001 * distance)
    d.clip_end = max(1000.0, 50.0 * distance)


def import_photo(context, path, scalp=None, name=None):
    """A photo camera for the image at `path` (Maya's import_reference): a camera with the photo as
    its background image (60% opaque, in front of the mesh), its sensor fit to the photo's width, in
    front of the groom's scalp at 35 mm, aimed at it. The photo stays where it is (Blender keeps
    the path, relative to the .blend once saved). Picked in the Photos tab. Returns the camera."""
    if not path or not os.path.isfile(path):
        raise ValueError("no image at %s" % path)
    image = _load_image(path)
    width, height = int(image.size[0]), int(image.size[1])
    scene = context.scene
    base = name or os.path.splitext(os.path.basename(path))[0] + "_ref"
    data = bpy.data.cameras.new(base)
    data.sensor_fit = "HORIZONTAL"
    data.sensor_width = SENSOR_WIDTH
    data.lens = DEFAULT_LENS
    data.show_background_images = True
    bg = data.background_images.new()
    bg.image = image
    bg.alpha = DEFAULT_OPACITY
    bg.display_depth = "FRONT"
    bg.frame_method = "FIT"
    cam = bpy.data.objects.new(base, data)
    _collection(scene).objects.link(cam)
    p = cam.groomlab_photo
    p.is_photo = True
    p.image_path = path
    p.width, p.height = width, height
    _place(cam, scalp)
    scene.groomlab_photos.photo = cam
    sync(scalp)
    return cam


def remove_photo(cam, scene=None):
    """Delete the photo's camera (Maya's remove_reference): its photo file stays on disk."""
    scene = scene or bpy.context.scene
    if scene.camera == cam:
        prev = _previous.pop(scene.name, None)
        scene.camera = prev if prev is not None and prev.name in scene.objects and not is_photo(prev) else None
    image = background(cam).image if background(cam) is not None else None
    data = cam.data
    bpy.data.objects.remove(cam)
    if data is not None and data.users == 0:
        bpy.data.cameras.remove(data)
    if image is not None and image.users == 0:
        bpy.data.images.remove(image)
    rest = list_photos(scene)
    scene.groomlab_photos.photo = rest[0] if rest else None


def rename_photo(cam, name):
    if not name.strip():
        raise ValueError("a photo needs a name")
    cam.name = name.strip()
    cam.data.name = cam.name
    return cam.name


# ---- Point pairs (Maya's add_pick, clear_point, remove_pair, clear_pairs) ---------------------------

def add_pick(cam, kind, value):
    """A picked point: `kind` "image" ((u, v) pixels from the photo's top-left) or "mesh" ((x, y, z)
    in the scalp object's space) goes into the first pair still missing one, else a new pair.
    Returns the pair's index."""
    if kind not in ("image", "mesh"):
        raise ValueError("a pick is an image or a mesh point")
    value = [float(v) for v in value]
    if len(value) != (2 if kind == "image" else 3):
        raise ValueError("an image point is (u, v), a mesh point (x, y, z)")
    pairs = cam.groomlab_photo.pairs
    index = next((i for i, p in enumerate(pairs) if not getattr(p, "has_" + kind)), len(pairs))
    pair = pairs.add() if index == len(pairs) else pairs[index]
    setattr(pair, kind, value)
    setattr(pair, "has_" + kind, True)
    pair.error = -1.0
    cam.groomlab_photo.pair_index = index
    return index


def clear_point(cam, index, kind):
    """Forget pair `index`'s photo or mesh point: the next pick of that kind fills it again."""
    pairs = cam.groomlab_photo.pairs
    if not 0 <= index < len(pairs):
        raise ValueError("no pair %d" % (index + 1))
    setattr(pairs[index], "has_" + kind, False)
    pairs[index].error = -1.0


def remove_pair(cam, index):
    p = cam.groomlab_photo
    if not 0 <= index < len(p.pairs):
        raise ValueError("no pair %d" % (index + 1))
    p.pairs.remove(index)
    p.pair_index = min(index, len(p.pairs) - 1)


def clear_pairs(cam):
    cam.groomlab_photo.pairs.clear()
    cam.groomlab_photo.pair_index = -1
    cam.groomlab_photo.rms = -1.0


# ---- The camera between Blender and the helper ------------------------------------------------------

def focal_px(cam):
    """The camera's focal length in the photo's pixels."""
    d, p = cam.data, cam.groomlab_photo
    if d.sensor_fit == "VERTICAL":
        return d.lens / d.sensor_height * p.height
    if d.sensor_fit == "AUTO" and p.height > p.width:
        return d.lens / d.sensor_width * p.height
    return d.lens / d.sensor_width * p.width


def set_focal_px(cam, value):
    d, p = cam.data, cam.groomlab_photo
    if d.sensor_fit == "VERTICAL":
        d.lens = value * d.sensor_height / p.height
    elif d.sensor_fit == "AUTO" and p.height > p.width:
        d.lens = value * d.sensor_width / p.height
    else:
        d.lens = value * d.sensor_width / p.width


def _space(scalp):
    return scalp.matrix_world if scalp is not None else Matrix.Identity(4)


def camera_pose(cam, scalp=None):
    """The camera as the helper takes it, in the scalp object's space: {"rotation" (rows; its columns
    the camera's axes), "position", "focalPx"}."""
    m = _space(scalp).inverted() @ cam.matrix_world
    r = m.to_3x3()
    cols = [Vector((r[0][j], r[1][j], r[2][j])).normalized() for j in range(3)]
    rows = [[cols[j][i] for j in range(3)] for i in range(3)]
    return {"rotation": rows, "position": list(m.translation), "focalPx": focal_px(cam)}


def apply_pose(cam, scalp, rotation, position, focal=None):
    """A pose in the scalp object's space (rows of a rotation, the eye) onto the camera (its scale kept at 1)."""
    m = Matrix(((rotation[0][0], rotation[0][1], rotation[0][2], position[0]),
                (rotation[1][0], rotation[1][1], rotation[1][2], position[1]),
                (rotation[2][0], rotation[2][1], rotation[2][2], position[2]),
                (0.0, 0.0, 0.0, 1.0)))
    loc, rot, _scale = (_space(scalp) @ m).decompose()
    cam.rotation_mode = "XYZ"
    cam.matrix_world = Matrix.LocRotScale(loc, rot, Vector((1.0, 1.0, 1.0)))
    if focal:
        set_focal_px(cam, focal)


def photo_record(cam, scalp=None):
    """The photo as the helper keeps it with the groom (set_reference_photos)."""
    p = cam.groomlab_photo
    pairs = []
    for pair in p.pairs:
        entry = {"image": list(pair.image) if pair.has_image else None, "mesh": list(pair.mesh) if pair.has_mesh else None}
        if pair.error >= 0.0 and pair.has_image and pair.has_mesh:
            entry["error"] = pair.error
        pairs.append(entry)
    return {"name": cam.name, "image": image_path(cam).replace("\\", "/"), "width": p.width, "height": p.height,
            "camera": camera_pose(cam, scalp), "pairs": pairs, "rms": p.rms if p.rms >= 0.0 else None}


def sync(scalp, scene=None):
    """The scene's photos into the groom (kept in its file and its state). Nothing without a groom
    whose session runs."""
    if scalp is None or not process.running():
        return False
    scene = scene or groom.scene_of(scalp)
    try:
        rt = groom.ensure(scalp, scene)
        process.call("set_reference_photos", session=rt.session,
                     photos=[photo_record(c, scalp) for c in list_photos(scene)])
    except HelperError as e:
        print("GroomLab: the photos were not kept with %s: %s" % (scalp.name, e))
        return False
    from . import tools
    if tools.is_hierarchy(scalp):
        tools.mark_state(scalp)
    return True


def restore_from_groom(scalp, scene=None):
    """The groom's photos (a groom file from the app, or saved elsewhere) as cameras where the scene
    has none of their names. Returns the cameras made."""
    scene = scene or groom.scene_of(scalp) or bpy.context.scene
    rt = groom.ensure(scalp, scene)
    stored = process.call("reference_photos", session=rt.session)["photos"]
    have = {c.name for c in list_photos(scene)}
    made = []
    for s in stored:
        if not s.get("name") or s["name"] in have:
            continue
        data = bpy.data.cameras.new(s["name"])
        data.sensor_fit = "HORIZONTAL"
        data.sensor_width = SENSOR_WIDTH
        data.lens = DEFAULT_LENS
        data.show_background_images = True
        bg = data.background_images.new()
        bg.alpha = DEFAULT_OPACITY
        bg.display_depth = "FRONT"
        bg.frame_method = "FIT"
        if s.get("image") and os.path.isfile(s["image"]):
            try:
                bg.image = _load_image(s["image"])
            except (RuntimeError, ValueError):
                pass
        cam = bpy.data.objects.new(s["name"], data)
        _collection(scene).objects.link(cam)
        p = cam.groomlab_photo
        p.is_photo = True
        p.image_path = s.get("image") or ""
        p.width, p.height = int(s.get("width") or 1), int(s.get("height") or 1)
        p.rms = s["rms"] if s.get("rms") is not None else -1.0
        for pair in s.get("pairs") or []:
            item = p.pairs.add()
            if pair.get("image"):
                item.image, item.has_image = pair["image"], True
            if pair.get("mesh"):
                item.mesh, item.has_mesh = pair["mesh"], True
            item.error = pair["error"] if pair.get("error") is not None else -1.0
        if s.get("camera"):
            apply_pose(cam, scalp, s["camera"]["rotation"], s["camera"]["position"], s["camera"].get("focalPx"))
        else:
            _place(cam, scalp)
        made.append(cam)
    if made and current(scene) is None:
        scene.groomlab_photos.photo = made[0]
    return made


# ---- The solve -----------------------------------------------------------------------------------

def solve(cam, scalp, keep_focal=False, apply=True):
    """Line the camera up with its pairs (Maya's solve): the position, rotation and (unless
    `keep_focal`) focal length that put every pair's mesh point on its photo point (6 complete pairs,
    4 with the focal length kept). With `apply` the camera takes them and each pair keeps its error.
    Returns the helper's answer (rotation, position, focalPx, rms, errors, bad)."""
    p = cam.groomlab_photo
    if apply and p.locked:
        raise ValueError("%s is locked: unlock it to align it again" % cam.name)
    complete = complete_pairs(cam)
    need = MIN_PAIRS_FIXED if keep_focal else MIN_PAIRS
    if len(complete) < need:
        raise ValueError("a solve needs %d complete pairs%s; %s has %d" % (
            need, " with the focal length kept" if keep_focal else " (4 with the focal length kept)", cam.name,
            len(complete)))
    pose = camera_pose(cam, scalp)
    rt = groom.ensure(scalp)
    pairs = [{"image": list(x.image) if x.has_image else None, "mesh": list(x.mesh) if x.has_mesh else None}
             for x in p.pairs]
    r = process.call("solve_camera", session=rt.session, pairs=pairs, width=p.width, height=p.height,
                     focalPx=pose["focalPx"], keepFocal=bool(keep_focal),
                     initial={"rotation": pose["rotation"], "position": pose["position"]})
    if apply:
        apply_pose(cam, scalp, r["rotation"], r["position"], r["focalPx"])
        for pair, error in zip(p.pairs, r["errors"]):
            pair.error = error if error is not None else -1.0
        p.rms = r["rms"]
        sync(scalp)
    return r


# ---- Looking through --------------------------------------------------------------------------------

def _view3d(context):
    """(area, rv3d) of the 3D view a Look Through acts on: the one worked in, else the largest."""
    area = context.area if context.area is not None and context.area.type == "VIEW_3D" else None
    if area is None and context.screen is not None:
        views = [a for a in context.screen.areas if a.type == "VIEW_3D"]
        area = max(views, key=lambda a: a.width * a.height) if views else None
    return (area, area.spaces.active.region_3d) if area is not None else (None, None)


def looking_through(context, cam):
    _area, rv3d = _view3d(context)
    return rv3d is not None and rv3d.view_perspective == "CAMERA" and context.scene.camera == cam


def look_through(context, cam):
    """Show the photo's camera in the 3D view (the scene's camera, its render size the photo's);
    again: back to the view and the camera before. Returns True when looking through it now."""
    scene = context.scene
    area, rv3d = _view3d(context)
    if looking_through(context, cam):
        rv3d.view_perspective = "PERSP"
        prev = _previous.pop(scene.name, None)
        if prev is not None and prev.name in scene.objects:
            scene.camera = prev
        area.tag_redraw()
        return False
    if scene.camera is not None and not is_photo(scene.camera):
        _previous[scene.name] = scene.camera
    scene.camera = cam
    p = cam.groomlab_photo
    scene.render.resolution_x, scene.render.resolution_y = p.width, p.height
    scene.render.pixel_aspect_x = scene.render.pixel_aspect_y = 1.0
    if rv3d is not None:
        rv3d.view_perspective = "CAMERA"
        area.tag_redraw()
    return True


# ---- Picking on the photo and the mesh --------------------------------------------------------------

def photo_rect(scene, region, rv3d, cam):
    """(left, bottom, right, top) of the photo in the region's pixels, in the camera view: the camera's
    frame, the photo fit inside it (its background image's Fit)."""
    from bpy_extras import view3d_utils
    frame = [cam.matrix_world @ v for v in cam.data.view_frame(scene=scene)]
    pts = [view3d_utils.location_3d_to_region_2d(region, rv3d, v) for v in frame]
    if any(q is None for q in pts):
        return None
    left, right = min(q.x for q in pts), max(q.x for q in pts)
    bottom, top = min(q.y for q in pts), max(q.y for q in pts)
    p = cam.groomlab_photo
    fw, fh = right - left, top - bottom
    if fw <= 0 or fh <= 0 or not p.width or not p.height:
        return None
    aspect = p.width / float(p.height)
    if aspect > fw / fh:
        h = fw / aspect
        bottom, top = bottom + 0.5 * (fh - h), bottom + 0.5 * (fh + h)
    else:
        w = fh * aspect
        left, right = left + 0.5 * (fw - w), left + 0.5 * (fw + w)
    return left, bottom, right, top


def photo_pixel(scene, region, rv3d, cam, x, y):
    """The photo's pixel (u, v from its top-left) under region pixel (x, y), or None off the photo."""
    rect = photo_rect(scene, region, rv3d, cam)
    if rect is None:
        return None
    left, bottom, right, top = rect
    p = cam.groomlab_photo
    u = (x - left) / (right - left) * p.width
    v = (top - y) / (top - bottom) * p.height
    if not (0.0 <= u <= p.width and 0.0 <= v <= p.height):
        return None
    return u, v


def region_of_pixel(scene, region, rv3d, cam, u, v):
    rect = photo_rect(scene, region, rv3d, cam)
    if rect is None:
        return None
    left, bottom, right, top = rect
    p = cam.groomlab_photo
    return left + u / p.width * (right - left), top - v / p.height * (top - bottom)


def mesh_point(context, region, rv3d, scalp, x, y):
    """The point on the scalp under region pixel (x, y), in the scalp object's space, or None."""
    from bpy_extras import view3d_utils
    origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, (x, y))
    direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, (x, y))
    inv = scalp.matrix_world.inverted()
    o = inv @ origin
    d = (inv.to_3x3() @ direction).normalized()
    ev = scalp.evaluated_get(context.evaluated_depsgraph_get())
    hit, location, _normal, _index = ev.ray_cast(o, d)
    return tuple(location) if hit else None


def _draw_pairs():
    """The picked photo's pairs, numbered: photo points in its camera view, mesh points in any view."""
    import blf
    import gpu
    from bpy_extras import view3d_utils
    from gpu_extras.batch import batch_for_shader
    context = bpy.context
    scene = context.scene
    cam = current(scene)
    region, rv3d = context.region, context.region_data
    if cam is None or region is None or rv3d is None:
        return
    scalp = active_groom(context)
    through = rv3d.view_perspective == "CAMERA" and scene.camera == cam
    marks = []
    for i, pair in enumerate(cam.groomlab_photo.pairs):
        if through and pair.has_image:
            q = region_of_pixel(scene, region, rv3d, cam, pair.image[0], pair.image[1])
            if q is not None:
                marks.append((q, i, pair.has_mesh))
        if not through and pair.has_mesh:
            world = _space(scalp) @ Vector(pair.mesh)
            q = view3d_utils.location_3d_to_region_2d(region, rv3d, world)
            if q is not None:
                marks.append(((q.x, q.y), i, pair.has_image))
    if not marks:
        return
    shader = gpu.shader.from_builtin("UNIFORM_COLOR")
    gpu.state.point_size_set(8.0)
    for done in (True, False):
        pts = [m[0] for m in marks if m[2] == done]
        if pts:
            batch = batch_for_shader(shader, "POINTS", {"pos": [(x, y, 0.0) for x, y in pts]})
            shader.uniform_float("color", (1.0, 0.85, 0.1, 1.0) if done else (1.0, 0.3, 0.2, 1.0))
            batch.draw(shader)
    gpu.state.point_size_set(1.0)
    blf.size(0, 13)
    for (x, y), i, _done in marks:
        blf.position(0, x + 7, y + 5, 0)
        blf.color(0, 1.0, 1.0, 1.0, 1.0)
        blf.draw(0, str(i + 1))


def _show_pairs(on):
    if on and _draw_handle[0] is None:
        _draw_handle[0] = bpy.types.SpaceView3D.draw_handler_add(_draw_pairs, (), "WINDOW", "POST_PIXEL")
    elif not on and _draw_handle[0] is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_draw_handle[0], "WINDOW")
        _draw_handle[0] = None
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.tag_redraw()


# ---- Guides from Photos -----------------------------------------------------------------------------

def guides_from_photos(scalp, guide_count=300, symmetry="auto", max_length=0.0, photos=None):
    """Guides grown from the photos (every photo camera, or `photos`: names) into guide group
    fromReference (replaced), one GroomLab undo step. Returns the helper's answer (guides,
    symmetry, roots tried, unseen and bald, the measured look)."""
    from . import tools
    if not tools.is_hierarchy(scalp):
        raise HelperError("state", "this groom's guides are Blender curves: Guides from Photos needs a groom made "
                                   "with GroomLab's tools")
    scene = groom.scene_of(scalp)
    if not list_photos(scene):
        raise HelperError("state", "no photos: import one in Groom Tools > Photos, line it up, then Guides from Photos")
    sync(scalp, scene)
    args = {"guideCount": int(guide_count), "symmetry": symmetry, "maxLength": float(max_length)}
    if photos:
        args["photos"] = list(photos)
    r = tools.call(scalp, "guides_from_photos", **args)
    tools.after_edit(scalp, scene, r.get("undo"), refresh=True)
    return r


# ---- Operators -------------------------------------------------------------------------------------

class _Tipped(object):
    @classmethod
    def description(cls, context, properties):
        return actions.tip_for(cls.bl_idname, properties, cls.__doc__ or "")


def _photo_op_poll(context):
    return current(context.scene) is not None


def _say(op, text, level="INFO"):
    op.report({level}, "GroomLab: " + text)


class GROOMLAB_OT_photo_import(_Tipped, bpy.types.Operator, ImportHelper):
    """A photo as a camera aimed at the groom"""
    bl_idname = "groomlab.photo_import"
    bl_label = "Import Photo"
    bl_options = {"REGISTER", "UNDO"}
    filter_glob: StringProperty(default=IMAGES, options={"HIDDEN"})

    def execute(self, context):
        try:
            cam = import_photo(context, bpy.path.abspath(self.filepath), active_groom(context))
        except (ValueError, RuntimeError, HelperError) as e:
            return _fail(self, e)
        _say(self, "%s imported: Look Through it, then line it up by eye or with Pick Points" % cam.name)
        return {"FINISHED"}


class GROOMLAB_OT_photo_look_through(_Tipped, bpy.types.Operator):
    """Show the photo's camera in the viewport (again: back to the view before)"""
    bl_idname = "groomlab.photo_look_through"
    bl_label = "Look Through"

    @classmethod
    def poll(cls, context):
        return _photo_op_poll(context)

    def execute(self, context):
        look_through(context, current(context.scene))
        return {"FINISHED"}


class GROOMLAB_OT_photo_rename(_Tipped, bpy.types.Operator):
    """Rename the photo's camera"""
    bl_idname = "groomlab.photo_rename"
    bl_label = "Rename Photo"
    bl_options = {"REGISTER", "UNDO"}
    new_name: StringProperty(name="Name")

    @classmethod
    def poll(cls, context):
        return _photo_op_poll(context)

    def invoke(self, context, event):
        self.new_name = current(context.scene).name
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        try:
            rename_photo(current(context.scene), self.new_name)
        except ValueError as e:
            return _fail(self, e)
        sync(active_groom(context))
        return {"FINISHED"}


class GROOMLAB_OT_photo_remove(_Tipped, bpy.types.Operator):
    """Remove the photo's camera (its photo file stays on disk)"""
    bl_idname = "groomlab.photo_remove"
    bl_label = "Remove Photo"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return _photo_op_poll(context)

    def execute(self, context):
        remove_photo(current(context.scene), context.scene)
        sync(active_groom(context))
        return {"FINISHED"}


class GROOMLAB_OT_photo_pick(bpy.types.Operator):
    """Looking through the photo's camera, click a feature on the photo; in another view, click the same feature on the mesh. Pairs fill in order and draw numbered (Esc or the button again: done)"""
    bl_idname = "groomlab.photo_pick"
    bl_label = "Pick Points"

    @classmethod
    def poll(cls, context):
        return _photo_op_poll(context) and active_groom(context) is not None

    def invoke(self, context, event):
        if _picking[0]:
            _picking[0] = False  # the running pick ends on its next event
            return {"FINISHED"}
        _picking[0] = True
        _show_pairs(True)
        context.window_manager.modal_handler_add(self)
        _say(self, "Pick Points: click the photo through its camera, the mesh in another view; Esc when done")
        return {"RUNNING_MODAL"}

    def finish(self):
        _picking[0] = False
        _show_pairs(False)
        return {"FINISHED"}

    def modal(self, context, event):
        scene = context.scene
        cam, scalp = current(scene), active_groom(context)
        if not _picking[0] or cam is None or scalp is None:
            return self.finish()
        if event.type == "ESC" and event.value == "PRESS":
            return self.finish()
        if event.type != "LEFTMOUSE" or event.value != "PRESS" or event.ctrl or event.alt:
            return {"PASS_THROUGH"}
        from . import tool_ops
        where = tool_ops.locate(context, event)  # a 3D view's region, not its header, toolbar or sidebar
        if where is None:
            return {"PASS_THROUGH"}
        region, rv3d, x, y = where
        if rv3d.view_perspective == "CAMERA" and scene.camera == cam:
            uv = photo_pixel(scene, region, rv3d, cam, x, y)
            if uv is not None:
                add_pick(cam, "image", uv)
        else:
            hit = mesh_point(context, region, rv3d, scalp, x, y)
            if hit is not None:
                add_pick(cam, "mesh", hit)
        sync(scalp, scene)
        for a in context.window.screen.areas:
            a.tag_redraw()
        return {"RUNNING_MODAL"}


class GROOMLAB_OT_pairs_solve(_Tipped, bpy.types.Operator):
    """Line the camera up with the pairs: its position, rotation and focal length"""
    bl_idname = "groomlab.pairs_solve"
    bl_label = "Solve"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return _photo_op_poll(context) and active_groom(context) is not None

    def execute(self, context):
        cam, scalp = current(context.scene), active_groom(context)
        keep = context.scene.groomlab_photos.keep_focal
        try:
            r = solve(cam, scalp, keep_focal=keep)
        except (ValueError, HelperError) as e:
            return _fail(self, e)
        bad = [str(i + 1) for i, b in enumerate(r.get("bad") or []) if b]
        text = "%s lined up: %.2f px off on average, %.1f mm" % (cam.name, r["rms"], cam.data.lens)
        if bad:
            text += "; pair%s %s stand%s out: pick them again" % ("s" if len(bad) > 1 else "", ", ".join(bad),
                                                                   "" if len(bad) > 1 else "s")
        context.scene.groomlab_photos.result = text
        _say(self, text, "WARNING" if bad else "INFO")
        return {"FINISHED"}


class GROOMLAB_OT_pairs_clear(_Tipped, bpy.types.Operator):
    """Forget every pair"""
    bl_idname = "groomlab.pairs_clear"
    bl_label = "Clear Pairs"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return _photo_op_poll(context)

    def execute(self, context):
        clear_pairs(current(context.scene))
        context.scene.groomlab_photos.result = ""
        sync(active_groom(context))
        return {"FINISHED"}


class GROOMLAB_OT_pairs_delete(_Tipped, bpy.types.Operator):
    """Delete the picked pair"""
    bl_idname = "groomlab.pairs_delete"
    bl_label = "Delete Pair"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        cam = current(context.scene)
        return cam is not None and 0 <= cam.groomlab_photo.pair_index < len(cam.groomlab_photo.pairs)

    def execute(self, context):
        cam = current(context.scene)
        remove_pair(cam, cam.groomlab_photo.pair_index)
        sync(active_groom(context))
        return {"FINISHED"}


class GROOMLAB_OT_pairs_repick(_Tipped, bpy.types.Operator):
    """Clear the pair's point, to pick it again"""
    bl_idname = "groomlab.pairs_repick"
    bl_label = "Pick Again"
    bl_options = {"REGISTER", "UNDO"}
    kind: EnumProperty(items=(("image", "Photo", ""), ("mesh", "Mesh", "")), default="image")

    @classmethod
    def poll(cls, context):
        return GROOMLAB_OT_pairs_delete.poll(context)

    def execute(self, context):
        cam = current(context.scene)
        clear_point(cam, cam.groomlab_photo.pair_index, self.kind)
        sync(active_groom(context))
        return {"FINISHED"}


class GROOMLAB_OT_photos_guides(_Tipped, bpy.types.Operator):
    """Grow guides from the photos into the fromReference guide group"""
    bl_idname = "groomlab.photos_guides"
    bl_label = "Guides from Photos"
    guide_count: IntProperty(name="Guides", default=300, min=10, max=5000,
                             description="Roots spread over the scalp; fewer survive (bald, unseen)")
    symmetry: EnumProperty(name="Symmetry", items=SYMMETRIES, default="auto")

    @classmethod
    def poll(cls, context):
        return active_groom(context) is not None

    def invoke(self, context, event):
        s = context.scene.groomlab_photos
        self.guide_count, self.symmetry = s.guide_count, s.symmetry
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scalp = active_groom(context)
        try:
            r = guides_from_photos(scalp, self.guide_count, self.symmetry)
        except (ValueError, HelperError) as e:
            return _fail(self, e)
        s = context.scene.groomlab_photos
        s.guide_count, s.symmetry = self.guide_count, self.symmetry
        look = r.get("look") or {}
        s.result = "%d guides in %s from %d photo%s (%d roots unseen, %d bald)%s" % (
            r["guides"], r["group"], r["views"], "" if r["views"] == 1 else "s", r["rootsUnseen"], r["rootsBald"],
            "; the hair's melanin %.2f, redness %.2f" % (look["melanin"], look["redness"]) if look.get("ok") else "")
        _say(self, s.result)
        return {"FINISHED"}


# ---- The Photos tab --------------------------------------------------------------------------------

class GROOMLAB_UL_photos(bpy.types.UIList):
    """The scene's photo cameras (bpy.data.objects, only the photos shown)."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        p = item.groomlab_photo
        row = layout.row(align=True)
        row.label(text=item.name, icon="OUTLINER_OB_CAMERA")
        sub = row.row()
        sub.alignment = "RIGHT"
        sub.label(text="%d x %d%s" % (p.width, p.height, "  ·  locked" if p.locked else ""))

    def filter_items(self, context, data, propname):
        objs = getattr(data, propname)
        scene = context.scene
        flags = [self.bitflag_filter_item if is_photo(o) and o.name in scene.objects else 0 for o in objs]
        return flags, []


class GROOMLAB_UL_photo_pairs(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index=0):
        row = layout.row(align=True)
        row.label(text="%d" % (index + 1))
        row.label(text="%.0f, %.0f" % tuple(item.image) if item.has_image else "pick on the photo")
        row.label(text="%.3f, %.3f, %.3f" % tuple(item.mesh) if item.has_mesh else "pick on the mesh")
        sub = row.row()
        sub.alignment = "RIGHT"
        sub.label(text="%.1f px" % item.error if item.error >= 0.0 else "")


def draw_tab(layout, context, chars):
    """The Photos tab (Maya's ReferenceTab): the photos (bar, list, right-click menu), how the picked
    one shows, Align by Points, and Guides from Photos."""
    from . import palette
    scene = context.scene
    s = scene.groomlab_photos
    # The photos (a header: how many, Import), the list, then the picked photo's box: rename and
    # delete, how it shows, its lens; Align by Points; Guides from Photos.
    head = palette.section_header(layout, "Photos", len(list_photos(scene)))
    sub = head.row(align=True)
    sub.alignment = "RIGHT"
    palette.bar_icons(sub, "lists/photos/bar", "photosbar", context, ("photo.import",))
    row = layout.row()  # the list (bpy.data.objects, the photos shown) with its right-click menu beside it
    row.template_list("GROOMLAB_UL_photos", "", bpy.data, "objects", s, "photo_index", rows=3)
    row.column(align=True).menu("GROOMLAB_MT_photo_context", text="", icon="DOWNARROW_HLT")
    cam = current(scene)
    if cam is None:
        _wrap(layout, "Import a photo to start.", width=chars)
    else:
        p = cam.groomlab_photo
        box = layout.box()
        row = box.row(align=True)
        row.alignment = "RIGHT"
        palette.bar_icons(row, "lists/photos/bar", "photosbar", context, ("photo.rename", "photo.remove"))
        row = box.row(align=True)
        row.operator("groomlab.photo_look_through", text="Look Through", depress=looking_through(context, cam),
                     icon="VIEW_CAMERA")
        row.prop(p, "locked", toggle=True, icon="LOCKED" if p.locked else "UNLOCKED")
        bg = background(cam)
        if bg is not None:
            row = box.row(align=True)
            row.prop(bg, "display_depth", expand=True)
            row.prop(bg, "show_background_image", text="", icon="HIDE_OFF" if bg.show_background_image else "HIDE_ON")
            col = box.column(align=True)
            col.prop(bg, "alpha", text="Opacity", slider=True)
            lens = col.row(align=True)
            lens.enabled = not p.locked  # (a locked camera keeps its focal length)
            lens.prop(cam.data, "lens", text="Focal Length")
        layout.label(text="ALIGN BY POINTS")
        _wrap(layout, "Pairs: a feature clicked on the photo (looking through its camera) and the same feature on the "
                      "mesh (in another view). 6 pairs solve the focal length too; 4 with it kept.", width=chars)
        row = layout.row(align=True)
        row.operator("groomlab.photo_pick", text="Pick Points", depress=_picking[0], icon="RESTRICT_SELECT_OFF")
        row.prop(s, "keep_focal")
        palette.list_with_menu(layout, "GROOMLAB_UL_photo_pairs", p, "pairs", "pair_index", "GROOMLAB_MT_pair_context",
                               rows=4)
        palette.draw_bar(layout, "lists/photos/pairsBar", "pairsbar", context)
        done = len(complete_pairs(cam))
        need = MIN_PAIRS_FIXED if s.keep_focal else MIN_PAIRS
        text = s.result if s.result else "%d complete pair%s: %s" % (
            done, "" if done == 1 else "s", "ready to Solve" if done >= need else "%d more to Solve" % (need - done))
        if p.rms >= 0.0 and not s.result:
            text += " (last solve %.2f px)" % p.rms
        _wrap(layout, text, width=chars)
    # Guides from Photos (experimental): folded (Blender 4.1+'s layout panels; a heading before).
    body = layout
    if hasattr(layout, "panel"):
        header, body = layout.panel("groomlab_photos_guides", default_closed=True)
        header.label(text="Guides from photos (experimental)")
    else:
        layout.label(text="GUIDES FROM PHOTOS (EXPERIMENTAL)")
    if body is not None:
        _wrap(body, "Every photo lined up with the mesh grows guides into the %s group (replaced each time); flow "
                    "follows the strands the photos show." % GROUP, width=chars)
        palette.draw_bar(body, "lists/photos/guidesBar", "photosguides", context)


classes = (GroomLabPhotoPair, GroomLabPhoto, GroomLabPhotos, GROOMLAB_OT_photo_import, GROOMLAB_OT_photo_look_through,
           GROOMLAB_OT_photo_rename, GROOMLAB_OT_photo_remove, GROOMLAB_OT_photo_pick, GROOMLAB_OT_pairs_solve,
           GROOMLAB_OT_pairs_clear, GROOMLAB_OT_pairs_delete, GROOMLAB_OT_pairs_repick, GROOMLAB_OT_photos_guides,
           GROOMLAB_UL_photos, GROOMLAB_UL_photo_pairs)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Object.groomlab_photo = PointerProperty(type=GroomLabPhoto)
    bpy.types.Scene.groomlab_photos = PointerProperty(type=GroomLabPhotos)


def unregister():
    _picking[0] = False
    if _draw_handle[0] is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_draw_handle[0], "WINDOW")
        _draw_handle[0] = None
    del bpy.types.Scene.groomlab_photos
    del bpy.types.Object.groomlab_photo
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
    _previous.clear()
