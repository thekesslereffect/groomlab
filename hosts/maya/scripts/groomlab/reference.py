# SPDX-License-Identifier: Apache-2.0
"""Grooming from photos: reference cameras (Groom Tools > Photos).

A reference is a Maya camera with the photo on an image plane, under one group
(groomlab_references). Look through it, line it up with the mesh (by eye, or by
pairs of points in the Photos window, ui/reference_window.py: a feature clicked on the photo
and the same feature clicked on the mesh, side by side, the camera solved for its position,
rotation and focal length as they come), lock it, and set how the photo shows: its opacity,
and in front of the mesh or behind it.

    import groomlab.reference as ref
    cam = ref.import_reference("C:/photos/front.jpg")    # copied into groomlab/<scene>/references
    ref.look_through(cam)
    ref.set_pair(cam, 0, image=(412, 230), mesh=(0, 9.5, 3.1))  # ... 6 pairs (4 keeping the focal length)
    ref.solve(cam)               # {"rms": 0.8, "focal": 52.3, ...}
    ref.lock_reference(cam)
    ref.set_reference_opacity(cam, 0.4)
    ref.look_back()

The photo fills the camera's film gate: the film back takes the image's aspect (36 mm
wide) and the image plane is sized to it, so a pixel of the photo is a direction from
the camera. The image plane shows only while looking through its camera.

Each camera keeps its settings in attributes that save with the scene: glReference,
glRefImage (the photo, relative to the scene like GroomLab's other files), glRefWidth and
glRefHeight (pixels), glRefPairs (the point pairs, JSON: [{"image": [u, v], "mesh": [x, y,
z]}, ...], pixels from the photo's top-left, world space), glRefDepth ("front" or
"behind"), glRefRms (the last solve's error in pixels, -1 before one) and glRefKeepFocal (a
solve keeps the focal length: set by hand, or Keep focal length).
"""

import filecmp
import json
import os
import re
import shutil

from maya import cmds

import groomlab

GROUP = "groomlab_references"
KIND = "references"  # groomlab/<scene>/references
FILM_WIDTH = 36.0 / 25.4  # the film back's width, inches (a full-frame 36 mm)
DEFAULT_FOCAL = 35.0
FOCAL_RANGE = (10.0, 200.0)  # the Focal Length sliders' starting range (mm)
DEFAULT_OPACITY = 0.6
DEPTHS = ("front", "behind")
MIN_PAIRS = 6          # pairs to solve the focal length too
MIN_PAIRS_FIXED = 4    # pairs with the focal length kept

# What lock_reference locks, besides the camera's own lockTransform (translate, rotate).
_LOCK_TRANSFORM = ("scaleX", "scaleY", "scaleZ")
_LOCK_CAMERA = ("focalLength", "horizontalFilmAperture", "verticalFilmAperture", "filmFit", "horizontalFilmOffset",
                "verticalFilmOffset", "lensSqueezeRatio", "cameraScale", "preScale", "postScale", "filmTranslateH",
                "filmTranslateV", "filmRollValue")
_LOCK_PLANE = ("fit", "sizeX", "sizeY", "offsetX", "offsetY", "rotate", "squeezeCorrection", "coverageX", "coverageY",
               "coverageOriginX", "coverageOriginY", "lockedToCamera")

_previous = {}  # model panel -> the camera it showed before looking through a reference


# ---------------------------------------------------------------------------
# Finding references


def is_reference(node):
    """True for a reference camera (its transform or shape)."""
    try:
        transform = _transform(node)
    except ValueError:
        return False
    return cmds.attributeQuery("glReference", node=transform, exists=True)


def _transform(node):
    """A camera's transform from its transform, shape or image plane."""
    if not node or not cmds.objExists(node):
        raise ValueError("%s does not exist" % node)
    kind = cmds.nodeType(node)
    if kind == "imagePlane":
        cameras = cmds.listConnections(node + ".message", type="camera") or []
        if not cameras:
            raise ValueError("%s is not on a camera" % node)
        node, kind = cameras[0], "camera"
    if kind == "camera":
        node = cmds.listRelatives(node, parent=True, fullPath=True)[0]
    return cmds.ls(node, long=True)[0]


def _camera(node):
    """The reference camera's transform (long name); ValueError for anything else."""
    transform = _transform(node)
    if not cmds.attributeQuery("glReference", node=transform, exists=True):
        raise ValueError("%s is not a GroomLab reference camera" % node.split("|")[-1])
    return transform


def _shape(camera):
    return cmds.listRelatives(camera, shapes=True, type="camera", fullPath=True)[0]


def image_plane(camera):
    """The reference's image plane shape."""
    planes = cmds.listConnections(_shape(_camera(camera)) + ".imagePlane", source=True, destination=False,
                                  shapes=True) or []
    if not planes:
        raise ValueError("%s has no image plane" % _short(camera))
    return cmds.ls(planes[0], long=True)[0]


