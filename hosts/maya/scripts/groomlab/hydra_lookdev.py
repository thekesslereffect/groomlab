# SPDX-License-Identifier: Apache-2.0
"""XPU lookdev in Maya's viewport (issue #82): each groom as USD curves in a MayaUSD proxy shape,
drawn by Maya Hydra with RenderMan XPU.

RenderMan XPU renders no procedural, and its batch path through XGen (renderman.sync_xpu) carries
no per-strand data. In the viewport, RenderMan's own Hydra renderer (hdPrman, which RenderMan for
Maya ships) draws USD, and USD curves carry everything: the root UVs (primvars:st, so maps on the
scalp work), and the RenderMan procedural's per-strand primvars (rman_*: groomLabExportUsd
-renderman), so GroomLab's hair network shades them as RIS does.

enable() gives each groom a proxy shape (<groom>_lookdev, never saved with the scene) whose stage
holds its curves (rewritten as the groom changes) and its material: the groom's own RenderMan
material from the Hypershade, else GroomLab's (the RenderMan procedural's), translated node for node
(Pxr nodes are RenderMan's shaders by the same names) and kept in step with the Hypershade's edits.
The grooms themselves are hidden from the viewport meanwhile (Maya Hydra would draw their
viewport lines as hair too), and the viewport draws with RenderMan XPU. disable() undoes it all.

What it needs (check() says what is missing): RenderMan for Maya, and MayaUSD with Maya Hydra
running the USD release RenderMan's Hydra plugins were built for: Maya 2025 (RenderMan 27.3). Maya
2026 runs an older USD than RenderMan 27.3's plugins for it; Maya 2024's USD (22.11) matches, but
doesn't know RenderMan's shader metadata, so registering them there would raise in other tools' USD
Python (MIN_USD). No settings
to change: GroomLab registers RenderMan's Hydra plugins with USD as it loads
(register_renderman_hydra), before anything asks USD's shader registry for a shader (it reads
RenderMan's shaders once; registered later, every material is dropped, and Maya must be restarted).
docs/research/hydra_xpu_viewport.md has the findings.
"""

import os
import re

try:
    from maya import cmds
except ImportError:  # outside Maya (docs, tests of the pure parts)
    cmds = None

PROXY_ATTR = "groomLabLookdev"  # message on the proxy shape: the groom it shows
REVISION_ATTR = "groomLabLookdevRevision"  # int: the curves file's number (a new file each update)
TOKEN_ATTR = "groomLabLookdevToken"  # string: this proxy's files' own part of their names
HIDDEN_ATTR = "groomLabLookdevHid"  # bool on the proxy shape: it hid the groom (lodVisibility)
# The oldest USD RenderMan's shaders are registered with: USD 22.11 (Maya 2024) doesn't know the
# metadata RenderMan 27.3's shader descriptions carry (uiHints, limits), and the first Python call
# that reads USD's shader registry then raises an exception of ~1,700 warnings, in anyone's code.
MIN_USD = (0, 23, 11)
XPU_OVERRIDES = ("mayaHydraRenderOverride_HdPrmanXpuGpuLoaderRendererPlugin",
                 "mayaHydraRenderOverride_HdPrmanXpuCpuLoaderRendererPlugin")
MATERIAL_PATH = "/GroomLabLook"

_state = {"callbacks": [], "panels": {}, "material_callbacks": {}, "queued": set()}


# ---------------------------------------------------------------------------
# What it needs

def _rfm_root():
    """RenderMan for Maya's folder, its plugin loaded or not (its module), or None."""
    root = os.environ.get("RFMTREE")
    if root and os.path.isdir(root):
        return root
    try:
        root = cmds.getModulePath(moduleName="RenderMan_for_Maya")
        if root and os.path.isdir(root):
            return root
    except RuntimeError:
        pass
    try:
        path = cmds.pluginInfo("RenderMan_for_Maya", query=True, path=True)
    except RuntimeError:
        return None
    # <RFMTREE>/plug-ins/RenderMan_for_Maya.py
    return os.path.dirname(os.path.dirname(path)) if path else None


