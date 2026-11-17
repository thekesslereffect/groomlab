# SPDX-License-Identifier: Apache-2.0
"""RenderMan for Maya: a groom rendered at render time by groomlab_renderman through RfM's own
generic RenderManProcedural node, with no GroomLab translator in RfM. EXPERIMENTAL: it needs
RenderMan for Maya 27 on Maya 2024 to 2026; rendered in all three with RfM 27.3 (the procedural's
render path is tested: tests/hosts/renderman, prman with the same call).

RfM's RenderManProcedural node drives RenderMan's older procedural API: `__dsoname` names the
plugin and its `data` string goes to the plugin's ConvertParameters as `-key value` pairs (the
procedural's header, hosts/renderman/groomlab_renderman.cpp): -file, -frame, -fps, -density,
-widthScale, -lengthScale, -motionStart, -motionEnd, -motionKeys, -searchPaths, -name, -debug. The
node's own `frame` param never reaches an old-style plugin, so the frame goes in the data string.

add_renderman_procedural(groom) makes the node, linked to the groom (a message connection), with a
pre-shape Python script RfM runs before translating it (each frame, each IPR restart): pre_shape
writes the groom to a .groom file at the current frame (or takes the groom's own groom file when it
renders from one: Render > Render from Groom File, which caches the scalp's animation) and puts
-file and -frame (and the shutter from rmanGlobals) into the data string, keeping the user's other
keys. The data-string helpers here need no Maya (the prman tests use them).
"""

import os
import sys

try:
    from maya import cmds
except ImportError:  # the data-string helpers alone, outside Maya (tests/hosts/renderman)
    cmds = None

RFM_PLUGIN = "RenderMan_for_Maya"
NODE_TYPE = "RenderManProcedural"
DSO_FILE = "groomlab_renderman" + (".dll" if sys.platform == "win32" else ".so")
DSO_ENV = "GROOMLAB_RENDERMAN_PROCEDURAL"  # a path to the plugin (or its folder) over the module's
GROOM_ATTR = "groomLabGroom"  # message: the groom the node renders
SHUTTER_ATTR = "groomLabShutterFromGlobals"  # bool: pre_shape writes the motion keys from rmanGlobals
# RfM's Shading Group on a RenderManProcedural (config/extensions.json): the node takes no set
# membership, so a shading group's message connected here is how its material reaches the curves.
SHADING_GROUP_ATTR = "rman__torattr___customShadingGroup"
# int on the groom shape, dirty with any change a render of it shows (its value counts them).
CHANGES_ATTR = "outVrayPluginData"

# The data string's keys, as the procedural spells them, by this module's argument names.
DATA_KEYS = (("file", "file"), ("frame", "frame"), ("fps", "fps"), ("density", "density"),
             ("width_scale", "widthScale"), ("length_scale", "lengthScale"), ("motion_start", "motionStart"),
             ("motion_end", "motionEnd"), ("motion_keys", "motionKeys"), ("search_paths", "searchPaths"),
             ("name", "name"), ("debug", "debug"), ("revision", "revision"))
_KEY_BY_ARG = dict(DATA_KEYS)
_KEY_CASE = dict((key.lower(), key) for _, key in DATA_KEYS)
_KEY_CASE["filename"] = "file"


# ---------------------------------------------------------------------------
# The data string (no Maya): the procedural's parser, in Python


def split_words(text):
    """The data string in words as the procedural splits it: whitespace between words, double or
    single quotes around a word with spaces (the quotes go; nothing escaped inside them)."""
    words, i, n = [], 0, len(text or "")
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        word = ""
        while i < n and not text[i].isspace():
            c = text[i]
            if c in "\"'":
                end = text.find(c, i + 1)
                if end < 0:
                    word, i = word + text[i + 1:], n
                else:
                    word, i = word + text[i + 1:end], end + 1
            else:
                word, i = word + c, i + 1
        words.append(word)
    return words


def _is_key(word):
    return len(word) > 1 and word[0] == "-" and word[1].isalpha()


def _is_int(word):
    try:
        return float(word) == int(float(word))
    except ValueError:
        return False


def parse_data(text):
    """The data string's (key, value) pairs in order, keys as the procedural spells them (any case
    in; -filename is -file) and unknown ones as written; -debug alone is ("debug", "1"). A word
    that is no key is dropped, as the procedural ignores it."""
    words, pairs, i = split_words(text), [], 0
    while i < len(words):
        word = words[i]
        i += 1
        if not _is_key(word):
            continue
        key = _KEY_CASE.get(word[1:].lower(), word[1:])
        value = words[i] if i < len(words) else None
        if key == "debug":
            if value is not None and _is_int(value):
                i += 1
            else:
                value = "1"
        elif key in _KEY_CASE.values():
            if value is None:
                continue
            i += 1
        elif value is not None and not _is_key(value):
            i += 1
        else:
            value = None
        pairs.append((key, value))
    return pairs


def quote(value):
    """A value as one word of the data string: quoted when it has spaces (double quotes, single
    ones when it holds a double quote)."""
    value = str(value)
    if value and not any(c.isspace() or c in "\"'" for c in value):
        return value
    if '"' not in value:
        return '"%s"' % value
    if "'" not in value:
        return "'%s'" % value
    raise ValueError("a data-string value cannot hold both quote characters: %r" % value)


def format_number(value):
    """A number as the data string has it: 12, 12.5, -0.25 (no exponent, no trailing zeros)."""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    text = ("%.6f" % float(value)).rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def _format(key, value):
    if key in ("file", "name"):
        return quote(str(value).replace("\\", "/") if key == "file" else value)
    if key == "searchPaths":
        if isinstance(value, (list, tuple)):
            value = ";".join(str(v) for v in value)
        return quote(str(value).replace("\\", "/"))
    if key == "motionKeys":
        return str(int(value))
    if key == "debug":
        return "1" if value else "0"
    if isinstance(value, str):
        return quote(value)
    return format_number(value)


