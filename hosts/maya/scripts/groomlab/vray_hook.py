# SPDX-License-Identifier: Apache-2.0
"""V-Ray for Maya through its post-translate script: the way GroomLab rendered in V-Ray for Maya
before its translator (hosts/maya/vray, vraytr_groomlab), which V-Ray loads from the module and
which renders every groom with nothing to set up, with the groom's own material. Kept for scenes
saved with the hook on: when the translator is installed (translator_installed), post_translate
does nothing, so the groom is not drawn twice. On Maya 2022, or without V-Ray for Maya loaded,
every entry point says "V-Ray for Maya 2024+ required" and changes nothing. In any Maya, V-Ray
Standalone renders the groom (groomlab.render_vray).

    import groomlab.vray_hook as hook
    hook.enable_hook()        # V-Ray's post-translate script calls hook.post_translate()
    hook.disable_hook()       # the line taken out again (the rest of the script kept)
    hook.load_vrayscene(path) # the fallback: a VRayScene node in Maya pointing at an exported .vrscene

The hook. V-Ray for Maya runs the Python in its Render Settings' "Post Translate Python Script"
(the vraySettings node's postTranslatePython attribute) after it has translated the Maya scene into
V-Ray plugins and before it renders, with V-Ray's `vray.utils` module available to edit them
(Chaos docs: V-Ray for Maya > "Python Access to Translated V-Ray Scene",
https://docs.chaos.com/display/VMAYA/Python+Access+to+Translated+V-Ray+Scene; and Render Settings >
Common > "MEL/Python callbacks", https://docs.chaos.com/display/VMAYA/Common+tab). enable_hook adds
one line to that script; post_translate then, for each visible groom:
  1. writes the groom to a .vrscene with groomlab.export_vrscene (vrscene_writer.h): the frame
     being rendered, or with V-Ray's camera motion blur on (vraySettings.cam_mbOn) its shutter:
     cam_mbDuration frames whose middle is cam_mbIntervalCenter after the frame (the defaults, 1
     and 0.5: the frame to the next), in cam_mbGeomSamples keys (Chaos docs: Render Settings >
     "Camera tab" > Motion blur, https://docs.chaos.com/display/VMAYA/Camera+tab); the render density and
     overrides; the Node in the file carries the groom's world matrix at each key;
  2. makes (or finds again) a VRayScene plugin named groomlab_<groom>@vrayScene with
     vray.utils.create("VRayScene", name) / vray.utils.findByName(name) and sets its filepath, and
     use_transform 0: the file's own Node already has the world matrix, so the groom is moved once,
     not twice (VRayScene's parameters: Chaos docs "VRayScene",
     https://docs.chaos.com/display/VMAYA/VRayScene, and V-Ray's plgparams VRayScene);
  3. deletes any Node plugin V-Ray made from the groom shape itself (vray.utils.findByType("Node"),
     vray.utils.delete), so the groom is not drawn twice (V-Ray for Maya does not translate GroomLab's
     shape today; this keeps it so if it ever starts).
The .vrscene files go under the scene's groomlab/<scene>/caches/vray folder, one per groom and frame.
"""

import os
import sys

try:
    from maya import cmds
except ImportError:  # the pure helpers alone (tests/hosts/vray/test_vray_scene.py, without Maya)
    cmds = None

REQUIRED = "V-Ray for Maya 2024+ required"
EXPERIMENTAL_NOTE = ("experimental, untested: V-Ray for Maya supports Maya 2024 and later only; in any Maya, "
                     "GroomLab > Render > V-Ray Standalone renders the groom")
VRAY_PLUGIN = "vrayformaya"
SETTINGS = "vraySettings"
SCRIPT_ATTR = "postTranslatePython"
SCRIPT_LINE = "import groomlab.vray_hook; groomlab.vray_hook.post_translate()"
PLUGIN_SUFFIX = "@vrayScene"


class VRayRequired(RuntimeError):
    """V-Ray for Maya (on Maya 2024 or later) is not there."""


# ---------------------------------------------------------------------------
# Pure helpers (no Maya, no V-Ray)

def add_line(script):
    """The post-translate script with the hook's line added (once), the user's lines kept."""
    lines = (script or "").splitlines()
    if SCRIPT_LINE in (line.strip() for line in lines):
        return script or ""
    return "\n".join([line for line in lines] + [SCRIPT_LINE])


def remove_line(script):
    """The post-translate script without the hook's line."""
    return "\n".join(line for line in (script or "").splitlines() if line.strip() != SCRIPT_LINE)


def shutter_frames(frame, on=False, duration=1.0, center=0.5, samples=2):
    """(start, end, step) of the keys a frame's groom is written with: the frame alone without motion
    blur; with it, V-Ray's shutter interval (`duration` frames whose middle is `center` after the
    frame: 0.5 and 1 give frame to frame + 1) in `samples` keys (2 at least)."""
    frame = float(frame)
    if not on or duration <= 0.0:
        return frame, frame, 1.0
    start = frame + float(center) - 0.5 * float(duration)
    end = start + float(duration)
    keys = max(2, int(samples))
    return start, end, (end - start) / (keys - 1)