def renderman_hydra_folder():
    """RenderMan for Maya's Hydra plugins for this Maya (<RFMTREE>/lib/<year>/usd_plugins_<usd>), or None."""
    root = _rfm_root()
    if not root:
        return None
    lib = os.path.join(root, "lib", cmds.about(majorVersion=True))
    if not os.path.isdir(lib):
        return None
    found = sorted(d for d in os.listdir(lib) if d.startswith("usd_plugins_"))
    return os.path.join(lib, found[-1]).replace("\\", "/") if found else None


def usd_version_of_folder(folder):
    """usd_plugins_2505 -> (0, 25, 5): the USD release RenderMan's plugins were built for."""
    m = re.search(r"usd_plugins_(\d\d)(\d\d)$", folder or "")
    return (0, int(m.group(1)), int(m.group(2))) if m else None


def _maya_usd_plugin_path():
    try:
        if not cmds.pluginInfo("mayaUsdPlugin", query=True, loaded=True):
            cmds.loadPlugin("mayaUsdPlugin", quiet=True)
        return cmds.pluginInfo("mayaUsdPlugin", query=True, path=True)
    except RuntimeError:
        return None


def maya_hydra_path():
    """Maya Hydra's plugin, beside the MayaUSD build Maya loaded (its USD's), or None."""
    usd = _maya_usd_plugin_path()
    if not usd:
        return None
    build = usd.replace("\\", "/").split("/plugin/")[0]  # .../MayaUSD_<usd version>
    path = build + "/lib/maya/mayaHydra.mll"
    return path if os.path.isfile(path) else None


def _usd_version():
    from pxr import Usd
    return tuple(Usd.GetVersion())


def _versions_match(folder):
    want = usd_version_of_folder(folder)
    return bool(want) and want == _usd_version()


def register_renderman_hydra():
    """RenderMan for Maya's Hydra plugins registered with USD, as GroomLab loads (the plugin runs
    this first), when RenderMan for Maya is installed and its plugins were built for the USD this
    Maya runs: then USD's shader registry learns RenderMan's shaders as it first reads, and
    Maya Hydra offers RenderMan's renderers. Nothing otherwise (RenderMan's plugins for another USD
    would fail to load). Never raises. Returns whether they are registered."""
    try:
        folder = renderman_hydra_folder()
        if not folder or int(cmds.about(majorVersion=True)) < 2025:
            return False
        # MayaUSD names its USD (MAYA_USD_VERSION, "0.24.11"): another than RenderMan's plugins',
        # nothing to do, without loading USD at all.
        named = re.match(r"(\d+)\.(\d+)\.(\d+)$", os.environ.get("MAYA_USD_VERSION", ""))
        if named and tuple(int(x) for x in named.groups()) != usd_version_of_folder(folder):
            return False
        from pxr import Plug
        if Plug.Registry().GetPluginWithName("hdPrmanLoader"):
            return True
        if not _versions_match(folder) or _usd_version() < MIN_USD:
            return False
        Plug.Registry().RegisterPlugins(folder)
        return bool(Plug.Registry().GetPluginWithName("hdPrmanLoader"))
    except Exception:  # noqa: BLE001 (Maya is starting: never in its way)
        return False


