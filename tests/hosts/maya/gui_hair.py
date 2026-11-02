# SPDX-License-Identifier: Apache-2.0
"""GroomLab's hair shading in the viewport (hair_shading.h; displayHairShading 1, the default).

Called by the viewport suite (gui_viewport.py: hair_check, on OpenGL and again on DirectX 11), and an
opt-in suite of its own for a look (GROOMLAB_GUI_SUITES=hair; GROOMLAB_HAIR_OUT=<folder> keeps the
pictures): a groom on a sphere under a directional light, drawn with Maya's hair tube shading and
with GroomLab's; the new shading draws the hair, darker for more melanin, brighter for a brighter
light, in the light's color, from where the light is, not at all from a light linked away, lit by a
point light and a spot light too, as lines and as ribbons, with multisample anti-aliasing, through
the level of detail and a GPU drag; its own shadows darken the hair the light reaches through other
hair.
"""

import json
import os

from maya import cmds

from gui_support import check, finish, persp_panel, say, soon

SIZE = (480, 360)


def start():
    soon(run)


def run():
    if os.environ.get("GROOMLAB_HAIR_COMPARE"):
        compare_scene(os.environ.get("GROOMLAB_HAIR_OUT") or ".")
    else:
        hair_check(os.environ.get("GROOMLAB_HAIR_OUT"))
    finish()


