# SPDX-License-Identifier: Apache-2.0
"""Shading: the groom's hair shader (aiStandardHair), its color read from the graph."""

import json

from maya import cmds

import groomlab_core



def assign_shading_group(groom, shading_group):
    """Assign a shading group (e.g. one holding aiStandardHair) to a groom.

    `sets -forceElement` rejects plugin shapes, so connect the instance
    group to the set directly, which is what it would do.
    """
    for plug in cmds.listConnections(groom + ".instObjGroups[0]", plugs=True, source=False) or []:
        cmds.disconnectAttr(groom + ".instObjGroups[0]", plug)
    cmds.connectAttr(groom + ".instObjGroups[0]", shading_group + ".dagSetMembers", nextAvailable=True)


def create_hair_shader(groom, name="groomHair", melanin=0.35, redness=0.2, from_groom=True):
    """An aiStandardHair (in its own shading group) assigned to the groom; returns the shader.

    With from_groom, the shader follows the groom's color nodes (HairColor, Bands,
    FeatherColor making the output, a Merge's other branches too, or a Blend mixing two
    colors): its base color reads their RGB (hair_color, aiUserDataColor) over no melanin,
    per strand, or per point along the curves with a tip color, bands or a feather pattern.
    That RGB is the hair's whole color, HairColor's pigment and dye already in it (mode
    melanin: the color whose absorption is the melanin's, so it renders as aiStandardHair's
    melanin would) with each strand's randomness (melanin or value, hue, saturation, a share
    per clump), so every strand renders as the viewport shows it. A groom without a color
    node renders by the melanin and melaninRedness channels (aiUserDataFloat; `melanin` and
    `redness` without them).

    A FeatherColor with iridescence (structural color) adds its approximation in
    Arnold: the angle between the camera ray and each strand's root normal
    (root_normal user data: a feather's vane) picks a color off a ramp of five
    keys of the iridescent color's hue turning, which the iridescence user data
    (per point) mixes into the base color and the specular tint, as the viewport
    and the app shade it (color_patterns.h's iridescentTint). An approximation: a
    ramp of five keys, the normal not leaned along the strand.

    The highlight is the groom's HairColor node's (hair_color_node: the one making the
    output; 0.3, 0.5 and 3 degrees without one), as the viewport shows it: its specular a
    strength whose default is Arnold's full specular tint, its roughness Arnold's roughness
    range, its tilt Arnold's shift. Where it varies per strand (a map, its randomness, or
    HairColor nodes making the output that set it apart) the shader reads the specular and
    roughness user data HairColor writes (highlight_channels). A RenderData node's opacity
    (per point) goes into the shader's opacity where it is set off opaque. Render-only: the
    viewport draws the color as before.

    A Wet node making the output (or a Simulate with water or rain) adds the wet network: the
    strands' wetness (user data, per point) darkens and saturates the color and sharpens and
    strengthens the highlight, as the viewport and the app shade it (_wet_network; core's
    wet_shading, no param of its own).
    """
    if not cmds.pluginInfo("mtoa", query=True, loaded=True):
        cmds.loadPlugin("mtoa")
    before = set(cmds.ls())
    rendered = _build_hair_shader(groom, name, melanin, redness, from_groom)
    if from_groom:  # what it was made for and of, for sync_hair_shader
        group = _shading_group(groom)
        made = sorted(set(cmds.ls()) - before)
        cmds.addAttr(group, longName=LOOK_ATTR, dataType="string")
        cmds.setAttr(group + "." + LOOK_ATTR, json.dumps({"name": name, "nodes": made,
                                                          "look": _look_signature(hair_look_json(groom))}),
                     type="string")
    return rendered


# The shading group create_hair_shader made holds what it was made for (sync_hair_shader).
LOOK_ATTR = "groomLabLook"


def _shading_group(groom):
    """The shading group the groom is assigned to, or None."""
    groups = cmds.listConnections(groom + ".instObjGroups[0]", source=False, destination=True,
                                  type="shadingEngine") or []
    return groups[0] if groups else None


# On a renderer's stand-in for a groom (RenderMan's procedural and XPU groom, Redshift's proxy
# mesh): the shading group it took from the groom, so it goes back to GroomLab's own material when
# the groom's changes, while a material the user assigned to the stand-in itself stays.
FROM_GROOM_ATTR = "groomLabMaterialFromGroom"