def check():
    """What XPU lookdev needs and this session lacks, as sentences ([] when it can run)."""
    problems = []
    try:
        rfm = cmds.pluginInfo("RenderMan_for_Maya", query=True, loaded=True)
    except RuntimeError:
        rfm = False
    if not rfm:
        problems.append("RenderMan for Maya is not loaded")
    if not _maya_usd_plugin_path():
        problems.append("MayaUSD (mayaUsdPlugin) is not installed")
        return problems
    if not maya_hydra_path():
        problems.append("Maya Hydra (mayaHydra.mll) is not part of this MayaUSD")
    folder = renderman_hydra_folder() if rfm else None
    if rfm and not folder:
        problems.append("RenderMan for Maya has no Hydra plugins for Maya %s" % cmds.about(majorVersion=True))
    if folder:
        want, have = usd_version_of_folder(folder), _usd_version()
        if want and (want != have or have < MIN_USD):
            problems.append("XPU lookdev runs in Maya 2025: in Maya %s, RenderMan for Maya's viewport renderer "
                            "doesn't fit the USD Maya runs (RenderMan's is built for %d.%d.%d, Maya runs %d.%d.%d)" %
                            ((cmds.about(majorVersion=True),) + want + have))
        elif register_renderman_hydra() and not _shaders_known():
            # USD learns RenderMan's shaders (PxrMarschnerHair, PxrTexture...) from RenderMan's
            # plugins once, the first time something asks: registered after that, the renderer
            # runs but every material is dropped (the hair renders grey).
            problems.append("USD read its shaders before GroomLab could add RenderMan's: restart Maya (GroomLab "
                            "adds them as it loads)")
    return problems


def _shaders_known():
    from pxr import Sdr
    for _ in range(2):  # (USD 22.11: RenderMan's metadata it doesn't know raises warnings once; asked again)
        try:
            return bool(Sdr.Registry().GetShaderNodeByIdentifier("PxrMarschnerHair"))
        except Exception:  # noqa: BLE001
            continue
    return False


def load():
    """MayaUSD, RenderMan's Hydra plugins (register_renderman_hydra) and Maya Hydra. Raises
    RuntimeError with what is missing."""
    problems = check()
    if problems:
        raise RuntimeError("; ".join(problems))
    # Maya Hydra makes a viewport renderer for each Hydra renderer it finds as it loads: one loaded
    # before RenderMan's were registered offers none of RenderMan's (loaded again).
    loaded = cmds.pluginInfo("mayaHydra", query=True, loaded=True)
    if loaded and not any(_override_exists(name) for name in XPU_OVERRIDES):
        cmds.unloadPlugin("mayaHydra", force=True)
        loaded = False
    if not loaded:
        cmds.loadPlugin(maya_hydra_path(), quiet=True)


def _override_exists(name):
    import maya.api.OpenMayaRender as omr
    try:
        return omr.MRenderer.findRenderOverride(name) is not None
    except Exception:  # noqa: BLE001
        return False


# ---------------------------------------------------------------------------
# The material: a RenderMan shading network as a USD material

# Maya attributes every node has (never a RenderMan param).
_BASE_ATTRS = None


def _base_attrs():
    global _BASE_ATTRS
    if _BASE_ATTRS is None:
        node = cmds.createNode("network", skipSelect=True)
        try:
            _BASE_ATTRS = set(cmds.listAttr(node) or [])
        finally:
            cmds.delete(node)
    return _BASE_ATTRS


def _usd_name(node):
    return re.sub(r"[^A-Za-z0-9_]", "_", node.split("|")[-1].split(":")[-1]) or "node"


def _value_type(node, attr):
    """(Sdf value type, how to convert Maya's value) of a RenderMan param, or None to skip it."""
    from pxr import Sdf
    plug = "%s.%s" % (node, attr)
    kind = cmds.getAttr(plug, type=True)
    if kind in ("float", "double", "doubleLinear", "doubleAngle", "floatLinear", "floatAngle"):
        return Sdf.ValueTypeNames.Float, float
    if kind in ("long", "short", "byte", "bool", "enum", "char"):
        return Sdf.ValueTypeNames.Int, int
    if kind in ("float3", "double3"):
        color = cmds.attributeQuery(attr, node=node, usedAsColor=True)
        return (Sdf.ValueTypeNames.Color3f if color else Sdf.ValueTypeNames.Float3), lambda v: tuple(float(x) for x in v[0])
    if kind in ("float2", "double2"):
        return Sdf.ValueTypeNames.Float2, lambda v: tuple(float(x) for x in v[0])
    if kind == "string":
        asset = cmds.attributeQuery(attr, node=node, usedAsFilename=True) or attr.lower().endswith("filename")
        return (Sdf.ValueTypeNames.Asset if asset else Sdf.ValueTypeNames.String), lambda v: v or ""
    return None


