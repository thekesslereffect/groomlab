# SPDX-License-Identifier: GPL-3.0-or-later
"""The hair's material: GroomLab's Arnold look (Maya's create_hair_shader, an aiStandardHair) made
with Blender's Principled Hair BSDF, so a groom renders in Cycles as it does in Arnold.

What Maya builds, and what this builds from the same evaluation (the helper's `look`):

- The RGB, whenever a color node makes the output (HairColor in either mode, Bands, FeatherColor,
  a Blend of two looks: `colorsByRgb`, `colorBlends`): its base color the strands' color (per point
  when it varies along them) over no melanin. HairColor's pigment, dye and per-strand randomness
  are in that color already.
- The pigment, for a groom without a color node: aiStandardHair's melanin and melaninRedness from
  each strand's channels (an Attribute's), its base color the look's dye.
- HairColor's highlight per strand where it writes it: the roughness (its roughness channel).
  (Its specular per strand is Arnold's alone: the material takes the node's own.) A RenderData
  node's opacity per point mixes the hair toward transparent.
- Iridescence: the base color mixed toward the structural color off a ramp of the angle between
  the view and each strand's root normal, by the per-point iridescence channel.
- The highlight: roughness 0.1 + 0.4 x HairColor's roughness, specular tint its specular / 0.3,
  the cuticle tilt (Principled Hair's offset) its tilt (0.5, 0.3 and 3 degrees without one).

Arnold's and Cycles' hair models differ in their numbers (fitted with kick against Cycles on the
same strands, docs/BLENDER.md "Rendering"): Arnold's melanin absorbs as m^1.9 times its pigment
colors, its base colors absorb about 1.18x what Chiang's formula gives plus a small constant (its
light hair keeps less energy), its highlight is about 1.5x Cycles' and a little wider. So the
material turns melanin, redness, dye and RGB into the absorption Arnold renders with (the
"GroomLab Hair Color" node group, per channel) and hands it to Principled Hair as a direct color
(the color whose absorption that is, at Cycles' roughness), with a second, black Principled Hair
adding the rest of the highlight. EEVEE renders hair as a diffuse color without highlights: its
output gets the color whose diffuse shade matches Cycles' average, from the same group.

Solid shading draws no material nodes (and Blender's Attribute color does not reach hair curves):
it shows the material's viewport color, which is set to the hair's APPEARANCE color (core's
hairAppearanceColor: what the hair looks like under even white light, as Maya's plain viewport
shading draws it) averaged over the strands, so the groom shows its look there too, not gray."""

import colorsys
import math

import bpy

GROUP = "GroomLab Hair Color"
GROUP_VERSION = 1
STRAND_COLOR = "groomlab_color"       # hair curves: the color nodes' RGB per strand (linear)
POINT_COLOR = "groomlab_point_color"   # hair curves: their RGB per point (a tip color, bands...)
ROOT_NORMAL = "groomlab_root_normal"   # hair curves: the strand's root normal (iridescence)
DEFAULT_MELANIN, DEFAULT_REDNESS = 0.35, 0.2  # create_hair_shader's, for strands without the channels

# The fit (Arnold 7.0 standard_hair against Cycles' Chiang hair, kick vs Blender 5.2 on the same
# strands, roughness 0.15-0.45). Absorption per unit of a strand's width:
#   melanin m, redness r: m^P * ((1 - r) E + r R) * (1 + K (ra - 0.3))
#   a base color c (a dye, or the RGB):  B * (ln c / f(ra))^2   (Chiang 2016's color to absorption)
#   plus S0 + S1 (ra - 0.3) on every strand.
P = 1.9
EUMELANIN = (2.270, 3.690, 6.996)
PHEOMELANIN = (1.567, 3.213, 8.111)
K = 0.4
B = 1.175
S0, S1 = 0.0325, -0.025
EXTRA_HIGHLIGHT = 1.5  # Arnold's highlight / Cycles' at the same tint
# create_hair_shader hands Arnold the RGB to the power 1 / this (core's kArnoldBaseColorGamma,
# sqrt(B)): Arnold then absorbs it as the viewport shades the RGB; the material does the same.
ARNOLD_BASE_COLOR_GAMMA = 1.0840
# EEVEE's hair is a diffuse shade of its color, without highlights or light scattered between
# strands: Cycles' direct color -> the EEVEE color whose shade matches Cycles' average under a
# neutral light: the curves fitted on a dense head of hair (light hair brightens by scattering)
# and on sparse fur (its highlight shows more), weighted 0.7 and 0.3 (geometric).
EEVEE_CURVE = ((0.0, 0.042), (0.0007, 0.0485), (0.0019, 0.0572), (0.0043, 0.071), (0.0097, 0.0973), (0.0212, 0.141),
               (0.043, 0.213), (0.0766, 0.307), (0.137, 0.450), (0.219, 0.616), (0.370, 0.834), (1.0, 1.089))


