# SPDX-License-Identifier: Apache-2.0
"""Review tools: a density heatmap, strands colored by a channel, a strand count HUD, a turntable
rig and its render.

    import groomlab
    groomlab.density_heatmap(groom)          # strands colored by root density
    groomlab.strand_color_sources(groom)     # the channels and color sets the strand colors can show
    groomlab.auto_strand_color_range(groom)  # the attribute colors' ramp over the channel's values
    groomlab.show_hud()                      # the active groom's counts in the viewport
    groomlab.turntable(groom, frames=120)    # an orbiting camera and a light rig
    job = groomlab.render_turntable(groom)   # rendered in the background, for dailies
"""

import json
import math

from maya import cmds

HUD = "groomLabHUD"
DENSITY_MODE = 5  # strandColorMode "density"
COLOR_SET_MODE, ATTRIBUTE_MODE, CLUMP_MODE = 1, 2, 4  # strandColorMode "colorSet", "attribute", "clump"


def density_heatmap(groom, on=True):
    """Color strands by how many grow per unit area around each root.

    Relative to the groom's median, on a log scale: blue at a quarter of it or
    less, green at the median, red at four times or more. Bald spots and
    bunched roots stand out. Off goes back to solid color.
    """
    cmds.setAttr(groom + ".strandColorMode", DENSITY_MODE if on else 0)


def strand_color_sources(groom):
    """{"channels", "colorSets"}: what the strand colors can read in the groom as shown (the viewed
    node's strands): the strands' channels (the attribute mode: clumpId, length, a painted map...)
    and the scalp's color sets (the color set mode, painted vertex maps among them), each sorted."""
    try:
        return json.loads(cmds.groomLabGraphStats(groom, strandColorSources=True))
    except (RuntimeError, ValueError):
        return {"channels": [], "colorSets": []}


def auto_strand_color_range(groom, source=None):
    """Sets strandColorMin and strandColorMax to the lowest and highest value of the strand channel
    `source` (the groom's strandColorSource) as shown: the attribute colors' ramp, blue to red, over
    every strand. Returns (lo, hi); ValueError when the strands have no such channel."""
    source = source or cmds.getAttr(groom + ".strandColorSource")
    lo_hi = cmds.groomLabGraphStats(groom, channelRange=source) or []
    if len(lo_hi) != 2:
        raise ValueError("The strands have no channel %r" % source)
    cmds.setAttr(groom + ".strandColorMin", lo_hi[0])
    cmds.setAttr(groom + ".strandColorMax", lo_hi[1])
    return tuple(lo_hi)


def groom_stats(groom):
    """{"strands", "points", "guides", "render_strands", "ms"} of a groom as the viewport shows it."""
    stats = json.loads(cmds.groomLabGraphStats(groom))
    preview = max(cmds.getAttr(groom + ".previewDensity"), 1e-6)
    return {
        "strands": stats.get("strands", 0),
        "points": stats.get("points", 0),
        "guides": stats.get("guides", 0),
        "render_strands": int(round(stats.get("strands", 0) / preview)),
        "ms": sum(stats.get("times", {}).values()),
    }


_hud_cache = [0.0, None, ""]  # time, groom, text


def hud_text():
    """The HUD line for the active groom (refreshed at most twice a second: it runs on every redraw)."""
    import time
    import groomlab
    groom = groomlab.active_groom()
    now = time.time()
    if groom == _hud_cache[1] and now - _hud_cache[0] < 0.5:
        return _hud_cache[2]
    if not groom:
        text = "no groom"
    else:
        s = groom_stats(groom)
        text = "%s: %s strands (%s at render) | %s guides | %.0f ms" % (
            groom.split("|")[-1], format(s["strands"], ","), format(s["render_strands"], ","),
            format(s["guides"], ","), s["ms"])
    _hud_cache[:] = [now, groom, text]
    return text


def show_hud(on=True):
    """The active groom's strand, render strand and guide counts and evaluation time, top left."""
    if cmds.headsUpDisplay(HUD, exists=True):
        cmds.headsUpDisplay(HUD, remove=True)
    if not on:
        return None
    block = cmds.headsUpDisplay(nextFreeBlock=0)
    cmds.headsUpDisplay(HUD, section=0, block=block, blockSize="small", label="GroomLab",
                        labelFontSize="small", dataFontSize="small",
                        command="import groomlab.review as r; r.hud_text()", attachToRefresh=True)
    return HUD


def hud_visible():
    return cmds.headsUpDisplay(HUD, exists=True)


def _bounds(groom):
    box = cmds.exactWorldBoundingBox(cmds.listRelatives(groom, parent=True, fullPath=True)[0])
    center = [(box[i] + box[i + 3]) * 0.5 for i in range(3)]
    radius = 0.5 * math.sqrt(sum((box[i + 3] - box[i]) ** 2 for i in range(3)))
    return center, max(radius, 1e-3)