def _params(node):
    """The node's RenderMan params: its own attributes (not Maya's every-node ones, not the
    compound's children, not RfM's rman* bookkeeping) that hold a value."""
    out = []
    base = _base_attrs()
    for attr in cmds.listAttr(node, connectable=True, write=True) or []:
        if "." in attr or attr in base or attr.startswith("rman") or attr.startswith("out"):
            continue
        if cmds.attributeQuery(attr, node=node, listParent=True):
            continue  # a child of a color or vector: the parent carries it
        if _value_type(node, attr):
            out.append(attr)
    return out


def _is_default(node, attr):
    """Whether the param holds its default (written or not: an empty string never is a value; a
    struct input, a PxrTexture's manifold, is one in Maya, and a string where RenderMan wants a
    connection stops the renderer)."""
    if cmds.getAttr("%s.%s" % (node, attr), type=True) == "string":
        return not (cmds.getAttr("%s.%s" % (node, attr)) or "")
    try:
        default = cmds.attributeQuery(attr, node=node, listDefault=True)
    except RuntimeError:
        return False
    value = cmds.getAttr("%s.%s" % (node, attr))
    if isinstance(value, list):
        value = list(value[0]) if value and isinstance(value[0], tuple) else value
    if default is None:
        return False
    try:
        if isinstance(value, (list, tuple)):
            return len(value) == len(default) and all(abs(float(a) - float(b)) < 1e-6 for a, b in zip(value, default))
        return abs(float(value) - float(default[0])) < 1e-6
    except (TypeError, ValueError):
        return False


def _set_input(shader, node, attr):
    """The USD input of `node.attr`: its value (when not the default, or always with force)."""
    kind = _value_type(node, attr)
    if not kind:
        return
    sdf_type, convert = kind
    value = cmds.getAttr("%s.%s" % (node, attr))
    shader.CreateInput(attr, sdf_type).Set(convert(value))


def write_material(stage, shading_group, path=MATERIAL_PATH):
    """`shading_group`'s RenderMan network as a USD material at `path` (replaced if there): a
    UsdShade.Shader per Pxr node (info:id the node type: RenderMan's shader of that name), each
    param that is not its default as an input, each connection as one, the surface shader as the
    material's ri:surface. Returns {Maya node: USD shader path}."""
    from pxr import Sdf, UsdShade
    if stage.GetPrimAtPath(path):
        stage.RemovePrim(path)
    material = UsdShade.Material.Define(stage, path)
    surfaces = cmds.listConnections(shading_group + ".surfaceShader", source=True, destination=False) or []
    shaders = {}

    def shader_of(node):
        if node in shaders:
            return UsdShade.Shader(stage.GetPrimAtPath(shaders[node]))
        prim_path = "%s/%s" % (path, _usd_name(node))
        shader = UsdShade.Shader.Define(stage, prim_path)
        shader.CreateIdAttr(cmds.nodeType(node))
        shaders[node] = prim_path
        for attr in _params(node):
            plug = "%s.%s" % (node, attr)
            sources = cmds.listConnections(plug, source=True, destination=False, plugs=True, skipConversionNodes=True) or []
            if sources:
                source_node, source_attr = sources[0].split(".", 1)
                upstream = shader_of(source_node)
                kind = _value_type(node, attr)
                output = upstream.CreateOutput(source_attr.split(".")[-1], kind[0] if kind else Sdf.ValueTypeNames.Token)
                shader.CreateInput(attr, kind[0] if kind else Sdf.ValueTypeNames.Token).ConnectToSource(output)
            elif not _is_default(node, attr):
                _set_input(shader, node, attr)
        return shader

    if surfaces:
        surface = shader_of(surfaces[0])
        material.CreateSurfaceOutput("ri").ConnectToSource(surface.CreateOutput("outColor", Sdf.ValueTypeNames.Token))
    return shaders


# ---------------------------------------------------------------------------
# Each groom's proxy shape