def compare_scene(folder):
    """Pictures to set beside Arnold's and the standalone app's (GROOMLAB_HAIR_COMPARE=1): the app's
    lookdev head (tests/data/app head_scalp.obj, head_graph.json) under the app's "key" light, seen
    by the app's camera (GROOMLAB_HAIR_CAMERA: "eye x y z target x y z fov f", as the app prints it),
    on black, color managed as plain sRGB of linear values (scene-linear Rec.709-sRGB, the
    un-tone-mapped view), 960x720: Maya's hair tube shading, GroomLab's hair without and with its
    self-shadowing and strand variation; and the groom as a .groom file (head.groom) for the app and
    kick (tests/data/app/arnold_compare.py --lights key --ocio-linear). The light is the app's key
    over pi: Viewport 2.0 shows pi x a render's radiance (a Lambert there shows color x light), so
    the pictures match the render's at the app's intensity."""
    import math
    import groomlab
    from groomlab import files
    data = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "data", "app")
    cmds.file(new=True, force=True)
    nodes = cmds.file(os.path.join(data, "head_scalp.obj"), i=True, type="OBJ", returnNewNodes=True)
    scalp = cmds.ls(nodes, type="transform")[0]
    groom = groomlab.create_groom(scalp)
    with open(os.path.join(data, "head_graph.json")) as f:
        groomlab.set_graph(groom, json.load(f))
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".previewPointBudget", 0.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr(groom + ".displayShadows", 0)
    material = cmds.shadingNode("lambert", asShader=True)
    cmds.setAttr(material + ".color", 0.36, 0.29, 0.25, type="double3")
    cmds.setAttr(material + ".diffuse", 1.0)
    cmds.select(scalp)
    cmds.hyperShade(assign=material)
    cmds.select(clear=True)
    say("compare: %d strands" % cmds.getAttr(groom + ".outStrandCount"))
    files.export_groom(groom, os.path.join(folder, "head.groom"))
    # The camera.
    words = (os.environ.get("GROOMLAB_HAIR_CAMERA") or
             "eye 22.8547 17.0531 39.2645 target -0.00736618 0.4109 -0.333695 fov 40").split()
    eye, target, fov = [float(w) for w in words[1:4]], [float(w) for w in words[5:8]], float(words[9])
    camera, shape = cmds.camera(name="compareCam")
    cmds.setAttr(camera + ".translate", *eye)
    cmds.viewLookAt(camera, pos=target)
    width, height = 960, 720
    cmds.setAttr(shape + ".horizontalFilmAperture", 1.0)
    cmds.setAttr(shape + ".verticalFilmAperture", float(height) / width)
    cmds.setAttr(shape + ".filmFit", 2)  # vertical
    cmds.setAttr(shape + ".focalLength", 25.4 * 0.5 * (float(height) / width) / math.tan(math.radians(fov) * 0.5))
    cmds.setAttr(shape + ".nearClipPlane", 0.1)
    panel = persp_panel()
    cmds.modelPanel(panel, edit=True, camera=camera)
    # The app's key light: from azimuth -35, elevation 35, (1, 0.97, 0.92) at 2 / pi.
    a, e = math.radians(-35.0), math.radians(35.0)
    toward = (math.sin(a) * math.cos(e), math.sin(e), math.cos(a) * math.cos(e))
    light = cmds.directionalLight(intensity=2.0 / math.pi, rgb=(1.0, 0.97, 0.92))
    rotation = cmds.angleBetween(euler=True, vector1=(0.0, 0.0, 1.0), vector2=toward)
    cmds.setAttr(cmds.listRelatives(light, parent=True)[0] + ".rotate", *rotation)
    cmds.modelEditor(panel, edit=True, displayLights="all", shadows=False, grid=False, headsUpDisplay=False)
    was = {"cm": cmds.colorManagementPrefs(query=True, cmEnabled=True),
           "space": cmds.colorManagementPrefs(query=True, renderingSpaceName=True),
           "view": cmds.colorManagementPrefs(query=True, viewTransformName=True),
           "background": [cmds.displayRGBColor(n, query=True) for n in ("background", "backgroundTop", "backgroundBottom")],
           "gradient": cmds.displayPref(query=True, displayGradient=True)}
    cmds.colorManagementPrefs(edit=True, cmEnabled=True)
    cmds.colorManagementPrefs(edit=True, renderingSpaceName="scene-linear Rec.709-sRGB")
    cmds.colorManagementPrefs(edit=True, viewTransformName="Un-tone-mapped (sRGB)")
    for n in ("background", "backgroundTop", "backgroundBottom"):
        cmds.displayRGBColor(n, 0.0, 0.0, 0.0)
    cmds.displayPref(displayGradient=False)
    # Multisample anti-aliasing (8 samples): the coverage as samples, as a render's pixel filter smooths it.
    msaa = (cmds.getAttr("hardwareRenderingGlobals.multiSampleEnable"), cmds.getAttr("hardwareRenderingGlobals.multiSampleCount"))
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", True)
    cmds.setAttr("hardwareRenderingGlobals.multiSampleCount", 8)

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "viewport_%s.png" % name)
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=(width, height), completeFilename=path)
        say("compare: %s" % path)
        return path

    api = cmds.groomLabGpuPreview(drawApi=True)
    cmds.setAttr(groom + ".displayHairShading", 0)
    shot("%s_maya" % api)
    cmds.setAttr(groom + ".displayHairShading", 1)
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    cmds.setAttr(groom + ".displayStrandVariation", 0.0)
    shot("%s_groomlab_plain" % api)
    cmds.setAttr(groom + ".displayStrandVariation", 1.0)
    shot("%s_groomlab_variation" % api)
    cmds.setAttr(groom + ".displaySelfShadowing", 1.0)
    cmds.setAttr(groom + ".displayStrandVariation", 0.0)
    shot("%s_groomlab_shadows" % api)
    cmds.setAttr(groom + ".displayStrandVariation", 1.0)
    shot("%s_groomlab_shadows_variation" % api)
    say("compare: grid %s" % json.loads(cmds.groomLabGraphStats(groom, hairShading=True))["grid"])
    # More pigments (GROOMLAB_HAIR_VARIANTS="name:melanin:redness,..."): the HairColor node's
    # melanin and redness set, each groom kept (head_<name>.groom) and shot with the shadows.
    for variant in (os.environ.get("GROOMLAB_HAIR_VARIANTS") or "").split(","):
        if not variant:
            continue
        name, melanin, redness = variant.split(":")
        groomlab.set_param(groom, "color", "melanin", float(melanin))
        groomlab.set_param(groom, "color", "redness", float(redness))
        cmds.refresh(force=True)
        files.export_groom(groom, os.path.join(folder, "head_%s.groom" % name))
        cmds.setAttr(groom + ".displaySelfShadowing", 1.0)
        shot("%s_%s" % (api, name))
        cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
        shot("%s_%s_noshadow" % (api, name))
        cmds.setAttr(groom + ".displaySelfShadowing", 1.0)
    # Picked colors (GROOMLAB_HAIR_RGB_VARIANTS="name:r,g,b[:tip r,g,b];..."): HairColor's mode
    # color, the color (and the tip color, at full strength) set, each groom kept
    # (head_<name>.groom: arnold_compare.py --pigment rgb) and shot with the shadows.
    for variant in (os.environ.get("GROOMLAB_HAIR_RGB_VARIANTS") or "").split(";"):
        if not variant:
            continue
        words = variant.split(":")
        name, color = words[0], [float(c) for c in words[1].split(",")]
        groomlab.set_param(groom, "color", "mode", "color")
        groomlab.set_param(groom, "color", "color", color)
        if len(words) > 2:
            groomlab.set_param(groom, "color", "tipColor", [float(c) for c in words[2].split(",")])
            groomlab.set_param(groom, "color", "tipStrength", 1.0)
        else:
            groomlab.set_param(groom, "color", "tipStrength", 0.0)
        cmds.refresh(force=True)
        files.export_groom(groom, os.path.join(folder, "head_%s.groom" % name))
        cmds.setAttr(groom + ".displaySelfShadowing", 1.0)
        shot("%s_%s" % (api, name))
    cmds.colorManagementPrefs(edit=True, cmEnabled=was["cm"])
    cmds.colorManagementPrefs(edit=True, renderingSpaceName=was["space"])
    cmds.colorManagementPrefs(edit=True, viewTransformName=was["view"])
    for n, rgb in zip(("background", "backgroundTop", "backgroundBottom"), was["background"]):
        cmds.displayRGBColor(n, *rgb)
    cmds.displayPref(displayGradient=was["gradient"])
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", msaa[0])
    cmds.setAttr("hardwareRenderingGlobals.multiSampleCount", msaa[1])
    cmds.modelPanel(panel, edit=True, camera="persp")