def arnold_roughness(look):
    """aiStandardHair's roughness for HairColor's (create_hair_shader): the helper's look lobes
    (core's hairLobes)."""
    return float((look.get("lobes") or {}).get("roughness", 0.3))


def cuticle_tilt(look):
    """aiStandardHair's shift (degrees) for HairColor's tilt, Principled Hair's offset: the
    helper's look lobes (core's hairLobes)."""
    return float((look.get("lobes") or {}).get("shift", 3.0))


def cycles_roughness(ra):
    """Principled Hair's roughness (and radial roughness) whose highlight spreads as Arnold's ra."""
    return 0.25 + 0.5 * ra


def roughness_factor(x):
    """Chiang 2016's azimuthal roughness factor (Cycles' sigma_from_reflectance)."""
    return (((((0.245 * x) + 5.574) * x - 10.73) * x + 2.532) * x - 0.215) * x + 5.969


def specular_tint(look):
    """aiStandardHair's specular tint for HairColor's specular: the helper's look lobes (core's
    hairLobes)."""
    return float((look.get("lobes") or {}).get("specularTint", 1.0))


def extra_highlight(look):
    """How much of a black Principled Hair (its highlight alone) goes over the color's: Arnold's
    highlight at HairColor's specular tint, less the one Principled Hair has (0 below a tint of
    2/3, where Cycles' own is already as strong)."""
    return max(0.0, min(1.0, EXTRA_HIGHLIGHT * specular_tint(look) - 1.0))


def absorption(melanin, redness, color, ra):
    """The absorption Arnold renders with (Cycles' units), per channel: what the node group
    computes (blender.addon_render checks it)."""
    out = []
    for i in range(3):
        sigma = S0 + S1 * (ra - 0.3)
        sigma += max(melanin, 0.0) ** P * ((1.0 - redness) * EUMELANIN[i] + redness * PHEOMELANIN[i]) * (1.0 + K * (ra - 0.3))
        q = math.log(max(color[i], 1e-4)) / roughness_factor(ra)
        sigma += B * q * q
        out.append(sigma)
    return out


def cycles_color(melanin, redness, color, ra):
    """The node group's Cycles output: the direct color with that absorption at Cycles' roughness."""
    fc = roughness_factor(cycles_roughness(ra))
    return [math.exp(-fc * math.sqrt(s)) for s in absorption(melanin, redness, color, ra)]


def set_viewport_color(mat, appearance):
    """The material's viewport color (Solid shading's) the strands' mean appearance (the helper's
    `appearance`: hair_color.h meanHairAppearance, every host's), for a material the add-on made.
    True when it changed."""
    if mat is None or not mat.get("groomlab") or not appearance:
        return False
    color = (float(appearance[0]), float(appearance[1]), float(appearance[2]), 1.0)
    if max(abs(a - b) for a, b in zip(mat.diffuse_color, color)) < 1e-4:
        return False
    mat.diffuse_color = color
    return True


def iridescent_tint(color, shift, grazing):
    """The structural color at `grazing` (0 facing the view .. 1 edge-on): Maya's shading.py's."""
    h, s, v = colorsys.rgb_to_hsv(*[max(0.0, float(c)) for c in color])
    return colorsys.hsv_to_rgb((h + shift * min(max(grazing, 0.0), 1.0)) % 1.0, s, v)


# ---- The node group ---------------------------------------------------------------------------

