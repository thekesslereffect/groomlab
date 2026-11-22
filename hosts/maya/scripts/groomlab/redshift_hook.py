# SPDX-License-Identifier: Apache-2.0
"""Redshift for Maya (issue 81): every visible groom renders in Redshift with nothing to set up.

Redshift has no public API for a renderer plug-in of GroomLab's own (its translator extensions and
procedurals are Maxon's), so GroomLab drives a Redshift Proxy per groom, made and kept for you:

    import groomlab.redshift_hook as rs
    rs.enable()      # the hook on: Redshift's Pre Render Frame MEL (redshiftOptions.preRenderFrameMel)
    rs.disable()     # the line taken out again (the rest of the script kept); the proxies stay
    rs.refresh()     # this frame written and the proxies set now (IPR, which runs no frame script)

pre_render_frame runs in Redshift alone (a Redshift Render Settings slot), before each frame of a
render (Render View, batch, a farm with GroomLab installed). For each visible groom it:
  1. writes the groom for the frame to Alembic (groomlab.export_alembic: the render density and
     overrides; with Redshift's deformation motion blur on, its shutter in its deformation steps:
     shutter_frames) under the scene's groomlab/<scene>/caches/redshift folder;
  2. finds or makes the groom's proxy (ensure_proxy): a RedshiftProxyMesh reading that file as
     Alembic (proxyFileType 2) into a placeholder mesh under the groom's transform, so it moves,
     hides and goes with the groom (the Alembic's points are the groom's own, as the transform
     places them);
  3. gives the placeholder a Principled Hair built from the groom's look (build_shader: the core's
     principledHair, groomLabGraphStats -hairLook's "principledHair"), unless it has a material of
     its own.
The placeholders render in Redshift only: sync_visibility shows them while the scene's renderer is
Redshift (defaultRenderGlobals.currentRenderer) and hides them otherwise, so Arnold, V-Ray or
RenderMan never draw them (GroomLab renders there itself).

Node and attribute names are Redshift 2026's (redshiftProxy.mel's redshiftProxyDoCreate;
AERedshiftProxyMeshTemplate, AERedshiftPrincipledHairTemplate, redshiftCreateMotionBlurTab.mel).
"""

import os

try:
    from maya import cmds, mel
except ImportError:  # the pure helpers alone (tests without Maya)
    cmds = mel = None

REDSHIFT_PLUGIN = "redshift4maya"
OPTIONS = "redshiftOptions"
SCRIPT_ATTR = "preRenderFrameMel"
SCRIPT_LINE = 'python("import groomlab.redshift_hook; groomlab.redshift_hook.pre_render_frame()");'
PROXY_TAG = "groomLabRedshiftProxy"  # string attr on the proxy node: the groom shape it renders
ALEMBIC = 2  # RedshiftProxyMesh.proxyFileType: RSProxyMesh 0, USD 1, Alembic 2
BOUNDING_BOX = 0  # RedshiftProxyMesh.displayMode: the viewport shows a box (the groom draws itself)


class RedshiftRequired(RuntimeError):
    """Redshift for Maya is not loaded."""


# ---------------------------------------------------------------------------
# Pure helpers (no Maya)

def add_line(script):
    """Redshift's Pre Render Frame MEL with the hook's line added (once), the user's lines kept."""
    lines = (script or "").splitlines()
    if SCRIPT_LINE in (line.strip() for line in lines):
        return script or ""
    return "\n".join(lines + [SCRIPT_LINE])


def remove_line(script):
    """The script without the hook's line."""
    return "\n".join(line for line in (script or "").splitlines() if line.strip() != SCRIPT_LINE)


def shutter_frames(frame, on=False, duration=1.0, shutter_start=0.25, shutter_end=0.75, position=0, steps=2):
    """(start, end, step) of the keys a frame's groom is written with: the frame alone without
    deformation blur; with it, Redshift's shutter: a window `duration` frames long placed on the
    frame by `position` (0 centered on it, 1 starting on it, 2 ending on it), open from
    `shutter_start` to `shutter_end` of it, in `steps` deformation steps (2 at least)."""
    frame = float(frame)
    duration = float(duration)
    if not on or duration <= 0.0 or shutter_end <= shutter_start:
        return frame, frame, 1.0
    window = {0: frame - 0.5 * duration, 1: frame, 2: frame - duration}.get(int(position), frame - 0.5 * duration)
    start = window + duration * float(shutter_start)
    end = window + duration * float(shutter_end)
    keys = max(2, int(steps))
    return start, end, (end - start) / (keys - 1)