def _groom_material(groom):
    """The shading group the groom renders with in RenderMan: its own RenderMan material (the
    Hypershade's), else its procedural's (GroomLab's hair network)."""
    from groomlab import renderman as rm
    sg = rm.groom_shading_group(groom)
    if sg:
        return sg
    for node in rm.procedurals_of(groom):
        if cmds.attributeQuery(rm.SHADING_GROUP_ATTR, node=node, exists=True):
            found = cmds.listConnections(node + "." + rm.SHADING_GROUP_ATTR, source=True, destination=False) or []
            if found:
                return found[0]
    rm.sync_renderman()  # (its procedural and network made now)
    for node in rm.procedurals_of(groom):
        found = cmds.listConnections(node + "." + rm.SHADING_GROUP_ATTR, source=True, destination=False) or []
        if found:
            return found[0]
    return None


def proxies_of(groom):
    plugs = cmds.listConnections(groom + ".message", source=False, destination=True, plugs=True) or []
    nodes = [p.split(".")[0] for p in plugs if p.endswith("." + PROXY_ATTR)]
    return cmds.ls(nodes, long=True, type="mayaUsdProxyShape") or []


def _folder():
    from groomlab.paths import asset_folder
    folder = asset_folder("renders") + "/hydra"
    if not os.path.isdir(folder):
        os.makedirs(folder)
    return folder


def _stem(groom, token):
    """The files of one proxy shape: <renders>/hydra/<groom>.lookdev.<token>. Names of their own each
    time lookdev turns on: USD keeps a layer it opened by its path, so a proxy made again on the
    same names would find the last one's layers as they were (its material gone)."""
    from groomlab.paths import asset_name
    return "%s/%s.lookdev.%s" % (_folder(), asset_name(groom), token)


def _new_token():
    _state["serial"] = _state.get("serial", 0) + 1
    return "%d_%d" % (os.getpid(), _state["serial"])


def _write_curves(groom, token, revision):
    path = "%s.%d.usda" % (_stem(groom, token), revision)
    cmds.groomLabExportUsd(groom, file=path, renderOverrides=True, renderman=True, scales="mesh")
    return path


def _stage(proxy):
    import mayaUsd.ufe
    return mayaUsd.ufe.getStage(cmds.ls(proxy, long=True)[0])


def _not_saved(node):
    import maya.api.OpenMaya as om2
    sel = om2.MSelectionList()
    sel.add(node)
    om2.MFnDependencyNode(sel.getDependNode(0)).setDoNotWrite(True)


def add_lookdev(groom):
    """The groom's proxy shape (<groom>_lookdev): its curves and material in a stage, the groom
    hidden from the viewport. Returns the proxy shape."""
    from groomlab.base import _shape_of
    groom = _shape_of(groom, "groomLabShape")
    existing = proxies_of(groom)
    if existing:
        return existing[0]
    from pxr import Sdf, Usd, UsdGeom, UsdShade
    from groomlab.paths import asset_name
    token = _new_token()
    curves = _write_curves(groom, token, 1)
    root = _stem(groom, token) + ".usda"
    layer = Sdf.Layer.CreateNew(root)
    layer.subLayerPaths.append(curves)
    # The material and its binding in the file before the viewport draws it: XPU stopped when a
    # textured material arrived after it had started rendering the stage.
    sg = _groom_material(groom)
    if sg:
        authoring = Usd.Stage.Open(layer)
        authoring.SetEditTarget(layer)
        write_material(authoring, sg)
        material = UsdShade.Material(authoring.GetPrimAtPath(MATERIAL_PATH))
        for prim in authoring.Traverse():
            if prim.IsA(UsdGeom.BasisCurves):
                UsdShade.MaterialBindingAPI.Apply(prim).Bind(material)
        del authoring
    layer.Save()
    base = cmds.listRelatives(groom, parent=True)[0].split("|")[-1].split(":")[-1] + "_lookdev"
    transform = cmds.createNode("transform", name=base, skipSelect=True)
    proxy = cmds.createNode("mayaUsdProxyShape", name=base + "Shape", parent=transform, skipSelect=True)
    proxy = cmds.ls(proxy, long=True)[0]
    cmds.setAttr(proxy + ".filePath", root, type="string")
    cmds.connectAttr("time1.outTime", proxy + ".time")
    cmds.addAttr(proxy, longName=PROXY_ATTR, attributeType="message")
    cmds.connectAttr(groom + ".message", proxy + "." + PROXY_ATTR)
    cmds.addAttr(proxy, longName=REVISION_ATTR, attributeType="long", defaultValue=1)
    cmds.addAttr(proxy, longName=TOKEN_ATTR, dataType="string")
    cmds.setAttr(proxy + "." + TOKEN_ATTR, token, type="string")
    cmds.addAttr(proxy, longName=HIDDEN_ATTR, attributeType="bool", defaultValue=False)
    for node in (transform, proxy):
        _not_saved(node)
    stage = _stage(proxy)
    stage.SetEditTarget(stage.GetRootLayer())
    if sg:
        _watch_material(proxy, sg)
    # Maya Hydra would draw the groom's own viewport lines too (as thick hair): hidden meanwhile.
    if cmds.getAttr(groom + ".lodVisibility"):
        cmds.setAttr(groom + ".lodVisibility", False)
        cmds.setAttr(proxy + "." + HIDDEN_ATTR, True)
    return proxy