def _math(nodes, links, op, a, b=None, c=None, x=0, y=0):
    n = nodes.new("ShaderNodeMath")
    n.operation = op
    n.location = (x, y)
    for i, v in enumerate((a, b, c)):
        if v is None:
            continue
        if isinstance(v, bpy.types.NodeSocket):
            links.new(v, n.inputs[i])
        else:
            n.inputs[i].default_value = v
    return n.outputs[0]


def hair_color_group():
    """The "GroomLab Hair Color" node group: Melanin, Redness, Color (a dye, or the RGB over no
    melanin) and Arnold's roughness in; Cycles (Principled Hair's direct color) and EEVEE out."""
    g = bpy.data.node_groups.get(GROUP)
    if g is not None and g.get("groomlab_version") == GROUP_VERSION:
        return g
    if g is None:
        g = bpy.data.node_groups.new(GROUP, "ShaderNodeTree")
    g.nodes.clear()
    g.interface.clear()
    for name, kind, default in (("Melanin", "NodeSocketFloat", 0.0), ("Redness", "NodeSocketFloat", 0.0),
                                ("Color", "NodeSocketColor", (1.0, 1.0, 1.0, 1.0)), ("Roughness", "NodeSocketFloat", 0.3)):
        s = g.interface.new_socket(name, in_out="INPUT", socket_type=kind)
        s.default_value = default
    g.interface.new_socket("Cycles", in_out="OUTPUT", socket_type="NodeSocketColor")
    g.interface.new_socket("EEVEE", in_out="OUTPUT", socket_type="NodeSocketColor")
    nodes, links = g.nodes, g.links
    gin = nodes.new("NodeGroupInput")
    gin.location = (-1200, 0)
    gout = nodes.new("NodeGroupOutput")
    gout.location = (1400, 0)
    ra = gin.outputs["Roughness"]
    melanin, redness = gin.outputs["Melanin"], gin.outputs["Redness"]

    def factor(x):
        t = _math(nodes, links, "MULTIPLY_ADD", x, 0.245, 5.574, -1000)
        for k in (-10.73, 2.532, -0.215, 5.969):
            t = _math(nodes, links, "MULTIPLY_ADD", t, x, k, -1000)
        return t

    fa = factor(ra)
    rc = _math(nodes, links, "MULTIPLY_ADD", ra, 0.5, 0.25, -1000)
    neg_fc = _math(nodes, links, "MULTIPLY", factor(rc), -1.0, x=-800)
    s0 = _math(nodes, links, "MULTIPLY_ADD", ra, S1, S0 - 0.3 * S1, -800)
    k = _math(nodes, links, "MULTIPLY_ADD", ra, K, 1.0 - 0.3 * K, -800)
    mk = _math(nodes, links, "MULTIPLY", _math(nodes, links, "POWER", melanin, P, x=-800), k, x=-600)
    split = nodes.new("ShaderNodeSeparateColor")
    split.location = (-800, -400)
    links.new(gin.outputs["Color"], split.inputs[0])
    join = nodes.new("ShaderNodeCombineColor")
    join.location = (1000, 0)
    for i in range(3):  # per channel: the absorption, then the direct color with it
        def m(op, a, b=None, c=None, x=0, row=-300 * (i + 1)):
            return _math(nodes, links, op, a, b, c, x=x, y=row)

        pigment = m("MULTIPLY", m("MULTIPLY_ADD", redness, PHEOMELANIN[i] - EUMELANIN[i], EUMELANIN[i], x=-400), mk, x=-200)
        q = m("DIVIDE", m("LOGARITHM", m("MAXIMUM", split.outputs[i], 1e-4, x=-400), math.e, x=-200), fa, x=0)
        dye = m("MULTIPLY", m("MULTIPLY", q, q, x=200), B, x=400)
        sigma = m("ADD", m("ADD", pigment, dye, x=400), s0, x=600)
        links.new(m("EXPONENT", m("MULTIPLY", m("SQRT", sigma, x=600), neg_fc, x=800), x=800), join.inputs[i])
    links.new(join.outputs[0], gout.inputs["Cycles"])
    links.new(_eevee_curve(nodes, links, join.outputs[0], (1200, -200)), gout.inputs["EEVEE"])
    g["groomlab_version"] = GROUP_VERSION
    return g