def material_from_groom(node):
    """The shading group `node` took from its groom (set_material_from_groom), or ""."""
    if not cmds.attributeQuery(FROM_GROOM_ATTR, node=node, exists=True):
        return ""
    return cmds.getAttr(node + "." + FROM_GROOM_ATTR) or ""


def set_material_from_groom(node, shading_group):
    if not cmds.attributeQuery(FROM_GROOM_ATTR, node=node, exists=True):
        cmds.addAttr(node, longName=FROM_GROOM_ATTR, dataType="string")
    cmds.setAttr(node + "." + FROM_GROOM_ATTR, shading_group or "", type="string")


def sync_materials(grooms=None):
    """Each groom's material (the shading group it is assigned to, in the Hypershade or any other
    way) to the renderers that render it through a stand-in: RenderMan's procedural and XPU groom
    (renderman.sync_material) and Redshift's proxy (redshift_hook.sync_material). Arnold and V-Ray
    read the groom's shading group themselves. The plugin queues it (queue_material_sync) when a
    groom's shading group changes. Never raises."""
    from groomlab import redshift_hook, renderman
    from groomlab.base import no_undo
    try:
        with no_undo():
            for groom in grooms or cmds.ls(type="groomLabShape", long=True, noIntermediate=True) or []:
                if not cmds.objExists(groom):
                    continue
                renderman.sync_material(groom)
                redshift_hook.sync_material(groom)
    except Exception as e:  # noqa: BLE001 (run on idle: warn and go on)
        cmds.warning("GroomLab: materials: %s" % e)


_material_sync_queued = [False]


def queue_material_sync():
    """sync_materials once the command that changed a groom's shading group is done (the Hypershade's
    assign breaks the old connection and makes the new one), once however many changes come before.
    The plugin calls it from the groom's connectionMade and connectionBroken, in interactive
    sessions."""
    if _material_sync_queued[0]:
        return

    def run():
        _material_sync_queued[0] = False
        sync_materials()

    _material_sync_queued[0] = True
    import maya.utils
    maya.utils.executeDeferred(run)


def _look_signature(look):
    """What create_hair_shader builds from a look (hair_look_json): the network's kind (pigment or
    RGB, a mix by strand, iridescence, wet, the user data it reads) and the constants it sets."""
    render_data = look.get("renderDataParams")
    return json.dumps({
        "rgb": bool(look.get("colorsByRgb") or look.get("colorBlends")),
        "iridescent": [look.get("iridescenceColor"), look.get("iridescenceShift")] if look.get("iridescent") else None,
        "wet": bool(look.get("wet")),
        "dye": [_single(c) for c in look.get("dye") or [1.0, 1.0, 1.0]],
        "lobes": {k: _single(v) for k, v in (look.get("lobes") or {}).items()},
        "highlight": highlight_channels(look),
        "renderData": render_data_channels(render_data) if render_data is not None else None,
    }, sort_keys=True)


def sync_hair_shader(groom):
    """Make the groom's hair shader again when create_hair_shader made it and the groom's look has
    changed what it would build (HairColor's mode, a tip color or a map turning the look to RGB, the
    dye, the highlight...): the shader is a snapshot of the look, so a pigment shader would keep
    rendering the brown pigment after the mode turned to color. Its nodes go and a new one, of the
    same name, is assigned. A shader of the user's own (or one made by hand) is left alone. Returns
    the new shader, or None when nothing changed."""
    group = _shading_group(groom)
    if not group or not cmds.attributeQuery(LOOK_ATTR, node=group, exists=True):
        return None
    try:
        made = json.loads(cmds.getAttr(group + "." + LOOK_ATTR) or "{}")
    except ValueError:
        return None
    look = hair_look_json(groom)
    if not look or made.get("look") == _look_signature(look):
        return None
    gone = [n for n in made.get("nodes") or [] if cmds.objExists(n) and n != groom]
    if gone:
        cmds.delete(gone)
    return create_hair_shader(groom, name=made.get("name") or "groomHair")


