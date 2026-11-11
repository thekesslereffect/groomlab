# SPDX-License-Identifier: Apache-2.0
"""Render the groom with Maya's Hardware 2.0 renderer: nothing to install, any Maya (2022 to 2027).

    import groomlab
    job = groomlab.render_hardware2()                    # this frame, the studio's (or the view's) camera
    job = groomlab.render_hardware2(start=1, end=120)    # a turntable or a shot: one image a frame
    job.images()                                         # the PNGs, in groomlab/<scene>/renders/hw2_<date>

Hardware 2.0 draws the scene as Viewport 2.0 does (the groom through GroomLab's draw override) into
an image (Maya's ogsRender), here in this Maya session, one frame at a time: seconds a frame, for
quick lookdev and turntables. For the render alone (all put back after the last frame, or a cancel,
outside the undo queue, the scene's modified flag as it was):
  - quality: multisample anti-aliasing (QUALITY: 4, 8 or 16 samples a pixel; fewer where the
    graphics card has fewer), the scene's lights (Hardware 2.0's lighting mode "All"; Maya's
    default light when the scene has none), and the images as the viewport shows them (the color
    management's view transform applied to them);
  - each groom drawn as a render shows it (its renderView: renderDensity with no point budget,
    renderOnly nodes on, renderWidthScale and renderLengthScale), with GroomLab's lit hair shading,
    width as coverage and its self-shadowing; not in Maya's shadow maps (GROOM_DISPLAY); guides,
    flow, maps and the viewport's level of detail off; other grooms (when `grooms` names some) hidden;
  - Arnold's area lights, which Hardware 2.0 takes as dim lights of their intensity alone, stand in
    as Maya lights as bright as Arnold's on what they face (mirror_light; light links kept), the
    Arnold ones hidden meanwhile; a sky dome lights as it is (Viewport 2.0 takes it as an ambient
    light). A studio's backdrop and mannequin materials draw as Lambert ones of their color
    (_plain_studio_surfaces). So a studio renders much as Arnold lights it;
  - the background: what the camera sees (a studio's backdrop), else nothing: the PNG's alpha is 0
    there (the Render View shows the camera's background color behind it).
The images land in <scene's groomlab folder>/renders/hw2_<date> (or `folder`), the last one in
Maya's Render View. Hardware 2.0 shows what the viewport can: no light bouncing, the hair's own
shading (GroomLab's hair, fitted to Arnold's), and hard shadow maps of the meshes instead of soft
traced shadows.
"""

import math
import os
import time

from maya import cmds

from groomlab import render_common, render_gather

# Multisample anti-aliasing for each quality (hardwareRenderingGlobals.multiSampleCount).
QUALITY = {"draft": 4, "preview": 8, "final": 16}
LIGHTS_GROUP = "groomlab_hw2_lights"
# The lights Viewport 2.0 lights with: Maya's, and Arnold's sky dome (as an ambient light).
VIEWPORT_LIGHT_TYPES = ("ambientLight", "directionalLight", "pointLight", "spotLight", "areaLight", "volumeLight",
                        "aiSkyDomeLight")

# What a groom shows while it renders (its own values put back after).
GROOM_DISPLAY = (
    ("renderView", True),              # renderDensity, renderOnly nodes, the render's width and length scales
    ("displayHair", True),             # the hair hidden in the viewport still renders
    ("displayGuides", False), ("displayFlow", False), ("displayTubes", False), ("scalpMapDisplay", ""),
    ("isolateGroup", ""), ("displayCards", False),
    ("displayClumpLod", False),        # renders never thin
    ("strandColorMode", 0),            # solid: the lit hair
    ("displayLighting", True), ("displayHairShading", 1), ("displayWidthCoverage", True), ("displayRibbons", False),
    ("displaySelfShadowing", 1.0),     # its own shadows and occlusion, as rendered
    # Not in Maya's shadow maps: a strand there is an opaque line a texel wide, and fine hair
    # (a face's fuzz) would shade the skin under it black.
    ("displayShadows", 0),
)
RENDER_GLOBALS = (
    ("hardwareRenderingGlobals.renderMode", 4),       # shaded and textured
    ("hardwareRenderingGlobals.multiSampleEnable", True),
    ("defaultRenderGlobals.imageFormat", 32),          # PNG
    ("defaultRenderGlobals.animation", False),         # the image named as asked: <prefix>.png
    ("defaultRenderGlobals.outFormatControl", 0),
)