def _plane_transform(camera):
    return cmds.listRelatives(image_plane(camera), parent=True, fullPath=True)[0]


def _short(node):
    return node.split("|")[-1]


def _name(node):
    """The shortest unique name (what the API returns)."""
    return cmds.ls(node)[0]


def list_references():
    """Every reference camera in the scene (transforms), in the group's order."""
    found = cmds.ls("*.glReference", objectsOnly=True, long=True, recursive=True) or []
    found = [n for n in found if cmds.nodeType(n) == "transform"]
    order = []
    if cmds.objExists("|" + GROUP):
        order = cmds.listRelatives("|" + GROUP, children=True, fullPath=True) or []
    ranked = {n: i for i, n in enumerate(order)}
    found.sort(key=lambda n: (ranked.get(n, len(ranked)), n))
    return [_name(n) for n in found]


# ---------------------------------------------------------------------------
# Making and removing


def image_size(path):
    """(width, height) of an image file, in pixels."""
    from maya.api import OpenMaya as om
    image = om.MImage()
    try:
        image.readFromFile(path)
    except RuntimeError:
        raise ValueError("cannot read the image %s" % path)
    width, height = image.getSize()
    if not width or not height:
        raise ValueError("cannot read the image %s" % path)
    return int(width), int(height)


def _copy_in(path):
    """The photo copied into the scene's references folder (the same file when it is there or
    one with the same content is); a free name when another file has its name."""
    folder = groomlab.asset_folder(KIND)
    destination = os.path.join(folder, os.path.basename(path)).replace("\\", "/")
    if os.path.exists(destination):
        if groomlab._same_file(path, destination) or filecmp.cmp(path, destination, shallow=False):
            return destination
        destination = groomlab._unique_path(destination)
    shutil.copy2(path, destination)
    return destination


def _unique_name(base):
    base = re.sub(r"[^A-Za-z0-9_]", "_", base) or "reference"
    if base[0].isdigit():
        base = "_" + base
    name, n = base, 2
    while cmds.objExists(name):
        name = "%s%d" % (base, n)
        n += 1
    return name


def _group():
    if not cmds.objExists("|" + GROUP):
        cmds.group(empty=True, name=GROUP, world=True)
    return "|" + GROUP


def _subject(groom=None):
    """(center, radius) of what the photos show: the groom's scalp, else every visible mesh."""
    meshes = []
    if groom:
        scalp = groomlab.scalp_mesh(groom)
        if scalp:
            meshes = [scalp]
    if not meshes:
        for mesh in cmds.ls(type="mesh", noIntermediate=True, long=True) or []:
            if mesh.startswith("|" + GROUP + "|") or not _visible(mesh):
                continue
            meshes.append(mesh)
    if not meshes:
        return (0.0, 0.0, 0.0), 10.0
    box = cmds.exactWorldBoundingBox(meshes, ignoreInvisible=True)
    center = tuple(0.5 * (box[i] + box[i + 3]) for i in range(3))
    radius = 0.5 * ((box[3] - box[0]) ** 2 + (box[4] - box[1]) ** 2 + (box[5] - box[2]) ** 2) ** 0.5
    return center, max(radius, 1e-3)


def _visible(node):
    """False when the node or a parent of it is hidden."""
    try:
        return all(cmds.getAttr(p + ".visibility") for p in _ancestors(node))
    except (RuntimeError, ValueError):
        return True


def _ancestors(node):
    full = cmds.ls(node, long=True)[0]
    parts = full.split("|")
    return ["|".join(parts[:i + 1]) for i in range(1, len(parts))]