def _build_hair_shader(groom, name, melanin, redness, from_groom):
    """create_hair_shader's network, assigned; the shader that renders."""
    hair = cmds.shadingNode("aiStandardHair", asShader=True, name=name)
    group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=hair + "SG")
    cmds.connectAttr(hair + ".outColor", group + ".surfaceShader")
    # The look as the core reads it from the graph (GroomSession::hairLookJson: the viewport's and
    # every host's): whether the RGB is the color, the iridescence, the highlight, its lobes.
    look = hair_look_json(groom) if from_groom else {}
    iridescent = look if look.get("iridescent") else None
    rgb = bool(look.get("colorsByRgb") or look.get("colorBlends"))
    if rgb:  # the nodes' RGB (per strand, or per point with a tip color), as the viewport and exports have it
        data = cmds.shadingNode("aiUserDataColor", asUtility=True, name=hair + "_hair_color")
        cmds.setAttr(data + ".attribute", "hair_color", type="string")
        cmds.setAttr(data + ".default", *groomlab_core.DEFAULT_HAIR_COLOR, type="double3")  # HairColor's default look
        cmds.connectAttr(data + ".outColor", hair + ".baseColor")
        cmds.setAttr(hair + ".melanin", 0.0)
    else:
        _melanin_base(hair, look, melanin, redness, from_groom)
    shaders, rendered = [hair], hair
    # The highlight HairColor asks for (specular, roughness, tilt), as the viewport shows it: its
    # default strength is Arnold's full specular tint, its spread Arnold's roughness range, its
    # tilt Arnold's shift.
    render_data = look.get("renderDataParams") if from_groom else None
    lobes = look.get("lobes") or _default_lobes()  # core's hairLobes
    tint = lobes["specularTint"]
    for shader in shaders:
        if from_groom:
            cmds.setAttr(shader + ".melaninRandomize", 0.0)  # HairColor varies it
        cmds.setAttr(shader + ".specularTint", tint, tint, tint, type="double3")
        cmds.setAttr(shader + ".roughness", lobes["roughness"])
        cmds.setAttr(shader + ".shift", lobes["shift"])
    if iridescent is not None:
        _iridescence_network(hair, iridescent, tint)  # structural color: the RGB's look
    if rgb:
        _arnold_base_color(hair)  # the RGB as Arnold should absorb it (after the iridescence's mix)
    if from_groom:
        for shader in shaders:
            _highlight_network(shader, look)
    if render_data is not None:
        for shader in shaders:
            _render_data_network(shader, render_data)
    if look.get("wet"):
        for shader in shaders:
            _wet_network(shader)  # the strands' wetness: darker, more saturated, glossier
    assign_shading_group(groom, group)
    return rendered


def _melanin_base(hair, look, melanin, redness, from_groom):
    """An aiStandardHair colored by the pigment: melanin and melaninRedness from the strand
    channels (aiUserDataFloat; `melanin` and `redness` without them) over the HairColor's dye
    (the look's)."""
    for attr, default in (("melanin", melanin), ("melaninRedness", redness)):
        cmds.setAttr(hair + "." + attr, default)
        if from_groom:
            data = cmds.shadingNode("aiUserDataFloat", asUtility=True, name="%s_%s" % (hair, attr))
            cmds.setAttr(data + ".attribute", attr, type="string")
            cmds.setAttr(data + ".default", default)
            cmds.connectAttr(data + ".outValue", hair + "." + attr)
    dye = look.get("dye") or [1.0, 1.0, 1.0]
    cmds.setAttr(hair + ".baseColor", _single(dye[0]), _single(dye[1]), _single(dye[2]), type="double3")


def _arnold_base_color(hair):
    """The base color's driver (the strands' RGB) through an aiColorCorrect of gamma
    ARNOLD_BASE_COLOR_GAMMA: aiStandardHair absorbs a base color about 1.175 times what the
    viewport's mapping does (hair_color.h kArnoldBaseColorGamma), so the color to the power 1 / it
    renders as the viewport shades the color."""
    source = _driver(hair, "baseColor")
    correct = cmds.shadingNode("aiColorCorrect", asUtility=True, name=hair + "_arnoldColor")
    if source:
        cmds.connectAttr(source, correct + ".input", force=True)
    else:
        cmds.setAttr(correct + ".input", *cmds.getAttr(hair + ".baseColor")[0], type="double3")
    cmds.setAttr(correct + ".gamma", groomlab_core.ARNOLD_BASE_COLOR_GAMMA)
    cmds.connectAttr(correct + ".outColor", hair + ".baseColor", force=True)


