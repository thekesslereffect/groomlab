# SPDX-License-Identifier: Apache-2.0
"""Redshift for Maya (issue 81): every visible groom renders in Redshift with nothing to set up.

Redshift has no public API for a renderer plug-in of GroomLab's own (its translator extensions and
procedurals are Maxon's), so GroomLab drives a Redshift Proxy per groom, made and kept for you.
Nothing to switch on: whenever the scene's renderer is Redshift (activate: on picking it, on
opening a scene that uses it, at GroomLab's start), GroomLab
  - adds its line to Redshift's Pre Render Frame MEL (redshiftOptions.preRenderFrameMel), so every
    frame Redshift renders (Render View, batch, a farm with GroomLab installed) writes the grooms
    first (pre_render_frame);
  - wraps Redshift's starts (wrap_ipr): Maya's Render View IPR (the renderer's
    startIprRenderProcedure, redshiftStartIprRender) and Redshift's own RenderView, whose Render and
    IPR buttons run redshiftRvRender (rsRender -rv), so each writes the frame first;
  - on picking Redshift, writes the frame and makes the proxies at once;
  - while Redshift's Render View window is open (or Maya's IPR runs), keeps the proxies current by
    itself (LiveIpr): a groom with no proxy gets one at once, and one that changed (an edit, a new
    frame: its live evaluations, groomLabGraphStats -evaluations) is written again once it has
    held still. The Render View's own IPR button starts IPR natively, past any MEL GroomLab could
    wrap, so this is what makes that IPR find the grooms and follow their edits.
Every write is a file of its own (<groom>.<frame>.<n>.abc, the older ones of that frame deleted):
Redshift keeps a proxy's file as it first read it, so a new name is what makes it read again.
For each visible groom that writes the frame to Alembic (groomlab.export_alembic: the render density
and overrides; with Redshift's deformation blur on, its shutter in its deformation steps:
shutter_frames) under the scene's groomlab/<scene>/caches/redshift folder, and finds or makes its
proxy (ensure_proxy): a transform of its own, <groom>_rsProxy, placed by the groom's world matrix
(its offsetParentMatrix: the Alembic's points are the groom's own), with a RedshiftProxyMesh reading
the file as Alembic into its placeholder mesh, hidden in the viewport (the groom draws itself), and a
Principled Hair from the groom's look (build_shader: the core's principledHair) unless it has a
material of its own. A proxy shows only while its groom is visible and the scene renders in
Redshift (sync_visibility), so Arnold, V-Ray or RenderMan never draw it. Redshift's IPR shows the
frame as last written: live, or GroomLab > Render > Refresh Redshift Grooms (refresh).

    import groomlab.redshift_hook as rs
    rs.refresh()       # this frame written and the proxies set now
    rs.remove_hook()   # the line taken out of Redshift's script (the proxies stay)

Node and attribute names are Redshift 2026's (redshiftProxy.mel's redshiftProxyDoCreate;
AERedshiftProxyMeshTemplate, AERedshiftPrincipledHairTemplate, redshiftCreateMotionBlurTab.mel,
redshiftRegisterRenderer.mel, redshiftStartIprRender.mel).
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
HIDE_IN_VIEWPORT = 3  # RedshiftProxyMesh.displayMode: Bounding Box 0, Preview Mesh 1, Linked Mesh 2, Hide 3
RENDERER = "redshift"  # Redshift's name as a Maya renderer (defaultRenderGlobals.currentRenderer)
REDSHIFT_IPR_START = "redshiftStartIprRender"
IPR_START = "groomLabRedshiftStartIpr"  # GroomLab's wrap: the grooms written, then Redshift's start
RV_RENDER = "redshiftRvRender"  # Redshift's RenderView Render / IPR buttons (redshiftRvRender.mel)
# Redshift 2026's redshiftRvRender, as GroomLab's wrap repeats it after writing the grooms; a
# Redshift whose procedure differs is left alone (rv_render_known).
RV_RENDER_BODY = ("rsRender -render -stopIpr;", 'rsRender -render -ipr -rv -cam "<renderview>";',
                  'rsRender -render -rv  -cam "<renderview>";')


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


def cache_path(folder, asset, frame, version=None):
    """Where a groom's frame goes: <folder>/<asset>.<frame>.abc, or with `version`
    <folder>/<asset>.<frame>.<version>.abc (each write a name of its own), forward slashes."""
    tag = frame_tag(frame) if version is None else "%s.%d" % (frame_tag(frame), int(version))
    return os.path.join(folder, "%s.%s.abc" % (asset, tag)).replace("\\", "/")


def cache_versions(names, asset, frame):
    """The versions of a groom's frame among file `names`: {version: name}."""
    prefix = "%s.%s." % (asset, frame_tag(frame))
    found = {}
    for name in names:
        if name.startswith(prefix) and name.endswith(".abc"):
            middle = name[len(prefix):-4]
            if middle.isdigit():
                found[int(middle)] = name
    return found


