# SPDX-License-Identifier: Apache-2.0
"""RenderMan scenes around GroomLab's RenderMan procedural: the RIB of a render and the prman run
that renders it. No Maya here (groomlab.prman_render gathers the Maya scene into render_gather's
plain data; tests/hosts/renderman/test_prman_scene.py checks the text without Maya or RenderMan).

A scene file (scene_text) holds one FrameBegin block per frame, each:

    Display        <image>.png (sRGB, as d_png writes linear renders) or <stem>.<frame>.png
    Hider, PixelVariance, PxrPathTracer   the quality's samples and bounces (QUALITY)
    Projection     PxrPerspective (its fov spans the image's shorter side) or orthographic
    the camera     Maya's camera matrix inverted, Z flipped (RenderMan's camera looks down its +Z)
    PxrRectLight   an area light (Arnold's quad light, Maya's area light): the light's matrix, turned
                   about Y (RenderMan's lights shine down their +Z, Maya's down -Z) and scaled 2 (its
                   rectangle spans -0.5..0.5, Maya's -1..1); intensity the radiance (Arnold's, divided
                   by the area with normalize); Arnold's spread as emissionFocus with a gain
                   (spread_to_focus, fitted against kick)
    PxrDomeLight   a sky dome: its color x intensity is the radiance
    PxrDistantLight a directional light: Arnold's intensity is the irradiance, PxrDistantLight's the
                   radiance of its disk (angleExtent): divided by the disk's solid angle
    meshes         PointsPolygons (triangles, Maya's normals per corner) or a Catmull-Clark
                   SubdivisionMesh (Arnold's subdivision, Maya's smooth preview), read from an archive
                   (.rib beside the scene: one per mesh, or per frame when it deforms), with a PxrSurface
                   from the shader's color, specular and roughness; light links as excluded lights,
                   meshes that cast no shadow invisible to transmission
    grooms         Procedural2 "DynamicLoad" groomlab_renderman (hosts/renderman) on the groom's .groom
                   at the frame, with the hair network of hosts/renderman/examples/groom.rib
                   (PxrHairColor + PxrMarschnerHair reading the procedural's primvars), and deformation
                   blur keys across the shutter when motion blur is on

Values that change over the frames (render_common's samples) are written at each frame's value.
"""

import glob
import math
import os
import re

from groomlab import render_common
from groomlab.render_common import frame_list, images_for, latest_image  # noqa: F401

ENV_VAR = "GROOMLAB_PRMAN"  # prman.exe, its folder or RenderMan Pro Server's (RMANTREE)
EXE = "prman.exe" if os.name == "nt" else "prman"
NOT_FOUND = ("RenderMan's prman was not found: install RenderMan Pro Server (free for non-commercial use), or set its "
             "path with groomlab.set_prman_path(path) (or the %s or RMANTREE environment variable)" % ENV_VAR)
PROCEDURAL_MISSING = ("GroomLab's RenderMan procedural (groomlab_renderman) is not installed: it comes with the "
                      "GroomLab module, in procedurals/renderman (or set GROOMLAB_RENDERMAN_PROCEDURAL)")

# Render quality: (max samples, min samples, pixel variance, indirect bounces). Hair needs many
# samples and bounces (light reaches the hair through many hairs): "final" for stills, "preview"
# to check a look, "draft" for framing.
QUALITY = {"draft": (32, 8, 0.05, 4), "preview": (128, 16, 0.02, 10), "final": (512, 32, 0.006, 10)}

# PxrDistantLight's disk (degrees): the sun's.
DISTANT_ANGLE = 0.53


# ---------------------------------------------------------------------------
# Finding prman

def _exe_in(path):
    return render_common.exe_in(path, EXE)