def _user_data(hair, attr, default):
    """An aiUserDataFloat reading strand channel `attr` (`default` without it); its output plug."""
    node = cmds.shadingNode("aiUserDataFloat", asUtility=True, name="%s_%s" % (hair, attr))
    cmds.setAttr(node + ".attribute", attr, type="string")
    cmds.setAttr(node + ".default", default)
    return node + ".outValue"


def render_data_node(groom):
    """The RenderData node the groom's opacity comes from, "" without one: the
    first met walking up from the output, as for hair_color_node (graph stats' "renderData")."""
    try:
        return json.loads(cmds.groomLabGraphStats(groom)).get("renderData") or ""
    except (RuntimeError, ValueError):
        return ""


def render_data_params(groom):
    """The params of the groom's RenderData node (render_data_node), None without one (the
    core's: hair_look_json)."""
    return hair_look_json(groom).get("renderDataParams")


def _neutral_ramp(value):
    """Whether a ramp param (keys [[x, y], ...], or absent) is 1 all along."""
    if value is None:
        return True
    try:
        return all(float(key[1]) == 1.0 for key in value)
    except (TypeError, ValueError, IndexError):
        return False


def render_data_channels(params):
    """The RenderData channels the shader reads as user data for these params: opacity where
    it is set off opaque."""
    channels = []
    if (isinstance(params.get("opacity"), dict) or float(params.get("opacity", 1.0)) != 1.0 or
            not _neutral_ramp(params.get("opacityProfile")) or _positive(params.get("opacityRandom", 0.0))):
        channels.append("opacity")
    return channels


def _render_data_network(hair, params):
    """An aiStandardHair driven by RenderData's channel (user data; a strand without it opaque):
    opacity per point into its opacity."""
    if "opacity" in render_data_channels(params):
        opacity = _user_data(hair, "opacity", 1.0)
        for c in "RGB":
            cmds.connectAttr(opacity, hair + ".opacity" + c, force=True)


def highlight_channels(look):
    """The HairColor highlight channels the shader reads as user data for a look (hair_look_json):
    specular and roughness where they vary per strand (the HairColor's map or randomness, or the
    HairColor nodes making the output setting them apart); a constant is set on the shader itself.
    (The tilt is one per shader: Arnold's shift takes no user data.)"""
    params = look.get("hairColorParams") or {}
    hair_colors = [n.get("params") or {} for n in look.get("colorNodes") or [] if n.get("type") == "HairColor"]
    defaults = {"specular": _single(groomlab_core.DEFAULT_HAIR_SPECULAR),
                "roughness": _single(groomlab_core.DEFAULT_HAIR_ROUGHNESS)}
    channels = []
    for name in ("specular", "roughness"):
        values = set()
        for p in hair_colors:
            value = p.get(name, defaults[name])
            values.add(_single(value) if not isinstance(value, dict) else None)
        if isinstance(params.get(name), dict) or _positive(params.get(name + "Random", 0.0)) or len(values) > 1:
            channels.append(name)
    return channels


