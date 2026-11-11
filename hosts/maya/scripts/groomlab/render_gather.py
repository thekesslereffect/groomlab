# SPDX-License-Identifier: Apache-2.0
"""The Maya scene as GroomLab's standalone renders take it (V-Ray Standalone: vray_render;
RenderMan's prman: prman_render), any Maya version: which grooms, meshes, camera and lights a render
shows, read over the frames into plain data (render_common's samples) that each renderer's scene
writer (vray_scene, prman_scene) turns into its own text.

    grooms = render_grooms()                    # every visible groom (shapes)
    camera = render_camera()                    # the studio's, else the view's, else a renderable one
    meshes = render_meshes(grooms, "visible")
    cam, lights, mesh_data, skipped = gather(frames, grooms, camera, meshes, aspect)

A light: {"kind": "rect" (Arnold's quad, Maya's area light) | "dome" (a sky dome) | "direct" (a
directional light), "name", "matrix" (samples of the world matrix), "color", "intensity" (2^exposure
times it: radiance for rect and dome, irradiance for direct, as Arnold has them), "normalize" (rect:
Arnold's), "spread" (rect: Arnold's, 1 even), "invisible" (unseen by the camera), "shadows"}.
A mesh: {"shape", "name", "points" (flat object-space xyz, samples when it deforms), "triangles",
"normals", "face_normals" (normal ids per triangle corner), "polygons" ((face vertex counts, face
vertex ids): the faces as Maya has them), "subdivide" (Arnold's subdivision or Maya's smooth
preview), "matrix" (samples), "material" (mesh_material), "primary_visibility", "casts_shadows",
"ignore_lights" (light names linked away from it)}.
A camera: {"matrix" (samples), "fov" (samples, horizontal, radians), "orthographic", "ortho_width"}.
"""

import datetime
import os

from maya import cmds

from groomlab import render_common

LIGHT_TYPES = ("aiAreaLight", "areaLight", "aiSkyDomeLight", "directionalLight")
SKIPPED_LIGHT_TYPES = ("pointLight", "spotLight", "volumeLight", "ambientLight", "aiMeshLight", "aiPhotometricLight",
                       "aiLightPortal", "aiPhysicalSky")
FILM_FIT = {0: "fill", 1: "horizontal", 2: "vertical", 3: "overscan"}


def _dag(node):
    from maya.api import OpenMaya as om
    sel = om.MSelectionList()
    sel.add(node)
    return sel.getDagPath(0)


def world_matrix(node):
    """A node's world matrix in centimeters (Maya's internal unit), row-major."""
    m = _dag(node).inclusiveMatrix()
    return [m[i] for i in range(16)]


def parent_of(shape):
    return cmds.listRelatives(shape, parent=True, fullPath=True)[0]


def visible(node):
    return bool(cmds.ls(node, visible=True))


def get(node, attr, default=None):
    """An attribute's value (a compound's tuple), or `default` when the node has none."""
    if not cmds.attributeQuery(attr, node=node, exists=True):
        return default
    value = cmds.getAttr(node + "." + attr)
    if isinstance(value, list) and len(value) == 1 and isinstance(value[0], tuple):
        return value[0]
    return value


def unique_names(nodes):
    """Names for Maya nodes a renderer takes: their short names made safe (render_common.safe_name),
    kept apart."""
    names, used = {}, set()
    for node in nodes:
        base = render_common.safe_name(node.split("|")[-1].split(":")[-1])
        name, k = base, 1
        while name in used:
            k += 1
            name = "%s_%d" % (base, k)
        used.add(name)
        names[node] = name
    return names


def render_grooms(grooms=None):
    """The grooms a render shows: `grooms` ([] for none), else (None) every visible one (shapes, long names)."""
    import groomlab
    if grooms is not None:
        found = []
        for g in ([grooms] if isinstance(grooms, str) else grooms):
            shapes = cmds.ls(g, long=True) or []
            if shapes and cmds.nodeType(shapes[0]) == "transform":
                shapes = cmds.listRelatives(shapes[0], shapes=True, type=groomlab.GROOM_TYPE, fullPath=True) or []
            found += [s for s in shapes if s not in found]
        return found
    return [g for g in cmds.ls(type=groomlab.GROOM_TYPE, long=True) or [] if visible(g)]