def turntable(groom, frames=120, start=1, elevation=10.0, lights=True, name="groomTurntable"):
    """A camera orbiting the groom once over `frames`, and a key, fill and rim light rig.

    The camera frames the groom's bounds and is made renderable; the lights stay
    put, so highlights travel over the hair as the camera goes round. Sets the
    playback and render range. Returns {"camera", "orbit", "lights"}.
    """
    center, radius = _bounds(groom)
    focal = 50.0
    fov = 2.0 * math.atan(0.5 * 36.0 / focal)  # horizontal, 36 mm film back
    distance = radius / math.sin(0.5 * fov) * 1.1

    orbit = cmds.group(empty=True, name=name + "_orbit")
    cmds.xform(orbit, worldSpace=True, translation=center)
    camera, camera_shape = cmds.camera(name=name + "_cam", focalLength=focal)
    camera = cmds.parent(camera, orbit)[0]
    tilt = math.radians(elevation)
    cmds.xform(camera, objectSpace=True, translation=(0.0, distance * math.sin(tilt), distance * math.cos(tilt)))
    cmds.xform(camera, objectSpace=True, rotation=(-elevation, 0.0, 0.0))
    cmds.setAttr(camera_shape + ".renderable", True)
    cmds.setAttr(camera_shape + ".nearClipPlane", distance * 0.01)
    cmds.setAttr(camera_shape + ".farClipPlane", distance * 10.0)

    end = start + frames
    cmds.setKeyframe(orbit, attribute="rotateY", time=start, value=0.0, inTangentType="linear", outTangentType="linear")
    cmds.setKeyframe(orbit, attribute="rotateY", time=end, value=360.0, inTangentType="linear", outTangentType="linear")
    cmds.playbackOptions(minTime=start, maxTime=end - 1)
    cmds.setAttr("defaultRenderGlobals.startFrame", start)
    cmds.setAttr("defaultRenderGlobals.endFrame", end - 1)

    rig = []
    if lights:
        arnold = cmds.pluginInfo("mtoa", query=True, loaded=True)
        rig_group = cmds.group(empty=True, name=name + "_lights")
        # (name, rotate x, rotate y, intensity): key high front right, fill low left, rim behind.
        for label, rx, ry, intensity in (("key", -35.0, 40.0, 1.0), ("fill", -10.0, -60.0, 0.35),
                                         ("rim", -25.0, 170.0, 1.2)):
            light = cmds.directionalLight(name=name + "_" + label, intensity=intensity)
            xform = cmds.listRelatives(light, parent=True)[0]
            xform = cmds.parent(xform, rig_group)[0]
            cmds.xform(xform, worldSpace=True, rotation=(rx, ry, 0.0))
            if arnold and cmds.attributeQuery("aiAngle", node=light, exists=True):
                cmds.setAttr(light + ".aiAngle", 2.0)  # soft shadows
            rig.append(xform)
    return {"camera": camera, "orbit": orbit, "lights": rig}


# ---------------------------------------------------------------------------
# Turntable renders (dailies)

TURNTABLE = "groomTurntable"
QUALITY = {"preview": 3, "final": 6}  # Arnold camera (AA) samples


def _turntable_camera(groom, frames):
    """The turntable rig's camera shape: the scene's, else a new rig round the groom."""
    existing = cmds.ls(TURNTABLE + "_cam*", type="transform") or []
    if existing:
        return cmds.listRelatives(existing[0], shapes=True, fullPath=True)[0]
    rig = turntable(groom, frames=frames, name=TURNTABLE)
    return cmds.listRelatives(rig["camera"], shapes=True, fullPath=True)[0]


def _has_own_shader(groom):
    groups = cmds.listConnections(groom + ".instObjGroups[0]", type="shadingEngine", source=False) or []
    return any(g != "initialShadingGroup" for g in groups)


def turntable_command(scene, camera, folder, start, end, step=1, width=960, height=540, quality="preview",
                      project=None):
    """The Render command (a list) that renders a scene's turntable camera to PNGs in `folder`."""
    import os
    render = os.path.join(os.environ.get("MAYA_LOCATION", ""), "bin", "Render.exe" if os.name == "nt" else "Render")
    command = [render, "-r", "arnold", "-s", str(start), "-e", str(end), "-b", str(step), "-rd", folder,
               "-im", "turntable", "-of", "png", "-fnc", "name.#.ext", "-pad", "4", "-x", str(int(width)),
               "-y", str(int(height)), "-cam", camera, "-ai:as", str(QUALITY.get(quality, quality)), "-ai:lve", "1"]
    if project:
        command += ["-proj", project]
    return command + [scene]