def _highlight_network(hair, look):
    """An aiStandardHair driven by HairColor's highlight channels (user data; a strand without
    them takes HairColor's defaults): its specular tint and roughness per strand as core's
    hair_lobes maps them (specular / HAIR_FULL_SPECULAR, up to 1; HAIR_ROUGHNESS_MIN +
    HAIR_ROUGHNESS_RANGE x roughness)."""
    channels = highlight_channels(look)
    if "specular" in channels:
        scaled = cmds.shadingNode("multiplyDivide", asUtility=True, name=hair + "_specularTint")
        specular = _user_data(hair, "specular", _single(groomlab_core.DEFAULT_HAIR_SPECULAR))
        for c in "XYZ":
            cmds.connectAttr(specular, scaled + ".input1" + c)
        full = 1.0 / groomlab_core.HAIR_FULL_SPECULAR
        cmds.setAttr(scaled + ".input2", full, full, full, type="double3")
        tint = cmds.shadingNode("clamp", asUtility=True, name=hair + "_specularTintClamped")
        cmds.connectAttr(scaled + ".output", tint + ".input")
        cmds.setAttr(tint + ".max", 1.0, 1.0, 1.0, type="double3")
        # Under the iridescence's highlight when it has one, else the shader's own tint.
        if cmds.objExists(hair + "_iridescence_specular"):
            cmds.connectAttr(tint + ".output", hair + "_iridescence_specular.color2", force=True)
            cmds.connectAttr(tint + ".output", hair + "_iridescence_tinted.input2", force=True)
        else:
            cmds.connectAttr(tint + ".output", hair + ".specularTint", force=True)
    if "roughness" in channels:
        scaled = cmds.shadingNode("multiplyDivide", asUtility=True, name=hair + "_roughnessRange")
        cmds.connectAttr(_user_data(hair, "roughness", _single(groomlab_core.DEFAULT_HAIR_ROUGHNESS)), scaled + ".input1X")
        cmds.setAttr(scaled + ".input2X", groomlab_core.HAIR_ROUGHNESS_RANGE)
        offset = cmds.shadingNode("plusMinusAverage", asUtility=True, name=hair + "_roughnessOffset")
        cmds.connectAttr(scaled + ".outputX", offset + ".input1D[0]")
        cmds.setAttr(offset + ".input1D[1]", groomlab_core.HAIR_ROUGHNESS_MIN)
        cmds.connectAttr(offset + ".output1D", hair + ".roughness", force=True)


def _driver(hair, attr):
    """The plug driving `hair`.`attr`, or None (the attribute's own value then)."""
    source = cmds.listConnections(hair + "." + attr, source=True, destination=False, plugs=True)
    return source[0] if source else None


def _wet_network(hair):
    """Wet hair in Arnold as the viewport and the app shade it (core's wet_shading, hair_color.h):
    the strands' wetness (user data, per point: Wet's channel, or a Simulate's with water or rain;
    0 without it) in front of what drives the shader's color and highlight. With w the wetness:
    melanin x sqrt(1 + WET_ABSORPTION w) (the absorption up: melanin's is its square), the base
    color to the power 1 + WET_ABSORPTION w, the specular tint x 1 + WET_SPECULAR w and the
    roughness x 1 - WET_ROUGHNESS w. The wet nodes are named
    <shader>_wet...: the viewport reads the dry look through them (and the wetness itself from
    the strands)."""
    wet = _user_data(hair, "wetness", 0.0)
    # (1 + a w, 1 + s w, 1 - r w).
    factors = cmds.shadingNode("multiplyDivide", asUtility=True, name=hair + "_wetFactors")
    for c in "XYZ":
        cmds.connectAttr(wet, factors + ".input1" + c)
    cmds.setAttr(factors + ".input2", groomlab_core.WET_ABSORPTION, groomlab_core.WET_SPECULAR,
                 -groomlab_core.WET_ROUGHNESS, type="double3")
    shares = cmds.shadingNode("plusMinusAverage", asUtility=True, name=hair + "_wetShares")
    cmds.connectAttr(factors + ".output", shares + ".input3D[0]")
    cmds.setAttr(shares + ".input3D[1]", 1.0, 1.0, 1.0, type="double3")
    absorption, specular, smoother = (shares + ".output3D" + c for c in "xyz")

    def scale_color(attr, name, by, power=False):
        """`attr` (a color) times the float plug `by`, or to its power."""
        source = _driver(hair, attr)
        scaled = cmds.shadingNode("multiplyDivide", asUtility=True, name=hair + "_wet" + name)
        if power:
            cmds.setAttr(scaled + ".operation", 3)
        if source:
            cmds.connectAttr(source, scaled + ".input1")
        else:
            cmds.setAttr(scaled + ".input1", *cmds.getAttr(hair + "." + attr)[0], type="double3")
        for c in "XYZ":
            cmds.connectAttr(by, scaled + ".input2" + c)
        cmds.connectAttr(scaled + ".output", hair + "." + attr, force=True)

    def scale_float(attr, name, by):
        source = _driver(hair, attr)
        scaled = cmds.shadingNode("multiplyDivide", asUtility=True, name=hair + "_wet" + name)
        if source:
            cmds.connectAttr(source, scaled + ".input1X")
        else:
            cmds.setAttr(scaled + ".input1X", cmds.getAttr(hair + "." + attr))
        cmds.connectAttr(by, scaled + ".input2X")
        cmds.connectAttr(scaled + ".outputX", hair + "." + attr, force=True)

    scale_color("baseColor", "Base", absorption, power=True)
    if _driver(hair, "melanin") or cmds.getAttr(hair + ".melanin") > 0.0:
        root = cmds.shadingNode("multiplyDivide", asUtility=True, name=hair + "_wetMelaninShare")
        cmds.setAttr(root + ".operation", 3)  # power: sqrt(1 + a w)
        cmds.connectAttr(absorption, root + ".input1X")
        cmds.setAttr(root + ".input2X", 0.5)
        scale_float("melanin", "Melanin", root + ".outputX")
    scale_color("specularTint", "Specular", specular)
    scale_float("roughness", "Roughness", smoother)


