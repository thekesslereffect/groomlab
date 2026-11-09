# SPDX-License-Identifier: Apache-2.0
"""V-Ray Standalone scenes around GroomLab's .vrscene grooms: the text of a scene and the vray.exe
run that renders it. No Maya here (groomlab.vray_render gathers the Maya scene into the plain data
these functions take; tests/hosts/vray/test_vray_scene.py checks them without Maya or V-Ray).

A scene file (scene_text) #includes the grooms' .vrscene files (vrscene_writer.h: GeomMayaHair, its
BRDFHair4 shader and a Node with the groom's world matrix) and adds:

    RenderView     the camera: its world matrix (V-Ray's camera looks down its -Z, Y up, as Maya's)
                   and its horizontal field of view in radians
    LightRectangle an area light (Arnold's quad light, Maya's area light): radiance color x intensity,
                   half sizes u_size/v_size; it shines down its -Z as Arnold's (both measured in vray:
                   a visible LightRectangle of intensity 1 renders its color)
    LightDome      a sky dome: radiance color x intensity, the whole sphere
    LightDirect    a distant light: V-Ray's intensity times pi is the irradiance (Arnold's intensity)
    GeomStaticMesh a mesh (triangles, Maya's normals per corner) with a BRDFVRayMtl and a Node
    SettingsLightLinker  lights a mesh ignores, and meshes that cast no shadow
    Settings*      the image size, the sampler, brute-force GI, units (centimeters), no default lights

A value that changes over the frames written (a turntable's camera and lights, a deforming mesh) is
written as V-Ray's interpolate((frame, value), ...) keys, as the grooms' are.
"""

import glob
import math
import os
import re
import sys

from groomlab import render_common
from groomlab.render_common import (IDENTITY, default_lights, first, frame_list, horizontal_fov, images_for,  # noqa: F401
                                    latest_image, mapped, unscaled)

ENV_VAR = "GROOMLAB_VRAY"  # vray.exe, or the folder holding it
# macOS: vray.bin inside the install's VRay.app (Standalone_for_<os>/VRay.app/Contents/MacOS).
EXE = "vray.exe" if os.name == "nt" else "vray.bin" if sys.platform == "darwin" else "vray"
NOT_FOUND = ("V-Ray Standalone (%s) was not found: install V-Ray Standalone, or set its path with "
             "groomlab.set_vray_path(path) (or the %s environment variable)" % (EXE, ENV_VAR))

# Render quality: the adaptive sampler's max subdivs (samples: their square), its noise threshold and
# brute-force GI's depth. Hair needs many samples: "final" for stills, "preview" to check a look.
QUALITY = {"draft": (4, 0.05, 2), "preview": (8, 0.02, 4), "final": (16, 0.005, 8)}


# ---------------------------------------------------------------------------
# Finding vray.exe

def _exe_in(path):
    """vray.exe at `path` (the file, its folder, or an install folder with bin/)."""
    return render_common.exe_in(path, EXE)


def default_paths():
    """Where V-Ray Standalone installs by default (newest layout first)."""
    if sys.platform == "darwin":
        return sorted(glob.glob("/Applications/Chaos/V-Ray/Standalone_for_*/VRay.app/Contents/MacOS"), reverse=True)
    roots = [os.environ.get("ProgramFiles", r"C:\Program Files"), r"C:\Program Files"] if os.name == "nt" else \
        ["/usr/ChaosGroup", "/opt/chaos", "/Applications/ChaosGroup"]
    paths = []
    for root in roots:
        paths += [os.path.join(root, "Chaos", "V-Ray", "Standalone for x64", "bin"),
                  os.path.join(root, "Chaos Group", "V-Ray", "Standalone for x64", "bin")]
    return paths


def find_vray(preferred=None, environ=None):
    """vray.exe: `preferred` (the user's setting), else $GROOMLAB_VRAY, else $VRAY_PATH, else the
    default install, else on the PATH. None when there is none."""
    environ = os.environ if environ is None else environ
    given = [preferred, environ.get(ENV_VAR), environ.get("VRAY_PATH")]
    if sys.platform == "darwin":  # the install folder or its VRay.app given: vray.bin inside it
        given = [p for g in given for p in ([g, os.path.join(str(g), "Contents", "MacOS"),
                                             os.path.join(str(g), "VRay.app", "Contents", "MacOS")] if g else [g])]
    return render_common.find_exe(EXE, given + default_paths(), environ)


# ---------------------------------------------------------------------------
# Values

def number(x):
    return "%.9g" % float(x)


def vector(v):
    return "Vector(%s, %s, %s)" % (number(v[0]), number(v[1]), number(v[2]))


def color(c):
    return "Color(%s, %s, %s)" % (number(c[0]), number(c[1]), number(c[2]))