class TurntableJob(object):
    """A turntable rendering in the background (Maya stays free): poll() for progress; when it is
    done the frames are PNGs in `folder` and, with ffmpeg on the PATH, turntable.mp4 beside them
    (encoded in the background too)."""

    NO_WINDOW = 0x08000000  # Windows: no console window for the child processes

    def __init__(self, command, folder, frames, fps=24):
        import os
        import subprocess
        self.folder, self.frames, self.fps = folder, frames, fps
        self.log = os.path.join(folder, "render.log")
        with open(self.log, "w") as out:
            self.process = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, creationflags=self._flags())
        self.encoder = None  # ffmpeg, once the frames are done
        self.movie = None    # the mp4's path once encoded, "" when there is none

    @classmethod
    def _flags(cls):
        import os
        return cls.NO_WINDOW if os.name == "nt" else 0

    def rendered(self):
        import glob
        import os
        return sorted(glob.glob(os.path.join(self.folder, "turntable.*.png")))

    def poll(self):
        """(frames written, frames asked, finished): finished once the render and the encode are."""
        if self.process.poll() is not None and self.movie is None:
            if self.encoder is None:
                self.encoder = self._start_encode() or False
            if self.encoder is False:
                self.movie = ""
            elif self.encoder.poll() is not None:
                movie = self._movie_path()
                import os
                self.movie = movie if self.encoder.returncode == 0 and os.path.exists(movie) else ""
        return len(self.rendered()), self.frames, self.movie is not None

    def ok(self):
        return self.process.poll() == 0 and len(self.rendered()) >= self.frames

    def cancel(self):
        """Stop the render and anything it started (Render hands the work to a child process)."""
        import os
        import subprocess
        for proc in (self.process, self.encoder or None):
            if proc is None or proc.poll() is not None:
                continue
            if os.name == "nt":
                subprocess.call(["taskkill", "/F", "/T", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, creationflags=self._flags())
            else:
                proc.kill()
        self.movie = self.movie or ""

    def wait(self, timeout=None):
        """Block until the render and the encode are done, or `timeout` seconds pass; returns poll()."""
        import subprocess
        import time
        end = None if timeout is None else time.time() + timeout
        try:
            self.process.wait(timeout)
        except subprocess.TimeoutExpired:
            return self.poll()
        while not self.poll()[2]:
            if end is not None and time.time() > end:
                break
            time.sleep(0.2)
        return self.poll()

    def _movie_path(self):
        import os
        return os.path.join(self.folder, "turntable.mp4")

    def _start_encode(self):
        """ffmpeg making turntable.mp4 from the frames, when ffmpeg is on the PATH."""
        import os
        import shutil
        import subprocess
        ffmpeg = shutil.which("ffmpeg")
        frames = self.rendered()
        if not ffmpeg or not frames:
            return None
        first = int(os.path.basename(frames[0]).split(".")[1])
        return subprocess.Popen([ffmpeg, "-y", "-loglevel", "error", "-framerate", str(self.fps),
                                 "-start_number", str(first), "-i", os.path.join(self.folder, "turntable.%04d.png"),
                                 "-pix_fmt", "yuv420p", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", self._movie_path()],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=self._flags())


def render_turntable(groom, frames=120, step=1, width=960, height=540, quality="preview", save=True):
    """Render the groom going round once, in Arnold, in the background: for dailies.

    Uses the scene's turntable rig (review.turntable), making one if there is none, gives the
    groom a hair shader when it has none, saves the scene (the render reads the saved file;
    `save` False renders the file as last saved) and starts Render on it. Frames go to the scene's
    groomlab/<scene>/renders/turntable_<date> folder. Returns a TurntableJob."""
    import datetime
    import os
    import groomlab
    if not cmds.pluginInfo("mtoa", query=True, loaded=True):
        cmds.loadPlugin("mtoa")
    scene = cmds.file(query=True, sceneName=True)
    if not scene:  # before anything changes in the scene
        raise ValueError("save the scene first: the turntable renders the saved file")
    import mtoa.core
    mtoa.core.createOptions()
    camera = cmds.listRelatives(_turntable_camera(groom, frames), parent=True)[0]
    if not _has_own_shader(groom):
        groomlab.create_hair_shader(groom)
    if save:
        cmds.file(save=True)
    start = int(cmds.playbackOptions(query=True, minTime=True))
    end = start + int(frames) - 1
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    folder = os.path.join(groomlab.asset_folder("renders"), "turntable_" + stamp).replace("\\", "/")
    os.makedirs(folder)
    command = turntable_command(scene, camera, folder, start, end, step, width, height, quality,
                                project=cmds.workspace(query=True, rootDirectory=True))
    return TurntableJob(command, folder, len(range(start, end + 1, max(1, int(step)))))