def iridescent_tint(color, shift, grazing):
    """The structural color at `grazing` (0 facing the view .. 1 edge-on): `color`'s hue turned
    by `shift` x grazing turns, its saturation and value kept (color_patterns.h's iridescentTint,
    as the viewport and the app shade it)."""
    import groomlab_core
    return tuple(groomlab_core.iridescent_tint([float(c) for c in color[:3]], float(shift), float(grazing)))


def _iridescence_network(hair, params, specular_tint):
    """An aiStandardHair's base color and specular tint mixed toward the structural color by
    the strands' iridescence (user data, per point), the color off a ramp by the angle between
    the camera ray and the strand's root normal (user data root_normal: GroomLab writes both
    with a FeatherColor's iridescence)."""
    color = [_single(c) for c in params.get("iridescenceColor") or [0.05, 0.55, 0.2]]
    shift = _single(params.get("iridescenceShift", 0.3))
    ray = cmds.shadingNode("aiStateVector", asUtility=True, name=hair + "_iridescence_ray")
    variables = cmds.attributeQuery("variable", node=ray, listEnum=True)[0].split(":")
    cmds.setAttr(ray + ".variable", variables.index("Rd"))  # the ray's direction
    normal = cmds.shadingNode("aiUserDataColor", asUtility=True, name=hair + "_root_normal")
    cmds.setAttr(normal + ".attribute", "root_normal", type="string")
    cmds.setAttr(normal + ".default", 0.0, 1.0, 0.0, type="double3")
    dot = cmds.shadingNode("aiDot", asUtility=True, name=hair + "_iridescence_facing")
    cmds.connectAttr(ray + ".outValue", dot + ".input1")
    cmds.connectAttr(normal + ".outColor", dot + ".input2")
    facing = cmds.shadingNode("aiAbs", asUtility=True, name=hair + "_iridescence_abs")
    for channel in "RGB":
        cmds.connectAttr(dot + ".outValue", facing + ".input" + channel)
    ramp = cmds.shadingNode("aiRampRgb", asTexture=True, name=hair + "_iridescence_ramp")
    cmds.setAttr(ramp + ".type", 0)  # custom: the input below
    cmds.connectAttr(facing + ".outColorR", ramp + ".input")
    for i, at in enumerate((0.0, 0.25, 0.5, 0.75, 1.0)):  # |cos|: 0 edge-on .. 1 facing
        key = "%s.ramp[%d]" % (ramp, i)
        cmds.setAttr(key + ".ramp_Position", at)
        cmds.setAttr(key + ".ramp_Color", *iridescent_tint(color, shift, 1.0 - at), type="double3")
        cmds.setAttr(key + ".ramp_Interp", 1)  # linear
    amount = cmds.shadingNode("aiUserDataFloat", asUtility=True, name=hair + "_iridescence")
    cmds.setAttr(amount + ".attribute", "iridescence", type="string")
    cmds.setAttr(amount + ".default", 0.0)
    base = cmds.listConnections(hair + ".baseColor", source=True, destination=False, plugs=True)
    mix = cmds.shadingNode("blendColors", asUtility=True, name=hair + "_iridescence_base")
    cmds.connectAttr(ramp + ".outColor", mix + ".color1")
    if base:
        cmds.connectAttr(base[0], mix + ".color2")
    else:
        cmds.setAttr(mix + ".color2", *cmds.getAttr(hair + ".baseColor")[0], type="double3")
    cmds.connectAttr(amount + ".outValue", mix + ".blender")
    cmds.connectAttr(mix + ".output", hair + ".baseColor", force=True)
    # The highlight: its tint times the structural color, by the same share.
    tinted = cmds.shadingNode("multiplyDivide", asUtility=True, name=hair + "_iridescence_tinted")
    cmds.connectAttr(ramp + ".outColor", tinted + ".input1")
    cmds.setAttr(tinted + ".input2", specular_tint, specular_tint, specular_tint, type="double3")
    highlight = cmds.shadingNode("blendColors", asUtility=True, name=hair + "_iridescence_specular")
    cmds.connectAttr(tinted + ".output", highlight + ".color1")
    cmds.setAttr(highlight + ".color2", specular_tint, specular_tint, specular_tint, type="double3")
    cmds.connectAttr(amount + ".outValue", highlight + ".blender")
    cmds.connectAttr(highlight + ".output", hair + ".specularTint", force=True)