def render_camera(camera=None):
    """The camera a render looks through (a transform): `camera`, else the studio's, else the
    viewport's with focus (in the UI), else the first renderable one, else persp."""
    from groomlab import studio
    if camera:
        node = cmds.ls(camera, long=True)[0]
        return parent_of(node) if cmds.nodeType(node) == "camera" else node
    if cmds.objExists(studio.CAMERA):
        return cmds.ls(studio.CAMERA, long=True)[0]
    if not cmds.about(batch=True):
        panel = cmds.getPanel(withFocus=True) or ""
        if panel not in (cmds.getPanel(type="modelPanel") or []):
            panel = cmds.getPanel(visiblePanels=True) or []
            panel = next((p for p in panel if p in (cmds.getPanel(type="modelPanel") or [])), "")
        if panel:
            cam = cmds.modelPanel(panel, query=True, camera=True)
            if cam and cmds.objExists(cam):
                node = cmds.ls(cam, long=True)[0]
                return parent_of(node) if cmds.nodeType(node) == "camera" else node
    renderable = [s for s in cmds.ls(type="camera", long=True) or [] if cmds.getAttr(s + ".renderable")]
    return parent_of(renderable[0]) if renderable else cmds.ls("persp", long=True)[0]


def camera_fov(shape, aspect):
    """A camera's horizontal field of view (radians) at image `aspect` (its film fit); 0 when orthographic."""
    if cmds.getAttr(shape + ".orthographic"):
        return 0.0
    return render_common.horizontal_fov(cmds.getAttr(shape + ".focalLength"), cmds.getAttr(shape + ".horizontalFilmAperture"),
                                        cmds.getAttr(shape + ".verticalFilmAperture"), aspect,
                                        FILM_FIT.get(cmds.getAttr(shape + ".filmFit"), "fill"),
                                        cmds.getAttr(shape + ".cameraScale"))


def _shader(shape):
    """The surface shader a mesh renders with (Arnold's override first), or None."""
    for group in cmds.listConnections(shape, type="shadingEngine") or []:
        for attr in ("aiSurfaceShader", "surfaceShader"):
            if cmds.attributeQuery(attr, node=group, exists=True):
                found = cmds.listConnections(group + "." + attr, source=True, destination=False) or []
                if found:
                    return found[0]
    return None


def mesh_material(shape):
    """{"diffuse", "reflect", "roughness", "ior"} for a mesh, from its shader's plain values
    (aiStandardSurface, standardSurface, lambert and the like; textures are not followed: their
    color is)."""
    shader = _shader(shape)
    grey = {"diffuse": (0.5, 0.5, 0.5), "reflect": (0.0, 0.0, 0.0)}
    if not shader:
        return grey
    kind = cmds.nodeType(shader)
    scale = lambda c, s: tuple(float(x) * float(s) for x in c)  # noqa: E731
    if kind in ("aiStandardSurface", "standardSurface"):
        return {"diffuse": scale(get(shader, "baseColor", (0.8, 0.8, 0.8)), get(shader, "base", 1.0)),
                "reflect": scale(get(shader, "specularColor", (1.0, 1.0, 1.0)), get(shader, "specular", 1.0)),
                "roughness": float(get(shader, "specularRoughness", 0.2)), "ior": float(get(shader, "specularIOR", 1.5))}
    color = get(shader, "color") or get(shader, "baseColor") or get(shader, "outColor")
    if not color:
        return grey
    material = {"diffuse": scale(color, get(shader, "diffuse", 1.0)), "reflect": (0.0, 0.0, 0.0)}
    if kind in ("blinn", "phong", "phongE"):
        material.update(reflect=scale(get(shader, "specularColor", (0.5, 0.5, 0.5)), 0.5), roughness=0.3, ior=1.5)
    return material