def next_version(names, asset, frame):
    """The version the next write of a groom's frame takes: one past the highest there (1 first)."""
    found = cache_versions(names, asset, frame)
    return max(found) + 1 if found else 1


def is_render_view_title(title):
    """Whether a window's title is Redshift's Render View's ("Redshift Render View", any case,
    with or without the frame or camera after it)."""
    t = " ".join((title or "").lower().split())
    return "redshift" in t and ("render view" in t or "renderview" in t)


def rv_render_known(source):
    """Whether Redshift's redshiftRvRender (its script's text) is the one GroomLab's wrap repeats."""
    text = " ".join((source or "").split())
    return all(" ".join(line.split()) in text for line in RV_RENDER_BODY)


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


def renderer_is_redshift():
    return bool(cmds) and cmds.getAttr("defaultRenderGlobals.currentRenderer") == RENDERER


def _options():
    """Redshift's render options node (made as Redshift's Render Settings make it)."""
    check()
    nodes = cmds.ls(type="RedshiftOptions") or []
    if nodes:
        return nodes[0]
    try:
        return mel.eval("redshiftGetRedshiftOptionsNode(1)")
    except RuntimeError:
        return cmds.createNode("RedshiftOptions", name=OPTIONS, skipSelect=True)


def hook_installed():
    """Whether Redshift runs GroomLab's line before each frame."""
    if not redshift_loaded():
        return False
    nodes = cmds.ls(type="RedshiftOptions") or []
    script = cmds.getAttr(nodes[0] + "." + SCRIPT_ATTR) if nodes else ""
    return SCRIPT_LINE in (line.strip() for line in (script or "").splitlines())


def install_hook():
    """GroomLab's line in Redshift's Pre Render Frame MEL (once; the user's lines kept)."""
    plug = _options() + "." + SCRIPT_ATTR
    script = cmds.getAttr(plug) or ""
    if add_line(script) != script:
        cmds.setAttr(plug, add_line(script), type="string")


def remove_hook():
    """GroomLab's line taken out of Redshift's script (the proxies stay as they are)."""
    if redshift_loaded():
        for node in cmds.ls(type="RedshiftOptions") or []:
            plug = node + "." + SCRIPT_ATTR
            cmds.setAttr(plug, remove_line(cmds.getAttr(plug)), type="string")


def wrap_ipr():
    """Redshift's starts through GroomLab's: the grooms written first. Its RenderView (wrap_rv) and
    Maya's Render View IPR (renderer -edit -startIprRenderProcedure, then Redshift's own start).
    Done again whenever activated (Redshift may register itself again). False when Maya's IPR start
    is not the one GroomLab knows (left alone)."""
    if not redshift_loaded() or not cmds.renderer(RENDERER, exists=True):
        return False
    wrap_rv()
    current = cmds.renderer(RENDERER, query=True, startIprRenderProcedure=True)
    if current == IPR_START:
        return True
    if current != REDSHIFT_IPR_START:
        return False
    mel.eval('global proc %s(string $editor, int $resolution0, int $resolution1, string $camera) {\n'
             '    python("import groomlab.redshift_hook; groomlab.redshift_hook.before_render()");\n'
             '    %s($editor, $resolution0, $resolution1, $camera);\n'
             '}' % (IPR_START, REDSHIFT_IPR_START))
    cmds.renderer(RENDERER, edit=True, startIprRenderProcedure=IPR_START)
    return True