def _positive(value):
    """A number param above 0, or a map (a dict)."""
    if isinstance(value, dict):
        return True
    try:
        return float(value) > 0.0
    except (TypeError, ValueError):
        return False


def hair_color_node(groom):
    """The HairColor node the groom's look comes from, "" without one: the one making the
    output, as the plugin picks it for the viewport (graph stats' "hairColor"). Walking up
    from the output, first inputs first (the output's whole first-input chain before any other
    branch), past bypassed nodes and a Switch's unpicked input, into compounds; the first
    HairColor met wins, so a look merged from several takes its main (first) input's. One
    inside a compound is "compound/inner"."""
    try:
        return json.loads(cmds.groomLabGraphStats(groom)).get("hairColor") or ""
    except (RuntimeError, ValueError):
        return ""


def hair_look_json(groom):
    """The groom's look as the core reads it from the graph (groomLabGraphStats -hairLook:
    GroomSession::hairLookJson): node, specular, roughness, specularRandom, roughnessRandom, tilt,
    dye, colorsByRgb, colorBlends, iridescent, iridescenceColor, iridescenceShift, wet, lobes
    (aiStandardHair's: specularTint, roughness, azimuthalRoughness, ior, shift), hairColorParams and
    renderDataParams (the nodes' params, inner ones with their compound's exposed params set; None
    without the node) and colorNodes ([{path, type, params}]). {} when it cannot be read."""
    try:
        return json.loads(cmds.groomLabGraphStats(groom, hairLook=True))
    except (RuntimeError, ValueError):
        return {}


def _single(value):
    """A number the core holds in single precision, as it was written (0.45, not 0.449999988)."""
    return float("%.7g" % float(value))


def _default_lobes():
    """aiStandardHair's lobes for HairColor's defaults (core's hairLobes)."""
    lobes = groomlab_core.hair_lobes(groomlab_core.DEFAULT_HAIR_SPECULAR, groomlab_core.DEFAULT_HAIR_ROUGHNESS)
    return {"specularTint": lobes["specular_tint"], "roughness": lobes["roughness"], "shift": lobes["shift"]}


def color_nodes(groom):
    """(type, params) of every color node making the output (HairColor, Bands, FeatherColor;
    graph stats' "colorNodes": walking up from the output, a Merge's other branches too), []
    without any. One inside a compound has the compound's exposed params applied (the core's:
    hair_look_json)."""
    return [(n.get("type", ""), n.get("params") or {}) for n in hair_look_json(groom).get("colorNodes") or []]


def hair_color_params(groom):
    """The params of the groom's HairColor node (hair_color_node: the one making the output),
    {} without one. One inside a compound has the compound's exposed params applied."""
    return hair_look_json(groom).get("hairColorParams") or {}


def hair_look(groom):
    """{"specular", "roughness", "tilt"} of the groom's HairColor node (hair_color_node: the one
    making the output; 0.3, 0.5 and 3 without one, or for one set to a map): the highlight the
    viewport draws the strands and cards with, and create_hair_shader gives the Arnold hair
    shader (the core's: hair_look_json)."""
    look = hair_look_json(groom)
    return {"specular": _single(look.get("specular", 0.3)), "roughness": _single(look.get("roughness", 0.5)),
            "tilt": _single(look.get("tilt", 3.0))}