def default_paths():
    """Where RenderMan Pro Server installs by default (newest first)."""
    if os.name == "nt":
        roots = [os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Pixar"), r"C:\Program Files\Pixar"]
    else:
        roots = ["/opt/pixar", "/Applications/Pixar"]
    found = []
    for root in roots:
        for path in glob.glob(os.path.join(root, "RenderManProServer-*")):
            if path not in found:
                found.append(path)

    def version(path):
        return [int(v) for v in re.findall(r"\d+", os.path.basename(path))]
    return [os.path.join(p, "bin") for p in sorted(found, key=version, reverse=True)]


def find_prman(preferred=None, environ=None):
    """prman: `preferred` (the user's setting), else $GROOMLAB_PRMAN, else $RMANTREE's, else the newest
    default install, else on the PATH. None when there is none."""
    environ = os.environ if environ is None else environ
    return render_common.find_exe(EXE, [preferred, environ.get(ENV_VAR), environ.get("RMANTREE")] + default_paths(),
                                  environ)


def rmantree(prman):
    """RenderMan Pro Server's folder (RMANTREE) of a prman executable: the folder above its bin."""
    return os.path.dirname(os.path.dirname(os.path.abspath(prman)))


# ---------------------------------------------------------------------------
# Values

def number(x):
    x = float(x)
    return "0" if abs(x) < 1e-12 else "%.9g" % x


def numbers(values):
    return " ".join(number(v) for v in values)


def color(c):
    return "[%s %s %s]" % (number(c[0]), number(c[1]), number(c[2]))


def path_text(path):
    """A path as a RIB string: absolute, forward slashes, in quotes."""
    return '"%s"' % os.path.abspath(path).replace("\\", "/").replace('"', "'")


def matrix(m):
    """A Maya matrix (16 values, row-major, translation last: RenderMan's own layout) as RIB's."""
    return "[%s]" % numbers(m)


# ---------------------------------------------------------------------------
# The camera

def shorter_side_fov(horizontal, width, height):
    """PxrPerspective's fov (degrees): it spans the image's shorter side; `horizontal` in radians."""
    if width >= height:
        return math.degrees(2.0 * math.atan(math.tan(0.5 * horizontal) * float(height) / float(width)))
    return math.degrees(horizontal)


def camera_text(camera, frame, width, height):
    """The projection and the camera's transform (before WorldBegin) at `frame`."""
    m = render_common.unscaled(render_common.at(camera["matrix"], frame))[0]
    lines = []
    if camera.get("orthographic"):
        half_w = 0.5 * float(render_common.at(camera.get("ortho_width", 1.0), frame))
        half_h = half_w * float(height) / float(width)
        lines.append('Projection "orthographic"\n')
        lines.append("ScreenWindow %s %s %s %s\n" % (number(-half_w), number(half_w), number(-half_h), number(half_h)))
    else:
        fov = shorter_side_fov(render_common.at(camera["fov"], frame), width, height)
        lines.append('Projection "PxrPerspective" "float fov" [%s]\n' % number(fov))
    # Maya's camera looks down its -Z; RenderMan's down +Z: world to Maya's camera, then Z flipped.
    lines.append("Scale 1 1 -1\n")
    lines.append("ConcatTransform %s\n" % matrix(render_common.inverse_rigid(m)))
    return "".join(lines)


# ---------------------------------------------------------------------------
# Lights

def spread_to_focus(spread):
    """(PxrRectLight's emissionFocus, an intensity gain) for Arnold's quad light spread. Arnold's
    narrowing beam brightens on its axis; emissionFocus only darkens off it, so the gain brings the
    axis back. Fitted on a white floor 5 units under a 2x2 light (build/probe/prman: kick and prman,
    the floor's radiance across the beam): spread 1, 0.6, 0.5, 0.35 match focus 0, 0.4-0.5, 1, 3 with
    gains 1, 1.2, 1.5, 2.6, within 5% out to where the beam falls to a tenth (0.35: within 15%)."""
    s = max(0.0, min(1.0, 1.0 - float(spread)))
    return 18.2 * s ** 4.2, 1.0 + 10.8 * s ** 4.4


def light_text(light, frame):
    """A light (render_gather's dict) at `frame` as an attribute block."""
    kind = light["kind"]
    name = render_common.safe_name(light["name"])
    rgb = render_common.at(light.get("color", (1.0, 1.0, 1.0)), frame)
    intensity = float(render_common.at(light.get("intensity", 1.0), frame))
    shadows = 1 if light.get("shadows", True) else 0
    # Its own light group (grouping membership): a mesh it leaves unlit excludes it (mesh_text).
    lines = ["AttributeBegin\n", '  Attribute "identifier" "string name" ["%s"]\n' % name,
             '  Attribute "grouping" "string membership" ["%s"]\n' % name]
    if light.get("invisible", True):
        lines.append('  Attribute "visibility" "int camera" [0]\n')
    if kind == "rect":
        m = render_common.at(light["matrix"], frame)
        _, lengths = render_common.unscaled(m)
        if light.get("normalize"):
            intensity /= max(4.0 * lengths[0] * lengths[1], 1e-12)
        focus, gain = spread_to_focus(light.get("spread", 1.0))
        lines += ["  ConcatTransform %s\n" % matrix(m), "  Rotate 180 0 1 0\n", "  Scale 2 2 1\n",
                  '  Light "PxrRectLight" "%s" "float intensity" [%s] "color lightColor" %s "float emissionFocus" [%s]'
                  ' "int enableShadows" [%d]\n' % (name, number(intensity * gain), color(rgb), number(focus), shadows)]
    elif kind == "dome":
        m = render_common.unscaled(render_common.at(light.get("matrix", render_common.IDENTITY), frame))[0]
        lines += ["  ConcatTransform %s\n" % matrix(m),
                  '  Light "PxrDomeLight" "%s" "float intensity" [%s] "color lightColor" %s "int enableShadows" [%d]\n'
                  % (name, number(intensity), color(rgb), shadows)]
    elif kind == "direct":
        m = render_common.unscaled(render_common.at(light["matrix"], frame))[0]
        solid_angle = 2.0 * math.pi * (1.0 - math.cos(math.radians(DISTANT_ANGLE) * 0.5))
        lines += ["  ConcatTransform %s\n" % matrix(m), "  Rotate 180 0 1 0\n",
                  '  Light "PxrDistantLight" "%s" "float intensity" [%s] "color lightColor" %s "float angleExtent" [%s]'
                  ' "int enableShadows" [%d]\n' % (name, number(intensity / solid_angle), color(rgb),
                                                   number(DISTANT_ANGLE), shadows)]
    else:
        raise ValueError("no RenderMan light for %r" % kind)
    lines.append("AttributeEnd\n")
    return "".join(lines)


# ---------------------------------------------------------------------------
# Meshes

def material_text(name, material):
    """A PxrSurface from render_gather.mesh_material's {"diffuse", "reflect", "roughness", "ior"}:
    the diffuse color, and a GGX specular whose Fresnel runs from the IOR's reflectance facing to
    the specular color at grazing (Schlick, PxrSurface's artistic mode)."""
    material = material or {}
    diffuse = material.get("diffuse", (0.5, 0.5, 0.5))
    reflect = material.get("reflect", (0.0, 0.0, 0.0))
    params = ['"color diffuseColor" %s' % color(diffuse), '"float diffuseGain" [1]']
    if max(reflect) > 0.0:
        ior = float(material.get("ior", 1.5))
        facing = ((ior - 1.0) / (ior + 1.0)) ** 2
        params += ['"int specularFresnelMode" [0]', '"color specularFaceColor" %s' % color([facing * c for c in reflect]),
                   '"color specularEdgeColor" %s' % color(reflect),
                   '"float specularRoughness" [%s]' % number(material.get("roughness", 0.5)),
                   '"int specularModelType" [1]']
    else:
        params += ['"color specularFaceColor" [0 0 0]', '"color specularEdgeColor" [0 0 0]']
    return 'Bxdf "PxrSurface" "%s" %s\n' % (name, " ".join(params))


def _wrapped(prefix, values, per_line=12):
    """Numbers over several lines."""
    items = [number(v) if isinstance(v, float) else str(v) for v in values]
    return ("\n" + prefix).join(" ".join(items[i:i + per_line]) for i in range(0, len(items), per_line))


def mesh_geometry_text(mesh, points):
    """A mesh's geometry (object space) for its archive: the faces as a Catmull-Clark subdivision
    surface when it subdivides, else its triangles with Maya's normals per corner."""
    if mesh.get("subdivide") and mesh.get("polygons"):
        counts, ids = mesh["polygons"]
        return ('SubdivisionMesh "catmull-clark" [%s]\n  [%s]\n  ["interpolateboundary"] [1 0] [2] []\n  "P" [%s]\n'
                % (_wrapped("  ", counts), _wrapped("  ", ids), _wrapped("  ", [float(p) for p in points])))
    triangles = mesh["triangles"]
    text = 'PointsPolygons [%s]\n  [%s]\n  "P" [%s]' % (_wrapped("  ", [3] * (len(triangles) // 3)),
                                                      _wrapped("  ", triangles), _wrapped("  ", [float(p) for p in points]))
    normals, ids = mesh.get("normals"), mesh.get("face_normals")
    if normals and ids and len(ids) == len(triangles):
        flat = [float(normals[3 * i + k]) for i in ids for k in range(3)]
        text += '\n  "facevarying normal N" [%s]' % _wrapped("  ", flat)
    return text + "\n"


def mesh_archive(mesh, frame):
    """The archive file name of a mesh's geometry at `frame` (one for a mesh that never deforms)."""
    if render_common.is_keyed(mesh["points"]):
        return "%s.%04d.rib" % (mesh["name"], int(frame))
    return "%s.rib" % mesh["name"]


def write_mesh_archives(mesh, folder, frames):
    """Write a mesh's archives into `folder` (mesh_archive's names); returns their paths."""
    written = []
    for frame in (frames if render_common.is_keyed(mesh["points"]) else frames[:1]):
        path = os.path.join(folder, mesh_archive(mesh, frame))
        with open(path, "w") as f:
            f.write("##RenderMan RIB\n# GroomLab: %s\n" % mesh.get("shape", mesh["name"]))
            f.write(mesh_geometry_text(mesh, render_common.at(mesh["points"], frame)))
        written.append(path)
    return written


def mesh_text(mesh, frame, folder):
    """A mesh at `frame`: its attributes, material and transform around its archive."""
    name = render_common.safe_name(mesh["name"])
    lines = ["AttributeBegin\n", '  Attribute "identifier" "string name" ["%s"]\n' % name]
    if not mesh.get("primary_visibility", True):
        lines.append('  Attribute "visibility" "int camera" [0]\n')
    if not mesh.get("casts_shadows", True):
        lines.append('  Attribute "visibility" "int transmission" [0]\n')
    unlit = [render_common.safe_name(n) for n in mesh.get("ignore_lights") or ()]
    if unlit:
        lines.append('  Attribute "lighting" "string excludesubset" ["%s"]\n' % " ".join(unlit))
    lines.append("  " + material_text(name + "_mtl", mesh.get("material")))
    lines.append("  ConcatTransform %s\n" % matrix(render_common.at(mesh["matrix"], frame)))
    lines.append("  ReadArchive %s\n" % path_text(os.path.join(folder, mesh_archive(mesh, frame))))
    lines.append("AttributeEnd\n")
    return "".join(lines)


# ---------------------------------------------------------------------------
# Grooms

def hair_network(prefix, tilt=3.0):
    """hosts/renderman/examples/groom.rib's shading network (renderman.PRIMVARS, as RenderMan for
    Maya's create_hair_network makes it), its handles prefixed: PxrPrimvar readers of the
    procedural's rman_* primvars, PxrHairColor's pigment (Physical) and RGB look (Artistic) mixed
    by rman_rgbShare into PxrMarschnerHair; specularOffset minus HairColor's `tilt`. The same text as
    the core's renderman_look.h hairNetworkRib (the app's): this module runs without groomlab_core
    (a render farm's Python), so it keeps its own copy."""
    from groomlab.renderman import PRIMVARS
    p = prefix + "_"
    lines = []
    for key, varname, kind, default_param, default in PRIMVARS:
        line = 'Pattern "PxrPrimvar" "%spv_%s" "string varname" ["%s"] "string type" ["%s"]' % (p, key, varname, kind)
        if default_param:
            value = color(default) if isinstance(default, tuple) else "[%s]" % number(default)
            line += ' "%s %s" %s' % ("color" if kind == "color" else "float", default_param, value)
        lines.append(line + "\n")
    lines.append('Pattern "PxrHairColor" "%spigment" "int mode" [0] "reference float melanin" ["%spv_melanin:resultF"]'
                 ' "reference float redness" ["%spv_redness:resultF"] "reference color dye" ["%spv_dye:resultRGB"]'
                 ' "float randomMelanin" [0] "float strayDensity" [0] "string hairIndexPrimvar" ["hair_index"]\n'
                 % (p, p, p, p))
    lines.append('Pattern "PxrHairColor" "%srgb" "int mode" [1] "reference color Color" ["%spv_color:resultRGB"]'
                 ' "color randomColor" [0 0 0] "float strayDensity" [0]\n' % (p, p))
    for lobe in ("Diff", "TT", "TRT"):
        lines.append('Pattern "PxrMix" "%smix_%s" "reference color color1" ["%spigment:result%s"]'
                     ' "reference color color2" ["%srgb:result%s"] "reference float mixer" ["%spv_share:resultF"]\n'
                     % (p, lobe.lower(), p, lobe, p, lobe, p))
    lines.append('Bxdf "PxrMarschnerHair" "%shair" "reference color diffuseColor" ["%smix_diff:resultRGB"]'
                 ' "reference color specularColorTT" ["%smix_tt:resultRGB"]'
                 ' "reference color specularColorTRT" ["%smix_trt:resultRGB"]'
                 ' "reference float specularGainR" ["%spv_gainR:resultF"] "float specularGainTT" [2]'
                 ' "float specularGainTRT" [1.2] "reference float specularConeAngleR" ["%spv_coneR:resultF"]'
                 ' "reference float specularConeAngleTT" ["%spv_coneTT:resultF"]'
                 ' "reference float specularConeAngleTRT" ["%spv_coneTRT:resultF"]'
                 ' "float specularOffset" [%s] "float specularIor" [1.55]'
                 ' "reference float presence" ["%spv_opacity:resultF"]\n'
                 % (p, p, p, p, p, p, p, p, number(-float(tilt)), p))
    return "".join(lines)


def procedural_text(groom, frame, motion=None):
    """The procedural call for a groom {"name", "file", "density", "width_scale", "length_scale",
    "search_paths"} at `frame`; `motion` (start, end, keys) in frames from it."""
    params = ['"string dsoname" ["groomlab_renderman"]', '"string filename" [%s]' % path_text(groom["file"]),
              '"float frame" [%s]' % number(frame), '"string name" ["%s"]' % render_common.safe_name(groom["name"])]
    for key, param in (("density", "density"), ("width_scale", "widthScale"), ("length_scale", "lengthScale")):
        if groom.get(key) is not None and float(groom[key]) != 1.0:
            params.append('"float %s" [%s]' % (param, number(groom[key])))
    if groom.get("search_paths"):
        params.append('"string searchPaths" ["%s"]' % ";".join(p.replace("\\", "/") for p in groom["search_paths"]))
    if motion:
        params.append('"float motionStart" [%s] "float motionEnd" [%s] "int motionKeys" [%d]'
                      % (number(motion[0]), number(motion[1]), int(motion[2])))
    return 'Procedural2 "DynamicLoad" "DSOBound"\n    %s\n' % "\n    ".join(params)


def groom_text(groom, frame, motion=None):
    """A groom at `frame`: its hair network and the procedural, in its transform (the groom's world
    matrix: the identity for a groom grown in world space)."""
    name = render_common.safe_name(groom["name"])
    body = hair_network(name, groom.get("tilt", 3.0)) + procedural_text(groom, frame, motion)
    lines = ["AttributeBegin\n", '  Attribute "identifier" "string name" ["%s"]\n' % name]
    if groom.get("matrix"):
        lines.append("  ConcatTransform %s\n" % matrix(render_common.at(groom["matrix"], frame)))
    lines.append("  " + body.rstrip("\n").replace("\n", "\n  ") + "\n")
    lines.append("AttributeEnd\n")
    return "".join(lines)


# ---------------------------------------------------------------------------
# The scene

MOTION = (0.0, 0.5, 2)  # the shutter in frames from the frame (open at it, half a frame), its keys


def scene_text(width, height, frames, image, camera, lights=(), meshes=(), grooms=(), quality="preview",
               procedurals=None, folder=None, motion_blur=False, comment=""):
    """The whole scene as RIB: one frame block per frame of `frames`, rendering to images_for(image,
    frames). `camera`, `lights`, `meshes` as render_gather.gather gives them (the meshes' archives
    in `folder`, write_mesh_archives'); `grooms` procedural_text's dicts; `procedurals` the folder
    holding groomlab_renderman."""
    max_samples, min_samples, variance, bounces = QUALITY.get(quality, QUALITY["preview"])
    folder = folder or os.path.dirname(os.path.abspath(image))
    s = ["##RenderMan RIB\n# GroomLab: a RenderMan scene (groomlab.render_prman)%s\nversion 3.04\n"
         % (": " + comment if comment else "")]
    if procedurals:
        s.append('Option "searchpath" "string procedural" ["%s:@"]\n' % os.path.abspath(procedurals).replace("\\", "/"))
    for frame, path in zip(frames, images_for(image, frames)):
        s.append("FrameBegin %d\n" % int(frame))
        s.append("Format %d %d 1\n" % (int(width), int(height)))
        s.append('Display %s "png" "rgba"\n' % path_text(path))
        s.append('Hider "raytrace" "int maxsamples" [%d] "int minsamples" [%d] "int incremental" [0]\n'
                 % (max_samples, min_samples))
        s.append("PixelVariance %s\n" % number(variance))
        s.append('PixelFilter "gaussian" 2 2\n')
        s.append('Integrator "PxrPathTracer" "groomlab_pt" "int maxIndirectBounces" [%d]\n' % bounces)
        if motion_blur:
            s.append("Shutter %s %s\n" % (number(MOTION[0]), number(MOTION[1])))
        s.append(camera_text(camera, frame, width, height))
        s.append("WorldBegin\n")
        for light in lights:
            s.append(light_text(light, frame))
        for mesh in meshes:
            s.append(mesh_text(mesh, frame, folder))
        for groom in grooms:
            s.append(groom_text(groom, frame, MOTION if motion_blur else None))
        s.append("WorldEnd\nFrameEnd\n")
    return "".join(s)


# ---------------------------------------------------------------------------
# Running prman

def command(prman, scene, threads=0):
    """prman's command line rendering `scene`, its progress in the log."""
    cmd = [prman, "-Progress"]
    if threads:
        cmd.append("-t:%d" % int(threads))
    return cmd + [scene]


def environment(prman, environ=None):
    """prman's environment: RMANTREE its own install."""
    env = dict(os.environ if environ is None else environ)
    env["RMANTREE"] = rmantree(prman)
    return env


_PROGRESS = re.compile(r"R90000\s+([0-9.]+)%")
_ERROR = re.compile(r"\{(ERROR|SEVERE)\}")


def progress_of(log_text):
    """(None: prman does not say which frame, the current frame's percent done) from prman's log so
    far; 0 between frames (the last one at 100%)."""
    percents = _PROGRESS.findall(log_text)
    percent = float(percents[-1]) if percents else 0.0
    return None, (0.0 if percent >= 100.0 else percent)


class PrmanJob(render_common.RenderJob):
    """prman rendering in the background (render_common.RenderJob): poll() for progress, cancel()
    to stop it. The images land in `folder` (images_for), prman's output in render.log beside them."""

    name = "RenderMan"
    progress_of = staticmethod(progress_of)

    @staticmethod
    def is_error(line):
        return bool(_ERROR.search(line))


def start(prman, scene, image, frames, threads=0):
    """prman rendering `scene` in the background: a PrmanJob."""
    folder = os.path.dirname(os.path.abspath(image))
    render_common.clear_images(image, frames)
    return PrmanJob(command(prman, scene, threads), folder, frames, image, scene, env=environment(prman))