# ---------------------------------------------------------------------------
# Lights

def mirror_light(entry, matrix):
    """The Maya light standing in for an Arnold area light (aiAreaLight) in Hardware 2.0, as
    {"type", "intensity", "color", "matrix", ...} (render_gather.light_entry's `entry`, its world
    `matrix`), or None for a light Viewport 2.0 takes as it is (a sky dome: an ambient light of its
    color, as bright as Arnold's dome lights an open surface; measured: Maya 2022, MtoA 5).

    The quad stands in as a spot light where it is, aimed as it faces, over the half-space in front
    of it (a 179 degree cone), decaying with the square of the distance, dimming off its axis as
    the quad's light does (dropoff 1: by the cosine; Arnold's spread below 1 narrows it: dropoff 1
    / spread), `intensity` the quad's radiance x its area / pi: what it faces gets the irradiance the
    quad gives it in Arnold, over pi (Viewport 2.0 shows pi x a render's radiance: a Lambert there
    shows its color x the light, as bright as Arnold's Lambert). Not a Maya area light: Viewport 2.0
    shades surfaces under one by the fourth power of the distance (measured: Maya 2022), where
    GroomLab's hair shading takes it as a point; a spot light both shade alike."""
    if entry["kind"] != "rect":
        return None
    rgb = tuple(float(c) for c in entry.get("color", (1.0, 1.0, 1.0)))
    intensity = float(entry.get("intensity", 1.0))
    unit, sizes = render_common.unscaled(matrix)
    area = 4.0 * sizes[0] * sizes[1]  # the quad spans -1..1 along its x and y
    if entry.get("normalize"):
        area = 1.0  # a normalized light's radiance is its power over its area
    # Arnold's spread focuses the quad's light keeping its power: as cos^n with n = 2 / spread - 1
    # (1 at spread 1: the cosine), (n + 1) / 2 = 1 / spread times as bright along its axis.
    spread = min(max(float(entry.get("spread", 1.0)), 0.05), 1.0)
    return {"type": "spotLight", "intensity": intensity * area / math.pi / spread, "color": rgb, "matrix": unit,
            "decayRate": 2, "coneAngle": 179.0, "penumbraAngle": 0.0, "dropoff": 2.0 / spread - 1.0}


def _arnold_lights():
    return [s for s in cmds.ls(type="aiAreaLight", long=True) or [] if render_gather.visible(s)]


def _viewport_lights():
    return [s for s in cmds.ls(type=list(VIEWPORT_LIGHT_TYPES), long=True) or [] if render_gather.visible(s)]


def _make_light(spec, name):
    """A Maya light of `spec` (mirror_light's) under LIGHTS_GROUP; returns its shape."""
    if not cmds.objExists(LIGHTS_GROUP):
        cmds.group(empty=True, name=LIGHTS_GROUP, world=True)
    shape = cmds.shadingNode(spec["type"], asLight=True, name=name)
    if cmds.nodeType(shape) == "transform":
        shape = cmds.listRelatives(shape, shapes=True, fullPath=True)[0]
    transform = cmds.listRelatives(shape, parent=True, fullPath=True)[0]
    transform = cmds.parent(transform, LIGHTS_GROUP)[0]
    shape = cmds.listRelatives(transform, shapes=True, fullPath=True)[0]
    if spec.get("matrix"):
        cmds.xform(transform, worldSpace=True, matrix=spec["matrix"])
    cmds.setAttr(shape + ".intensity", spec["intensity"])
    cmds.setAttr(shape + ".color", *spec["color"], type="double3")
    if spec["type"] == "ambientLight":
        cmds.setAttr(shape + ".ambientShade", 0.0)  # light from everywhere
    for attr in ("decayRate", "coneAngle", "penumbraAngle", "dropoff"):
        if attr in spec:
            cmds.setAttr(shape + "." + attr, spec[attr])
    return shape