def acolor(c, alpha=1.0):
    return "AColor(%s, %s, %s, %s)" % (number(c[0]), number(c[1]), number(c[2]), number(alpha))


def scene_path(path):
    return os.path.abspath(path).replace("\\", "/")


def string(text):
    return '"%s"' % str(text).replace("\\", "/").replace('"', "'")


def plugin_name(text):
    """A V-Ray plugin name: letters, digits and '_' (vrscene_writer.h vray::pluginName's rule)."""
    return render_common.safe_name(text)


def transform(m):
    """A Maya world matrix (16 values, row-major, the translation in the last row: each row the
    image of an axis) as a V-Ray Transform (its Matrix takes the images of the axes as columns)."""
    return "Transform(Matrix(%s, %s, %s), %s)" % (vector(m[0:3]), vector(m[4:7]), vector(m[8:11]), vector(m[12:15]))


def keyed(samples, fmt):
    """A value over the frames: `samples` [(frame, value)] (or one value for every frame) written
    once when it never changes, else as interpolate((frame, value), ...) keys."""
    if not isinstance(samples, list) or not samples or not isinstance(samples[0], tuple):
        return fmt(samples)
    texts = [(f, fmt(v)) for f, v in samples]
    if all(t == texts[0][1] for _, t in texts):
        return texts[0][1]
    return "interpolate(%s)" % ", ".join("(%s, %s)" % (number(f), t) for f, t in texts)