def import_reference(image_path, name=None, groom=None, copy=True):
    """A reference camera for the photo at `image_path`: a camera with the photo on an image
    plane, under the groomlab_references group, its film back of the photo's aspect. With
    `copy` (the default) the photo is copied into the scene's groomlab/<scene>/references
    folder first (and saved relative to the scene, so it travels with it); copy=False keeps
    the original file where it is. It starts in front of the groom's scalp (or the scene's
    meshes) at 35 mm, aimed at it, the photo drawn in front of the mesh at 60% opacity.
    Returns the camera's transform. One undo step."""
    groomlab.ensure_plugin()
    if not image_path or not os.path.isfile(image_path):
        raise ValueError("no image at %s" % image_path)
    width, height = image_size(image_path)
    path = _copy_in(image_path) if copy else os.path.abspath(image_path).replace("\\", "/")
    base = name or os.path.splitext(os.path.basename(image_path))[0] + "_ref"
    selection = cmds.ls(selection=True, long=True) or []  # kept: the groom stays the active one
    with groomlab.undo_chunk("import reference"):
        transform, shape = cmds.camera()
        transform = cmds.rename(transform, _unique_name(base))
        transform = cmds.parent(transform, _group())[0]
        transform = cmds.ls(transform, long=True)[0]
        shape = _shape(transform)
        for attr, kind in (("glReference", "bool"), ("glRefWidth", "long"), ("glRefHeight", "long"),
                           ("glRefRms", "double"), ("glRefKeepFocal", "bool")):
            cmds.addAttr(transform, longName=attr, shortName=attr, attributeType=kind)
        for attr in ("glRefImage", "glRefPairs", "glRefDepth"):
            cmds.addAttr(transform, longName=attr, shortName=attr, dataType="string")
        cmds.setAttr(transform + ".glReference", True)
        cmds.setAttr(transform + ".glRefWidth", width)
        cmds.setAttr(transform + ".glRefHeight", height)
        cmds.setAttr(transform + ".glRefRms", -1.0)
        cmds.setAttr(transform + ".glRefImage", groomlab.portable_path(path), type="string")
        cmds.setAttr(transform + ".glRefPairs", "[]", type="string")
        cmds.setAttr(transform + ".glRefDepth", "front", type="string")
        # The film back takes the photo's aspect; the whole gate shows in the view (overscan).
        cmds.setAttr(shape + ".horizontalFilmAperture", FILM_WIDTH)
        cmds.setAttr(shape + ".verticalFilmAperture", FILM_WIDTH * height / float(width))
        cmds.setAttr(shape + ".filmFit", 3)  # Overscan
        cmds.setAttr(shape + ".overscan", 1.05)
        cmds.setAttr(shape + ".focalLength", DEFAULT_FOCAL)
        cmds.setAttr(shape + ".displayFilmGate", False)
        cmds.setAttr(shape + ".displayResolution", False)
        plane = cmds.imagePlane(camera=transform, fileName=path)
        plane_shape = cmds.ls(plane[1], long=True)[0] if len(plane) > 1 else image_plane(transform)
        # The photo covers the film gate exactly, whatever the view's shape.
        cmds.setAttr(plane_shape + ".fit", 4)  # To Size
        cmds.setAttr(plane_shape + ".sizeX", FILM_WIDTH)
        cmds.setAttr(plane_shape + ".sizeY", FILM_WIDTH * height / float(width))
        cmds.setAttr(plane_shape + ".displayOnlyIfCurrent", True)
        cmds.setAttr(plane_shape + ".alphaGain", DEFAULT_OPACITY)
        _place(transform, groom)
        _apply_depth(transform)
        if selection:
            cmds.select(selection, replace=True)
        else:
            cmds.select(clear=True)
    return _name(transform)


def image_plane_cameras():
    """Cameras in the scene with an image plane that are not references yet (adopt_camera's
    candidates): their transforms."""
    out = []
    for shape in cmds.ls(type="camera", long=True) or []:
        planes = cmds.listConnections(shape + ".imagePlane", source=True, destination=False, shapes=True) or []
        transform = cmds.listRelatives(shape, parent=True, fullPath=True)[0]
        if planes and not is_reference(transform):
            out.append(_name(transform))
    return out


def adopt_camera(camera):
    """Make a camera that already has a photo on an image plane a reference (as Import Photo
    would: the film back takes the photo's aspect, the image plane covers the film gate, the
    photo shows only through it). The photo stays where it is. Returns the camera. One undo step."""
    transform = _transform(camera)
    shape = _shape(transform)
    if is_reference(transform):
        return _name(transform)
    planes = cmds.listConnections(shape + ".imagePlane", source=True, destination=False, shapes=True) or []
    if not planes:
        raise ValueError("%s has no image plane" % _short(transform))
    plane = cmds.ls(planes[0], long=True)[0]
    path = cmds.getAttr(plane + ".imageName") or ""
    if not path or not os.path.isfile(path):
        raise ValueError("%s's image plane has no image file" % _short(transform))
    width, height = image_size(path)
    with groomlab.undo_chunk("adopt reference camera"):
        for attr, kind in (("glReference", "bool"), ("glRefWidth", "long"), ("glRefHeight", "long"),
                           ("glRefRms", "double"), ("glRefKeepFocal", "bool")):
            cmds.addAttr(transform, longName=attr, shortName=attr, attributeType=kind)
        for attr in ("glRefImage", "glRefPairs", "glRefDepth"):
            cmds.addAttr(transform, longName=attr, shortName=attr, dataType="string")
        cmds.setAttr(transform + ".glReference", True)
        cmds.setAttr(transform + ".glRefWidth", width)
        cmds.setAttr(transform + ".glRefHeight", height)
        cmds.setAttr(transform + ".glRefRms", -1.0)
        cmds.setAttr(transform + ".glRefImage", groomlab.portable_path(path.replace("\\", "/")), type="string")
        cmds.setAttr(transform + ".glRefPairs", "[]", type="string")
        cmds.setAttr(transform + ".glRefDepth", "front", type="string")
        aperture = cmds.getAttr(shape + ".horizontalFilmAperture")
        cmds.setAttr(shape + ".verticalFilmAperture", aperture * height / float(width))
        cmds.setAttr(plane + ".fit", 4)  # To Size: the photo covers the film gate
        cmds.setAttr(plane + ".sizeX", aperture)
        cmds.setAttr(plane + ".sizeY", aperture * height / float(width))
        cmds.setAttr(plane + ".offsetX", 0.0)
        cmds.setAttr(plane + ".offsetY", 0.0)
        cmds.setAttr(plane + ".displayOnlyIfCurrent", True)
        _apply_depth(transform)
    return _name(transform)