def build_data(**values):
    """A data string from this module's argument names (DATA_KEYS: file, frame, fps, density,
    width_scale, length_scale, motion_start, motion_end, motion_keys, search_paths (a list or a
    ';' string), name, debug); None leaves a key out, debug False too."""
    return update_data("", **values)


def update_data(text, **values):
    """The data string `text` with these keys set (build_data's names): a key already there keeps
    its place, a new one goes at the end, None removes it (debug False too). The other keys,
    unknown ones included, stay as they are."""
    unknown = set(values) - set(_KEY_BY_ARG)
    if unknown:
        raise TypeError("unknown data-string argument(s): %s" % ", ".join(sorted(unknown)))
    # Each key with its value as it is written (quoted again where it has spaces).
    words = [[key, None if value is None else quote(value)] for key, value in parse_data(text)]
    for arg, key in DATA_KEYS:
        if arg not in values:
            continue
        value = values[arg]
        if value is None or (key == "debug" and value is False):
            words = [w for w in words if w[0] != key]
            continue
        written = _format(key, value)
        same = [w for w in words if w[0] == key]
        if same:
            same[0][1] = written
            words = [w for w in words if w[0] != key or w is same[0]]
        else:
            words.append([key, written])
    return " ".join("-%s %s" % (key, value) if value is not None else "-" + key for key, value in words)


# ---------------------------------------------------------------------------
# Finding things: RfM, the plugin, the node, the groom


def rfm_loaded():
    """Whether RenderMan for Maya's plugin is loaded (False outside Maya, or on a Maya it never loads in)."""
    if cmds is None:
        return False
    try:
        return bool(cmds.pluginInfo(RFM_PLUGIN, query=True, loaded=True))
    except RuntimeError:
        return False