def update_lookdev(groom):
    """The groom's curves written again (a new file: one the viewport has open stays) and put in
    its stage in place of the last."""
    for proxy in proxies_of(groom):
        revision = cmds.getAttr(proxy + "." + REVISION_ATTR) + 1
        path = _write_curves(groom, cmds.getAttr(proxy + "." + TOKEN_ATTR), revision)
        stage = _stage(proxy)
        layer = stage.GetRootLayer()
        old = list(layer.subLayerPaths)
        del layer.subLayerPaths[:]
        layer.subLayerPaths.append(path)
        cmds.setAttr(proxy + "." + REVISION_ATTR, revision)
        for stale in old:
            try:
                if os.path.exists(stale) and stale != path:
                    os.remove(stale)
            except OSError:
                pass  # still open: left
        return path
    return None


def remove_lookdev(groom):
    for proxy in proxies_of(groom):
        if cmds.getAttr(proxy + "." + HIDDEN_ATTR) and cmds.objExists(groom):
            cmds.setAttr(groom + ".lodVisibility", True)
        _unwatch_material(proxy)
        stem = _stem(groom, cmds.getAttr(proxy + "." + TOKEN_ATTR))
        cmds.delete(cmds.listRelatives(proxy, parent=True, fullPath=True)[0])
        folder, name = os.path.split(stem)
        for f in os.listdir(folder):
            if f.startswith(name + "."):
                try:
                    os.remove(os.path.join(folder, f))
                except OSError:
                    pass  # still open: left


# ---------------------------------------------------------------------------
# The material kept in step with the Hypershade

def update_shader(stage, node, path=MATERIAL_PATH):
    """`node`'s params set again on its USD shader (an edit in the Hypershade): in place, so the
    viewport's renderer updates that shader alone. False when the material has no such shader."""
    from pxr import UsdShade
    prim = stage.GetPrimAtPath("%s/%s" % (path, _usd_name(node)))
    if not prim:
        return False
    shader = UsdShade.Shader(prim)
    for attr in _params(node):
        plug = "%s.%s" % (node, attr)
        if cmds.listConnections(plug, source=True, destination=False):
            continue  # (a connection: rebuilt with the material when it changes)
        if _is_default(node, attr):
            existing = shader.GetInput(attr)
            if existing and existing.GetAttr().HasAuthoredValue():
                existing.GetAttr().Clear()
        else:
            _set_input(shader, node, attr)
    return True