def pixels(path):
    from groomlab.ui.qt import QtGui
    image = QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)
    return image.width(), image.height(), bytes(image.constBits())[:image.byteCount()]


def hair_stats(path, background):
    """The pixels of `path` that differ from `background` (the same view without the groom) by more
    than a little: (how many, their mean r, g, b in 0..255)."""
    w, h, a = pixels(path)
    w2, h2, b = pixels(background)
    if (w, h) != (w2, h2):
        return 0, (0.0, 0.0, 0.0)
    count, total = 0, [0, 0, 0]
    for i in range(0, len(a), 4):
        if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12:
            count += 1
            total[0] += a[i + 2]
            total[1] += a[i + 1]
            total[2] += a[i]
    if not count:
        return 0, (0.0, 0.0, 0.0)
    return count, tuple(t / float(count) for t in total)


def differ(a, b):
    """The share of pixels that differ between two captures."""
    w, h, da = pixels(a)
    w2, h2, db = pixels(b)
    if (w, h) != (w2, h2):
        return 1.0
    n = sum(1 for i in range(0, len(da), 4) if da[i:i + 3] != db[i:i + 3])
    return n / float(w * h)


def luma(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def linear_rgb(rgb):
    """A mean 8-bit sRGB color as linear values (near enough for ratios)."""
    return [c / 255.0 / 12.92 if c <= 10.31 else ((c / 255.0 + 0.055) / 1.055) ** 2.4 for c in rgb]


def picked_color_check(groom, api, stats):
    """A color picked as RGB (HairColor's mode color) draws as Arnold's aiStandardHair renders it as
    its base color (hair_bsdf.glsl hairColorSigma: Chiang et al.'s color to absorption), not as the
    fiber's transmittance of one pass (squared and more by the lobes and the multiple scattering: a
    skin tone drew dark red, a mid grey a tenth of Arnold's brightness). Against the default
    melanin 0.35 brown in the same view, as Arnold renders the head groom
    (tests/data/app/head_graph.json, the key light, arnold_compare.py --linear-srgb, the pixels
    Arnold lights): a picked 0.5 grey 7.0 times the brown's luminance (the viewport 11.2 on DirectX
    11; 0.7 before), a skin tone with white tips 1.06 times the grey's red over blue (the viewport
    1.05; 2.6 before). Here, the skin without tips: a little warmer."""
    import groomlab
    brown = linear_rgb(stats("picked_brown")[1])
    groomlab.set_param(groom, "color", "mode", "color")
    try:
        groomlab.set_param(groom, "color", "color", [0.5, 0.5, 0.5])
        grey = linear_rgb(stats("picked_grey")[1])
        groomlab.set_param(groom, "color", "color", [0.8, 0.56, 0.45])
        skin = linear_rgb(stats("picked_skin")[1])
    finally:
        groomlab.set_param(groom, "color", "mode", "melanin")
    bright = luma(grey) / max(luma(brown), 1e-6)
    warm = (skin[0] / max(skin[2], 1e-6)) / max(grey[0] / max(grey[2], 1e-6), 1e-6)
    check("GroomLab's hair on %s draws a picked color as Arnold renders it: a 0.5 grey %.2f times the melanin "
          "brown's luminance (Arnold 7.0 on the head), a skin tone's red over blue %.2f times the grey's (linear %s, %s, %s)"
          % (api, bright, warm, ["%.3f" % c for c in grey], ["%.3f" % c for c in skin], ["%.3f" % c for c in brown]),
          3.0 < bright < 40.0 and 1.03 < warm < 1.9)


def hair_check(snapshots=None):
    import tempfile
    import groomlab
    folder = snapshots or tempfile.mkdtemp(prefix="groomlab_hair_")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    api = cmds.groomLabGpuPreview(drawApi=True)
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=1, subdivisionsX=60, subdivisionsY=30)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_param(groom, "scatter", "density", 3000.0)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr(groom + ".displayShadows", 0)
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)  # (its own check, below)
    panel = persp_panel()
    cmds.setAttr("persp.rotate", -15.0, 30.0, 0.0)
    cmds.setAttr("perspShape.focalLength", 35.0)
    cmds.viewFit("persp", [groom], fitFactor=0.9)
    light = cmds.directionalLight(intensity=1.0)
    light_xf = cmds.listRelatives(light, parent=True)[0]
    cmds.setAttr(light_xf + ".rotate", -40.0, 35.0, 0.0)
    cmds.modelEditor(panel, edit=True, displayLights="all", shadows=False)
    say("hair shading: %d strands on %s" % (cmds.getAttr(groom + ".outStrandCount"), api))
    say("color management: %s, rendering space %s, view %s; spaces %s; views %s" % (
        cmds.colorManagementPrefs(query=True, cmEnabled=True), cmds.colorManagementPrefs(query=True, renderingSpaceName=True),
        cmds.colorManagementPrefs(query=True, viewTransformName=True), cmds.colorManagementPrefs(query=True, renderingSpaceNames=True),
        cmds.colorManagementPrefs(query=True, viewTransformNames=True)))

    def shot(name):
        cmds.refresh(currentView=True, force=True)
        cmds.refresh(currentView=True, force=True)  # (the lights a draw takes: the first draw's after them)
        path = os.path.join(folder, "hair_%s_%s.png" % (api, name))
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=SIZE, completeFilename=path)
        return path

    cmds.setAttr(groom + ".visibility", False)
    background = shot("background")
    cmds.setAttr(groom + ".visibility", True)

    cmds.setAttr(groom + ".displayHairShading", 0)
    legacy = shot("maya")
    cmds.setAttr(groom + ".displayHairShading", 1)
    hair = shot("groomlab")
    lights = json.loads(cmds.groomLabGraphStats(groom, hairShading=True))
    count, rgb = hair_stats(hair, background)
    cmds.setAttr(groom + ".displayStrandVariation", 0.0)
    say("hair shading on %s without the strand variation: mean luma %.1f" % (api, luma(hair_stats(shot("groomlab_plain"), background)[1])))
    cmds.setAttr(groom + ".displayStrandVariation", 1.0)
    legacy_count, legacy_rgb = hair_stats(legacy, background)
    say("hair shading on %s: %d hair pixels, mean %s (Maya's: %d, %s); lights %s"
        % (api, count, ["%.1f" % c for c in rgb], legacy_count, ["%.1f" % c for c in legacy_rgb], lights))
    check("GroomLab's hair shading on %s draws the hair, lit (%d pixels, mean luma %.1f; Maya's %d), other than Maya's "
          "(%.3f of the pixels differ), with the scene's light (%s)"
          % (api, count, luma(rgb), legacy_count, differ(hair, legacy), lights.get("count")),
          count > 0.5 * legacy_count and luma(rgb) > 8.0 and differ(hair, legacy) > 0.02 and lights.get("count") == 1
          and lights["lights"][0]["kind"] == "directional")

    # Melanin: more is darker.
    lumas = []
    for melanin in (0.1, 0.5, 0.9):
        groomlab.set_param(groom, "color", "melanin", melanin)
        lumas.append(luma(hair_stats(shot("melanin_%g" % melanin), background)[1]))
    groomlab.set_param(groom, "color", "melanin", 0.35)
    check("GroomLab's hair on %s: more melanin draws darker hair (mean luma %s for melanin 0.1, 0.5, 0.9)"
          % (api, ["%.1f" % v for v in lumas]), lumas[0] > lumas[1] > lumas[2] and lumas[0] > 1.5 * lumas[2])
    picked_color_check(groom, api, lambda name: hair_stats(shot(name), background))
    wet_check(groom, api, lambda name: hair_stats(shot(name), background))

    # The light: brighter, colored, moved, linked away.
    base = luma(hair_stats(shot("light_1"), background)[1])
    cmds.setAttr(light + ".intensity", 3.0)
    bright = luma(hair_stats(shot("light_3"), background)[1])
    cmds.setAttr(light + ".intensity", 1.0)
    cmds.setAttr(light + ".color", 1.0, 0.1, 0.1, type="double3")
    red = hair_stats(shot("light_red"), background)[1]
    cmds.setAttr(light + ".color", 1.0, 1.0, 1.0, type="double3")
    check("GroomLab's hair on %s takes the light's intensity (mean luma %.1f at 1, %.1f at 3) and color (%s under red)"
          % (api, base, bright, ["%.1f" % c for c in red]), bright > 1.3 * base and red[0] > 1.5 * red[1] and red[0] > 1.5 * red[2])
    front = shot("light_front")
    cmds.setAttr(light_xf + ".rotate", -40.0, 215.0, 0.0)
    behind = shot("light_behind")
    cmds.setAttr(light_xf + ".rotate", -40.0, 35.0, 0.0)
    check("GroomLab's hair on %s shades from where the light is (%.3f of the pixels differ with the light behind)"
          % (api, differ(front, behind)), differ(front, behind) > 0.05)
    # Light linking: Maya's lightlink takes a groom's shading group (not the groom itself), and a light
    # not lighting everything (out of defaultLightSet) lights only what is linked to it.
    import time
    material = cmds.shadingNode("lambert", asShader=True)
    cmds.setAttr(material + ".color", 0.3, 0.2, 0.12, type="double3")
    group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="hairCheckSG")
    cmds.connectAttr(material + ".outColor", group + ".surfaceShader")
    from groomlab.shading import assign_shading_group
    assign_shading_group(groom, group)  # (sets -forceElement rejects plugin shapes)
    linked = luma(hair_stats(shot("linked"), background)[1])

    def relink(step):
        step()
        time.sleep(1.1)  # (the groom's ignored lights are looked at again after a second)
        cmds.setAttr(groom + ".ribbonWidthScale", cmds.getAttr(groom + ".ribbonWidthScale") + 0.01)  # (an update)
        cmds.refresh(force=True)

    relink(lambda: cmds.lightlink(b=True, light=light, object=group))
    unlinked = luma(hair_stats(shot("unlinked"), background)[1])
    unlinked_lights = json.loads(cmds.groomLabGraphStats(groom, hairShading=True))
    relink(lambda: cmds.lightlink(make=True, light=light, object=group))
    relinked = luma(hair_stats(shot("relinked"), background)[1])
    relink(lambda: cmds.sets(light_xf, remove="defaultLightSet"))
    not_default = luma(hair_stats(shot("not_default"), background)[1])
    not_default_lights = json.loads(cmds.groomLabGraphStats(groom, hairShading=True))
    relink(lambda: cmds.sets(light_xf, add="defaultLightSet"))
    say("light linking: %s; %s" % (unlinked_lights, not_default_lights))
    check("GroomLab's hair on %s keeps the light linking (a light linked away from its shading group lights nothing: "
          "%d lights, mean luma %.1f; linked again %.1f; a light not lighting everything, unlinked: %d lights, %.1f; "
          "linked %.1f)" % (api, unlinked_lights.get("count"), unlinked, relinked, not_default_lights.get("count"),
                            not_default, linked),
          unlinked_lights.get("count") == 0 and unlinked < 0.5 * relinked and not_default_lights.get("count") == 0
          and not_default < 0.5 * linked and relinked > 0.8 * linked)
    cmds.delete(group, material)
    # Point and spot lights.
    cmds.setAttr(light + ".intensity", 0.0)
    point = cmds.pointLight(intensity=30.0, position=(3.0, 4.0, 4.0))
    lit_point = luma(hair_stats(shot("point"), background)[1])
    kinds = [l["kind"] for l in json.loads(cmds.groomLabGraphStats(groom, hairShading=True))["lights"]]
    cmds.delete(cmds.listRelatives(point, parent=True) or point)
    spot = cmds.spotLight(intensity=30.0, coneAngle=60.0)
    spot_xf = cmds.listRelatives(spot, parent=True)[0]
    cmds.setAttr(spot_xf + ".translate", 3.0, 4.0, 4.0)
    cmds.aimConstraint(scalp, spot_xf, aimVector=(0, 0, -1))
    lit_spot = luma(hair_stats(shot("spot"), background)[1])
    spot_kinds = [l["kind"] for l in json.loads(cmds.groomLabGraphStats(groom, hairShading=True))["lights"]]
    cmds.delete(spot_xf)
    cmds.setAttr(light + ".intensity", 1.0)
    check("GroomLab's hair on %s is lit by a point light (%s, mean luma %.1f) and a spot light (%s, %.1f)"
          % (api, kinds, lit_point, spot_kinds, lit_spot), "point" in kinds and "spot" in spot_kinds and lit_point > 8.0 and lit_spot > 8.0)

    # Ribbons, multisample anti-aliasing, the strands' own colors.
    cmds.setAttr(groom + ".displayRibbons", True)
    ribbons = hair_stats(shot("ribbons"), background)
    cmds.setAttr(groom + ".displayRibbons", False)
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", True)
    msaa = hair_stats(shot("msaa"), background)
    cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", False)
    check("GroomLab's hair on %s draws ribbons (%d pixels, luma %.1f) and lines on a multisampled target (%d, %.1f)"
          % (api, ribbons[0], luma(ribbons[1]), msaa[0], luma(msaa[1])),
          ribbons[0] > 0.5 * count and luma(ribbons[1]) > 4.0 and msaa[0] > 0.5 * count and luma(msaa[1]) > 4.0)

    def restyle():
        cmds.setAttr(groom + ".ribbonWidthScale", cmds.getAttr(groom + ".ribbonWidthScale") + 0.001)  # (styled again)

    # Each strand its own: the debug view of each strand's random numbers shows many colors (one a
    # strand, from its ID), and the strand variation sets neighboring strands apart.
    cmds.optionVar(intValue=("groomLabHairDebug", 2))
    restyle()
    strand_colors = distinct_colors(shot("debug_strands"), background)
    # The ribbons too: their uvs carry each strand's number as the lines' do.
    cmds.setAttr(groom + ".displayRibbons", True)
    ribbon_colors = distinct_colors(shot("debug_strands_ribbons"), background)
    cmds.setAttr(groom + ".displayRibbons", False)
    # The structural color's hue turned by each strand's number (hairHue's fractions): every hue.
    cmds.optionVar(intValue=("groomLabHairDebug", 3))
    restyle()
    hues = hue_bins(shot("debug_hue"), background)
    cmds.optionVar(intValue=("groomLabHairDebug", 1))
    restyle()
    tangent_colors = distinct_colors(shot("debug_tangents"), background)
    cmds.optionVar(remove="groomLabHairDebug")
    restyle()
    check("GroomLab's hair on %s gives each ribbon its strand's random numbers (%d colors of them) and turns the "
          "structural color's hue through every sector (%d of 12)" % (api, ribbon_colors, hues), ribbon_colors > 200 and hues >= 10)
    # Self-shadowing: the hair's density grid shadows the hair the light reaches through other hair (the light it
    # scatters on through them brightens it again: darker by a little, on the whole).
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    unshadowed = hair_stats(shot("self_shadow_off"), background)[1]
    cmds.setAttr(groom + ".displaySelfShadowing", 1.0)
    shadowed = hair_stats(shot("self_shadow_on"), background)[1]
    grid = json.loads(cmds.groomLabGraphStats(groom, hairShading=True))["grid"]
    say("hair shading grid on %s: %s" % (api, grid))
    check("GroomLab's hair on %s shadows itself (a %s grid of %.3g cells: mean luma %.1f against %.1f unshadowed)"
          % (api, grid.get("size"), grid.get("cell", 0), luma(shadowed), luma(unshadowed)),
          grid.get("valid") and luma(shadowed) < 0.95 * luma(unshadowed) and luma(shadowed) > 0.05 * luma(unshadowed))
    # A GPU drag moves the grid with the strands.
    serial = grid.get("serial", 0)
    if api in ("DirectX11", "OpenGLCoreProfile"):
        taken = cmds.groomLabGpuPreview(groom, node="noise", param="amplitude", value=0.5)
        shot("self_shadow_drag")
        dragged = json.loads(cmds.groomLabGraphStats(groom, hairShading=True))["grid"]
        cmds.groomLabGpuPreview(groom, endDrag=True)
        check("GroomLab's hair on %s: a GPU drag (taken %s) writes the grid again from the dragged strands (%s grids, %s before)"
              % (api, taken, dragged.get("serial"), serial), not taken or (dragged.get("valid") and dragged.get("serial", 0) > serial))

    # The level of detail leaves a near view alone: every strand drawn, no impostors.
    cmds.viewFit("persp", [groom], fitFactor=4.0)  # (close: the groom four times the view)
    cmds.refresh(force=True)
    lod = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"].get("lod", {})
    shot("near")
    cmds.setAttr(groom + ".visibility", False)
    background = shot("near_background")
    cmds.setAttr(groom + ".visibility", True)
    check("GroomLab's hair on %s gives each strand its own random numbers (%d colors of them) and each point its "
          "tangent (%d colors of them)" % (api, strand_colors, tangent_colors), strand_colors > 200 and tangent_colors > 50)
    check("GroomLab's hair on %s, near: the level of detail draws every strand (%s)" % (api, lod),
          lod.get("drawnStrands", lod.get("totalStrands")) == lod.get("totalStrands") and not lod.get("impostors"))
    say("hair shading pictures: %s" % folder)
    check("GroomLab's hair made no errors", not groomlab.errors(groom))
    tangent_check(folder, api)
    variation_check(folder, api)
    cmds.file(new=True, force=True)