def frame_tag(frame):
    """A file-safe frame number: 12 -> "0012", 12.5 -> "0012_5", -3 -> "n0003"."""
    whole = int(abs(frame))
    text = "%04d" % whole
    rest = abs(frame) - whole
    if rest > 1e-6:
        text += "_" + ("%g" % rest)[2:]
    return ("n" if frame < 0 else "") + text


def vrayscene_name(groom_name):
    """The VRayScene plugin a groom gets: groomlab_<name>@vrayScene (a V-Ray plugin name)."""
    from groomlab.vray_scene import plugin_name
    return "groomlab_" + plugin_name(groom_name) + PLUGIN_SUFFIX


def translated_by_vray(plugin_names, groom):
    """The plugin names V-Ray for Maya may have made from the groom shape `groom` (a DAG path):
    names holding its shape's or its transform's short name as a whole word."""
    import re
    parts = [p for p in groom.split("|") if p]
    words = set(p.split(":")[-1] for p in parts[-2:])
    found = []
    for name in plugin_names:
        tokens = set(t for t in re.split(r"[^A-Za-z0-9_]+", name.split("@")[0]) if t)
        if tokens & words and not name.endswith(PLUGIN_SUFFIX):
            found.append(name)
    return found


def place_groom(utils, name, path, translated=()):
    """The VRayScene plugin `name` (made, or found again on a re-translation) pointing at `path`,
    without its own transform (the file's Node carries the groom's world matrix); the Node plugins
    in `translated` (V-Ray's own of the groom) deleted. `utils` is vray.utils (documented: create,
    findByName, findByType, delete, Plugin.set). Returns the plugin."""
    found = [p for p in utils.findByName(name) or [] if p.name() == name]
    plugin = found[0] if found else utils.create("VRayScene", name)
    if plugin is None:
        raise RuntimeError("V-Ray made no VRayScene plugin %s" % name)
    plugin.set("filepath", path.replace("\\", "/"))
    plugin.set("use_transform", False)
    plugin.set("add_lights", False)
    for node in utils.findByType("Node") or []:
        if node.name() in translated:
            utils.delete(node)
    return plugin


# ---------------------------------------------------------------------------
# Maya

def maya_version():
    """Maya's year (2022, 2024, ...)."""
    return int(cmds.about(apiVersion=True)) // 10000


def vray_loaded():
    try:
        return bool(cmds.pluginInfo(VRAY_PLUGIN, query=True, loaded=True))
    except RuntimeError:
        return False


def check():
    """Raise VRayRequired unless this is Maya 2024+ with V-Ray for Maya loaded."""
    if cmds is None:
        raise VRayRequired(REQUIRED + " (not in Maya)")
    version = maya_version()
    if version < 2024:
        raise VRayRequired("%s (this is Maya %d)" % (REQUIRED, version))
    if not vray_loaded():
        raise VRayRequired("%s: load %s in the Plug-in Manager" % (REQUIRED, VRAY_PLUGIN))


def _settings():
    """vraySettings (V-Ray makes it when it becomes the renderer; made here as V-Ray does otherwise)."""
    if not cmds.objExists(SETTINGS):
        cmds.createNode("VRaySettingsNode", name=SETTINGS, shared=True)
    if not cmds.attributeQuery(SCRIPT_ATTR, node=SETTINGS, exists=True):
        raise VRayRequired("%s: %s has no %s" % (REQUIRED, SETTINGS, SCRIPT_ATTR))
    return SETTINGS


def enable_hook():
    """Make V-Ray for Maya render the grooms at render time (post_translate). EXPERIMENTAL."""
    check()
    plug = _settings() + "." + SCRIPT_ATTR
    cmds.setAttr(plug, add_line(cmds.getAttr(plug)), type="string")
    return True


def disable_hook():
    """Take the hook's line out of V-Ray's post-translate script. Returns whether it was there."""
    if cmds is None or not cmds.objExists(SETTINGS) or not cmds.attributeQuery(SCRIPT_ATTR, node=SETTINGS, exists=True):
        return False
    plug = SETTINGS + "." + SCRIPT_ATTR
    script = cmds.getAttr(plug) or ""
    if SCRIPT_LINE not in script:
        return False
    cmds.setAttr(plug, remove_line(script), type="string")
    return True


def hook_enabled():
    if cmds is None or not cmds.objExists(SETTINGS) or not cmds.attributeQuery(SCRIPT_ATTR, node=SETTINGS, exists=True):
        return False
    return SCRIPT_LINE in (cmds.getAttr(SETTINGS + "." + SCRIPT_ATTR) or "")


def motion_blur():
    """V-Ray's camera motion blur from vraySettings: (on, duration, interval center, geometry samples)."""
    def get(attr, default):
        if cmds.objExists(SETTINGS) and cmds.attributeQuery(attr, node=SETTINGS, exists=True):
            return cmds.getAttr(SETTINGS + "." + attr)
        return default
    return (bool(get("cam_mbOn", False)), float(get("cam_mbDuration", 1.0)), float(get("cam_mbIntervalCenter", 0.5)),
            int(get("cam_mbGeomSamples", 2)))