def _watch_material(proxy, shading_group):
    import maya.api.OpenMaya as om2
    import maya.utils
    _unwatch_material(proxy)
    ids = []
    # The shading network alone: upstream of the surface shader, no DAG node (the shading group's
    # history holds its members, the groom among them, and hiding it then rebuilt the material while
    # the renderer started: XPU stops on that).
    surfaces = cmds.listConnections(shading_group + ".surfaceShader", source=True, destination=False) or []
    history = cmds.listHistory(surfaces[0]) if surfaces else []
    dag = set(cmds.ls(history, dag=True) or [])
    nodes = [n for n in history if n not in dag and cmds.nodeType(n) != "shadingEngine"]
    uuid = cmds.ls(proxy, uuid=True)[0]

    pending = {"nodes": set(), "rebuild": False}

    def changed(message, plug, other, *_):
        connection = message & (om2.MNodeMessage.kConnectionMade | om2.MNodeMessage.kConnectionBroken)
        if not (connection or message & om2.MNodeMessage.kAttributeSet):
            return
        if connection:
            pending["rebuild"] = True
        else:
            pending["nodes"].add(om2.MFnDependencyNode(plug.node()).name())
        key = ("material", uuid)
        if key in _state["queued"]:
            return
        _state["queued"].add(key)

        def run():
            _state["queued"].discard(key)
            nodes, rebuild = set(pending["nodes"]), pending["rebuild"]
            pending["nodes"].clear()
            pending["rebuild"] = False
            found = cmds.ls(uuid, long=True)
            if not found:
                return
            try:
                stage = _stage(found[0])
                # A value: that shader's inputs in place. A connection (or a node the material
                # lacks): the material again.
                if rebuild or not all(update_shader(stage, n) for n in nodes if cmds.objExists(n)):
                    write_material(stage, shading_group)
            except Exception as e:  # noqa: BLE001 (on idle: warn and go on)
                cmds.warning("GroomLab: XPU lookdev material: %s" % e)

        maya.utils.executeDeferred(run)

    for node in nodes:
        sel = om2.MSelectionList()
        try:
            sel.add(node)
        except RuntimeError:
            continue
        ids.append(om2.MNodeMessage.addAttributeChangedCallback(sel.getDependNode(0), changed))
    _state["material_callbacks"][uuid] = ids


def _unwatch_material(proxy):
    import maya.api.OpenMaya as om2
    found = cmds.ls(proxy, uuid=True)
    for cb in _state["material_callbacks"].pop(found[0] if found else None, []):
        try:
            om2.MMessage.removeCallback(cb)
        except RuntimeError:
            pass


# ---------------------------------------------------------------------------
# On and off

def _model_panels():
    return cmds.getPanel(type="modelPanel") or []


def _set_override(panel, name):
    try:
        cmds.modelEditor(panel, edit=True, rendererOverrideName=name)
    except RuntimeError:
        return False
    return cmds.modelEditor(panel, query=True, rendererOverrideName=True) == name


def is_on():
    return any(proxies_of(g) for g in cmds.ls(type="groomLabShape", long=True) or [])


def enable(panel=None):
    """XPU lookdev on: every groom's proxy shape (add_lookdev), and `panel` (default: the one with
    focus, else the first model panel) drawing with RenderMan XPU (on the GPU, else the CPU).
    Returns the override set. Raises RuntimeError when something it needs is missing (check())."""
    load()
    grooms = cmds.ls(type="groomLabShape", long=True, noIntermediate=True) or []
    if not grooms:
        raise RuntimeError("no groom in the scene")
    for groom in grooms:
        add_lookdev(groom)
    panels = _model_panels()
    if panel is None:
        focus = cmds.getPanel(withFocus=True)
        panel = focus if focus in panels else (panels[0] if panels else None)
    used = None
    if panel:
        previous = cmds.modelEditor(panel, query=True, rendererOverrideName=True) or ""
        if previous.startswith("mayaHydraRenderOverride_"):
            # Never from one Hydra renderer straight to another (Maya 2025 crashed doing so):
            # through Viewport 2.0.
            _set_override(panel, "")
            cmds.refresh(force=True)
            previous = ""
        for name in XPU_OVERRIDES:
            if _set_override(panel, name):
                used = name
                _state["panels"][panel] = previous
                break
    _install_guards()
    return used