def _place(camera, groom=None):
    """In front (+Z) of the subject, far enough for it to fill most of the photo's short side."""
    import math
    shape = _shape(camera)
    center, radius = _subject(groom)
    focal = cmds.getAttr(shape + ".focalLength")
    half = min(math.atan(0.5 * cmds.getAttr(shape + ".horizontalFilmAperture") * 25.4 / focal),
               math.atan(0.5 * cmds.getAttr(shape + ".verticalFilmAperture") * 25.4 / focal))
    distance = radius / math.sin(half) * 1.05
    cmds.setAttr(camera + ".rotateOrder", 0)  # xyz, as the solve gives it
    cmds.xform(camera, worldSpace=True, translation=(center[0], center[1], center[2] + distance),
               rotation=(0.0, 0.0, 0.0))
    cmds.setAttr(shape + ".nearClipPlane", max(0.01, 0.001 * distance))
    cmds.setAttr(shape + ".farClipPlane", max(10000.0, 50.0 * distance))
    cmds.setAttr(shape + ".centerOfInterest", distance)


def remove_reference(camera, delete_file=False):
    """Delete the reference camera and its image plane. With `delete_file`, its photo goes too
    when it is in the scene's references folder and no other reference shows it."""
    camera = _camera(camera)
    path = image_path(camera)
    with groomlab.undo_chunk("remove reference"):
        for panel, previous in list(_previous.items()):
            if _same_node(previous, camera):
                del _previous[panel]
        for panel in cmds.getPanel(type="modelPanel") or []:
            if _same_node(_panel_camera(panel), camera):
                cmds.lookThru(panel, "persp")
        cmds.delete(camera)
        if cmds.objExists("|" + GROUP) and not cmds.listRelatives("|" + GROUP, children=True):
            cmds.delete("|" + GROUP)
    if delete_file and path and os.path.isfile(path):
        folder = groomlab.asset_folder(KIND, create=False)
        inside = groomlab._relative_to(path, folder) is not None
        shared = any(groomlab._same_file(image_path(c), path) for c in list_references() if image_path(c))
        if inside and not shared:
            os.remove(path)


def rename_reference(camera, name):
    """Rename the reference camera; returns its new name."""
    camera = _camera(camera)
    if not name or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
        raise ValueError("names are letters, digits and _ (not starting with a digit)")
    return _name(cmds.rename(camera, name))


def image_path(camera):
    """The photo's file, resolved (absolute)."""
    stored = cmds.getAttr(_camera(camera) + ".glRefImage") or ""
    return groomlab.resolve_path(stored, node=_camera(camera)).replace("\\", "/") if stored else ""


def _set_image(camera, stored, absolute=None):
    """Point the reference at a photo: `stored` as saved, the image plane at its file."""
    camera = _camera(camera)
    cmds.setAttr(camera + ".glRefImage", stored, type="string")
    absolute = absolute or image_path(camera)
    plane = image_plane(camera)
    was = cmds.getAttr(plane + ".imageName", lock=True)
    cmds.setAttr(plane + ".imageName", lock=False)
    cmds.setAttr(plane + ".imageName", absolute, type="string")
    cmds.setAttr(plane + ".imageName", lock=was)


def relink():
    """Every reference's image plane at its photo as the scene resolves it now (a scene that
    moved, or opened on another machine, with its groomlab folder). The plugin calls it when
    a scene opens. Returns the references repointed."""
    changed = []
    for camera in list_references():
        try:
            stored = cmds.getAttr(camera + ".glRefImage") or ""
            now = cmds.getAttr(image_plane(camera) + ".imageName") or ""
            path = image_path(camera)
        except (RuntimeError, ValueError):
            continue
        if stored and path and os.path.isfile(path) and not (now and os.path.isfile(now) and groomlab._same_file(now, path)):
            _set_image(camera, stored, path)
            changed.append(camera)
    return changed


# ---------------------------------------------------------------------------
# Looking through


def _panel_camera(panel):
    try:
        camera = cmds.modelPanel(panel, query=True, camera=True)
    except RuntimeError:
        return None
    return camera


def _same_node(a, b):
    if not a or not b or not cmds.objExists(a) or not cmds.objExists(b):
        return False
    try:
        return _transform(a) == _transform(b)
    except ValueError:
        return False


def active_panel():
    """The model panel a Look Through acts on: the viewport last worked in (or with focus)."""
    try:
        focus = cmds.getPanel(withFocus=True)
    except RuntimeError:
        focus = None
    if focus and cmds.getPanel(typeOf=focus) == "modelPanel":
        return focus
    try:
        editor = cmds.playblast(activeEditor=True)
    except RuntimeError:
        editor = None
    if editor:
        panel = editor.split("|")[-1]
        if cmds.getPanel(typeOf=panel) == "modelPanel":
            return panel
    for panel in cmds.getPanel(visiblePanels=True) or []:
        if cmds.getPanel(typeOf=panel) == "modelPanel":
            return panel
    return None