def module_root():
    """The folder of the GroomLab module these scripts are in: three folders up from this file
    (<module>/scripts/groomlab/renderman.py), forward slashes."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(os.path.dirname(here)).replace("\\", "/")


def _maya_module_root():
    """Maya's path of the GroomLab module (a source checkout's scripts are not in one), or None."""
    if cmds is None:
        return None
    try:
        return cmds.getModulePath(moduleName="GroomLab") or None
    except (RuntimeError, TypeError):
        return None


def dso_path(root=None):
    """The procedural plugin for the node's `__dsoname`: $GROOMLAB_RENDERMAN_PROCEDURAL (the file,
    or its folder) when set, else <module>/procedurals/renderman/groomlab_renderman.dll, the module
    `root` (default: these scripts' own, then the one Maya loaded). Forward slashes; None when none
    is there."""
    candidates = []
    env = os.environ.get(DSO_ENV, "")
    if env:
        candidates.append(os.path.join(env, DSO_FILE) if os.path.isdir(env) else env)
    roots = [root] if root else [module_root(), _maya_module_root()]
    candidates += [os.path.join(r, "procedurals", "renderman", DSO_FILE) for r in roots if r]
    for path in candidates:
        if os.path.isfile(path):
            return os.path.abspath(path).replace("\\", "/")
    return None


def pre_shape_script(node):
    """The node's preShapePythonScript: pre_shape on the node named by its UUID (renames keep it)."""
    uuid = cmds.ls(node, uuid=True)[0]
    return "import groomlab.renderman\ngroomlab.renderman.pre_shape(%r)\n" % uuid


def _rfm_current_shape():
    """The shape RfM is translating (rfm2.api.strings' shape vars, set before its pre-shape
    script), when RfM is there; else None."""
    try:
        from rfm2.api import strings  # pylint: disable=import-error
    except ImportError:
        return None
    for var in ("shapepath", "shape", "node"):
        try:
            value = strings.get_var(var)
        except Exception:  # noqa: BLE001 (RfM's own errors: whichever it raises, fall back)
            continue
        if value and cmds.objExists(value) and cmds.attributeQuery(GROOM_ATTR, node=value, exists=True):
            return value
    return None


def procedural_node(node_or_uuid):
    """The procedural node pre_shape works on: RfM's current shape when it is a GroomLab one, else
    the node of this name or UUID; None when there is none."""
    current = _rfm_current_shape()
    if current:
        return current
    found = cmds.ls(node_or_uuid, long=True) or []
    return found[0] if found else None


def groom_of(node):
    """The groom a procedural node renders (its groomLabGroom connection), or None."""
    if not cmds.attributeQuery(GROOM_ATTR, node=node, exists=True):
        return None
    sources = cmds.listConnections(node + "." + GROOM_ATTR, source=True, destination=False, shapes=True) or []
    return sources[0] if sources else None


def frame_tag(frame):
    """A frame in a file name: 0012, or 0012_500 for a subframe."""
    frame = float(frame)
    if frame == int(frame):
        return "%04d" % int(frame)
    return ("%08.3f" % frame).replace(".", "_")


def groom_file_for(groom, frame):
    """The .groom the procedural reads at `frame`: (path, export). The groom's own groom file when it
    renders from one (renderMode 1: Render > Render from Groom File, the scalp's animation cached),
    else <scene's renders folder>/renderman/<groom>.<frame>.groom, to export (one file a frame, so
    RIB written ahead for a farm keeps each frame's)."""
    from groomlab.files import groom_file
    from groomlab.paths import asset_folder, asset_name

    if cmds.getAttr(groom + ".renderMode") == 1:
        linked = groom_file(groom)
        if linked:
            return linked, False
    folder = asset_folder("renders") + "/renderman"
    return "%s/%s.%s.groom" % (folder, asset_name(groom), frame_tag(frame)), True


def shutter():
    """RfM's shutter as the procedural's motion keys, in frames from the frame: (start, end, keys),
    from rmanGlobals (3D motion blur on: its shutter angle, timing and motion samples); None when
    motion blur is off or there is no rmanGlobals."""
    globals_node = "rmanGlobals"
    if not cmds.objExists(globals_node):
        return None

    def get(attr, default):
        try:
            return cmds.getAttr("%s.%s" % (globals_node, attr))
        except (RuntimeError, ValueError):
            return default

    if get("motionBlur", 0) != 1:
        return None
    length = float(get("shutterAngle", 180)) / 360.0
    timing = get("shutterTiming", 0)  # frame open, center, close
    start = {0: 0.0, 1: -0.5 * length, 2: -length}.get(timing, 0.0)
    keys = max(2, int(get("motionSamples", 2)))
    return start, start + length, keys


def export_free(groom, path):
    """Write the groom to `path`, or, while RenderMan still has that file open, to the first
    `<stem>.ipr<n>.groom` beside it that can be written: an interactive render renders the frame's
    last file, and Windows refuses to replace an open file, so an update would otherwise keep
    showing the groom as it was. The `.ipr<n>` files of earlier updates are removed when free.
    Returns the path written."""
    from groomlab.files import export_groom
    stem = path[:-len(".groom")] if path.endswith(".groom") else path
    candidates = [path] + ["%s.ipr%d.groom" % (stem, n) for n in range(1, 33)]
    written, error = None, None
    for candidate in candidates:
        try:
            export_groom(groom, candidate)
            written = candidate
            break
        except RuntimeError as e:
            if not os.path.exists(candidate):  # not a file in use: a real failure
                raise
            error = e
    if written is None:
        raise error
    for leftover in candidates[1:]:
        if leftover != written and os.path.exists(leftover):
            try:
                os.remove(leftover)
            except OSError:
                pass  # still open: the next update removes it
    return written


# ---------------------------------------------------------------------------
# The pre-shape script and the node


# RenderMan's XPU renders no procedural of any kind (prman: "Geometry ... is of unsupported type:
# Ri:Procedural"; RfM's own RenderManArchive is one too), so it leaves grooms out. RfM renders XGen
# Interactive Groom there through a translator of its own that writes plain curves.
XPU_NOTE = ("RenderMan XPU renders no procedurals, and GroomLab renders its grooms there through XGen, which "
            "is not loaded: load xgenToolkit, or render with RIS (Render Settings > Features > Renderer)")
_xpu_warned = [None]  # the scene the warning was given for


def renderer_is_xpu():
    """Whether RfM's render settings (rmanGlobals.renderVariant) pick its XPU renderer."""
    if cmds is None or not cmds.objExists("rmanGlobals") or not cmds.attributeQuery("renderVariant", node="rmanGlobals",
                                                                                     exists=True):
        return False
    return (cmds.getAttr("rmanGlobals.renderVariant") or "").lower().startswith("xpu")


def warn_if_xpu(force=False):
    """The XPU warning (XPU_NOTE), when RfM renders with XPU: once per scene (each frame and procedural
    runs pre_shape), or again with `force`. Returns whether it warned."""
    if not renderer_is_xpu():
        _xpu_warned[0] = None
        return False
    scene = cmds.file(query=True, sceneName=True) or "untitled"
    if _xpu_warned[0] == scene and not force:
        return False
    _xpu_warned[0] = scene
    cmds.warning("GroomLab: " + XPU_NOTE)
    return True


# ---------------------------------------------------------------------------
# RenderMan XPU through XGen (issue #82): XPU renders no procedural, but RfM renders an XGen
# Interactive Groom there through its own translator. Each groom gets one (<groom>_xpu), fed by
# a spline cache GroomLab writes (groomLabExportAlembic -xgen), hidden in the viewport, rendered
# only while rmanGlobals.renderVariant picks XPU (RIS renders the procedural). XGen's curves
# carry no primvars, so the groom renders with one look: its own RenderMan material when it has
# one (assigned in the Hypershade), else a PxrHairColor + PxrMarschnerHair network set to the
# groom's look (create_hair_network with constants), which the artist changes in the Hypershade.

XGEN_PLUGIN = "xgenToolkit"
XPU_ATTR = "groomLabXpuGroom"  # message on the XGen description: the groom it renders in XPU
XPU_REVISION_ATTR = "groomLabXpuRevision"  # int: the cache file's number (a new file each update)


def _xgen_descriptions():
    """Every XGen Interactive Groom description, [] while XGen is not loaded (asking for its type
    then makes Maya warn "Unknown object type")."""
    try:
        if not cmds.pluginInfo(XGEN_PLUGIN, query=True, loaded=True):
            return []
    except RuntimeError:
        return []
    return cmds.ls(type="xgmSplineDescription", long=True) or []


def xgen_available():
    """Whether XGen's plugin is (or can be) loaded."""
    try:
        if not cmds.pluginInfo(XGEN_PLUGIN, query=True, loaded=True):
            cmds.loadPlugin(XGEN_PLUGIN, quiet=True)
        return bool(cmds.pluginInfo(XGEN_PLUGIN, query=True, loaded=True))
    except RuntimeError:
        return False


def xpu_grooms_of(groom):
    """The XGen descriptions rendering `groom` in XPU (its message into their groomLabXpuGroom)."""
    plugs = cmds.listConnections(groom + ".message", source=False, destination=True, plugs=True) or []
    nodes = [p.split(".")[0] for p in plugs if p.endswith("." + XPU_ATTR)]
    return (cmds.ls(nodes, long=True, type="xgmSplineDescription") or []) if nodes else []


def _xpu_cache_path(groom, revision):
    """<scene's renders folder>/renderman/<groom>.xpu.<revision>.abc."""
    from groomlab.paths import asset_folder, asset_name
    folder = asset_folder("renders") + "/renderman"
    if not os.path.isdir(folder):
        os.makedirs(folder)
    return "%s/%s.xpu.%d.abc" % (folder, asset_name(groom), revision)


def _write_xpu_cache(groom, revision):
    """The groom's render strands at the current frame as XGen's spline cache: (path, its look)."""
    import json
    path = _xpu_cache_path(groom, revision)
    result = json.loads(cmds.groomLabExportAlembic(groom, file=path, xgen=True, renderOverrides=True))
    return path, result.get("look") or {}


def _base_name(groom):
    return cmds.listRelatives(groom, parent=True)[0].split("|")[-1].split(":")[-1]


def xpu_shading_group(groom, look=None):
    """The material the groom renders with in XPU: its own RenderMan shading group (assigned in
    the Hypershade, the procedural's too), else <groom>_xpu_SG, made from `look` (the -xgen
    export's) when it is not there yet."""
    sg = groom_shading_group(groom)
    if sg:
        return sg
    name = _base_name(groom) + "_xpu"
    if cmds.objExists(name + "_SG") and cmds.nodeType(name + "_SG") == "shadingEngine":
        return name + "_SG"
    return create_hair_network(name, constants=look or {})


def _remove_files(paths):
    for path in paths:
        try:
            if os.path.exists(path):
                os.remove(path)
        except OSError:
            pass  # still open (XGen, an interactive render): the next update removes it


def add_xpu_groom(groom):
    """The groom's XGen Interactive Groom for RenderMan XPU (<groom>_xpu): made from a fresh spline
    cache, or, when it exists, its cache written again (update_xpu_groom). Returns its description,
    or None when XGen is not available."""
    from groomlab.base import _shape_of
    groom = _shape_of(groom, "groomLabShape")
    existing = xpu_grooms_of(groom)
    if existing:
        update_xpu_groom(groom)
        # A RenderMan material assigned to the groom since (in the Hypershade) is the XPU hair's too.
        own = groom_shading_group(groom)
        if own and own not in (cmds.listConnections(existing[0] + ".instObjGroups", source=False, destination=True,
                                                    type="shadingEngine") or []):
            cmds.sets(existing[0], edit=True, forceElement=own)
        return existing[0]
    if not xgen_available():
        return None
    path, look = _write_xpu_cache(groom, 1)
    before = set(cmds.ls(long=True))
    import maya.mel
    maya.mel.eval('xgmSplineCache -import -file "%s"' % path.replace("\\", "/"))
    made = [n for n in cmds.ls(long=True) if n not in before]
    descriptions = cmds.ls(made, type="xgmSplineDescription", long=True) or []
    if not descriptions:
        raise RuntimeError("XGen made no Interactive Groom of %s" % path)
    description = descriptions[0]
    caches = cmds.ls(made, type="xgmSplineCache") or []
    # XGen's own default material (hairPhysicalShader) goes: the groom's replaces it.
    for node in cmds.ls(made, type=("hairPhysicalShader", "shadingEngine", "materialInfo")) or []:
        if cmds.objExists(node):
            cmds.delete(node)
    base = _base_name(groom) + "_xpu"
    transform = cmds.listRelatives(description, parent=True, fullPath=True)[0]
    transform = cmds.rename(transform, base)
    description = cmds.ls(cmds.rename(cmds.listRelatives(transform, shapes=True, fullPath=True)[0], base + "Shape"),
                          long=True)[0]
    if caches:
        cmds.rename(caches[0], base + "_cache")
    cmds.addAttr(description, longName=XPU_ATTR, attributeType="message")
    cmds.connectAttr(groom + ".message", description + "." + XPU_ATTR, force=True)
    cmds.addAttr(description, longName=XPU_REVISION_ATTR, attributeType="long", defaultValue=1)
    # Rendered, never drawn: the groom draws itself (lodVisibility hides it in the viewport only;
    # visibility, which RfM honors, follows the render variant: xpu_visibility).
    cmds.setAttr(description + ".lodVisibility", False)
    for attr in ("visibleInReflections", "visibleInRefractions"):  # strands lighting strands
        if cmds.attributeQuery(attr, node=description, exists=True):
            cmds.setAttr(description + "." + attr, True)
    for attr in ("translate", "rotate", "scale"):  # the cache is in world space
        cmds.setAttr("%s.%s" % (transform, attr), lock=True)
    cmds.sets(description, edit=True, forceElement=xpu_shading_group(groom, look))
    xpu_visibility()
    return description


def _xpu_cache_node(description):
    caches = cmds.listConnections(description + ".inSplineData", source=True, destination=False,
                                  type="xgmSplineCache") or []
    return caches[0] if caches else None


def update_xpu_groom(groom):
    """Write the groom's spline cache again (a new file: Windows keeps one XGen or a render has
    open from being replaced) and point its XGen Interactive Groom at it. Returns the path, or
    None without one."""
    for description in xpu_grooms_of(groom):
        cache = _xpu_cache_node(description)
        if not cache:
            continue
        revision = cmds.getAttr(description + "." + XPU_REVISION_ATTR) + 1
        old = (cmds.getAttr(cache + ".fileName") or "").replace("\\", "/")
        path, _look = _write_xpu_cache(groom, revision)
        from groomlab.base import no_undo
        with no_undo():
            cmds.setAttr(description + "." + XPU_REVISION_ATTR, revision)
            cmds.setAttr(cache + ".fileName", path, type="string")
        # Earlier files of this groom (the one just left may still be open: the next update takes it).
        folder = os.path.dirname(path)
        stem = os.path.basename(path).split(".xpu.")[0] + ".xpu."
        _remove_files([folder + "/" + f for f in os.listdir(folder)
                       if f.startswith(stem) and f.endswith(".abc") and folder + "/" + f not in (path, old)])
        _remove_files([old] if old and old != path else [])
        return path
    return None


def xpu_visibility():
    """Each XGen groom renders in XPU only (its visibility on while rmanGlobals.renderVariant picks
    XPU and its groom is visible), and each procedural in RIS only: the hair renders once, whichever
    renderer is picked."""
    xpu = renderer_is_xpu()
    from groomlab.base import no_undo
    with no_undo():
        for description in _xgen_descriptions():
            if not cmds.attributeQuery(XPU_ATTR, node=description, exists=True):
                continue
            groom = (cmds.listConnections(description + "." + XPU_ATTR, source=True, destination=False, shapes=True)
                     or [None])[0]
            shown = bool(xpu and groom and cmds.getAttr(groom + ".visibility") and
                         cmds.getAttr(cmds.listRelatives(groom, parent=True, fullPath=True)[0] + ".visibility"))
            if cmds.getAttr(description + ".visibility") != shown:
                cmds.setAttr(description + ".visibility", shown)
        for node in cmds.ls(type=NODE_TYPE, long=True) or []:
            if cmds.attributeQuery(GROOM_ATTR, node=node, exists=True) and cmds.getAttr(node + ".visibility") == xpu:
                cmds.setAttr(node + ".visibility", not xpu)


def sync_xpu():
    """While RfM renders with XPU: every groom gets its XGen groom (add_xpu_groom, its cache
    written at the current frame), and one whose groom is gone is deleted; with RIS, the XGen grooms
    are left hidden. Never raises. Returns the descriptions."""
    found = []
    try:
        for description in _xgen_descriptions():
            if cmds.attributeQuery(XPU_ATTR, node=description, exists=True) and not cmds.listConnections(
                    description + "." + XPU_ATTR, source=True, destination=False):
                cache = _xpu_cache_node(description)
                cmds.delete([n for n in (cmds.listRelatives(description, parent=True, fullPath=True)[0], cache) if n])
        if renderer_is_xpu():
            if not xgen_available():
                warn_if_xpu()
            else:
                for groom in cmds.ls(type="groomLabShape", long=True, noIntermediate=True) or []:
                    description = add_xpu_groom(groom)
                    if description:
                        found.append(description)
        xpu_visibility()
    except Exception as e:  # noqa: BLE001 (a callback: whatever went wrong, warn and go on)
        cmds.warning("GroomLab: RenderMan XPU: %s" % e)
    return found


def pre_shape(node_or_uuid):
    """RfM's pre-shape script for a GroomLab procedural node: the groom written to its .groom at the
    current frame (groom_file_for), and -file, -frame and (with groomLabShutterFromGlobals on) the
    shutter's motion keys put into the node's data string, the user's other keys kept. Never raises
    (a render goes on): a problem is a warning. Returns the data string, or None."""
    try:
        node = procedural_node(node_or_uuid)
        if not node:
            cmds.warning("GroomLab: RenderMan procedural %s not found" % node_or_uuid)
            return None
        groom = groom_of(node)
        if not groom:
            cmds.warning("GroomLab: %s renders no groom (connect a groom's message to its %s)" % (node, GROOM_ATTR))
            return None
        frame = cmds.currentTime(query=True)
        path, export = groom_file_for(groom, frame)
        if export:
            folder = os.path.dirname(path)
            if not os.path.isdir(folder):
                os.makedirs(folder)
            path = export_free(groom, path)
        values = {"file": path, "frame": frame}
        if not cmds.attributeQuery(SHUTTER_ATTR, node=node, exists=True) or cmds.getAttr(node + "." + SHUTTER_ATTR):
            keys = shutter()
            values.update(motion_start=keys[0] if keys else None, motion_end=keys[1] if keys else None,
                          motion_keys=keys[2] if keys else None)
        if cmds.attributeQuery(CHANGES_ATTR, node=groom, exists=True):
            # The groom's change counter: a change makes the data string differ, so an interactive
            # render renders the procedural again (the file's name stays the frame's). Read, it is
            # clean again: the groom's next change dirties it (watch_grooms).
            values["revision"] = cmds.getAttr(groom + "." + CHANGES_ATTR)
        current = cmds.getAttr(node + ".data") or ""
        data = update_data(current, **values)
        if data != current:  # an unchanged write would make RfM's IPR translate the node again, endlessly
            cmds.setAttr(node + ".data", data, type="string")
        return data
    except Exception as e:  # noqa: BLE001 (a render must go on: report whatever went wrong)
        try:
            cmds.warning("GroomLab: RenderMan procedural: %s" % e)
        except Exception:  # noqa: BLE001
            pass
        return None


def link_procedural(node, groom, density=None, width_scale=None, length_scale=None, debug=False,
                    shutter_from_globals=True):
    """Make `node` (a RenderManProcedural, or any node with a string `data` and a
    `preShapePythonScript`) render `groom`: the groom's message into its groomLabGroom, the data
    string's settings (the name, density, scales, debug) and the pre-shape script that fills in the
    rest at render time."""
    from groomlab.base import _shape_of
    groom = _shape_of(groom, "groomLabShape") if cmds.nodeType(groom) != "groomLabShape" else groom
    if not cmds.attributeQuery(GROOM_ATTR, node=node, exists=True):
        cmds.addAttr(node, longName=GROOM_ATTR, attributeType="message")
    cmds.connectAttr(groom + ".message", node + "." + GROOM_ATTR, force=True)
    if not cmds.attributeQuery(SHUTTER_ATTR, node=node, exists=True):
        cmds.addAttr(node, longName=SHUTTER_ATTR, attributeType="bool", defaultValue=True)
    cmds.setAttr(node + "." + SHUTTER_ATTR, bool(shutter_from_globals))
    transform = cmds.listRelatives(groom, parent=True)[0]
    data = update_data(cmds.getAttr(node + ".data") or "", name=transform.split("|")[-1].split(":")[-1],
                       density=density, width_scale=width_scale, length_scale=length_scale, debug=debug)
    cmds.setAttr(node + ".data", data, type="string")
    cmds.setAttr(node + ".preShapePythonScript", pre_shape_script(node), type="string")
    return node


def add_renderman_procedural(groom, density=None, width_scale=None, length_scale=None, debug=False, shader=True,
                             shutter_from_globals=True):
    """A RenderManProcedural node (and its transform, <groom>_rman) that renders `groom` in RenderMan
    for Maya through groomlab_renderman: `__dsoname` the module's plugin, the data string's
    settings, and the pre-shape script (pre_shape) that exports the groom at each rendered frame.
    With `shader`, a PxrMarschnerHair + PxrHairColor network reading the procedural's primvars
    (create_hair_network) is assigned. The groom's own shape renders nothing in RfM.

    sync_renderman makes one for every groom while RfM is loaded, so a scene renders in RfM with
    nothing to set up. A groom that has one already keeps it (repaired: repair_procedural). Raises
    RuntimeError when RfM is not loaded or the plugin is not installed. Returns the procedural's
    shape."""
    from groomlab.base import _shape_of
    if not rfm_loaded():
        raise RuntimeError("RenderMan for Maya is not loaded (it needs RenderMan for Maya 27, Maya 2024-2026)")
    dso = dso_path()
    if not dso:
        raise RuntimeError("the RenderMan procedural (%s) is not installed under %s/procedurals/renderman" %
                           (DSO_FILE, module_root()))
    groom = _shape_of(groom, "groomLabShape")
    existing = procedurals_of(groom)
    if existing:
        repair_procedural(existing[0], groom, shader=shader)
        return existing[0]
    base = cmds.listRelatives(groom, parent=True)[0].split("|")[-1].split(":")[-1] + "_rman"
    transform = cmds.createNode("transform", name=base, skipSelect=True)
    node = cmds.createNode(NODE_TYPE, name=base + "Shape", parent=transform, skipSelect=True)
    node = cmds.ls(node, long=True)[0]
    cmds.setAttr(node + ".__dsoname", dso, type="string")
    link_procedural(node, groom, density=density, width_scale=width_scale, length_scale=length_scale, debug=debug,
                    shutter_from_globals=shutter_from_globals)
    try:  # the node's own frame (the RfM UI's; the plugin reads -frame)
        cmds.connectAttr("time1.outTime", node + ".frame", force=True)
    except RuntimeError:
        pass
    repair_procedural(node, groom, shader=shader)
    pre_shape(node)  # the data string complete now, not only at render time
    return node


def procedurals_of(groom):
    """The procedural nodes rendering `groom` (its message into their groomLabGroom), long names."""
    plugs = cmds.listConnections(groom + ".message", source=False, destination=True, plugs=True) or []
    nodes = [p.split(".")[0] for p in plugs if p.endswith("." + GROOM_ATTR)]
    return [n for n in (cmds.ls(nodes, long=True, type=NODE_TYPE) or [])]


def _is_renderman_shader(node):
    """Whether RfM renders `node` as a material: one of RenderMan's own (a Pxr or Lama node)."""
    classes = " ".join(cmds.getClassification(cmds.nodeType(node)) or [])
    return "pixar" in classes.lower() or cmds.nodeType(node).startswith(("Pxr", "Lama"))


def groom_shading_group(groom):
    """The groom's own shading group when its surface shader is a RenderMan material (assigned in
    the Hypershade), else None: RfM cannot render another renderer's (an aiStandardHair)."""
    for sg in cmds.listConnections(groom + ".instObjGroups", source=False, destination=True, type="shadingEngine") or []:
        if sg == "initialShadingGroup":
            continue
        shaders = cmds.listConnections(sg + ".surfaceShader", source=True, destination=False) or []
        if shaders and _is_renderman_shader(shaders[0]):
            return sg
    return None


def repair_procedural(node, groom, shader=True):
    """Make `node` render `groom` as it should, whenever it was made: indirect visibility on, and its
    material the groom's own RenderMan shading group (groom_shading_group), else the one it has,
    else the GroomLab hair network (create_hair_network) made for it."""
    # RfM's indirect visibility inherits these, and a new shape's are off in Maya 2025 and later (on
    # in 2024): without them the strands light no strand, and the hair renders dark.
    for attr in ("visibleInReflections", "visibleInRefractions"):
        if not cmds.getAttr(node + "." + attr):
            cmds.setAttr(node + "." + attr, True)
    if not shader:
        return
    sg = groom_shading_group(groom)
    if not sg and cmds.attributeQuery(SHADING_GROUP_ATTR, node=node, exists=True):
        current = cmds.listConnections(node + "." + SHADING_GROUP_ATTR, source=True, destination=False) or []
        sg = current[0] if current else None
    base = cmds.listRelatives(node, parent=True)[0].split("|")[-1].split(":")[-1]
    if not sg and cmds.objExists(base + "_SG") and cmds.nodeType(base + "_SG") == "shadingEngine":
        sg = base + "_SG"  # made by an earlier GroomLab, never bound (it used cmds.sets)
    if not sg:
        try:
            from groomlab.shading import hair_look
            sg = create_hair_network(base, tilt=hair_look(groom)["tilt"])
        except (RuntimeError, ValueError, TypeError) as e:
            cmds.warning("GroomLab: the RenderMan hair shader was not made (%s): see docs/DEVELOPING.md, "
                         "'RenderMan for Maya', for the network" % e)
            return
    assign_shading_group(node, sg)


def sync_renderman():
    """Every groom rendered by RenderMan for Maya with nothing to set up, run by the plugin when a
    scene opens or is imported, when RfM loads, and when a groom is made or deleted: while RfM is
    loaded, each groom gets its procedural (add_renderman_procedural) and an existing one is
    repaired (repair_procedural); a procedural whose groom is gone is deleted (it would go on
    rendering that groom's last file); with XPU picked, each its XGen groom too (sync_xpu). Without
    RfM, nothing. Never raises. Returns the procedural shapes."""
    if cmds is None or not rfm_loaded() or not dso_path():
        return []
    found = []
    try:
        for node in cmds.ls(type=NODE_TYPE, long=True) or []:
            if cmds.attributeQuery(GROOM_ATTR, node=node, exists=True) and not groom_of(node):
                transform = cmds.listRelatives(node, parent=True, fullPath=True)
                lone = transform and len(cmds.listRelatives(transform[0], children=True) or []) == 1
                cmds.delete(transform[0] if lone else node)
        for groom in cmds.ls(type="groomLabShape", long=True, noIntermediate=True) or []:
            found.append(add_renderman_procedural(groom))
        watch_grooms()
    except Exception as e:  # noqa: BLE001 (a callback: whatever went wrong, warn and go on)
        cmds.warning("GroomLab: RenderMan: %s" % e)
    sync_xpu()  # (XPU renders the grooms through XGen: their caches written now)
    return found


_sync_queued = [False]


def queue_renderman_sync():
    """sync_renderman once the command that made or deleted grooms is done (maya.utils.
    executeDeferred: Maya's evalDeferred queue can wait for the next user event, past an
    interactive render's start), once however many times this is called before (the plugin calls
    it as each groom is made or deleted, in interactive sessions). Nothing without RfM."""
    if cmds is None or _sync_queued[0] or not rfm_loaded():
        return

    def run():
        _sync_queued[0] = False
        sync_renderman()

    _sync_queued[0] = True
    import maya.utils
    maya.utils.executeDeferred(run)


def assign_shading_group(node, shading_group):
    """Shade the procedural `node`'s curves with `shading_group`: its message into RfM's Shading
    Group (SHADING_GROUP_ATTR, added as RfM's own string attribute when the node lacks it). A
    RenderManProcedural ignores cmds.sets: in the shading group's set, RfM writes no material for it
    and prman shades the hair a default grey."""
    if not cmds.attributeQuery(SHADING_GROUP_ATTR, node=node, exists=True):
        cmds.addAttr(node, longName=SHADING_GROUP_ATTR, dataType="string")
    cmds.connectAttr(shading_group + ".message", node + "." + SHADING_GROUP_ATTR, force=True)


# ---------------------------------------------------------------------------
# The shading network (hosts/renderman/examples/groom.rib's, as Maya nodes)

# (name, PxrPrimvar varname, type, default param, default) as groom.rib reads them.
PRIMVARS = (("melanin", "rman_melanin", "float", None, None),
            ("redness", "rman_redness", "float", None, None),
            ("dye", "rman_dye", "color", "defaultColor", (1.0, 1.0, 1.0)),
            ("color", "rman_color", "color", None, None),
            ("share", "rman_rgbShare", "float", None, None),
            ("gainR", "rman_specularGainR", "float", "defaultFloat", 1.75),
            ("coneR", "rman_coneAngleR", "float", "defaultFloat", 8.0),
            ("coneTT", "rman_coneAngleTT", "float", "defaultFloat", 4.0),
            ("coneTRT", "rman_coneAngleTRT", "float", "defaultFloat", 16.0),
            ("opacity", "opacity", "float", "defaultFloat", 1.0))


def _set(node, attr, value):
    """Set a RfM node's param whatever Maya type RfM gave it (a string, an enum of a mapper, a
    number, a color)."""
    plug = "%s.%s" % (node, attr)
    kind = cmds.getAttr(plug, type=True)
    if kind == "string":
        cmds.setAttr(plug, value, type="string")
    elif kind == "enum":
        names = cmds.attributeQuery(attr, node=node, listEnum=True)[0].split(":")
        lowered = [n.split("=")[0].lower() for n in names]
        cmds.setAttr(plug, lowered.index(str(value).lower()) if str(value).lower() in lowered else value)
    elif isinstance(value, (tuple, list)):
        cmds.setAttr(plug, *value)
    else:
        cmds.setAttr(plug, value)


def create_hair_network(name="groomLabRman", tilt=3.0, constants=None):
    """The procedural's shading network as RfM nodes (groom.rib's): PxrPrimvar readers of the
    rman_* primvars, a PxrHairColor in Physical mode (melanin, redness, dye) and one in Artistic
    mode (the RGB look), three PxrMix by rman_rgbShare into a PxrMarschnerHair's diffuse, TT and TRT
    colors, the R gain and cones from their primvars, presence from opacity, specularOffset minus
    `tilt` (HairColor's cuticle tilt: the param takes no connection). Returns the shading group.

    With `constants` (groomLabExportAlembic -xgen's "look": melanin, redness, dye, color, rgbShare,
    specularGainR, coneAngleR, coneAngleTT, coneAngleTRT, specularOffset), the same network with
    those values set instead of the primvar readers: one look for the whole groom, for curves that
    carry no primvars (RenderMan XPU through XGen), every value an artist can change in the
    Hypershade. Needs RfM loaded (its node types)."""
    if not rfm_loaded():
        raise RuntimeError("RenderMan for Maya is not loaded")
    pv = {}
    if constants is None:
        for key, varname, kind, default_param, default in PRIMVARS:
            node = cmds.shadingNode("PxrPrimvar", asUtility=True, name="%s_pv_%s" % (name, key))
            _set(node, "varname", varname)
            _set(node, "type", kind)
            if default_param:
                _set(node, default_param, default)
            pv[key] = node
    # Each primvar's output and the constants' key for it.
    outputs = {"melanin": ("resultF", "melanin"), "redness": ("resultF", "redness"), "dye": ("resultRGB", "dye"),
               "color": ("resultRGB", "color"), "share": ("resultF", "rgbShare"),
               "gainR": ("resultF", "specularGainR"), "coneR": ("resultF", "coneAngleR"),
               "coneTT": ("resultF", "coneAngleTT"), "coneTRT": ("resultF", "coneAngleTRT"),
               "opacity": ("resultF", None)}

    def feed(key, plug):
        output, constant = outputs[key]
        if constants is None:
            cmds.connectAttr("%s.%s" % (pv[key], output), plug, force=True)
        elif constant and constant in constants:
            value = constants[constant]
            if isinstance(value, (list, tuple)):
                cmds.setAttr(plug, *[float(v) for v in value[:3]])
            else:
                cmds.setAttr(plug, float(value))

    pigment = cmds.shadingNode("PxrHairColor", asUtility=True, name=name + "_pigment")
    _set(pigment, "mode", 0)
    feed("melanin", pigment + ".melanin")
    feed("redness", pigment + ".redness")
    feed("dye", pigment + ".dye")
    _set(pigment, "randomMelanin", 0.0)
    _set(pigment, "strayDensity", 0.0)
    _set(pigment, "hairIndexPrimvar", "hair_index")
    rgb = cmds.shadingNode("PxrHairColor", asUtility=True, name=name + "_rgb")
    _set(rgb, "mode", 1)
    feed("color", rgb + ".Color")
    _set(rgb, "randomColor", (0.0, 0.0, 0.0))
    _set(rgb, "strayDensity", 0.0)
    hair = cmds.shadingNode("PxrMarschnerHair", asShader=True, name=name + "_hair")
    for lobe, target in (("Diff", "diffuseColor"), ("TT", "specularColorTT"), ("TRT", "specularColorTRT")):
        mix = cmds.shadingNode("PxrMix", asUtility=True, name="%s_mix_%s" % (name, lobe.lower()))
        cmds.connectAttr(pigment + ".result" + lobe, mix + ".color1", force=True)
        cmds.connectAttr(rgb + ".result" + lobe, mix + ".color2", force=True)
        feed("share", mix + ".mixer")
        cmds.connectAttr(mix + ".resultRGB", hair + "." + target, force=True)
    feed("gainR", hair + ".specularGainR")
    feed("coneR", hair + ".specularConeAngleR")
    feed("coneTT", hair + ".specularConeAngleTT")
    feed("coneTRT", hair + ".specularConeAngleTRT")
    feed("opacity", hair + ".presence")
    offset = float(constants["specularOffset"]) if constants and "specularOffset" in constants else -float(tilt)
    for attr, value in (("specularGainTT", 2.0), ("specularGainTRT", 1.2), ("specularOffset", offset),
                        ("specularIor", 1.55)):
        _set(hair, attr, value)
    sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=name + "_SG")
    cmds.connectAttr(hair + ".outColor", sg + ".surfaceShader", force=True)
    return sg