def wet_check(groom, api, stats):
    """The wetness channel (Wet's; hair_color.h wetShading): the same blonde strands wet draw darker
    than dry (soaked by an Attribute node's strand channel of the name, so they stay where they
    were, where a Wet clumps them; `stats(name)` shoots the view: (hair pixels, mean rgb))."""
    import groomlab
    blonde = 0.15
    groomlab.set_param(groom, "color", "melanin", blonde)
    dry = stats("wet_dry")
    graph = groomlab.get_graph(groom, expand_default=True)
    saved = json.loads(json.dumps(graph))
    feed = next(c for c in graph["connections"] if c["to"] == "output" and c["input"] == "strands")
    graph["nodes"].insert(-1, {"name": "wet", "type": "Attribute", "params": {"name": "wetness", "value": 1.0}})
    graph["connections"].append({"from": feed["from"], "to": "wet", "input": "strands"})
    feed["from"] = "wet"
    groomlab.set_graph(groom, graph)
    wet = stats("wet_soaked")
    groomlab.set_graph(groom, saved)
    groomlab.set_param(groom, "color", "melanin", 0.35)
    check("GroomLab's hair on %s draws wet hair darker (Wet's wetness: mean luma %.1f against %.1f dry; %d and %d "
          "pixels)" % (api, luma(wet[1]), luma(dry[1]), wet[0], dry[0]),
          luma(wet[1]) < 0.85 * luma(dry[1]) and luma(wet[1]) > 0.1 * luma(dry[1]) and wet[0] > 0.6 * dry[0])