def look_through(camera, panel=None):
    """Show the reference's view in `panel` (the active viewport); look_back() returns to the
    camera it showed before. Returns the panel."""
    camera = _camera(camera)
    panel = panel or active_panel()
    if not panel:
        raise RuntimeError("no viewport to look through")
    current = _panel_camera(panel)
    if current and not is_reference(current):
        _previous[panel] = _transform(current)
    cmds.lookThru(panel, camera)
    return panel


def look_back(panel=None):
    """The viewport back on the camera it showed before looking through a reference
    (persp by default). Returns the camera."""
    panel = panel or active_panel()
    if not panel:
        raise RuntimeError("no viewport")
    camera = _previous.pop(panel, None)
    if not camera or not cmds.objExists(camera) or is_reference(camera):
        camera = "persp"
    cmds.lookThru(panel, camera)
    return _name(camera)


def current_reference(panel=None):
    """The reference the viewport looks through, or None."""
    panel = panel or active_panel()
    camera = _panel_camera(panel) if panel else None
    return _name(_transform(camera)) if camera and is_reference(camera) else None


# ---------------------------------------------------------------------------
# How the photo shows


def set_reference_opacity(camera, opacity):
    """The photo's opacity, 0 (invisible) to 1 (opaque): its image plane's alpha gain."""
    cmds.setAttr(image_plane(camera) + ".alphaGain", min(max(float(opacity), 0.0), 1.0))


def reference_opacity(camera):
    return cmds.getAttr(image_plane(camera) + ".alphaGain")


def set_reference_focal(camera, focal, keep=True):
    """The photo camera's focal length (mm, 9 to 1080: the solve's plausible lenses), set by hand; with `keep`
    Keep focal length turns on, so a solve keeps it. A locked camera keeps its own (unlock it first)."""
    camera = _camera(camera)
    if is_locked(camera):
        raise RuntimeError("%s is locked: unlock it to change its focal length" % _short(camera))
    cmds.setAttr(_shape(camera) + ".focalLength", min(max(float(focal), 9.0), 1080.0))
    if keep:
        set_keep_focal(camera, True)


def keep_focal(camera):
    """Whether a solve keeps the camera's focal length (else 6 or more pairs solve it too)."""
    camera = _camera(camera)
    return cmds.attributeQuery("glRefKeepFocal", node=camera, exists=True) and         bool(cmds.getAttr(camera + ".glRefKeepFocal"))


def set_keep_focal(camera, on=True):
    camera = _camera(camera)
    if not cmds.attributeQuery("glRefKeepFocal", node=camera, exists=True):
        cmds.addAttr(camera, longName="glRefKeepFocal", shortName="glRefKeepFocal", attributeType="bool")
    cmds.setAttr(camera + ".glRefKeepFocal", bool(on))


def set_reference_visible(camera, visible=True):
    """Show or hide the photo (the camera stays)."""
    cmds.setAttr(_plane_transform(camera) + ".visibility", bool(visible))


def reference_visible(camera):
    return bool(cmds.getAttr(_plane_transform(camera) + ".visibility"))


def set_reference_depth(camera, where):
    """"front": the photo draws over the mesh (see the mesh through it with the opacity);
    "behind": the mesh draws over the photo. The image plane sits between the camera and the
    subject, or past it (its depth follows the camera: solves and moves keep the side)."""
    if where not in DEPTHS:
        raise ValueError("depth is one of %s" % ", ".join(DEPTHS))
    camera = _camera(camera)
    cmds.setAttr(camera + ".glRefDepth", where, type="string")
    _apply_depth(camera)


def reference_depth(camera):
    return cmds.getAttr(_camera(camera) + ".glRefDepth") or "front"


def _apply_depth(camera):
    """The image plane's distance for the camera's side (front or behind the subject)."""
    shape = _shape(camera)
    center, radius = _subject()
    eye = cmds.xform(camera, query=True, worldSpace=True, translation=True)
    distance = sum((a - b) ** 2 for a, b in zip(eye, center)) ** 0.5
    near, far = cmds.getAttr(shape + ".nearClipPlane"), cmds.getAttr(shape + ".farClipPlane")
    if reference_depth(camera) == "front":
        depth = max(2.0 * near, 0.5 * (distance - radius))
    else:
        depth = min(0.9 * far, distance + 2.0 * radius)
    cmds.setAttr(image_plane(camera) + ".depth", depth)


# ---------------------------------------------------------------------------
# Locking