def wrap_rv():
    """Redshift's RenderView Render and IPR buttons (redshiftRvRender) through GroomLab's: the
    grooms written, then the same rsRender calls. Only over the procedure GroomLab knows
    (rv_render_known on its script); True when wrapped. MEL's whatIs tells them apart: Redshift's
    is "found in" its script, GroomLab's "entered interactively" (so a Redshift that sourced its
    script again is wrapped again)."""
    try:
        where = mel.eval('whatIs "%s"' % RV_RENDER)
    except RuntimeError:
        return False
    if "entered interactively" in where:
        return True  # GroomLab's already
    path = where.split(": ", 1)[1].strip() if "found in" in where and ": " in where else ""
    if not path or not os.path.isfile(path):
        return False
    with open(path, encoding="utf-8", errors="replace") as f:
        if not rv_render_known(f.read()):
            return False
    mel.eval('global proc %s(string $action, string $camera) {\n'
             '    python("import groomlab.redshift_hook; groomlab.redshift_hook.before_render()");\n'
             '    %s\n'
             '    if ($action == "ipr") { %s } else { %s }\n'
             '}' % ((RV_RENDER,) + RV_RENDER_BODY))
    return True


def before_render():
    """What every Redshift start runs first (RenderView render or IPR, Maya's IPR): the frame
    written, the proxies set, live IPR watching. Never raises into Redshift."""
    try:
        pre_render_frame()
        LiveIpr.start()
    except Exception as e:  # noqa: BLE001
        cmds.warning("GroomLab: Redshift: %s" % e)


def activate(write=False):
    """While the scene renders in Redshift: the frame hook and the IPR wrap in place, and with
    `write` the frame written and the proxies made now; the proxies' visibility synced either way.
    Returns whether the scene renders in Redshift."""
    if not redshift_loaded():
        return False
    on = renderer_is_redshift()
    if on:
        install_hook()
        wrap_ipr()
        LiveIpr.start()
        if write:
            pre_render_frame()
            return True
    sync_visibility()
    return on


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


def _transform(node):
    return (cmds.listRelatives(node, parent=True, fullPath=True) or [None])[0]


def _proxies():
    """Every proxy GroomLab made: [(proxy node, the groom shape it renders or None, its placeholder mesh)]."""
    out = []
    for node in cmds.ls(type="RedshiftProxyMesh") or []:
        if not cmds.attributeQuery(PROXY_TAG, node=node, exists=True):
            continue
        groom = (cmds.ls(cmds.getAttr(node + "." + PROXY_TAG) or "", long=True) or [None])[0]
        meshes = cmds.listConnections(node + ".outMesh", type="mesh", shapes=True) or []
        out.append((node, groom, cmds.ls(meshes[0], long=True)[0] if meshes else None))
    return out


def find_proxy(groom):
    """The groom's RedshiftProxyMesh and its placeholder mesh, or (None, None)."""
    for node, renders, mesh in _proxies():
        if renders == groom:
            return node, mesh
    return None, None


def ensure_proxy(groom, path):
    """The groom's proxy reading `path` (Alembic): made the first time as an object of its own
    (<groom>_rsProxy, at the top of the Outliner) placed by the groom's world matrix (its
    offsetParentMatrix), its placeholder hidden in the viewport; its file set every time. Returns
    (proxy, placeholder mesh)."""
    import groomlab
    proxy, mesh = find_proxy(groom)
    if mesh is not None and _transform(mesh) == _transform(groom):
        cmds.delete(mesh)  # (the first builds' placeholder, under the groom: made again as its own object)
        mesh = None
    base = groomlab.asset_name(groom) + "_rsProxy"
    if proxy is None:
        proxy = cmds.createNode("RedshiftProxyMesh", name=base + "Node", skipSelect=True)
        cmds.addAttr(proxy, longName=PROXY_TAG, dataType="string")
        cmds.setAttr(proxy + "." + PROXY_TAG, groom, type="string")
    if mesh is None:
        transform = cmds.createNode("transform", name=base, skipSelect=True)
        mesh = cmds.createNode("mesh", name=base + "Shape", parent=transform, skipSelect=True)
        mesh = cmds.ls(mesh, long=True)[0]
        cmds.connectAttr(proxy + ".outMesh", mesh + ".inMesh", force=True)
    transform = _transform(mesh)
    groom_transform = _transform(groom)
    if groom_transform and not cmds.isConnected(groom_transform + ".worldMatrix[0]", transform + ".offsetParentMatrix"):
        cmds.connectAttr(groom_transform + ".worldMatrix[0]", transform + ".offsetParentMatrix", force=True)
    if cmds.getAttr(proxy + ".displayMode") != HIDE_IN_VIEWPORT:
        cmds.setAttr(proxy + ".displayMode", HIDE_IN_VIEWPORT)
    if cmds.getAttr(proxy + ".proxyFileType") != ALEMBIC:
        cmds.setAttr(proxy + ".proxyFileType", ALEMBIC)
    if cmds.getAttr(proxy + ".fileName") != path:
        cmds.setAttr(proxy + ".fileName", path, type="string")
    _proxy_material(mesh, groom)
    return proxy, mesh