def _eevee_curve(nodes, links, color, location):
    """EEVEE_CURVE on a Cycles direct color (an RGB Curves node's C curve); its output."""
    curve = nodes.new("ShaderNodeRGBCurve")
    curve.location = location
    mapping = curve.mapping
    mapping.use_clip = False
    pts = mapping.curves[3].points  # C: all three channels
    while len(pts) > 2:
        pts.remove(pts[len(pts) - 1])
    pts[0].location = EEVEE_CURVE[0]
    pts[1].location = EEVEE_CURVE[-1]
    for x, y in EEVEE_CURVE[1:-1]:
        pts.new(x, y)
    for p in pts:
        p.handle_type = "AUTO_CLAMPED"
    mapping.update()
    links.new(color, curve.inputs["Color"])
    return curve.outputs[0]


# ---- The material -----------------------------------------------------------------------------

def wiring_key(present, look):
    """What the material is built from: rebuilt when this changes."""
    rgb = bool(look.get("colorsByRgb") or look.get("colorBlends"))
    parts = ["v%d" % GROUP_VERSION, "rgb" if rgb else "pigment",
             ",".join(sorted(present)), "%.4f" % arnold_roughness(look), "%.4f" % extra_highlight(look),
             "%.4f" % cuticle_tilt(look),
             "%.4f,%.4f,%.4f" % tuple(look.get("dye") or (1.0, 1.0, 1.0))]
    if look.get("iridescent"):
        parts.append("ir %s %.4f" % (",".join("%.4f" % c for c in look.get("iridescenceColor") or ()),
                                     float(look.get("iridescenceShift", 0.3))))
    return "|".join(parts)


def _attribute(nodes, name, location):
    a = nodes.new("ShaderNodeAttribute")
    a.attribute_type = "GEOMETRY"
    a.attribute_name = name
    a.location = location
    return a


def _hair_inputs(b, rc, tilt, **values):
    """Arnold's highlight on a Principled Hair (by socket name: unavailable sockets are not found by
    key)."""
    values.update({"Roughness": rc, "Radial Roughness": rc, "Coat": 0.0, "IOR": 1.55, "Offset": math.radians(tilt),
                   "Random Color": 0.0, "Random Roughness": 0.0})
    for socket in b.inputs:
        if socket.name in values and not socket.is_linked:
            socket.default_value = values[socket.name]


def _hair_bsdf(nodes, links, color, rc, tilt, location):
    b = nodes.new("ShaderNodeBsdfHairPrincipled")
    b.model = "CHIANG"
    b.parametrization = "COLOR"
    b.location = location
    links.new(color, b.inputs["Color"])
    _hair_inputs(b, rc, tilt)
    return b