def frame_tag(frame):
    """A file-safe frame number: 12 -> "0012", 12.5 -> "0012_5", -3 -> "n0003"."""
    whole = int(abs(frame))
    text = "%04d" % whole
    rest = abs(frame) - whole
    if rest > 1e-6:
        text += "_" + ("%g" % rest)[2:]
    return ("n" if frame < 0 else "") + text


def cache_path(folder, asset, frame):
    """Where a groom's frame goes: <folder>/<asset>.<frame>.abc, forward slashes."""
    return os.path.join(folder, "%s.%s.abc" % (asset, frame_tag(frame))).replace("\\", "/")


def shader_settings(principled):
    """What build_shader sets on a Principled Hair from a look's "principledHair" (the core's
    principledHair): {attr: value} of the shader, and the user data feeding it as
    [(node type, attribute name read, shader input)]. By RGB: the strands' color as Albedo (Albedo
    Mix 1, no melanin); else the melanin channels, the dye as Tint. The variations 0."""
    p = principled or {}
    attrs = {"roughness_l": p.get("roughness", 0.3), "roughness_a": p.get("azimuthalRoughness", 0.2),
             "ior": p.get("ior", 1.55), "alpha": p.get("angleShift", 3.0), "tint": tuple(p.get("tint") or (1, 1, 1)),
             "melaninVar": 0.0, "pheomelaninVar": 0.0, "roughnessLVar": 0.0, "roughnessAVar": 0.0}
    if p.get("byColor"):
        attrs.update({"albedoMix": 1.0, "melanin": 0.0, "pheomelanin": 0.0})
        data = [("RedshiftUserDataColor", p.get("colorData", "Cd"), "albedo")]
    else:
        attrs.update({"albedoMix": 0.0, "melanin": p.get("melanin", 0.35), "pheomelanin": p.get("melaninRedness", 0.2)})
        data = [("RedshiftUserDataScalar", p.get("melaninData", "melanin"), "melanin"),
                ("RedshiftUserDataScalar", p.get("rednessData", "melaninRedness"), "pheomelanin")]
    return attrs, data


# ---------------------------------------------------------------------------
# Redshift in Maya

def redshift_loaded():
    return bool(cmds) and bool(cmds.pluginInfo(REDSHIFT_PLUGIN, query=True, loaded=True))


def check():
    """Raises RedshiftRequired unless Redshift for Maya is loaded."""
    if not redshift_loaded():
        raise RedshiftRequired("Redshift for Maya is not loaded (Windows > Settings/Preferences > Plug-in Manager: "
                               "redshift4maya)")


def _options():
    """Redshift's render options node (made as Redshift's Render Settings make it)."""
    check()
    nodes = cmds.ls(type="RedshiftOptions") or []
    if nodes:
        return nodes[0]
    try:
        return mel.eval("redshiftGetRedshiftOptionsNode(true)")
    except RuntimeError:
        return cmds.createNode("RedshiftOptions", name=OPTIONS, skipSelect=True)


def hook_enabled():
    """Whether Redshift runs the hook before each frame."""
    if not redshift_loaded():
        return False
    nodes = cmds.ls(type="RedshiftOptions") or []
    script = cmds.getAttr(nodes[0] + "." + SCRIPT_ATTR) if nodes else ""
    return SCRIPT_LINE in (line.strip() for line in (script or "").splitlines())


def enable():
    """The hook on: Redshift writes and sets every visible groom before each frame. Returns True."""
    node = _options()
    plug = node + "." + SCRIPT_ATTR
    cmds.setAttr(plug, add_line(cmds.getAttr(plug)), type="string")
    sync_visibility()
    watch_renderer()
    return True


def disable():
    """The hook off (the user's own lines in the script kept). The proxies stay, hidden from other
    renderers as ever. Returns False."""
    if redshift_loaded():
        for node in cmds.ls(type="RedshiftOptions") or []:
            plug = node + "." + SCRIPT_ATTR
            cmds.setAttr(plug, remove_line(cmds.getAttr(plug)), type="string")
    return False


def toggle():
    """The menu's switch: on when off, off when on. Returns whether it is on now."""
    return disable() if hook_enabled() else enable()