def tangent_check(folder, api):
    """A few curly strands: the hair shading takes each point's own tangent (the debug view of the
    tangents shows many directions along each strand, not one a strand)."""
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polyPlane(width=2, height=2, subdivisionsX=4, subdivisionsY=4)[0]
    groom = groomlab.create_groom(scalp)
    graph = groomlab.get_graph(groom, expand_default=True)
    graph["nodes"].insert(-1, {"name": "curl", "type": "Curl", "params": {"radius": 0.2, "frequency": 1.5}})
    graph["connections"] = [c for c in graph["connections"] if c["to"] != "output"]
    last = [c for c in graph["connections"] if c["to"] == "curl"]
    feeding = [n["name"] for n in graph["nodes"] if n["name"] not in ("output", "curl")][-1]
    graph["connections"] += [{"from": feeding, "to": "curl", "input": "strands"}, {"from": "curl", "to": "output", "input": "strands"}]
    groomlab.set_graph(groom, graph)
    groomlab.set_param(groom, "scatter", "density", 4.0)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    count = cmds.getAttr(groom + ".outStrandCount")
    cmds.viewFit("persp", [groom], fitFactor=0.9)
    cmds.optionVar(intValue=("groomLabHairDebug", 1))
    cmds.refresh(force=True)
    path = os.path.join(folder, "hair_%s_curly_tangents.png" % api)
    cmds.refresh(currentView=True, force=True)
    cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                   percent=100, widthHeight=SIZE, completeFilename=path)
    cmds.optionVar(remove="groomLabHairDebug")
    cmds.setAttr(groom + ".visibility", False)
    background = path.replace(".png", "_background.png")
    cmds.refresh(currentView=True, force=True)
    cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                   percent=100, widthHeight=SIZE, completeFilename=background)
    colors = distinct_colors(path, background)
    check("GroomLab's hair on %s takes each point's own tangent: %d curly strands show %d tangent colors (per strand: at most %d)"
          % (api, count, colors, count), count > 0 and colors > 4 * count)