def groom_shading_group(groom):
    """The groom's own shading group when its surface shader is a Redshift material (assigned in the
    Hypershade), else None: Redshift renders only its own."""
    for sg in cmds.listConnections(groom + ".instObjGroups", source=False, destination=True, type="shadingEngine") or []:
        shaders = cmds.listConnections(sg + ".surfaceShader", source=True, destination=False) or []
        if shaders and (cmds.nodeType(shaders[0]).startswith("Redshift") or
                        "redshift" in " ".join(cmds.getClassification(cmds.nodeType(shaders[0])) or []).lower()):
            return sg
    return None


def _proxy_material(mesh, groom):
    """The proxy's placeholder renders the groom's own Redshift material, else one the user gave the
    placeholder itself, else GroomLab's Principled Hair (build_shader). One taken from the groom
    (shading.material_from_groom) goes back to GroomLab's when the groom's changes."""
    from groomlab.shading import material_from_groom, set_material_from_groom
    groups = [g for g in cmds.listConnections(mesh, type="shadingEngine") or [] if g != "initialShadingGroup"]
    current = groups[0] if groups else None
    own = groom_shading_group(groom)
    from_groom = material_from_groom(mesh)
    sg = own or (current if current and current != from_groom else None) or build_shader(groom)
    if sg != current:
        cmds.sets(mesh, edit=True, forceElement=sg)
    if (own or "") != from_groom:
        set_material_from_groom(mesh, own)


def sync_material(groom):
    """The groom's material to its proxy (_proxy_material), when it has one. Nothing without Redshift."""
    if not redshift_loaded():
        return
    _, mesh = find_proxy(groom)
    if mesh is not None:
        _proxy_material(mesh, groom)


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
    asset = groomlab.asset_name(groom)
    before = cache_versions(os.listdir(folder), asset, frame)
    path = cache_path(folder, asset, frame, max(before) + 1 if before else 1)
    groomlab.export_alembic(groom, path, start=start, end=end, step=step, render_overrides=True, vertex_data=True)
    ensure_proxy(groom, path)
    for name in before.values():  # (one Redshift still reads stays until the next write)
        try:
            os.remove(os.path.join(folder, name))
        except OSError:
            pass
    return path


def pre_render_frame(grooms=None, frame=None, folder=None):
    """Redshift's Pre Render Frame step, and IPR's start (the module's docstring). Never raises into
    Redshift's render: a failure is printed and that groom left out. Returns the files written."""
    from groomlab.render_gather import render_grooms
    written = []
    try:
        targets = render_grooms(grooms)
    except Exception as e:  # noqa: BLE001
        cmds.warning("GroomLab: Redshift: %s" % e)
        return written
    for groom in targets:
        try:
            written.append(write_groom(groom, frame, folder))
        except Exception as e:  # noqa: BLE001 (Redshift renders on without this groom)
            cmds.warning("GroomLab: Redshift: %s left out: %s" % (groom, e))
    sync_visibility(rendering=True)
    return written


def refresh(grooms=None):
    """This frame written and every groom's proxy set now (a new file each, which a running IPR
    reads). Returns the files written."""
    check()
    if renderer_is_redshift():
        install_hook()
        wrap_ipr()
        LiveIpr.start()
    return pre_render_frame(grooms)


def sync_visibility(rendering=None):
    """Each proxy shown while its groom is visible and the scene renders in Redshift (`rendering`
    None: the scene's renderer says), hidden otherwise (its groom hidden or gone, or another
    renderer picked), so other renderers never draw it."""
    if not redshift_loaded():
        return
    from groomlab.render_gather import visible
    rendering = renderer_is_redshift() if rendering is None else bool(rendering)
    for node, groom, mesh in _proxies():
        if mesh is None:
            continue
        show = rendering and groom is not None and visible(groom)
        if cmds.getAttr(mesh + ".visibility") != show:
            cmds.setAttr(mesh + ".visibility", show)