def motion_blur():
    """Redshift's deformation blur as shutter_frames takes it: (on, duration, shutter start,
    shutter end, position, steps). Off unless both motion blur and deformation blur are on."""
    nodes = cmds.ls(type="RedshiftOptions") or []
    if not nodes:
        return (False,)
    n = nodes[0]

    def get(attr, default):
        return cmds.getAttr(n + "." + attr) if cmds.attributeQuery(attr, node=n, exists=True) else default

    on = bool(get("motionBlurEnable", False)) and bool(get("motionBlurDeformationEnable", False))
    return (on, get("motionBlurFrameDuration", 1.0), get("motionBlurShutterStart", 0.25), get("motionBlurShutterEnd", 0.75),
            get("motionBlurShutterPosition", 0), get("motionBlurNumDeformationSteps", 2))


def _transform(groom):
    return (cmds.listRelatives(groom, parent=True, fullPath=True) or [None])[0]


def find_proxy(groom):
    """The groom's RedshiftProxyMesh and its placeholder mesh, or (None, None)."""
    for node in cmds.ls(type="RedshiftProxyMesh") or []:
        if cmds.attributeQuery(PROXY_TAG, node=node, exists=True) and \
                (cmds.ls(cmds.getAttr(node + "." + PROXY_TAG), long=True) or [None])[0] == groom:
            meshes = cmds.listConnections(node + ".outMesh", type="mesh", shapes=True) or []
            return node, (cmds.ls(meshes[0], long=True)[0] if meshes else None)
    return None, None


def ensure_proxy(groom, path):
    """The groom's proxy reading `path` (Alembic), made under the groom's transform the first time
    (redshiftProxyDoCreate's way), its file set every time. Returns (proxy, placeholder mesh)."""
    import groomlab
    proxy, mesh = find_proxy(groom)
    transform = _transform(groom)
    if proxy is None:
        base = groomlab.asset_name(groom)
        proxy = cmds.createNode("RedshiftProxyMesh", name=base + "_rsProxy", skipSelect=True)
        cmds.addAttr(proxy, longName=PROXY_TAG, dataType="string")
        cmds.setAttr(proxy + "." + PROXY_TAG, groom, type="string")
    if mesh is None:
        mesh = cmds.createNode("mesh", name=groomlab.asset_name(groom) + "_rsProxyShape", parent=transform, skipSelect=True)
        mesh = cmds.ls(mesh, long=True)[0]
        cmds.connectAttr(proxy + ".outMesh", mesh + ".inMesh", force=True)
        cmds.setAttr(proxy + ".displayMode", BOUNDING_BOX)
    if cmds.getAttr(proxy + ".proxyFileType") != ALEMBIC:
        cmds.setAttr(proxy + ".proxyFileType", ALEMBIC)
    if cmds.getAttr(proxy + ".fileName") != path:
        cmds.setAttr(proxy + ".fileName", path, type="string")
    if not _own_material(mesh):
        cmds.sets(mesh, edit=True, forceElement=build_shader(groom))
    return proxy, mesh


def _own_material(mesh):
    """Whether the placeholder has a material other than the default one GroomLab replaces."""
    groups = cmds.listConnections(mesh, type="shadingEngine") or []
    return any(g != "initialShadingGroup" for g in groups)


def build_shader(groom):
    """A Principled Hair for the groom's look, with the user data reading its strands' channels
    (shader_settings). Returns its shading group. Made once per groom (found again by name)."""
    import groomlab
    from groomlab.shading import hair_look_json
    base = groomlab.asset_name(groom) + "_rsHair"
    sg = base + "SG"
    if cmds.objExists(sg):
        return sg
    attrs, data = shader_settings(hair_look_json(groom).get("principledHair"))
    shader = cmds.shadingNode("RedshiftPrincipledHair", asShader=True, name=base)
    for attr, value in attrs.items():
        if not cmds.attributeQuery(attr, node=shader, exists=True):
            continue
        if isinstance(value, tuple):
            cmds.setAttr(shader + "." + attr, *value, type="double3")
        else:
            cmds.setAttr(shader + "." + attr, value)
    for node_type, name, target in data:
        reader = cmds.shadingNode(node_type, asUtility=True, name="%s_%s" % (base, name))
        cmds.setAttr(reader + ".attributeName", name, type="string")
        cmds.connectAttr(reader + ".out", shader + "." + target, force=True)
    sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=sg)
    cmds.connectAttr(shader + ".outColor", sg + ".surfaceShader", force=True)
    return sg


