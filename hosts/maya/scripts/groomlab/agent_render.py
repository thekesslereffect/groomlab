# SPDX-License-Identifier: Apache-2.0
"""render_frame for the agent link (agent_link.py; engine/editor agent_tools.h): a frame of a groom
through one of Maya's renderers GroomLab already drives, as a PNG.

    arnold     MtoA's .ass (GroomLab's translator) rendered by kick, in the background
    hardware2  Maya's Hardware 2.0, in this session (the UI only)
    vray       V-Ray Standalone (vray_render.py), in the background
    renderman  RenderMan's prman (prman_render.py), in the background

The camera is the one named, else render_gather.render_camera's (the studio's, the view's, a
renderable one). The scene is left as it was (the frame put back). Each one raises RuntimeError
with what to do when its renderer is missing.
"""

import json
import os
import subprocess
import time

from maya import cmds

from groomlab import render_gather

ARNOLD_SAMPLES = {"draft": 2, "preview": 3, "final": 6}  # camera (AA) samples
TIMEOUT = 900  # seconds a background render may take


def _shape(groom):
    found = render_gather.render_grooms([groom])
    if not found:
        raise RuntimeError("no groom '%s' in the scene" % groom)
    return found[0]


def _folder():
    """<scene's groomlab folder>/renders/agent_<date>, as GroomLab's other renders go."""
    return render_gather.output_folder(None, "agent")


def _quality_of_samples(samples, table):
    """The quality whose samples come nearest `samples` (render settings that take a quality only)."""
    return min(table, key=lambda q: abs(table[q] - samples))


def _arnold(shape, camera, request, folder):
    if not cmds.pluginInfo("mtoa", query=True, registered=True) and not _mtoa_installed():
        raise RuntimeError("Arnold (MtoA) is not installed in this Maya: install MtoA, or render with another renderer "
                           "(hardware2 is always there)")
    if not cmds.pluginInfo("mtoa", query=True, loaded=True):
        # GroomLab's translator is found through MTOA_EXTENSIONS_PATH (the module's): the build's
        # extensions folder beside the plugin when it is not set.
        plugin = cmds.pluginInfo("GroomLab", query=True, path=True) or ""
        plugin_dir = os.path.dirname(plugin)
        extensions = os.path.join(os.path.dirname(plugin_dir), "extensions", os.path.basename(plugin_dir))
        if os.path.isdir(extensions) and extensions not in os.environ.get("MTOA_EXTENSIONS_PATH", ""):
            os.environ["MTOA_EXTENSIONS_PATH"] = os.pathsep.join(p for p in [extensions, os.environ.get("MTOA_EXTENSIONS_PATH")] if p)
        cmds.loadPlugin("mtoa", quiet=True)
    import mtoa.core
    mtoa.core.createOptions()
    mtoa_dir = os.path.dirname(os.path.dirname(cmds.pluginInfo("mtoa", query=True, path=True)))
    kick = os.path.join(mtoa_dir, "bin", "kick" + (".exe" if os.name == "nt" else ""))
    if not os.path.isfile(kick):
        raise RuntimeError("Arnold's kick was not found beside MtoA (%s)" % kick)
    samples = request.get("samples") or ARNOLD_SAMPLES[request.get("quality", "draft")]
    scene, image = os.path.join(folder, "frame.ass"), os.path.join(folder, "frame.png")
    keep = {}
    for plug, value in (("defaultArnoldRenderOptions.AASamples", int(samples)),):
        keep[plug] = cmds.getAttr(plug)
        cmds.setAttr(plug, value)
    driver = cmds.getAttr("defaultArnoldDriver.ai_translator")
    cmds.setAttr("defaultArnoldDriver.ai_translator", "png", type="string")
    lights = []
    try:
        lights = _temporary_lights(camera)
        cmds.arnoldExportAss(filename=scene, asciiAss=True, cam=camera)
    finally:
        if lights:
            cmds.undoInfo(stateWithoutFlush=False)
            try:
                cmds.delete(lights)
            finally:
                cmds.undoInfo(stateWithoutFlush=True)
        for plug, value in keep.items():
            cmds.setAttr(plug, value)
        cmds.setAttr("defaultArnoldDriver.ai_translator", driver, type="string")
    with open(scene) as f:
        if "curves" not in f.read():
            raise RuntimeError("Arnold's scene has no hair: MtoA loaded before GroomLab's translator was found "
                               "(MTOA_EXTENSIONS_PATH); restart Maya with the GroomLab module")
    command = [kick, "-i", scene, "-l", os.path.join(mtoa_dir, "shaders"), "-o", image, "-r", str(request["width"]),
               str(request["height"]), "-as", str(int(samples)), "-dw", "-dp", "-v", "1"]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True, timeout=TIMEOUT, creationflags=0x08000000 if os.name == "nt" else 0)
    if result.returncode != 0 or not os.path.isfile(image):
        raise RuntimeError("kick failed: %s" % result.stdout[-1500:])
    return image, {"samples": int(samples)}