def build(mat, present, look):
    """The material's nodes for this look (present: the hair's attributes)."""
    group = hair_color_group()
    if mat.node_tree is None:  # (Blender 4: a new material's nodes are made on use_nodes)
        mat.use_nodes = True
    tree = mat.node_tree
    nodes, links = tree.nodes, tree.links
    nodes.clear()
    ra = arnold_roughness(look)
    rc = cycles_roughness(ra)
    tilt = cuticle_tilt(look)
    rgb = bool(look.get("colorsByRgb") or look.get("colorBlends"))
    dye = tuple(look.get("dye") or (1.0, 1.0, 1.0))

    def color_node(location):
        n = nodes.new("ShaderNodeGroup")
        n.node_tree = group
        n.location = location
        n.inputs["Roughness"].default_value = ra
        return n

    looks = []  # the group node: the pigment's or the RGB's
    if not rgb:
        pig = color_node((-300, 300))
        pig.label = "Pigment"
        pig.inputs["Color"].default_value = dye + (1.0,)
        for name, socket, default, y in (("melanin", "Melanin", DEFAULT_MELANIN, 400), ("melaninRedness", "Redness", DEFAULT_REDNESS, 250)):
            if name in present:
                links.new(_attribute(nodes, name, (-600, y)).outputs["Fac"], pig.inputs[socket])
            else:
                pig.inputs[socket].default_value = default
        looks.append(pig)
    if rgb:
        name = POINT_COLOR if POINT_COLOR in present else STRAND_COLOR
        base = _attribute(nodes, name, (-900, -150)).outputs["Color"] if name in present else None
        rgb_node = color_node((-300, -150))
        rgb_node.label = "RGB"
        if base is None:
            rgb_node.inputs["Color"].default_value = (0.038, 0.014, 0.0022, 1.0)  # create_hair_shader's default (HairColor's)
        if look.get("iridescent") and "iridescence" in present:
            base = _iridescence(nodes, links, look, base, present)
        if base is not None:
            gamma = nodes.new("ShaderNodeGamma")  # create_hair_shader's aiColorCorrect: c^(1 / gamma)
            gamma.location = (-500, -150)
            gamma.inputs["Gamma"].default_value = 1.0 / ARNOLD_BASE_COLOR_GAMMA
            links.new(base, gamma.inputs["Color"])
            links.new(gamma.outputs["Color"], rgb_node.inputs["Color"])
        looks.append(rgb_node)

    extra = extra_highlight(look)
    for engine, y in (("CYCLES", 0), ("EEVEE", -900)):
        output = "Cycles" if engine == "CYCLES" else "EEVEE"
        colors = [n.outputs[output] for n in looks]
        bsdfs = [_hair_bsdf(nodes, links, c, rc, tilt, (300, y + 300 - 450 * i)) for i, c in enumerate(colors)]
        if "roughness" in present:  # HairColor's per strand: Cycles' for Arnold's 0.1 + 0.4 x it
            strand = _math(nodes, links, "MULTIPLY_ADD", _attribute(nodes, "roughness", (0, y - 650)).outputs["Fac"],
                           0.2, 0.3, x=150, y=y - 650)
            for b in bsdfs:
                for name in ("Roughness", "Radial Roughness"):
                    if name in b.inputs:
                        links.new(strand, b.inputs[name])
        shader = bsdfs[0].outputs[0]
        if engine == "CYCLES" and extra > 0.0:
            black = nodes.new("ShaderNodeBsdfHairPrincipled")
            black.label = "Highlight"
            black.model = "CHIANG"
            black.parametrization = "MELANIN"
            black.location = (300, y - 600)
            _hair_inputs(black, rc, tilt, **{"Melanin": 1.0, "Melanin Redness": 0.0})
            scaled = nodes.new("ShaderNodeMixShader")  # extra x the highlight (the other input: nothing)
            scaled.location = (600, y - 450)
            scaled.inputs[0].default_value = extra
            links.new(black.outputs[0], scaled.inputs[2])
            add = nodes.new("ShaderNodeAddShader")
            add.location = (850, y)
            links.new(shader, add.inputs[0])
            links.new(scaled.outputs[0], add.inputs[1])
            shader = add.outputs[0]
        if "opacity" in present:  # RenderData: the hair mixed toward transparent by its opacity per point
            clear = nodes.new("ShaderNodeBsdfTransparent")
            clear.location = (850, y + 250)
            seen = nodes.new("ShaderNodeMixShader")
            seen.location = (975, y + 150)
            links.new(_attribute(nodes, "opacity", (850, y + 400)).outputs["Fac"], seen.inputs[0])
            links.new(clear.outputs[0], seen.inputs[1])
            links.new(shader, seen.inputs[2])
            shader = seen.outputs[0]
        out = nodes.new("ShaderNodeOutputMaterial")
        out.target = engine
        out.location = (1100, y)
        links.new(shader, out.inputs["Surface"])