def _mirror_arnold_lights(meshes, saved):
    """Maya lights for the scene's Arnold area lights (mirror_light), their light links copied onto
    `meshes`, the Arnold lights hidden meanwhile (`saved`: Viewport 2.0 takes them as weak area
    lights of their intensity alone); returns the stand-ins' shapes."""
    made = []
    names = render_gather.unique_names(_arnold_lights())
    for source, name in names.items():
        entry = render_gather.light_entry(source, name)
        spec = mirror_light(entry, render_gather.world_matrix(source)) if entry else None
        if not spec:
            continue
        shape = _make_light(spec, "groomlab_hw2_" + name)
        for mesh in render_gather._linked_meshes(source, meshes):
            cmds.lightlink(b=True, light=shape, object=mesh)
        saved.set(source + ".visibility", False)
        made.append(shape)
    return made


def _plain_studio_surfaces():
    """The studio's backdrop and mannequin materials (aiStandardSurface) as Lambert ones of their
    color while Hardware 2.0 renders: Viewport 2.0 shades an aiStandardSurface two to four times
    brighter than Arnold renders it (and not in proportion to its color; measured: Maya 2022, MtoA
    5), its Lambert as Arnold does. Returns [(shading group, the material, the Lambert)]."""
    from groomlab import studio
    swapped = []
    for material in (studio.BACKDROP_MATERIAL, studio.MANNEQUIN_MATERIAL):
        group = material + "SG"
        if not cmds.objExists(material) or not cmds.objExists(group):
            continue
        if (cmds.listConnections(group + ".surfaceShader", source=True, destination=False) or [None])[0] != material:
            continue
        base = float(render_gather.get(material, "base", 1.0))
        color = [base * float(c) for c in render_gather.get(material, "baseColor", (0.5, 0.5, 0.5))]
        lambert = cmds.shadingNode("lambert", asShader=True, name="groomlab_hw2_" + material)
        cmds.setAttr(lambert + ".color", *color, type="double3")
        cmds.setAttr(lambert + ".diffuse", 1.0)
        cmds.connectAttr(lambert + ".outColor", group + ".surfaceShader", force=True)
        swapped.append((group, material, lambert))
    return swapped


# ---------------------------------------------------------------------------
# The render's settings, and putting them back

class _Saved(object):
    """Attribute values a render changed, put back in reverse order."""

    def __init__(self):
        self.values = []  # (plug, value)

    def set(self, plug, value):
        node, attr = plug.split(".", 1)
        if not cmds.objExists(node) or not cmds.attributeQuery(attr, node=node, exists=True):
            return False
        if cmds.getAttr(plug, lock=True) or cmds.listConnections(plug, source=True, destination=False):
            return False
        before = cmds.getAttr(plug)
        if before is None and isinstance(value, str):
            before = ""  # (an empty string attribute reads as None)
        if before == value:
            return True
        try:
            _set_plug(plug, value)
        except RuntimeError:  # (a child plug connected, locked)
            return False
        self.values.append((plug, before))
        return True

    def restore(self):
        for plug, value in reversed(self.values):
            try:
                _set_plug(plug, value)
            except (RuntimeError, ValueError):
                pass
        self.values = []


def _set_plug(plug, value):
    if isinstance(value, str):
        cmds.setAttr(plug, value, type="string")
    elif isinstance(value, (list, tuple)):
        values = value[0] if value and isinstance(value[0], (list, tuple)) else value  # (getAttr's [(r, g, b)])
        cmds.setAttr(plug, *values, type="double3")
    else:
        cmds.setAttr(plug, value)


def _NoUndo():
    """Commands kept out of the undo queue (the render's changes and their undoing)."""
    from groomlab.base import no_undo
    return no_undo()


# ---------------------------------------------------------------------------
# The render