LIGHT_TYPES = ("light", "aiSkyDomeLight", "aiAreaLight", "aiMeshLight", "aiPhotometricLight", "aiLightPortal")


def _temporary_lights(camera):
    """With no light in the scene (Arnold renders black without one, where Maya's views light by
    default): a key light from over the camera's shoulder and a dim sky dome, for this render only
    (made and deleted outside the undo queue). Returns what was made."""
    if cmds.ls(type=LIGHT_TYPES):
        return []
    cmds.undoInfo(stateWithoutFlush=False)
    try:
        made = []
        key = cmds.directionalLight(name="groomlabAgentKey", intensity=2.5)
        key = cmds.listRelatives(key, parent=True, fullPath=True)[0]
        m = cmds.xform(camera, query=True, worldSpace=True, matrix=True)
        cmds.xform(key, worldSpace=True, matrix=m)
        cmds.rotate(-25, 25, 0, key, relative=True, objectSpace=True)
        made.append(key)
        dome = cmds.shadingNode("aiSkyDomeLight", asLight=True, name="groomlabAgentSky")
        cmds.setAttr(cmds.listRelatives(dome, shapes=True, fullPath=True)[0] + ".intensity", 0.4)
        made.append(cmds.ls(dome, long=True)[0])
        return made
    finally:
        cmds.undoInfo(stateWithoutFlush=True)


def _mtoa_installed():
    try:
        return bool(cmds.pluginInfo("mtoa", query=True, path=True))
    except RuntimeError:
        return False


def _hardware2(shape, camera, request, folder):
    if cmds.about(batch=True):
        raise RuntimeError("Hardware 2.0 renders in Maya's UI only (this Maya runs without one): render with arnold")
    from groomlab import hardware2_render
    quality = request.get("quality", "draft")
    if request.get("samples"):
        quality = _quality_of_samples(request["samples"], hardware2_render.QUALITY)
    job = hardware2_render.render_hardware2(grooms=[shape], camera=camera, width=request["width"], height=request["height"],
                                            quality=quality, folder=folder, name="frame", show=False)
    images = job.images()
    if not images or not os.path.isfile(images[0]):
        raise RuntimeError("Hardware 2.0 wrote no image: %s" % "; ".join(job.errors() or []))
    return images[0], {"samples": hardware2_render.QUALITY[quality]}


def _background_job(start, shape, camera, request, folder, name):
    job = start(grooms=[shape], camera=camera, width=request["width"], height=request["height"],
                quality=request.get("quality", "draft"), folder=folder, name=name)
    job.wait(TIMEOUT)
    images = [p for p in job.images() if os.path.isfile(p)]
    if not images:
        raise RuntimeError("%s wrote no image (its log is in %s)" % (name, folder))
    return images[0], ({"note": "samples follow the quality in %s" % name} if request.get("samples") else {})


def _vray(shape, camera, request, folder):
    from groomlab import vray_render
    return _background_job(vray_render.render_vray, shape, camera, request, folder, "vray")


def _renderman(shape, camera, request, folder):
    from groomlab import prman_render
    return _background_job(prman_render.render_prman, shape, camera, request, folder, "renderman")


RENDERERS = {"arnold": _arnold, "hardware2": _hardware2, "vray": _vray, "renderman": _renderman}


def render(request):
    """A frame for render_frame's `request` (agent_link.h beginHostRender's "render"): {"png",
    "seconds", "info"}, or {"error"} saying what went wrong."""
    started = time.time()
    shape = None
    now = cmds.currentTime(query=True)
    try:
        shape = _shape(request["groom"])
        renderer = RENDERERS.get(request.get("renderer"))
        if renderer is None:
            raise RuntimeError("Maya renders with %s" % ", ".join(sorted(RENDERERS)))
        camera = render_gather.render_camera(request.get("camera") or None)
        if request.get("frame") is not None:
            cmds.currentTime(float(request["frame"]), update=True)
        png, info = renderer(shape, camera, request, _folder())
        info = dict(info, camera=camera.split("|")[-1])
        return {"png": png.replace("\\", "/"), "seconds": time.time() - started, "info": info}
    except (RuntimeError, ValueError, subprocess.SubprocessError, OSError) as e:
        return {"error": str(e)}
    finally:
        if cmds.currentTime(query=True) != now:
            cmds.currentTime(now, update=True)


def render_json(request_text):
    """render() for groomLabAgent's glue: JSON in, JSON out."""
    return json.dumps(render(json.loads(request_text)))