def variation_check(folder, api):
    """Straight parallel strands side on (an orthographic front view, lit from the front and side):
    without the strand variation every strand shades alike but for its coverage; with it each strand
    its own highlight and pigment, so the strands' columns differ from each other."""
    import groomlab
    cmds.file(new=True, force=True)
    scalp = cmds.polyPlane(width=4, height=4, subdivisionsX=8, subdivisionsY=8)[0]
    groom = groomlab.create_groom(scalp)
    groomlab.set_graph(groom, {
        "version": 1, "output": "output",
        "nodes": [{"name": "scalp", "type": "Scalp"},
                  {"name": "scatter", "type": "Scatter", "params": {"density": 3.0, "length": 3.0}},
                  {"name": "color", "type": "HairColor", "params": {"melanin": 0.4}},
                  {"name": "width", "type": "Width", "params": {"width": 0.04}},
                  {"name": "output", "type": "Output"}],
        "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                        {"from": "scatter", "to": "color", "input": "strands"},
                        {"from": "color", "to": "width", "input": "strands"},
                        {"from": "width", "to": "output", "input": "strands"}]})
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".displayGuides", False)
    cmds.setAttr(groom + ".displaySelfShadowing", 0.0)
    panel = persp_panel()
    cmds.setAttr("front.translate", 0.0, 1.5, 100.0)
    cmds.setAttr("front.rotate", 0.0, 0.0, 0.0)
    cmds.setAttr("frontShape.orthographicWidth", 4.5)
    cmds.modelPanel(panel, edit=True, camera="front")
    light = cmds.directionalLight(intensity=3.0)
    cmds.setAttr(cmds.listRelatives(light, parent=True)[0] + ".rotate", -20.0, 30.0, 0.0)
    cmds.modelEditor(panel, edit=True, displayLights="all")
    spreads = {}
    for variation in (0.0, 1.0, 4.0):
        cmds.setAttr(groom + ".displayStrandVariation", variation)
        cmds.refresh(currentView=True, force=True)
        cmds.refresh(currentView=True, force=True)
        path = os.path.join(folder, "hair_%s_variation_side_%g.png" % (api, variation))
        cmds.playblast(frame=[1], format="image", compression="png", viewer=False, showOrnaments=False,
                       percent=100, widthHeight=SIZE, completeFilename=path)
        w, h, data = pixels(path)
        # Each strand's color: the mean luma of a column's strand pixels over the strands' middle
        # rows (the hashed coverage keeps whole pixels of it, the rest is the background), and how
        # far apart the strands are.
        columns = []
        for x in range(w):
            values = []
            for y in range(int(0.35 * h), int(0.65 * h)):
                i = (y * w + x) * 4
                value = 0.0722 * data[i] + 0.7152 * data[i + 1] + 0.2126 * data[i + 2]
                if value < 235.0:  # a strand's pixel (hashed coverage: the strand's color, opaque)
                    values.append(value)
            if len(values) >= 20:
                columns.append(sum(values) / len(values))
        m = sum(columns) / max(len(columns), 1)
        spreads[variation] = (sum((c - m) ** 2 for c in columns) / max(len(columns), 1)) ** 0.5
    say("strand variation side on: column spreads %s; %.3f of the pixels differ" % (spreads, differ(os.path.join(folder, "hair_%s_variation_side_0.png" % api), os.path.join(folder, "hair_%s_variation_side_1.png" % api))))
    cmds.modelPanel(panel, edit=True, camera="persp")
    check("GroomLab's hair on %s shades each strand its own: parallel strands' columns %.1f apart with the strand "
          "variation, %.1f without (blending at the strands' edges sets them apart some without)" % (api, spreads[1.0], spreads[0.0]),
          spreads[1.0] > 1.08 * spreads[0.0])