def lock_reference(camera, locked=True):
    """Lock (or unlock) the reference where it is: the camera's position and rotation
    (Maya's Lock Camera: tumbling, tracking and dollying through it do nothing), its scale,
    focal length and film back, and the image plane's placement on the film."""
    camera = _camera(camera)
    shape = _shape(camera)
    plane = image_plane(camera)
    cmds.camera(camera, edit=True, lockTransform=bool(locked))
    for node, attrs in ((camera, _LOCK_TRANSFORM), (shape, _LOCK_CAMERA), (plane, _LOCK_PLANE)):
        for attr in attrs:
            if cmds.attributeQuery(attr, node=node, exists=True):
                cmds.setAttr(node + "." + attr, lock=bool(locked))


def is_locked(camera):
    return bool(cmds.camera(_camera(camera), query=True, lockTransform=True))


# ---------------------------------------------------------------------------
# Point pairs and the solve


def pairs(camera):
    """The point pairs: [{"image": [u, v] or None, "mesh": [x, y, z] or None, "error": px}, ...]."""
    text = cmds.getAttr(_camera(camera) + ".glRefPairs") or "[]"
    try:
        data = json.loads(text)
    except ValueError:
        data = []
    out = []
    for p in data if isinstance(data, list) else []:
        if isinstance(p, dict):
            out.append({"image": p.get("image"), "mesh": p.get("mesh"), "error": p.get("error")})
    return out


def _store_pairs(camera, data):
    clean = []
    for p in data:
        entry = {"image": [float(v) for v in p["image"]] if p.get("image") else None,
                 "mesh": [float(v) for v in p["mesh"]] if p.get("mesh") else None}
        if p.get("error") is not None and entry["image"] and entry["mesh"]:
            entry["error"] = float(p["error"])
        clean.append(entry)
    cmds.setAttr(_camera(camera) + ".glRefPairs", json.dumps(clean), type="string")


def set_pair(camera, index, image=None, mesh=None):
    """Set pair `index`'s image point and/or mesh point (index == the count: a new pair)."""
    data = pairs(camera)
    if index == len(data):
        data.append({"image": None, "mesh": None})
    if not 0 <= index < len(data):
        raise ValueError("no pair %d" % (index + 1))
    if image is not None:
        data[index]["image"] = list(image)
    if mesh is not None:
        data[index]["mesh"] = list(mesh)
    data[index]["error"] = None
    _store_pairs(camera, data)


def remove_pair(camera, index):
    data = pairs(camera)
    if not 0 <= index < len(data):
        raise ValueError("no pair %d" % (index + 1))
    del data[index]
    _store_pairs(camera, data)


def clear_pairs(camera):
    _store_pairs(camera, [])
    cmds.setAttr(_camera(camera) + ".glRefRms", -1.0)


def complete_pairs(camera):
    """The pairs with both points, as (index, pair)."""
    return [(i, p) for i, p in enumerate(pairs(camera)) if p.get("image") and p.get("mesh")]


def solve(camera, fix_focal=None, apply=True):
    """Line the camera up with its pairs: the position, rotation and (unless `fix_focal`, by
    default the camera's Keep focal length) focal length that put every pair's mesh point on its image point (6 complete pairs, 4 with
    the focal length kept). With `apply` the camera takes them (one undo step; its film back
    stays) and each pair keeps its error. Returns {"translate", "rotate", "focal" (mm), "rms"
    (pixels), "errors" (pixels, per complete pair)}."""
    camera = _camera(camera)
    shape = _shape(camera)
    if apply and is_locked(camera):
        raise ValueError("%s is locked: unlock it to align it again" % _short(camera))
    if fix_focal is None:
        fix_focal = keep_focal(camera)
    complete = complete_pairs(camera)
    need = MIN_PAIRS_FIXED if fix_focal else MIN_PAIRS
    if len(complete) < need:
        raise ValueError("a solve needs %d complete pairs%s; %s has %d" % (
            need, " with the focal length kept" if fix_focal else " (4 with the focal length kept)",
            _short(camera), len(complete)))
    width, height = cmds.getAttr(camera + ".glRefWidth"), cmds.getAttr(camera + ".glRefHeight")
    flags = dict(imagePoint=[tuple(p["image"]) for _, p in complete],
                 worldPoint=[tuple(p["mesh"]) for _, p in complete],
                 imageSize=(width, height),
                 filmBack=(cmds.getAttr(shape + ".horizontalFilmAperture"),
                           cmds.getAttr(shape + ".verticalFilmAperture")),
                 focal=cmds.getAttr(shape + ".focalLength"), camera=camera)
    if fix_focal:
        flags["fixFocal"] = True
    values = cmds.groomLabCameraSolve(**flags)
    result = {"translate": tuple(values[0:3]), "rotate": tuple(values[3:6]), "focal": values[6], "rms": values[7],
              "errors": list(values[8:])}
    if apply:
        with groomlab.undo_chunk("align reference"):
            cmds.setAttr(camera + ".rotateOrder", 0)
            cmds.xform(camera, worldSpace=True, translation=result["translate"], rotation=result["rotate"])
            cmds.setAttr(shape + ".focalLength", result["focal"])
            data = pairs(camera)
            for (index, _), error in zip(complete, result["errors"]):
                data[index]["error"] = error
            _store_pairs(camera, data)
            cmds.setAttr(camera + ".glRefRms", result["rms"])
            _apply_depth(camera)
    return result