def _list(kind, values, fmt, per_line=8):
    items = [fmt(v) for v in values]
    lines = [", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line)]
    return "%s(\n    %s\n  )" % (kind, ",\n    ".join(lines)) if items else "%s()" % kind


def plugin(kind, name, params):
    """A plugin block: `params` [(name, value text)] in order."""
    body = "".join("  %s=%s;\n" % (k, v) for k, v in params if v is not None)
    return "%s %s {\n%s}\n\n" % (kind, name, body)


# ---------------------------------------------------------------------------
# Cameras

def camera_plugin(camera):
    """RenderView from {"matrix": samples of the world matrix, "fov": samples (radians, horizontal),
    "orthographic": bool, "ortho_width": scene units}."""
    params = [("transform", keyed(mapped(camera["matrix"], lambda m: unscaled(m)[0]), transform)),
              ("fov", keyed(camera["fov"], number)), ("clipping", "0")]
    if camera.get("orthographic"):
        params += [("orthographic", "1"), ("orthographicWidth", keyed(camera.get("ortho_width", 1.0), number))]
    return plugin("RenderView", "groomlab_camera", params)


# ---------------------------------------------------------------------------
# Lights

def spread_to_directional(spread):
    """V-Ray's LightRectangle directional for Arnold's quad light spread. V-Ray keeps a directional
    light's power (its beam gets brighter as it narrows); Arnold's spread brightens less. Fitted on a
    grey floor under a light (build/probe: kick and vray, the floor's profile across the beam):
    spread 1, 0.8, 0.5, 0.35 match directional 0, 0.02, 0.16, 0.33 within 10% on the beam's axis."""
    return max(0.0, min(1.0, 0.78 * (1.0 - float(spread)) ** 2))


def light_plugin(light):
    """A light from {"kind": "rect" | "dome" | "direct", "name", "matrix" (samples), "color",
    "intensity" (radiance for rect and dome, irradiance for direct: Arnold's), "invisible",
    "shadows", "directional" (rect: 0 even, toward 1 a narrow beam; else from Arnold's "spread"),
    "normalize" (rect: the intensity spread over the light's area, Arnold's normalize)}."""
    kind = light["kind"]
    name = light["name"]
    rgb = light.get("color", (1.0, 1.0, 1.0))
    intensity = light.get("intensity", 1.0)
    common = [("enabled", "1"), ("color", keyed(rgb, acolor)), ("shadows", "1" if light.get("shadows", True) else "0")]
    if kind == "rect":
        matrix = light["matrix"]
        _, lengths = unscaled(first(matrix))
        u, v = lengths[0], lengths[1]  # Maya's quad spans -1..1: its half sizes are the scales
        level = intensity
        if light.get("normalize"):
            level = mapped(intensity, lambda i: i / max(4.0 * u * v, 1e-12))
        return plugin("LightRectangle", name, [
            ("transform", keyed(mapped(matrix, lambda m: unscaled(m)[0]), transform))] + common + [
            ("intensity", keyed(level, number)), ("units", "0"),
            ("u_size", number(u)), ("v_size", number(v)),
            ("directional", number(max(0.0, min(1.0, light["directional"] if "directional" in light else
                                                spread_to_directional(light.get("spread", 1.0)))))),
            ("invisible", "1" if light.get("invisible", True) else "0"),
            ("noDecay", "0"), ("affectSpecular", "1"), ("affectReflections", "1")])
    if kind == "dome":
        return plugin("LightDome", name, [("transform", keyed(mapped(light.get("matrix", IDENTITY),
                                                                    lambda m: unscaled(m)[0]), transform))] + common + [
            ("intensity", keyed(intensity, number)), ("units", "0"), ("dome_spherical", "1"),
            ("invisible", "1" if light.get("invisible", True) else "0")])
    if kind == "direct":
        return plugin("LightDirect", name, [
            ("transform", keyed(mapped(light["matrix"], lambda m: unscaled(m)[0]), transform))] + common + [
            ("intensity", keyed(mapped(intensity, lambda i: i / math.pi), number)), ("units", "0")])
    raise ValueError("no V-Ray light for %r" % kind)


# ---------------------------------------------------------------------------
# Meshes

def mesh_plugins(mesh):
    """GeomStaticMesh, BRDFVRayMtl, MtlSingleBRDF and Node from {"name", "points" (samples of flat
    object-space xyz), "triangles" (vertex ids), "normals" (flat xyz), "face_normals" (normal ids per
    triangle corner), "matrix" (samples), "material": {"diffuse", "reflect", "roughness", "ior"},
    "primary_visibility"}."""
    n = mesh["name"]
    triples = lambda flat: [flat[i:i + 3] for i in range(0, len(flat), 3)]  # noqa: E731
    points = mapped(mesh["points"], triples)
    geometry = [("vertices", keyed(points, lambda pts: _list("ListVector", pts, vector, 4))),
                ("faces", _list("ListInt", mesh["triangles"], str, 24))]
    if mesh.get("normals") and mesh.get("face_normals"):
        geometry += [("normals", keyed(mapped(mesh["normals"], triples), lambda ns: _list("ListVector", ns, vector, 4))),
                     ("faceNormals", _list("ListInt", mesh["face_normals"], str, 24))]
    geometry += [("primary_visibility", "1" if mesh.get("primary_visibility", True) else "0")]
    material = mesh.get("material") or {}
    reflect = material.get("reflect", (0.0, 0.0, 0.0))
    brdf = [("diffuse", acolor(material.get("diffuse", (0.5, 0.5, 0.5)))), ("brdf_type", "4")]
    if max(reflect) > 0.0:
        brdf += [("reflect", acolor(reflect)), ("option_use_roughness", "1"),
                 ("reflect_glossiness", number(material.get("roughness", 0.5))), ("fresnel", "1"),
                 ("fresnel_ior_lock", "0"), ("fresnel_ior", number(material.get("ior", 1.5)))]
    return (plugin("GeomStaticMesh", n + "@mesh", geometry) + plugin("BRDFVRayMtl", n + "@brdf", brdf) +
            plugin("MtlSingleBRDF", n + "@mtl", [("brdf", n + "@brdf")]) +
            plugin("Node", n + "@node", [("transform", keyed(mesh["matrix"], transform)), ("geometry", n + "@mesh"),
                                         ("material", n + "@mtl"), ("visible", "1")]))


# ---------------------------------------------------------------------------
# The scene

def scene_text(width, height, includes=(), camera=None, lights=(), meshes=(), quality="preview", units=None,
               comment="", motion_blur=False):
    """The whole scene as .vrscene text. `includes`: the grooms' .vrscene files; `camera`, `lights`,
    `meshes`: camera_plugin's, light_plugin's and mesh_plugins' dicts (a mesh's "ignore_lights": light
    names that leave it unlit; "casts_shadows" False: no light's shadow); `units`: {"meters_scale",
    "up", "fps"} written only when no groom brings its own (vrscene_writer.h writes them)."""
    subdivs, threshold, depth = QUALITY.get(quality, QUALITY["preview"])
    s = ["// GroomLab: a V-Ray Standalone scene (groomlab.render_vray)%s\n" % (": " + comment if comment else "")]
    for path in includes:
        s.append('#include "%s"\n' % scene_path(path))
    s.append("\n")
    if units and not includes:
        up = "Vector(0, 0, 1)" if units.get("up", "Y") == "Z" else "Vector(0, 1, 0)"
        s.append(plugin("SettingsUnitsInfo", "groomlab_units", [
            ("meters_scale", number(units.get("meters_scale", 0.01))), ("scene_upDir", up),
            ("frames_scale", number(units.get("fps", 24.0))), ("seconds_scale", number(1.0 / units.get("fps", 24.0))),
            ("photometric_scale", "0.001")]))
    s.append(plugin("SettingsOutput", "groomlab_output", [("img_width", str(int(width))), ("img_height", str(int(height)))]))
    s.append(plugin("SettingsOptions", "groomlab_options", [("light_doDefaultLights", "0")]))
    # Brute-force GI (hair is lit through many hairs: BRDFHair4's own bounces, trace_depth, do most).
    s.append(plugin("SettingsGI", "groomlab_gi", [("on", "1"), ("primary_engine", "2"), ("secondary_engine", "2")]))
    s.append(plugin("SettingsDMCGI", "groomlab_dmcgi", [("depth", str(depth))]))
    s.append(plugin("SettingsImageSampler", "groomlab_sampler", [
        ("type", "1"), ("dmc_minSubdivs", "1"), ("dmc_maxSubdivs", str(subdivs)), ("dmc_threshold", number(threshold))]))
    s.append(plugin("FilterGaussian", "groomlab_filter", [("size", "1.5")]))
    if motion_blur:
        s.append(plugin("SettingsMotionBlur", "groomlab_motion_blur", [
            ("on", "1"), ("duration", "1"), ("interval_center", "0.5"), ("geom_samples", "2")]))
    if camera:
        s.append(camera_plugin(camera))
    for light in lights:
        s.append(light_plugin(light))
    for mesh in meshes:
        s.append(mesh_plugins(mesh))
    ignored, shadowless = [], []
    for light in lights:
        unlit = [m["name"] + "@node" for m in meshes if light["name"] in (m.get("ignore_lights") or ())]
        if unlit:
            ignored.append([light["name"]] + unlit)
        dark = [m["name"] + "@node" for m in meshes if not m.get("casts_shadows", True)]
        if dark:
            shadowless.append([light["name"]] + dark)
    if ignored or shadowless:
        lists = lambda groups: "List(%s)" % ", ".join("List(%s)" % ", ".join(g) for g in groups)  # noqa: E731
        params = []
        if ignored:
            params += [("ignored_lights", lists(ignored)),
                       ("include_exclude_light_flags", "ListInt(%s)" % ", ".join("0" for _ in ignored))]
        if shadowless:
            params += [("ignored_shadow_lights", lists(shadowless)),
                       ("include_exclude_shadow_flags", "ListInt(%s)" % ", ".join("0" for _ in shadowless))]
        s.append(plugin("SettingsLightLinker", "groomlab_light_linker", params))
    return "".join(s)


# ---------------------------------------------------------------------------
# Running vray.exe

def frames_argument(frames):
    """vray's -frames value for a list of whole frames: "b-e,s" when evenly spaced, else "a;b;c"."""
    frames = [int(f) for f in frames]
    if len(frames) == 1:
        return str(frames[0])
    step = frames[1] - frames[0]
    if step > 0 and all(b - a == step for a, b in zip(frames, frames[1:])):
        return "%d-%d" % (frames[0], frames[-1]) + (",%d" % step if step != 1 else "")
    return ";".join(str(f) for f in frames)


def command(vray, scene, image, frames, threads=0):
    """The vray.exe command line rendering `scene`'s `frames` to `image` (a PNG; with more than one
    frame vray writes <image stem>.<frame, 4 digits>.png), no window, progress in the log."""
    cmd = [vray, "-sceneFile=" + scene, "-imgFile=" + image, "-display=0", "-autoClose=1",
           "-frames=" + frames_argument(frames), "-verboseLevel=3", "-showProgress=2", "-progressUseCR=0",
           "-progressIncrement=5", "-progressUseColor=0"]
    if threads:
        cmd.append("-numThreads=%d" % int(threads))
    return cmd


_PROGRESS = re.compile(r"Rendering image\.\.\.: ([0-9.]+)%")
_FRAME = re.compile(r"Starting frame (-?[0-9.]+)")


def progress_of(log_text):
    """(the frame vray is on or None, its percent done) from vray's log so far."""
    frames = _FRAME.findall(log_text)
    percents = _PROGRESS.findall(log_text[log_text.rfind("Starting frame"):] if frames else log_text)
    return (float(frames[-1]) if frames else None), (float(percents[-1]) if percents else 0.0)



class VRayJob(render_common.RenderJob):
    """vray.exe rendering in the background (render_common.RenderJob): poll() for progress, cancel()
    to stop it. The images land in `folder` (images_for), vray's output in render.log beside them."""

    name = "V-Ray"
    progress_of = staticmethod(progress_of)


def start(vray, scene, image, frames, threads=0):
    """vray.exe rendering `scene` in the background: a VRayJob."""
    folder = os.path.dirname(os.path.abspath(image))
    render_common.clear_images(image, frames)  # a finished check must not count last time's images
    return VRayJob(command(vray, scene, image, frames, threads), folder, frames, image, scene)