def write_groom(groom, frame=None, folder=None):
    """The groom's frame to Alembic for Redshift (render density and overrides; Redshift's
    deformation blur its shutter keys) and its proxy set to it. Returns the file."""
    import groomlab
    frame = cmds.currentTime(query=True) if frame is None else float(frame)
    start, end, step = shutter_frames(frame, *motion_blur())
    folder = folder or os.path.join(groomlab.asset_folder("caches"), "redshift")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    path = cache_path(folder, groomlab.asset_name(groom), frame)
    groomlab.export_alembic(groom, path, start=start, end=end, step=step, render_overrides=True)
    ensure_proxy(groom, path)
    return path


def pre_render_frame(grooms=None, frame=None, folder=None):
    """Redshift's Pre Render Frame step (the module's docstring). Never raises into Redshift's
    render: a failure is printed and that groom left out. Returns the files written."""
    from groomlab.render_gather import render_grooms
    written = []
    try:
        targets = render_grooms(grooms)
    except Exception as e:  # noqa: BLE001
        cmds.warning("GroomLab: Redshift hook: %s" % e)
        return written
    for groom in targets:
        try:
            written.append(write_groom(groom, frame, folder))
        except Exception as e:  # noqa: BLE001 (Redshift renders on without this groom)
            cmds.warning("GroomLab: Redshift hook: %s left out: %s" % (groom, e))
    sync_visibility(True)
    return written


def refresh(grooms=None):
    """This frame written and every groom's proxy set now: what an IPR session shows (Redshift's
    IPR runs no frame script). Returns the files written."""
    check()
    return pre_render_frame(grooms)


def renderer_is_redshift():
    return bool(cmds) and cmds.getAttr("defaultRenderGlobals.currentRenderer") == "redshift"


def sync_visibility(show=None):
    """The placeholders shown while Redshift renders the scene (`show` None: the scene's renderer
    is Redshift), hidden otherwise, so other renderers never draw them."""
    show = renderer_is_redshift() if show is None else bool(show)
    for node in cmds.ls(type="RedshiftProxyMesh") or [] if redshift_loaded() else []:
        if not cmds.attributeQuery(PROXY_TAG, node=node, exists=True):
            continue
        for mesh in cmds.listConnections(node + ".outMesh", type="mesh", shapes=True) or []:
            if cmds.getAttr(mesh + ".visibility") != show:
                cmds.setAttr(mesh + ".visibility", show)


_watch = {"job": None, "scene": None}


def watch_renderer():
    """Keep the placeholders' visibility with the scene's renderer as it changes, for this Maya
    session: a job on defaultRenderGlobals.currentRenderer (it goes with the scene) and one on
    SceneOpened that syncs the opened scene and watches its renderer again."""
    if _watch["scene"] is None or not cmds.scriptJob(exists=_watch["scene"]):
        _watch["scene"] = cmds.scriptJob(event=["SceneOpened", _scene_opened])
    job = _watch["job"]
    if job is not None and cmds.scriptJob(exists=job):
        return job
    _watch["job"] = cmds.scriptJob(attributeChange=["defaultRenderGlobals.currentRenderer", sync_visibility])
    return _watch["job"]


def _scene_opened():
    _watch["job"] = None
    if redshift_loaded():
        sync_visibility()
    watch_renderer()


def start():
    """GroomLab's start (its menu's install): the renderer watched, so placeholders saved in a
    scene keep out of other renderers whether Redshift loads before GroomLab or after."""
    watch_renderer()
    if redshift_loaded():
        sync_visibility()


def menu_toggle():
    """GroomLab > Render > Redshift: the hook on or off, said in the script editor and a message."""
    try:
        on = toggle()
    except RedshiftRequired as e:
        cmds.warning("GroomLab: %s" % e)
        return
    text = ("Grooms render in Redshift: each frame writes them and sets their proxies" if on else
            "Redshift hook off: the grooms' proxies keep their last frame")
    print("GroomLab: " + text)
    cmds.inViewMessage(assistMessage=text, position="topCenter", fade=True)


def menu_refresh():
    """GroomLab > Render > Refresh Redshift Grooms: this frame written now (for IPR)."""
    try:
        written = refresh()
    except RedshiftRequired as e:
        cmds.warning("GroomLab: %s" % e)
        return
    print("GroomLab: Redshift: %d groom(s) written for this frame" % len(written))