class Hardware2Job(object):
    """A Hardware 2.0 render in this Maya session, frame by frame: poll() renders the next frame
    (GroomLab > Render's progress window calls it as often as Maya is idle) and says how far it
    is, as a background renderer's job does (render_common.RenderJob): (frames done, frames asked,
    finished, percent of the current frame). cancel() stops it between frames; wait() renders the
    rest. Starting it sets the scene up for the render (the module's docstring); the last frame, a
    cancel or an error puts everything back."""

    name = "Maya Hardware 2.0"
    poll_interval = 0  # (ms) the progress window polls again as soon as Maya is idle

    def __init__(self, frames, folder, image, camera, width, height, quality="preview", grooms=None, show=True):
        self.frames, self.folder, self.image = list(frames), folder, image
        self.camera, self.width, self.height = camera, int(width), int(height)
        self.samples = QUALITY[quality]
        self.grooms = render_gather.render_grooms(grooms)
        self.show = show
        self.log = os.path.join(folder, "render.log")
        self.cancelled = False
        self.failed = []
        self.started = time.time()
        self.seconds = []  # each frame's
        self.lights = []   # the stand-in lights made
        self.strands = {}  # each groom's strands drawn (at its render density)
        self._next = 0
        self._active = False
        self._log("Maya Hardware 2.0: %d frame(s) %s, %dx%d, %d samples, camera %s, grooms %s"
                  % (len(self.frames), self.frames[:1] + self.frames[-1:], self.width, self.height, self.samples,
                     camera, ", ".join(g.split("|")[-1] for g in self.grooms) or "none"), "w")
        render_common.clear_images(image, self.frames)
        self._begin()

    # -- The protocol of the render jobs ---------------------------------------

    def images(self):
        return [p for p in render_common.images_for(self.image, self.frames) if os.path.isfile(p)]

    def poll(self):
        if not self.finished():
            self._render_next()
        count = len(self.images())
        return count, len(self.frames), self.finished(), 100.0 if self.finished() else 0.0

    def finished(self):
        return self.cancelled or bool(self.failed) or self._next >= len(self.frames)

    def ok(self):
        return not self.cancelled and not self.failed and len(self.images()) == len(self.frames)

    def errors(self):
        return self.failed[-5:]

    def cancel(self):
        self.cancelled = True
        self._end()

    def wait(self, timeout=None):
        """Render the frames left (all of them, or as many as `timeout` seconds take); returns poll()."""
        until = None if timeout is None else time.time() + timeout
        while not self.finished() and (until is None or time.time() < until):
            self._render_next()
        return self.poll()

    # -- Setting up and putting back ---------------------------------------

    def _log(self, text, mode="a"):
        try:
            with open(self.log, mode) as f:
                f.write(text + "\n")
        except (IOError, OSError):
            pass

    def _begin(self):
        self._saved = _Saved()
        self._modified = cmds.file(query=True, modified=True)
        self._time = cmds.currentTime(query=True)
        self._made = []
        self._swapped = []  # (shading group, material, Lambert): _plain_studio_surfaces
        self._color = None  # the color management's output settings before
        self._active = True
        try:
            with _NoUndo():
                self._setup()
        except Exception:
            self._end()
            raise

    def _setup(self):
        saved = self._saved
        for plug, value in RENDER_GLOBALS:
            saved.set(plug, value)
        saved.set("hardwareRenderingGlobals.multiSampleCount", self.samples)
        # Each image written to the render's folder as <folder>/hw2_frame.png (named after).
        saved.set("defaultRenderGlobals.imageFilePrefix", self.folder.replace("\\", "/") + "/hw2_frame")
        # The images as the viewport shows them (its view transform: sRGB, ACES, ...), not the
        # rendering space's linear values.
        if cmds.colorManagementPrefs(query=True, cmEnabled=True):
            self._color = {flag: cmds.colorManagementPrefs(query=True, **{flag: True})
                           for flag in ("outputTransformEnabled", "outputUseViewTransform")}
            cmds.colorManagementPrefs(edit=True, outputTransformEnabled=True)
            cmds.colorManagementPrefs(edit=True, outputUseViewTransform=True)
        for groom in self.grooms:
            for attr, value in GROOM_DISPLAY:
                saved.set(groom + "." + attr, value)
            self.strands[groom] = cmds.getAttr(groom + ".outStrandCount")  # (evaluated as the render draws it)
        self._log("strands: %s" % ", ".join("%s %d" % (g.split("|")[-1], n) for g, n in self.strands.items()))
        import groomlab
        for other in cmds.ls(type=groomlab.GROOM_TYPE, long=True) or []:
            if other not in self.grooms and render_gather.visible(other):
                saved.set(other + ".visibility", False)
        self._swapped = _plain_studio_surfaces()
        # The lights: Arnold's area lights stood in for by Maya lights; the scene's lights, else
        # Maya's default light.
        meshes = render_gather.render_meshes(self.grooms, "visible")
        self.lights = _mirror_arnold_lights(meshes, saved)
        if self.lights:
            self._made.append(LIGHTS_GROUP)
        others = [s for s in _viewport_lights() if not s.startswith("|" + LIGHTS_GROUP + "|")]
        lit = bool(self.lights or others)
        saved.set("hardwareRenderingGlobals.lightingMode", 1 if lit else 0)  # All, else Default
        self._log("lights: %s" % (", ".join(s.split("|")[-1] for s in self.lights + others) or "Maya's default light"))

    def _end(self):
        """Put the scene back as it was (once)."""
        if not self._active:
            return
        self._active = False
        with _NoUndo():
            for group, material, lambert in self._swapped:
                if cmds.objExists(material) and cmds.objExists(group):
                    cmds.connectAttr(material + ".outColor", group + ".surfaceShader", force=True)
                self._made.append(lambert)
            self._swapped = []
            for node in self._made:
                if cmds.objExists(node):
                    cmds.delete(node)
            self._made = []
            self._saved.restore()
            for flag, value in (self._color or {}).items():
                cmds.colorManagementPrefs(edit=True, **{flag: value})
            self._color = None
            if cmds.currentTime(query=True) != self._time:
                cmds.currentTime(self._time, update=True)
        if not self._modified:
            cmds.file(modified=False)
        total = time.time() - self.started
        self._log("%s: %d of %d frame(s) in %.1f s%s" % ("cancelled" if self.cancelled else "failed" if self.failed else "done",
                                                      len(self.images()), len(self.frames), total,
                                                      "; " + "; ".join(self.failed) if self.failed else ""))
        if self.show and self.ok():
            render_gather.show_image(self.images()[-1])

    # -- A frame ---------------------------------------------------------------

    def _ogs_render(self, width, height):
        return cmds.ogsRender(camera=self.camera, width=width, height=height, currentFrame=True, noRenderView=True,
                              enableMultisample=True)

    def _render_next(self):
        frame = self.frames[self._next]
        target = render_common.images_for(self.image, self.frames)[self._next]
        started = time.time()
        try:
            with _NoUndo():
                if cmds.currentTime(query=True) != frame:
                    cmds.currentTime(frame, update=True)
                # A first, small draw: the groom's strands built at the render's density and its
                # self-shadowing grid written from them (the grid settles in the draw after the
                # strands are first drawn), then the image.
                self._ogs_render(max(16, self.width // 8), max(16, self.height // 8))
                written = self._ogs_render(self.width, self.height)
            if not written or not os.path.isfile(written):
                raise RuntimeError("Hardware 2.0 wrote no image for frame %g" % frame)
            if os.path.normcase(os.path.abspath(written)) != os.path.normcase(os.path.abspath(target)):
                if os.path.isfile(target):
                    os.remove(target)
                os.rename(written, target)
        except Exception as e:  # (an error ends the render; the scene goes back as it was)
            self.failed.append("frame %g: %s" % (frame, str(e).strip()))
            self._log("error: " + self.failed[-1])
            self._end()
            return
        self.seconds.append(time.time() - started)
        self._log("frame %g: %s (%.2f s)" % (frame, target, self.seconds[-1]))
        self._next += 1
        if self.finished():
            self._end()


def render_hardware2(grooms=None, camera=None, start=None, end=None, step=1, width=None, height=None,
                     quality="preview", folder=None, name="hw2", wait=True, show=True):
    """Render with Maya's Hardware 2.0 in this session (the module's docstring). Frames: `start`
    (default the current frame) to `end` by `step`; `width`, `height` default the render settings'
    resolution; `quality` "draft", "preview" or "final" (4, 8 or 16 samples a pixel); `camera`
    default the studio's, else the view's, else a renderable one; `grooms` default every visible
    one (others named: the rest hidden). `folder` default <scene's groomlab folder>/renders/<name>_<date>.
    Renders every frame before returning (wait=False: none yet; poll() renders the next one).
    Returns the Hardware2Job (images(), ok(), errors(), seconds: each frame's)."""
    if quality not in QUALITY:
        raise ValueError("quality is one of %s" % ", ".join(sorted(QUALITY)))
    now = cmds.currentTime(query=True)
    start = now if start is None else start
    frames = render_common.frame_list(start, end if end is not None else start, step)
    if not frames:
        raise ValueError("no frames from %s to %s" % (start, end))
    width, height = (int(width or render_gather.resolution()[0]), int(height or render_gather.resolution()[1]))
    out = render_gather.output_folder(folder, name)
    job = Hardware2Job(frames, out, os.path.join(out, name + ".png").replace("\\", "/"), render_gather.render_camera(camera),
                       width, height, quality, grooms, show)
    if wait:
        job.wait()
    return job