# ---------------------------------------------------------------------------
# Interactive renders: RfM's IPR renders a node again when an attribute of it is set, and the
# groom's changes reach the procedural through no attribute of its own. In an interactive session,
# a change to a groom (WATCHED going dirty) sets the -revision of its procedurals' data
# strings; a running IPR translates them again, and pre_shape writes the groom file anew. (RfM's
# rfm2.render.ipr_is_running() said False while its IPR rendered into it: no gate on it.)

_watched = {}  # groom UUID -> callback id
# The groom's attributes whose change a render shows (the inputs of CHANGES_ATTR, groom_shape.cpp):
# the plugs that send a dirty message when they change. (Under the Evaluation Manager, Maya's
# default, the outputs they affect send none.) touch_procedurals writes only a real change.
WATCHED = frozenset((
    "inMesh", "restMesh", "followScalp", "scalpTopology", "colliderMeshes", "extraScalps", "simGuides",
    "guideCache", "guideCacheObject", "simCaches", "guideCurves", "graph", "guideData", "correctiveData",
    "correctiveWeight", "sculptData", "sculptNode", "flowData", "partData", "scalpMaps", "renderDensity",
    "renderWidthScale", "renderLengthScale", "renderMode", "groomFile", CHANGES_ATTR))
_touch_queued = set()