def camera_dict(camera):
    """The reference camera as the core's reference code takes it: {"matrix" (its world matrix),
    "focal_mm", "aperture_in" (the horizontal film aperture), "width", "height" (the photo's pixels)}."""
    camera = _camera(camera)
    shape = _shape(camera)
    return {"matrix": cmds.getAttr(camera + ".worldMatrix[0]"), "focal_mm": cmds.getAttr(shape + ".focalLength"),
            "aperture_in": cmds.getAttr(shape + ".horizontalFilmAperture"),
            "width": cmds.getAttr(camera + ".glRefWidth"), "height": cmds.getAttr(camera + ".glRefHeight")}


def focal_px(camera):
    """The camera's focal length in the photo's pixels."""
    camera = _camera(camera)
    shape = _shape(camera)
    return cmds.getAttr(shape + ".focalLength") / 25.4 / cmds.getAttr(shape + ".horizontalFilmAperture") * \
        cmds.getAttr(camera + ".glRefWidth")


def apply_solved_camera(camera, solved):
    """A solve from the core (groomlab_core.solve_reference_landmarks' dict: position, rotate_xyz,
    focal_px, rms) onto the camera: its position, rotation and focal length (its film back stays),
    the image plane's depth after it. No undo chunk of its own (the Reference window keeps its own
    stack; scripts wrap it in theirs). False when the camera is locked (nothing changes)."""
    camera = _camera(camera)
    if is_locked(camera):
        return False
    shape = _shape(camera)
    cmds.setAttr(camera + ".rotateOrder", 0)
    cmds.xform(camera, worldSpace=True, translation=tuple(solved["position"]), rotation=tuple(solved["rotate_xyz"]))
    width = cmds.getAttr(camera + ".glRefWidth")
    cmds.setAttr(shape + ".focalLength", solved["focal_px"] * cmds.getAttr(shape + ".horizontalFilmAperture") * 25.4 / width)
    cmds.setAttr(camera + ".glRefRms", solved.get("rms", -1.0))
    _apply_depth(camera)
    return True


def reference_info(camera):
    """What the Photos tab shows of a reference."""
    camera = _camera(camera)
    shape = _shape(camera)
    return {"camera": _name(camera), "name": _short(camera), "image": image_path(camera),
            "width": cmds.getAttr(camera + ".glRefWidth"), "height": cmds.getAttr(camera + ".glRefHeight"),
            "opacity": reference_opacity(camera), "visible": reference_visible(camera),
            "depth": reference_depth(camera), "locked": is_locked(camera),
            "focal": cmds.getAttr(shape + ".focalLength"), "keepFocal": keep_focal(camera),
            "rms": cmds.getAttr(camera + ".glRefRms"), "pairs": pairs(camera)}


# ---- Morph to Photos --------------------------------------------------------------------------------

MORPH_ATTR = "glPhotoMorph"  # on the blendShape node Morph to Photos makes (one per scalp)
MORPH_PINS_ATTR = "glPhotoMorphPins"  # on that node: each pair's stored world point and its rest point (6 per pair)
MORPH_REST_ATTR = "glPhotoMorphRest"  # on that node: the scalp's object-space points before the morph (x, y, z, ...)


def photo_morph_node(groom):
    """The blendShape node holding the groom's scalp's photo morph, or None."""
    shape = groomlab.scalp_mesh(groom)
    if not shape:
        return None
    for node in cmds.ls(cmds.listHistory(shape) or [], type="blendShape") or []:
        if cmds.attributeQuery(MORPH_ATTR, node=node, exists=True):
            return node
    return None


def _object_points(shape):
    from maya.api import OpenMaya as om
    fn = om.MFnMesh(om.MSelectionList().add(shape).getDagPath(0))
    return fn, [(p.x, p.y, p.z) for p in fn.getPoints(om.MSpace.kObject)]


