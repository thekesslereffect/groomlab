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
EXPERIMENTAL_NOTE = ("experimental: needs RenderMan for Maya 27 on Maya 2024-2026; not yet verified in Maya")

# The data string's keys, as the procedural spells them, by this module's argument names.
DATA_KEYS = (("file", "file"), ("frame", "frame"), ("fps", "fps"), ("density", "density"),
             ("width_scale", "widthScale"), ("length_scale", "lengthScale"), ("motion_start", "motionStart"),
             ("motion_end", "motionEnd"), ("motion_keys", "motionKeys"), ("search_paths", "searchPaths"),
             ("name", "name"), ("debug", "debug"))
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


# ---------------------------------------------------------------------------
# The pre-shape script and the node


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
            from groomlab.files import export_groom
            folder = os.path.dirname(path)
            if not os.path.isdir(folder):
                os.makedirs(folder)
            export_groom(groom, path)
        values = {"file": path, "frame": frame}
        if not cmds.attributeQuery(SHUTTER_ATTR, node=node, exists=True) or cmds.getAttr(node + "." + SHUTTER_ATTR):
            keys = shutter()
            values.update(motion_start=keys[0] if keys else None, motion_end=keys[1] if keys else None,
                          motion_keys=keys[2] if keys else None)
        data = update_data(cmds.getAttr(node + ".data") or "", **values)
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

    Experimental (EXPERIMENTAL_NOTE). Raises RuntimeError when RfM is not loaded or the plugin is
    not installed. Returns the procedural's shape."""
    from groomlab.base import _shape_of
    if not rfm_loaded():
        raise RuntimeError("RenderMan for Maya is not loaded (it needs RenderMan for Maya 27, Maya 2024-2026)")
    dso = dso_path()
    if not dso:
        raise RuntimeError("the RenderMan procedural (%s) is not installed under %s/procedurals/renderman" %
                           (DSO_FILE, module_root()))
    groom = _shape_of(groom, "groomLabShape")
    base = cmds.listRelatives(groom, parent=True)[0].split("|")[-1].split(":")[-1] + "_rman"
    transform = cmds.createNode("transform", name=base)
    node = cmds.createNode(NODE_TYPE, name=base + "Shape", parent=transform)
    node = cmds.ls(node, long=True)[0]
    cmds.setAttr(node + ".__dsoname", dso, type="string")
    # RfM's indirect visibility inherits these, and a new shape's are off in Maya 2025 and later (on
    # in 2024): without them the strands light no strand, and the hair renders dark.
    for attr in ("visibleInReflections", "visibleInRefractions"):
        cmds.setAttr(node + "." + attr, True)
    link_procedural(node, groom, density=density, width_scale=width_scale, length_scale=length_scale, debug=debug,
                    shutter_from_globals=shutter_from_globals)
    try:  # the node's own frame (the RfM UI's; the plugin reads -frame)
        cmds.connectAttr("time1.outTime", node + ".frame", force=True)
    except RuntimeError:
        pass
    if shader:
        try:
            from groomlab.shading import hair_look
            sg = create_hair_network(base, tilt=hair_look(groom)["tilt"])
            assign_shading_group(node, sg)
        except (RuntimeError, ValueError, TypeError) as e:
            cmds.warning("GroomLab: the RenderMan hair shader was not made (%s): see docs/DEVELOPING.md, "
                         "'RenderMan for Maya', for the network" % e)
    pre_shape(node)  # the data string complete now, not only at render time
    cmds.select(transform)
    return node


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


def create_hair_network(name="groomLabRman", tilt=3.0):
    """The procedural's shading network as RfM nodes (groom.rib's): PxrPrimvar readers of the
    rman_* primvars, a PxrHairColor in Physical mode (melanin, redness, dye) and one in Artistic
    mode (the RGB look), three PxrMix by rman_rgbShare into a PxrMarschnerHair's diffuse, TT and TRT
    colors, the R gain and cones from their primvars, presence from opacity, specularOffset minus
    `tilt` (HairColor's cuticle tilt: the param takes no connection). Returns the shading group.
    Needs RfM loaded (its node types)."""
    if not rfm_loaded():
        raise RuntimeError("RenderMan for Maya is not loaded")
    pv = {}
    for key, varname, kind, default_param, default in PRIMVARS:
        node = cmds.shadingNode("PxrPrimvar", asUtility=True, name="%s_pv_%s" % (name, key))
        _set(node, "varname", varname)
        _set(node, "type", kind)
        if default_param:
            _set(node, default_param, default)
        pv[key] = node
    pigment = cmds.shadingNode("PxrHairColor", asUtility=True, name=name + "_pigment")
    _set(pigment, "mode", 0)
    cmds.connectAttr(pv["melanin"] + ".resultF", pigment + ".melanin", force=True)
    cmds.connectAttr(pv["redness"] + ".resultF", pigment + ".redness", force=True)
    cmds.connectAttr(pv["dye"] + ".resultRGB", pigment + ".dye", force=True)
    _set(pigment, "randomMelanin", 0.0)
    _set(pigment, "strayDensity", 0.0)
    _set(pigment, "hairIndexPrimvar", "hair_index")
    rgb = cmds.shadingNode("PxrHairColor", asUtility=True, name=name + "_rgb")
    _set(rgb, "mode", 1)
    cmds.connectAttr(pv["color"] + ".resultRGB", rgb + ".Color", force=True)
    _set(rgb, "randomColor", (0.0, 0.0, 0.0))
    _set(rgb, "strayDensity", 0.0)
    hair = cmds.shadingNode("PxrMarschnerHair", asShader=True, name=name + "_hair")
    for lobe, target in (("Diff", "diffuseColor"), ("TT", "specularColorTT"), ("TRT", "specularColorTRT")):
        mix = cmds.shadingNode("PxrMix", asUtility=True, name="%s_mix_%s" % (name, lobe.lower()))
        cmds.connectAttr(pigment + ".result" + lobe, mix + ".color1", force=True)
        cmds.connectAttr(rgb + ".result" + lobe, mix + ".color2", force=True)
        cmds.connectAttr(pv["share"] + ".resultF", mix + ".mixer", force=True)
        cmds.connectAttr(mix + ".resultRGB", hair + "." + target, force=True)
    cmds.connectAttr(pv["gainR"] + ".resultF", hair + ".specularGainR", force=True)
    cmds.connectAttr(pv["coneR"] + ".resultF", hair + ".specularConeAngleR", force=True)
    cmds.connectAttr(pv["coneTT"] + ".resultF", hair + ".specularConeAngleTT", force=True)
    cmds.connectAttr(pv["coneTRT"] + ".resultF", hair + ".specularConeAngleTRT", force=True)
    cmds.connectAttr(pv["opacity"] + ".resultF", hair + ".presence", force=True)
    for attr, value in (("specularGainTT", 2.0), ("specularGainTRT", 1.2), ("specularOffset", -float(tilt)),
                        ("specularIor", 1.55)):
        _set(hair, attr, value)
    sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=name + "_SG")
    cmds.connectAttr(hair + ".outColor", sg + ".surfaceShader", force=True)
    return sg