def distinct_colors(path, background):
    """The colors (quantized to 16 levels a channel) of the pixels off the background."""
    w, h, a = pixels(path)
    _, _, b = pixels(background)
    seen = set()
    for i in range(0, len(a), 4):
        if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12:
            seen.add((a[i] >> 4, a[i + 1] >> 4, a[i + 2] >> 4))
    return len(seen)


def hue_bins(path, background):
    """How many of 12 hue sectors hold at least 1% of the saturated pixels off the background."""
    import colorsys
    w, h, a = pixels(path)
    _, _, b = pixels(background)
    bins = [0] * 12
    total = 0
    for i in range(0, len(a), 4):
        if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12:
            hue, sat, val = colorsys.rgb_to_hsv(a[i + 2] / 255.0, a[i + 1] / 255.0, a[i] / 255.0)
            if sat > 0.3 and val > 0.1:
                bins[min(int(hue * 12), 11)] += 1
                total += 1
    return sum(1 for n in bins if total and n > 0.01 * total)


def neighbor_contrast(path, background):
    """The mean luma difference between horizontally neighboring hair pixels (both off the background)."""
    w, h, a = pixels(path)
    _, _, b = pixels(background)

    def hair(i):
        return abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 12

    total, count = 0.0, 0
    for y in range(h):
        row = y * w * 4
        for x in range(w - 1):
            i, j = row + x * 4, row + x * 4 + 4
            if hair(i) and hair(j):
                total += abs(0.0722 * (a[i] - a[j]) + 0.7152 * (a[i + 1] - a[j + 1]) + 0.2126 * (a[i + 2] - a[j + 2]))
                count += 1
    return total / max(count, 1)