def menu_refresh():
    """GroomLab > Render > Refresh Redshift Grooms: this frame written now (for IPR)."""
    try:
        written = refresh()
    except RedshiftRequired as e:
        cmds.warning("GroomLab: %s" % e)
        return
    print("GroomLab: Redshift: %d groom(s) written for this frame" % len(written))


def render_view_open():
    """Whether Redshift's Render View window shows: a visible top-level window, or a Maya workspace
    panel it is docked in, titled as is_render_view_title says."""
    try:
        try:
            from PySide6 import QtWidgets
        except ImportError:
            from PySide2 import QtWidgets
        app = QtWidgets.QApplication.instance()
        if app is not None:
            for w in app.topLevelWidgets():
                if w.isVisible() and is_render_view_title(w.windowTitle()):
                    return True
    except Exception:  # noqa: BLE001 (no Qt: batch)
        pass
    for control in cmds.lsUI(type="workspaceControl") or []:
        try:
            if cmds.workspaceControl(control, query=True, visible=True) and \
                    is_render_view_title(cmds.workspaceControl(control, query=True, label=True)):
                return True
        except RuntimeError:
            continue
    return False


class LiveIpr(object):
    """While the scene renders in Redshift and its Render View window is open (or Maya's IPR runs):
    the proxies kept current. Polls every TICK_MS: a visible groom with no proxy is written at once
    (its proxy made: what an IPR started from the window's own button then finds), and one whose
    live evaluations (groomLabGraphStats -evaluations) moved, then held for a tick, is written again
    (write_groom: a new file, which IPR reads). Idle otherwise."""
    TICK_MS = 600
    _timer = None
    _seen = {}     # groom -> the evaluations last seen
    _written = {}  # groom -> the evaluations last written

    @classmethod
    def start(cls):
        if cls._timer is not None:
            return
        try:
            from PySide6 import QtCore
        except ImportError:
            from PySide2 import QtCore
        cls._timer = QtCore.QTimer()
        cls._timer.setInterval(cls.TICK_MS)
        cls._timer.timeout.connect(cls.tick)
        cls._timer.start()

    @classmethod
    def ipr_running(cls):
        try:
            return redshift_loaded() and bool(mel.eval("redshiftIsRunningIpr()"))
        except RuntimeError:
            return False

    @classmethod
    def active(cls):
        """Whether the proxies are kept current now."""
        return redshift_loaded() and renderer_is_redshift() and (cls.ipr_running() or render_view_open())

    @classmethod
    def tick(cls):
        try:
            if not cls.active():
                cls._seen.clear()
                cls._written.clear()
                return
            from groomlab.render_gather import render_grooms
            for groom in render_grooms():
                count = cmds.groomLabGraphStats(groom, evaluations=True)
                last = cls._seen.get(groom)
                cls._seen[groom] = count
                if find_proxy(groom)[0] is None:
                    write_groom(groom)  # no proxy yet: made now
                    cls._written[groom] = count
                    continue
                if last is None:
                    cls._written.setdefault(groom, count)  # (its proxy as it was when watching began)
                    continue
                if count == last and cls._written.get(groom) != count:
                    write_groom(groom)  # changed, then held a tick: written again
                    cls._written[groom] = count
        except Exception as e:  # noqa: BLE001 (a timer: never raise into Maya's event loop)
            cmds.warning("GroomLab: Redshift live IPR: %s" % e)


_watch = {"job": None, "scene": None}


def _renderer_changed():
    activate(write=True)  # Redshift picked: the proxies made at once; another renderer: hidden


def watch_renderer():
    """Follow the scene's renderer for this Maya session: a job on
    defaultRenderGlobals.currentRenderer (it goes with the scene) and one on SceneOpened that
    activates the opened scene and watches its renderer again."""
    if _watch["scene"] is None or not cmds.scriptJob(exists=_watch["scene"]):
        _watch["scene"] = cmds.scriptJob(event=["SceneOpened", _scene_opened])
    job = _watch["job"]
    if job is not None and cmds.scriptJob(exists=job):
        return job
    _watch["job"] = cmds.scriptJob(attributeChange=["defaultRenderGlobals.currentRenderer", _renderer_changed])
    return _watch["job"]


def _scene_opened():
    _watch["job"] = None
    activate()
    watch_renderer()


def start():
    """GroomLab's start (its menu's install): the renderer followed, and a scene already rendering
    in Redshift set up (hook, IPR wrap, visibility), whether Redshift loads before GroomLab or after."""
    watch_renderer()
    activate()