def render_meshes(grooms, which="visible"):
    """The meshes a render shows (shapes, long names): every visible renderable mesh, or with
    "selected" the selected ones and the grooms' scalps and colliders, or a list."""
    if which not in ("visible", "selected", None, "none"):
        shapes = cmds.ls(which, dag=True, type="mesh", noIntermediate=True, long=True) or []
    elif which == "selected":
        from groomlab import studio
        from groomlab.base import selected_meshes
        shapes = selected_meshes()  # (faces selected: their mesh)
        for groom in grooms:
            shapes += studio.head_meshes(groom)
    elif which == "visible":
        shapes = cmds.ls(type="mesh", noIntermediate=True, long=True) or []
    else:
        shapes = []
    out = []
    for shape in shapes:
        if shape in out or not visible(shape):
            continue
        if cmds.attributeQuery("primaryVisibility", node=shape, exists=True) and not cmds.getAttr(shape + ".primaryVisibility") \
                and not cmds.getAttr(shape + ".castsShadows"):
            continue
        out.append(shape)
    return out


def deforms(shape):
    """Whether a mesh's points move over time (a deformer, or input history that is animated)."""
    history = cmds.listHistory(shape, pruneDagObjects=True) or []
    if not history:
        return False
    return bool(cmds.ls(history, type="geometryFilter") or cmds.listConnections(history, type="animCurve",
                                                                               destination=False))


def mesh_geometry(shape):
    """(flat object-space points, triangle vertex ids, flat normals, normal ids per triangle corner,
    (face vertex counts, face vertex ids))."""
    from maya.api import OpenMaya as om
    fn = om.MFnMesh(_dag(shape))
    points = [c for p in fn.getPoints(om.MSpace.kObject) for c in (p.x, p.y, p.z)]
    _counts, offsets = fn.getTriangleOffsets()
    counts, vertices = fn.getVertices()
    normals = [c for n in fn.getNormals(om.MSpace.kObject) for c in (n.x, n.y, n.z)]
    _ncounts, normal_ids = fn.getNormalIds()
    return (points, [vertices[o] for o in offsets], normals, [normal_ids[o] for o in offsets],
            (list(counts), list(vertices)))


def mesh_points(shape):
    from maya.api import OpenMaya as om
    return [c for p in om.MFnMesh(_dag(shape)).getPoints(om.MSpace.kObject) for c in (p.x, p.y, p.z)]


def subdivided(shape):
    """Whether a mesh renders subdivided: Arnold's subdivision on, or Maya's smooth mesh preview."""
    return bool(get(shape, "aiSubdivType", 0)) or bool(get(shape, "displaySmoothMesh", 0))


def light_entry(shape, name):
    """A light's entry (the module's docstring; its matrix filled in per frame), or None."""
    kind = cmds.nodeType(shape)
    intensity = float(get(shape, "intensity", 1.0)) * 2.0 ** float(get(shape, "aiExposure", 0.0) or 0.0)
    rgb = tuple(get(shape, "color", (1.0, 1.0, 1.0)))
    if kind in ("aiAreaLight", "areaLight"):
        normalize = get(shape, "aiNormalize", get(shape, "normalize", True))
        camera = get(shape, "aiCamera", 0.0)
        return {"kind": "rect", "name": name, "color": rgb, "intensity": intensity, "normalize": bool(normalize),
                "spread": float(get(shape, "aiSpread", 1.0)), "invisible": float(camera or 0.0) <= 0.0,
                "shadows": bool(get(shape, "aiCastShadows", True))}
    if kind == "aiSkyDomeLight":
        return {"kind": "dome", "name": name, "color": rgb, "intensity": intensity,
                "invisible": float(get(shape, "camera", 1.0)) <= 0.0, "shadows": True}
    if kind == "directionalLight":
        return {"kind": "direct", "name": name, "color": rgb, "intensity": intensity,
                "shadows": bool(get(shape, "aiCastShadows", True))}
    return None


def _linked_meshes(light, meshes):
    """The meshes of `meshes` the light leaves unlit (Maya's light links)."""
    try:
        lit = set(cmds.ls(cmds.lightlink(query=True, light=light, shapes=True, transforms=False) or [], long=True))
    except RuntimeError:
        return []
    if not lit:
        return []
    return [m for m in meshes if m not in lit and parent_of(m) not in lit]