def morph_to_photos(groom, stiffness=0.0, symmetric=True):
    """The groom's scalp morphed so every lined-up reference camera's pairs land on their photo
    points (groomlab_core.morph_to_pins: one photo moves a pair across its view, two or more set
    its depth too; `symmetric`: mirrored across the scalp's local x = 0), as a blendShape target
    at 1 on the scalp (its own mesh left as it is; reset_photo_morph removes it). Always from the
    scalp as it was before the first morph: a pair picked on the morphed scalp counts at the same
    spot of the original surface, so morphs never stack. One undo step. Returns {"pins",
    "rms_before", "rms_after"} (pixels)."""
    import groomlab_core
    from maya.api import OpenMaya as om
    shape = groomlab.scalp_mesh(groom)
    if not shape:
        raise ValueError("%s has no scalp" % groom)
    transform = cmds.listRelatives(shape, parent=True, fullPath=True)[0]
    to_object = om.MMatrix(cmds.getAttr(transform + ".worldMatrix[0]")).inverse()
    node = photo_morph_node(groom)
    # The shape shown (the pairs were picked on it) and the rest shape (the morph off).
    fn, _shown = _object_points(shape)
    intersector = om.MMeshIntersector()
    intersector.create(fn.object(), om.MMatrix())  # (object space)
    # The rest shape kept on the morph node (turning the morph off to read it is not enough: Maya's
    # Evaluation Manager hands back the cached, morphed points, and morphs stacked).
    rest_points = _shown
    if node and cmds.attributeQuery(MORPH_REST_ATTR, node=node, exists=True):
        flat = cmds.getAttr(node + "." + MORPH_REST_ATTR) or []
        if len(flat) == 3 * len(_shown):
            rest_points = [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]
    # The pairs the last morph used: their rest points as found then (a pair's world point stays
    # where it was picked while the surface moves away; found again on the moved surface it lands
    # on another spot of the rest shape, and the morphs crept).
    known = []
    if node and cmds.attributeQuery(MORPH_PINS_ATTR, node=node, exists=True):
        flat = cmds.getAttr(node + "." + MORPH_PINS_ATTR) or []
        known = [(flat[i:i + 3], flat[i + 3:i + 6]) for i in range(0, len(flat) - 5, 6)]
    bound = []  # (world point as stored, rest point)
    views, pins = [], []
    size = max(1e-6, max(abs(c) for p in rest_points for c in p))
    for camera in list_references():
        complete = complete_pairs(camera)
        if cmds.getAttr(camera + ".glRefRms") < 0.0 or not complete:
            continue  # not lined up: its pairs say nothing about the scalp's shape
        cam = camera_dict(camera)
        cam["matrix"] = list(om.MMatrix(cam["matrix"]) * to_object)
        views.append(cam)
        for _i, pair in complete:
            rest = next((list(r) for w, r in known if sum((w[c] - pair["mesh"][c]) ** 2 for c in range(3)) <= 1e-12), None)
            if rest is None:  # picked on the shape shown: the same spot of the rest shape
                p = om.MPoint(pair["mesh"]) * to_object
                hit = intersector.getClosestPoint(p)
                tri = fn.getPolygonTriangleVertices(hit.face, hit.triangle)
                u, v = hit.barycentricCoords
                weights = (u, v, 1.0 - u - v)
                rest = [sum(weights[k] * rest_points[tri[k]][c] for k in range(3)) for c in range(3)]
            bound.append((list(pair["mesh"]), rest))
            pin = next((q for q in pins if sum((q["mesh"][c] - rest[c]) ** 2 for c in range(3)) ** 0.5 <= 1e-4 * size),
                       None)
            if pin is None:
                pin = {"mesh": rest, "sights": []}
                pins.append(pin)
            pin["sights"].append((len(views) - 1, float(pair["image"][0]), float(pair["image"][1])))
    if not pins:
        raise ValueError("no lined-up reference camera has pairs: pick pairs and line a photo up first")
    r = groomlab_core.morph_to_pins(rest_points, pins, views, float(stiffness), bool(symmetric), 0)
    if not r["ok"]:
        raise ValueError(r["error"])
    with groomlab.undo_chunk("morph to photos"):
        if node:
            cmds.delete(node)
        target = cmds.duplicate(transform, name="groomLabPhotoMorphTarget")[0]
        target_shape = cmds.listRelatives(target, shapes=True, fullPath=True, noIntermediate=True)[0]
        tfn = om.MFnMesh(om.MSelectionList().add(target_shape).getDagPath(0))
        tfn.setPoints([om.MPoint(p) for p in r["points"]], om.MSpace.kObject)
        node = cmds.blendShape(target, transform, name=transform.split("|")[-1] + "PhotoMorph", frontOfChain=True)[0]
        cmds.setAttr(node + ".w[0]", 1.0)
        cmds.addAttr(node, longName=MORPH_ATTR, attributeType="bool")
        cmds.addAttr(node, longName=MORPH_REST_ATTR, dataType="doubleArray")
        cmds.setAttr(node + "." + MORPH_REST_ATTR, [c for p in rest_points for c in p], type="doubleArray")
        cmds.addAttr(node, longName=MORPH_PINS_ATTR, dataType="doubleArray")
        cmds.setAttr(node + "." + MORPH_PINS_ATTR, [c for w, r in bound for c in list(w) + list(r)], type="doubleArray")
        cmds.delete(target)
    return {"pins": len(pins), "rms_before": r["rms_before"], "rms_after": r["rms_after"]}


def reset_photo_morph(groom):
    """The scalp as it was before Morph to Photos (its blendShape removed); False when none. Undoable."""
    node = photo_morph_node(groom)
    if not node:
        return False
    with groomlab.undo_chunk("reset photo morph"):
        cmds.delete(node)
    return True