def touch_procedurals(groom):
    """Set the -revision of `groom`'s procedurals to its change count, outside the undo queue (an
    interactive render's bookkeeping, not an edit)."""
    if not cmds.objExists(groom) or not cmds.attributeQuery(CHANGES_ATTR, node=groom, exists=True):
        return
    revision = cmds.getAttr(groom + "." + CHANGES_ATTR)  # clean again: the next change dirties it
    for node in procedurals_of(groom):
        current = cmds.getAttr(node + ".data") or ""
        data = update_data(current, revision=revision)
        if data != current:
            from groomlab.base import no_undo
            with no_undo():
                cmds.setAttr(node + ".data", data, type="string")


def watch_grooms():
    """Watch every groom that has a procedural (sync_renderman runs this), in an interactive
    session: its changes touch its procedurals (touch_procedurals, once per idle however many
    changes come; never inside the dirty callback itself)."""
    import maya.api.OpenMaya as om2
    import maya.utils
    if cmds.about(batch=True):
        return
    for uuid in list(_watched):
        if not cmds.ls(uuid):
            try:
                om2.MMessage.removeCallback(_watched.pop(uuid))
            except RuntimeError:
                pass
    for groom in cmds.ls(type="groomLabShape", long=True) or []:
        uuid = cmds.ls(groom, uuid=True)[0]
        if uuid in _watched or not procedurals_of(groom):
            continue
        sel = om2.MSelectionList()
        sel.add(groom)

        def dirty(node, plug, *_args, uuid=uuid):
            if plug.partialName(useLongNames=True) not in WATCHED or uuid in _touch_queued:
                return

            def run():
                _touch_queued.discard(uuid)
                found = cmds.ls(uuid, long=True)
                if found:
                    touch_procedurals(found[0])
                    if renderer_is_xpu():
                        try:
                            update_xpu_groom(found[0])  # XPU's XGen groom follows the edit
                        except Exception as e:  # noqa: BLE001 (on idle: warn and go on)
                            cmds.warning("GroomLab: RenderMan XPU: %s" % e)

            _touch_queued.add(uuid)
            maya.utils.executeDeferred(run)

        _watched[uuid] = om2.MNodeMessage.addNodeDirtyPlugCallback(sel.getDependNode(0), dirty)
    # Render Settings switched to XPU or back with grooms in the scene: their XGen grooms made or
    # shown, or hidden again, at once (sync_xpu).
    if cmds.objExists("rmanGlobals") and _watched:
        uuid = cmds.ls("rmanGlobals", uuid=True)[0]
        if uuid not in _watched:
            sel = om2.MSelectionList()
            sel.add("rmanGlobals")

            def changed(message, plug, *_args):
                if message & om2.MNodeMessage.kAttributeSet and plug.partialName(useLongNames=True) == "renderVariant":
                    maya.utils.executeDeferred(lambda: cmds.ls(type="groomLabShape") and sync_xpu())

            _watched[uuid] = om2.MNodeMessage.addAttributeChangedCallback(sel.getDependNode(0), changed)