def resolution():
    """(width, height) of the render settings."""
    return int(cmds.getAttr("defaultResolution.width")), int(cmds.getAttr("defaultResolution.height"))


def gather(frames, grooms, camera, mesh_shapes, aspect):
    """The scene over `frames` (the module's docstring): (camera, lights, meshes, skipped lights).
    Without a light a renderer takes, render_common.default_lights."""
    cam_shape = cmds.listRelatives(camera, shapes=True, type="camera", fullPath=True)[0]
    light_shapes = [s for s in cmds.ls(type=list(LIGHT_TYPES), long=True) or [] if visible(s)]
    skipped = [s for s in cmds.ls(type=list(SKIPPED_LIGHT_TYPES), long=True) or [] if visible(s)]
    light_names = unique_names(light_shapes)
    mesh_names = unique_names(mesh_shapes)
    lights = [dict(light_entry(s, light_names[s]), matrix=[]) for s in light_shapes]
    meshes = []
    for shape in mesh_shapes:
        points, triangles, normals, normal_ids, polygons = mesh_geometry(shape)
        if not triangles:
            continue
        meshes.append({"shape": shape, "name": "mesh_" + mesh_names[shape], "points": points, "triangles": triangles,
                       "normals": normals, "face_normals": normal_ids, "polygons": polygons,
                       "subdivide": subdivided(shape), "matrix": [], "material": mesh_material(shape),
                       "primary_visibility": bool(get(shape, "primaryVisibility", True)),
                       "casts_shadows": bool(get(shape, "castsShadows", True)),
                       "deforms": len(frames) > 1 and deforms(shape)})
    for light, shape in zip(lights, light_shapes):
        unlit = _linked_meshes(shape, [m["shape"] for m in meshes])
        for m in meshes:
            if m["shape"] in unlit:
                m.setdefault("ignore_lights", []).append(light["name"])
    cam = {"matrix": [], "fov": [], "orthographic": bool(cmds.getAttr(cam_shape + ".orthographic"))}
    if cam["orthographic"]:
        from maya.api import OpenMaya as om
        cam["ortho_width"] = om.MDistance.uiToInternal(cmds.getAttr(cam_shape + ".orthographicWidth"))
    now = cmds.currentTime(query=True)
    deforming = [m for m in meshes if m["deforms"]]
    for m in deforming:
        m["points"] = []
    try:
        for frame in frames:
            if len(frames) > 1 or frame != now:
                cmds.currentTime(frame, update=True)
            cam["matrix"].append((frame, world_matrix(camera)))
            cam["fov"].append((frame, camera_fov(cam_shape, aspect)))
            for light, shape in zip(lights, light_shapes):
                light["matrix"].append((frame, world_matrix(shape)))
            for m in meshes:
                m["matrix"].append((frame, world_matrix(m["shape"])))
            for m in deforming:
                m["points"].append((frame, mesh_points(m["shape"])))
    finally:
        if cmds.currentTime(query=True) != now:
            cmds.currentTime(now, update=True)
    if not lights:
        lights = render_common.default_lights(render_common.first(cam["matrix"]))
    return cam, lights, meshes, skipped


def output_folder(folder, name):
    """The render's folder: `folder`, else <scene's groomlab folder>/renders/<name>_<date>; made."""
    import groomlab
    if folder:
        path = folder
    else:
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(groomlab.asset_folder("renders"), "%s_%s" % (name, stamp))
    path = os.path.abspath(path).replace("\\", "/")
    if not os.path.isdir(path):
        os.makedirs(path)
    return path


def fps():
    from maya import mel
    try:
        return float(mel.eval("currentTimeUnitToFPS()"))
    except RuntimeError:
        return 24.0


def up_axis():
    return "Z" if cmds.upAxis(query=True, axis=True) == "z" else "Y"


def show_image(path):
    """An image in Maya's Render View (opened when closed). Nothing in batch."""
    if cmds.about(batch=True) or not path or not os.path.isfile(path):
        return False
    from maya import mel
    if not cmds.renderWindowEditor("renderView", exists=True):
        mel.eval("RenderViewWindow")
    cmds.renderWindowEditor("renderView", edit=True, loadImage=path.replace("\\", "/"))
    return True