def _iridescence(nodes, links, look, base, present):
    """create_hair_shader's iridescence network: a ramp of the structural color by |cos| of the
    angle between the view and the strand's root normal, mixed over the base color by the
    iridescence channel."""
    color = look.get("iridescenceColor") or (0.05, 0.55, 0.2)
    shift = float(look.get("iridescenceShift", 0.3))
    geo = nodes.new("ShaderNodeNewGeometry")
    geo.location = (-1500, -500)
    facing = None
    if ROOT_NORMAL in present:
        normal = nodes.new("ShaderNodeVectorTransform")  # the scalp's space (the hair's object) to world
        normal.vector_type = "NORMAL"
        normal.convert_from = "OBJECT"
        normal.convert_to = "WORLD"
        normal.location = (-1300, -650)
        links.new(_attribute(nodes, ROOT_NORMAL, (-1500, -700)).outputs["Vector"], normal.inputs[0])
        unit = nodes.new("ShaderNodeVectorMath")
        unit.operation = "NORMALIZE"
        unit.location = (-1150, -650)
        links.new(normal.outputs[0], unit.inputs[0])
        dot = nodes.new("ShaderNodeVectorMath")
        dot.operation = "DOT_PRODUCT"
        dot.location = (-1000, -550)
        links.new(geo.outputs["Incoming"], dot.inputs[0])
        links.new(unit.outputs[0], dot.inputs[1])
        facing = dot.outputs["Value"]
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.location = (-850, -450)
    ramp.color_ramp.interpolation = "LINEAR"
    elements = ramp.color_ramp.elements
    keys = (0.0, 0.25, 0.5, 0.75, 1.0)  # |cos|: 0 edge-on .. 1 facing
    while len(elements) < len(keys):
        elements.new(0.5)
    for e, at in zip(elements, keys):
        e.position = at
        e.color = tuple(iridescent_tint(color, shift, 1.0 - at)) + (1.0,)
    if facing is not None:
        absolute = nodes.new("ShaderNodeMath")
        absolute.operation = "ABSOLUTE"
        absolute.location = (-1000, -400)
        links.new(facing, absolute.inputs[0])
        links.new(absolute.outputs[0], ramp.inputs["Fac"])
    else:
        ramp.inputs["Fac"].default_value = 1.0
    m = nodes.new("ShaderNodeMix")
    m.data_type = "RGBA"
    m.location = (-550, -250)

    def socket(sockets, name, kind):
        return next(x for x in sockets if x.name == name and x.type == kind)

    links.new(_attribute(nodes, "iridescence", (-850, -150)).outputs["Fac"], socket(m.inputs, "Factor", "VALUE"))
    if base is not None:
        links.new(base, socket(m.inputs, "A", "RGBA"))
    else:
        socket(m.inputs, "A", "RGBA").default_value = (0.3, 0.2, 0.12, 1.0)
    links.new(ramp.outputs["Color"], socket(m.inputs, "B", "RGBA"))
    return socket(m.outputs, "Result", "RGBA")


def ensure_material(scalp, hair, present, look):
    """The hair's material, while it is the one the add-on made (a material of the user's is left
    alone), built for this look when it changed."""
    mat = hair.data.materials[0] if len(hair.data.materials) else None
    if mat is None:
        mat = bpy.data.materials.new("%s GroomLab Hair" % scalp.name)
        mat["groomlab"] = True
        hair.data.materials.append(mat)
    if not mat.get("groomlab"):
        return mat
    key = wiring_key(present, look or {})
    if mat.get("groomlab_wiring") == key and mat.node_tree is not None and hair_color_group() is not None:
        return mat
    build(mat, present, look or {})
    mat["groomlab_wiring"] = key
    return mat


# ---- Render settings --------------------------------------------------------------------------

def use_render_settings(scene):
    """What a groom needs of the scene's render settings, set when a groom is made or opened:
    EEVEE's hair as strips (its Strand shape draws every strand one pixel wide, whatever its
    width; Strip renders the radius, as Cycles' and Arnold's ribbons do)."""
    if scene.render.hair_type == "STRAND":
        scene.render.hair_type = "STRIP"


def render_notes(scene, scalp):
    """What the panel says of settings that render the hair other than Arnold would."""
    notes = []
    if scene.render.engine == "BLENDER_EEVEE" and scene.render.hair_type == "STRAND":
        notes.append("EEVEE draws Strand hair one pixel wide, whatever the strands' width: set Render > Curves > "
                     "Shape to Strip")
    s = scalp.matrix_world.to_scale()
    if max(abs(s.x), abs(s.y), abs(s.z)) > 1.001 * min(abs(s.x), abs(s.y), abs(s.z)):
        notes.append("The scalp's scale is not the same on every axis: the strands' width renders as an average of "
                     "its scales (apply the scale for exact widths)")
    return notes