# The translator's file as V-Ray for Maya looks for it: vraytr_*.dll on Windows, libvraytr_*.so on macOS.
TRANSLATOR = ("vraytr_groomlab.dll" if os.name == "nt"
              else "libvraytr_groomlab.so" if sys.platform == "darwin" else "vraytr_groomlab.so")


def translator_installed():
    """Whether V-Ray for Maya loads GroomLab's translator (vraytr_groomlab, on this Maya's
    VRAY_FOR_MAYA<year>_TRANSLATORS_PATH, as GroomLab.mod puts it): V-Ray then renders every groom
    itself, as it renders a mesh, and the hook has nothing to do."""
    if cmds is None:
        return False
    folders = os.environ.get("VRAY_FOR_MAYA%d_TRANSLATORS_PATH" % maya_version(), "")
    return any(f and os.path.isfile(os.path.join(f, TRANSLATOR)) for f in folders.split(os.pathsep))


def _vray_utils():
    try:
        import vray.utils as utils  # V-Ray for Maya's own module, there while it translates
    except ImportError:
        raise VRayRequired(REQUIRED + ": vray.utils is not available (run by V-Ray's post-translate script)")
    return utils


def post_translate(utils=None, grooms=None, frame=None, folder=None):
    """V-Ray's post-translate step: each visible groom as a VRayScene plugin of its .vrscene for
    this frame (the module's docstring). `utils` is vray.utils (tests pass a stand-in). Never
    raises into V-Ray's translation: a failure is printed and that groom left out. Returns the
    VRayScene plugin names made."""
    import groomlab
    from groomlab import vray_render
    if utils is None and translator_installed():
        return []  # GroomLab's translator renders the grooms: the hook would draw them twice
    try:
        utils = utils or _vray_utils()
    except VRayRequired as e:
        cmds.warning("GroomLab: %s" % e)
        return []
    frame = cmds.currentTime(query=True) if frame is None else float(frame)
    start, end, step = shutter_frames(frame, *motion_blur())
    folder = folder or os.path.join(groomlab.asset_folder("caches"), "vray")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    names = [p.name() for p in utils.findByType("Node") or []]
    made = []
    for groom in vray_render.render_grooms(grooms):
        try:
            asset = groomlab.asset_name(groom)
            path = os.path.join(folder, "%s.%s.vrscene" % (asset, frame_tag(frame))).replace("\\", "/")
            groomlab.export_vrscene(groom, path, start=start, end=end, step=step, render_overrides=True)
            plugin = place_groom(utils, vrayscene_name(asset), path, translated_by_vray(names, groom))
            made.append(plugin.name())
        except Exception as e:  # noqa: BLE001 (V-Ray's translation goes on without this groom)
            cmds.warning("GroomLab: V-Ray hook: %s left out: %s" % (groom, e))
    return made


# ---------------------------------------------------------------------------
# The fallback: a VRayScene node in Maya

def load_vrayscene(path, name=None):
    """A VRayScene node in Maya pointing at the .vrscene `path` (exported with groomlab.export_vrscene:
    its Node already has the groom's world matrix, so leave the new transform at the origin), with
    the playback time connected and a preview mesh, as V-Ray's own Create > VRayScene makes it (Chaos
    docs "VRayScene": FilePath, inputTime, outMesh). Returns the transform. EXPERIMENTAL."""
    check()
    if not os.path.isfile(path):
        raise ValueError("no file %s" % path)
    from groomlab.vray_scene import plugin_name
    base = "VRayScene_" + plugin_name(name or os.path.splitext(os.path.basename(path))[0])
    transform = cmds.createNode("transform", name=base + "Transform")
    node = cmds.createNode("VRayScene", name=base, parent=transform)
    cmds.setAttr(node + ".FilePath", path.replace("\\", "/"), type="string")
    if cmds.attributeQuery("inputTime", node=node, exists=True):
        cmds.connectAttr("time1.outTime", node + ".inputTime", force=True)
    if cmds.attributeQuery("outMesh", node=node, exists=True):
        mesh = cmds.createNode("mesh", name=base + "Shape", parent=transform)
        cmds.connectAttr(node + ".outMesh", mesh + ".inMesh", force=True)
        cmds.sets(mesh, edit=True, forceElement="initialShadingGroup")
    for attr in ("translate", "rotate", "scale"):
        cmds.setAttr(transform + "." + attr, lock=True)  # the file has the world matrix: moving it would add one
    return cmds.ls(transform, long=True)[0]


def export_and_load(groom, path=None, start=None, end=None):
    """The fallback in one step: the groom over the playback range (default) to a .vrscene in the
    scene's exports folder, then a VRayScene node pointing at it. Returns the transform."""
    import groomlab
    check()
    if path is None:
        path = os.path.join(groomlab.asset_folder("exports"), groomlab.asset_name(groom) + ".vrscene")
    start = cmds.playbackOptions(query=True, minTime=True) if start is None else start
    end = cmds.playbackOptions(query=True, maxTime=True) if end is None else end
    groomlab.export_vrscene(groom, path, start=start, end=end, render_overrides=True)
    return load_vrayscene(path, groomlab.asset_name(groom))