def disable():
    """XPU lookdev off: the panels back to what they drew with, the proxy shapes deleted, the
    grooms shown again."""
    for panel, previous in list(_state["panels"].items()):
        if cmds.modelPanel(panel, exists=True):
            _set_override(panel, "")  # (Viewport 2.0: never one Hydra renderer straight to another)
    _state["panels"].clear()
    for panel in _model_panels():  # (any other panel drawing with XPU: back to Viewport 2.0)
        if (cmds.modelEditor(panel, query=True, rendererOverrideName=True) or "") in XPU_OVERRIDES:
            _set_override(panel, "")
    for groom in cmds.ls(type="groomLabShape", long=True) or []:
        remove_lookdev(groom)
    _remove_guards()


def toggle():
    if is_on():
        disable()
        return False
    enable()
    return True


# ---------------------------------------------------------------------------
# Updates and guards

def _queue_update(uuid):
    import maya.utils
    if uuid in _state["queued"]:
        return
    _state["queued"].add(uuid)

    def run():
        _state["queued"].discard(uuid)
        found = cmds.ls(uuid, long=True)
        if found:
            try:
                update_lookdev(found[0])
            except Exception as e:  # noqa: BLE001
                cmds.warning("GroomLab: XPU lookdev: %s" % e)

    maya.utils.executeDeferred(run)


def _install_guards():
    """While on: a groom's change rewrites its curves; a save writes the scene without XPU in its
    panels (a scene or prefs opened with it before RenderMan loads crash RenderMan) and with the
    grooms shown; quitting turns it off first."""
    import maya.api.OpenMaya as om2
    from groomlab import renderman as rm
    if _state["callbacks"]:
        return
    ids = _state["callbacks"]
    for groom in cmds.ls(type="groomLabShape", long=True) or []:
        uuid = cmds.ls(groom, uuid=True)[0]
        sel = om2.MSelectionList()
        sel.add(groom)

        def dirty(node, plug, *_args, uuid=uuid):
            if plug.partialName(useLongNames=True) in rm.WATCHED:
                _queue_update(uuid)

        ids.append(om2.MNodeMessage.addNodeDirtyPlugCallback(sel.getDependNode(0), dirty))

    held = {}

    def before_save(*_):
        held["panels"] = {p: cmds.modelEditor(p, query=True, rendererOverrideName=True) for p in _model_panels()}
        for p, name in held["panels"].items():
            if name in XPU_OVERRIDES:
                _set_override(p, "")
        held["grooms"] = []
        for groom in cmds.ls(type="groomLabShape", long=True) or []:
            if any(cmds.getAttr(p + "." + HIDDEN_ATTR) for p in proxies_of(groom)):
                cmds.setAttr(groom + ".lodVisibility", True)
                held["grooms"].append(groom)

    def after_save(*_):
        for p, name in held.get("panels", {}).items():
            if name in XPU_OVERRIDES and cmds.modelPanel(p, exists=True):
                _set_override(p, name)
        for groom in held.get("grooms", []):
            if cmds.objExists(groom):
                cmds.setAttr(groom + ".lodVisibility", False)

    def scene_changed(*_):  # a new or opened scene: the old proxies are gone with it
        _state["panels"].clear()
        _remove_guards()

    ids.append(om2.MSceneMessage.addCallback(om2.MSceneMessage.kBeforeSave, before_save))
    ids.append(om2.MSceneMessage.addCallback(om2.MSceneMessage.kAfterSave, after_save))
    ids.append(om2.MSceneMessage.addCallback(om2.MSceneMessage.kMayaExiting, lambda *_: disable()))
    ids.append(om2.MSceneMessage.addCallback(om2.MSceneMessage.kBeforeNew, lambda *_: disable()))
    ids.append(om2.MSceneMessage.addCallback(om2.MSceneMessage.kBeforeOpen, lambda *_: disable()))
    ids.append(om2.MSceneMessage.addCallback(om2.MSceneMessage.kAfterNew, scene_changed))


def _remove_guards():
    import maya.api.OpenMaya as om2
    for cb in _state["callbacks"]:
        try:
            om2.MMessage.removeCallback(cb)
        except RuntimeError:
            pass
    del _state["callbacks"][:]
