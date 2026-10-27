# SPDX-License-Identifier: Apache-2.0
"""The preset contact sheets (ctest maya.preset_sheet, maya.preset_sheet_dx11): every shipped preset
applied the way the Graph Editor's Presets menu applies it (presets.apply_preset, fitted to the
scalp), drawn by Viewport 2.0 from the perspective camera at the groom's default preview density,
one picture each, then laid out on a labelled sheet per scene. A look-over tool, not a check: the
suite fails only when a preset errors or draws nothing.

A suite of the GUI session (gui_runner.py; harness: gui_support.py), opt-in: it runs only when
GROOMLAB_GUI_SUITES names it. The scenes:

  head      tests/data/app/head_scalp.obj (a scalp cap, ~17 cm) on a head and neck
  creature  a wolf-sized four-legged body (~140 cm long), one mesh: the scalp is the whole body
  small     the head at a tenth of the size (~1.7 units): the fit must give the same picture

A preset that reads guides gets guides first, as an artist would groom them (a small graph baked
to guides, comb_graph): on the head, a shoulder-length style split by a center part (drawn with
the Part tool, so the presets' Interpolate and Clump respect it), lift at the crown, hanging under
gravity off the head and neck (colliders); feathers and scales get guides of their own strand
length. Presets that make their own shapes (Flow, Bend) get none.

Environment (all optional):
  GROOMLAB_PRESET_SHEET_DIR      where the pictures and sheets go (default: <temp>/groomlab_preset_sheet)
  GROOMLAB_PRESET_SHEET_SCENES   head,creature,small (default head,creature)
  GROOMLAB_PRESET_SHEET_NAMES    only these presets (comma-separated)
  GROOMLAB_PRESET_SHEET_TAG      a prefix for the sheets' names (before, after)
  GROOMLAB_PRESET_SHEET_SIZE     each picture's size in pixels (default 420)
  GROOMLAB_PRESET_SHEET_DENSITY  the grooms' previewDensity (default: the groom's own, 0.1)
  GROOMLAB_PRESET_SHEET_PARAMS   params set on every node of a type once a preset is applied,
                                 Type.param=value;... (a value as JSON: Interpolate.blend="offsets")
"""

import math
import os
import tempfile
import time

from maya import cmds

from gui_support import check, finish, persp_panel, say, soon

HERE = os.path.dirname(os.path.abspath(__file__))
HEAD_SCALP = os.path.normpath(os.path.join(HERE, "..", "..", "data", "app", "head_scalp.obj"))


def start():
    soon(run)


def _env_list(name, default):
    value = os.environ.get(name, "")
    return [v.strip() for v in value.split(",") if v.strip()] or list(default)


# -- Scenes ---------------------------------------------------------------------------------------

def _import_head_scalp(scale):
    before = set(cmds.ls(type="mesh", long=True))
    cmds.file(HEAD_SCALP, i=True, type="OBJ", ignoreVersion=True, options="mo=0", namespace="head")
    meshes = [m for m in cmds.ls(type="mesh", long=True) if m not in before]
    scalp = cmds.listRelatives(meshes[0], parent=True, fullPath=True)[0]
    scalp = cmds.rename(scalp, "scalp")
    if scale != 1.0:
        cmds.setAttr(scalp + ".scale", scale, scale, scale)
        cmds.makeIdentity(scalp, apply=True, scale=True)
    return scalp


def _context(name, mesh, color):
    """A non-scalp mesh for context (the head under the scalp, a neck), shaded in `color`."""
    shader = cmds.shadingNode("lambert", asShader=True, name=name + "Skin")
    cmds.setAttr(shader + ".color", *color, type="double3")
    group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=shader + "SG")
    cmds.connectAttr(shader + ".outColor", group + ".surfaceShader")
    cmds.sets(mesh, edit=True, forceElement=group)
    return mesh


def head_scene(scale=1.0):
    scalp = _import_head_scalp(scale)
    # The skull under the cap (its ellipsoid, 8 x 10 x 9 cm half-axes, center 2 cm down) a hair
    # smaller, and a neck.
    head = cmds.polySphere(radius=1, subdivisionsX=48, subdivisionsY=32, name="headMesh")[0]
    cmds.setAttr(head + ".scale", 7.95 * scale, 9.95 * scale, 8.95 * scale)
    cmds.setAttr(head + ".translate", 0, -2.0 * scale, 0)
    neck = cmds.polyCylinder(radius=4.2 * scale, height=12 * scale, subdivisionsX=32, name="neckMesh")[0]
    cmds.setAttr(neck + ".translate", 0, -13.0 * scale, -1.0 * scale)
    for mesh in (head, neck):
        _context(mesh, mesh, (0.62, 0.5, 0.44))
    _context(scalp, scalp, (0.62, 0.5, 0.44))
    return scalp, (-24.0, 30.0)  # camera: a three-quarter look from the front, above (the part shows)


def creature_scene():
    """A wolf-sized body in one mesh (y up, nose toward +z): an ellipsoid torso, a head, a neck,
    four legs and a tail, united; the scalp is all of it (fur grows where the skin is)."""
    parts = []
    torso = cmds.polySphere(radius=1, subdivisionsX=64, subdivisionsY=32)[0]
    cmds.setAttr(torso + ".scale", 20, 22, 55)
    cmds.setAttr(torso + ".translate", 0, 70, 0)
    parts.append(torso)
    head = cmds.polySphere(radius=1, subdivisionsX=40, subdivisionsY=24)[0]
    cmds.setAttr(head + ".scale", 13, 13, 18)
    cmds.setAttr(head + ".translate", 0, 92, 62)
    parts.append(head)
    neck = cmds.polyCylinder(radius=11, height=34, subdivisionsX=32, subdivisionsY=4)[0]
    cmds.setAttr(neck + ".translate", 0, 84, 44)
    cmds.setAttr(neck + ".rotate", 55, 0, 0)
    parts.append(neck)
    for x, z in ((-11, 36), (11, 36), (-11, -36), (11, -36)):
        leg = cmds.polyCylinder(radius=5, height=58, subdivisionsX=24, subdivisionsY=6)[0]
        cmds.setAttr(leg + ".translate", x, 30, z)
        parts.append(leg)
    tail = cmds.polyCylinder(radius=5, height=45, subdivisionsX=24, subdivisionsY=6)[0]
    cmds.setAttr(tail + ".translate", 0, 62, -70)
    cmds.setAttr(tail + ".rotate", -60, 0, 0)
    parts.append(tail)
    body = cmds.polyUnite(*parts, constructionHistory=False, name="creature")[0]
    cmds.polyAutoProjection(body, constructionHistory=False)  # UVs for maps and uvGrid
    _context(body, body, (0.45, 0.4, 0.36))
    return body, (-14.0, 50.0)


# -- Guides ---------------------------------------------------------------------------------------

def _scatter_length(preset):
    """The length the preset's strands grow to (the Scatter upstream of its first Interpolate, else
    its first Scatter), or 0."""
    nodes = {n["name"]: n for n in preset.get("nodes", [])}
    feeding = {c["to"]: c["from"] for c in preset.get("connections", []) if c.get("input") == "strands"}
    for name, n in nodes.items():
        if n.get("type") != "Interpolate":
            continue
        src = feeding.get(name)
        while src and nodes.get(src, {}).get("type") != "Scatter":
            src = feeding.get(src)
        if src:
            return float(nodes[src].get("params", {}).get("length", 1.0))
    for n in preset.get("nodes", []):
        if n.get("type") == "Scatter":
            return float(n.get("params", {}).get("length", 1.0))
    return 0.0


def _uses_guides(preset):
    nodes = {n["name"]: n for n in preset.get("nodes", [])}
    for c in preset.get("connections", []):
        if nodes.get(c["from"], {}).get("type") == "Guides":
            return True
    return False


HAIR_CATEGORIES = ("Hair", "Motion", "Techniques", "Games", "Add-ons")


def comb_graph(length, spacing, colliders=()):
    """Guides as an artist grooms a shoulder-length style: about `spacing` apart, `length` long at
    the back and shorter toward the face, split by a center part (each side combed along the skin
    away from it, the front swept back from the face), a little lift at the roots for volume at the
    crown, then hanging under gravity, kept off the scalp and the `colliders` (ColliderMesh names:
    the head and neck under the scalp cap)."""
    side = "(smoothstep(-0.12, 0.12, $nx) * 2 - 1)"
    nodes = [
        {"name": "scalp", "type": "Scalp"},
        {"name": "roots", "type": "Scatter", "params": {"density": 1.0 / (spacing * spacing), "length": length,
                                                         "randomness": 0.0, "seed": 3}},
        # Longer at the back, shorter toward the face, a little uneven.
        {"name": "lengths", "type": "Transform", "params": {"scaleRandom": 0.06, "scale": {
            "expression": "1 - 0.3 * smoothstep(-0.4, 0.8, $nz)", "min": 0.0, "max": 1.0}}},
        # Away from the part, down the sides and back, the front swept back off the face.
        {"name": "part", "type": "Bend", "params": {
            "direction": {"expression": "normalize([%s * (1 + 2 * smoothstep(0.3, 0.9, $nz)), -0.5 + 1.2 * smoothstep(0.35, 0.9, $nz), -0.6 - 1.4 * smoothstep(0.3, 0.85, $nz)])" % side},
            "angle": 60.0, "start": 0.0, "end": 0.2, "along": "direction"}},
        {"name": "lift", "type": "Bend", "params": {"angle": 8.0, "end": 0.0}},
        {"name": "hang", "type": "Gravity", "params": {"strength": 1.3, "stiffness": 0.2}},
    ]
    chain = ["roots", "lengths", "part", "lift", "hang"]
    for i, name in enumerate(colliders):
        nodes.append({"name": "mesh%d" % i, "type": "ColliderMesh", "params": {"name": name}})
        nodes.append({"name": "collide%d" % i, "type": "Collide", "params": {
            "offset": length * 0.012, "maxDepth": length, "iterations": 4}})
        chain.append("collide%d" % i)
    nodes += [{"name": "resample", "type": "Resample", "params": {"segments": 12}}, {"name": "output", "type": "Output"}]
    chain += ["resample", "output"]
    connections = [{"from": "scalp", "to": "roots", "input": "scalp"}]
    connections += [{"from": a, "to": b, "input": "strands"} for a, b in zip(chain, chain[1:])]
    for i, _ in enumerate(colliders):
        connections.append({"from": "scalp", "to": "collide%d" % i, "input": "scalp"})
        connections.append({"from": "mesh%d" % i, "to": "collide%d" % i, "input": "collider"})
    return {"version": 1, "output": "output", "nodes": nodes, "connections": connections}


def give_guides(groom, preset, size, colliders=()):
    """Guides for a preset that reads them: for hair, a shoulder-length style (1.3 times the scalp
    size at the back, whatever the preset's own strand length); otherwise its strand length. A
    hand-placed look (a guide group it requires, hand_placed_feathers) gets a few guides drawn into
    that group, as an artist draws them."""
    import groomlab
    from groomlab import presets
    groomlab.clear_guides(groom)
    if not _uses_guides(preset):
        return 0
    groups = presets.required_groups_of(preset)
    category = (preset.get("about") or {}).get("category")
    spacing = size / 10.0
    if groups:
        length, spacing = 0.7 * size, size / 4.0
    elif category in HAIR_CATEGORIES:
        length = 1.3 * size
    else:
        length = (_scatter_length(preset) or 10.0) * size / float(preset.get("size") or size)
    groomlab.set_graph(groom, comb_graph(length, spacing=spacing, colliders=colliders))
    errors = groomlab.errors(groom)
    if errors:
        raise RuntimeError("the guide graph: %s" % "; ".join(errors))
    from groomlab import bake
    made = bake.bake_guides(groom, "resample", group=groups[0]["name"] if groups else "primary", density=1.0)
    return len(made)


def center_part(groom, scale):
    """A center part from the front hairline to the crown, on the head scalp's ellipsoid."""
    import groomlab
    points = []
    for k in range(9):
        z = (6.0 - 8.5 * k / 8.0)
        y = -2.0 + 10.0 * math.sqrt(max(0.0, 1.0 - (z / 9.0) ** 2))
        points.append((0.0, y * scale, z * scale))
    curve = cmds.curve(degree=2, point=points)
    groomlab.add_part(groom, [curve], name="center", delete_curves=True)


# -- Pictures -------------------------------------------------------------------------------------

def frame(camera_angles, objects):
    """The perspective camera turned by `camera_angles` (x, y degrees) and moved back along its
    view until the objects' bounding sphere fills a square picture."""
    box = cmds.exactWorldBoundingBox(objects)
    center = [(box[i] + box[i + 3]) * 0.5 for i in range(3)]
    radius = 0.5 * math.sqrt(sum((box[i + 3] - box[i]) ** 2 for i in range(3)))
    cmds.setAttr("perspShape.horizontalFilmAperture", 1.0)
    cmds.setAttr("perspShape.verticalFilmAperture", 1.0)
    cmds.setAttr("perspShape.filmFit", 1)  # horizontal: the square picture shows the aperture
    cmds.setAttr("perspShape.focalLength", 50.0)
    half = math.atan(0.5 * 25.4 / 50.0)
    distance = radius * 0.86 / math.sin(half)
    rx, ry = math.radians(camera_angles[0]), math.radians(camera_angles[1])
    back = (math.sin(ry) * math.cos(rx), -math.sin(rx), math.cos(ry) * math.cos(rx))  # the camera's +Z
    cmds.setAttr("persp.rotate", camera_angles[0], camera_angles[1], 0)
    cmds.setAttr("persp.translate", *[center[i] + back[i] * distance for i in range(3)])
    cmds.setAttr("perspShape.centerOfInterest", distance)


def shoot(path, size):
    cmds.refresh(currentView=True, force=True)
    cmds.playblast(frame=[cmds.currentTime(query=True)], format="image", compression="png", viewer=False,
                   showOrnaments=False, percent=100, widthHeight=(size, size), completeFilename=path)
    return path


def override_params(groom):
    """GROOMLAB_PRESET_SHEET_PARAMS: each Type.param=value set on every node of that type."""
    spec = os.environ.get("GROOMLAB_PRESET_SHEET_PARAMS", "").strip()
    if not spec:
        return
    import json

    import groomlab
    graph = groomlab.get_graph(groom)
    for item in filter(None, (part.strip() for part in spec.split(";"))):
        target, value = item.split("=", 1)
        node_type, param = target.split(".", 1)
        for n in graph["nodes"]:
            if n.get("type") == node_type:
                n.setdefault("params", {})[param] = json.loads(value)
    groomlab.set_graph(groom, graph)


def contact_sheet(cells, path, title, size):
    """A grid of the pictures, each labelled with the preset's name, category and its stats."""
    from PySide2 import QtCore, QtGui
    columns = 6 if len(cells) > 12 else max(1, min(len(cells), 4))
    rows = (len(cells) + columns - 1) // columns
    label = 40
    head = 44
    sheet = QtGui.QImage(columns * size, head + rows * (size + label), QtGui.QImage.Format_RGB32)
    sheet.fill(QtGui.QColor(24, 24, 26))
    painter = QtGui.QPainter(sheet)
    painter.setRenderHint(QtGui.QPainter.TextAntialiasing)
    font = painter.font()
    font.setPixelSize(22)
    font.setBold(True)
    painter.setFont(font)
    painter.setPen(QtGui.QColor(235, 235, 235))
    painter.drawText(QtCore.QRect(12, 0, columns * size - 24, head), QtCore.Qt.AlignVCenter, title)
    for i, cell in enumerate(cells):
        x, y = (i % columns) * size, head + (i // columns) * (size + label)
        image = QtGui.QImage(cell["path"])
        if not image.isNull():
            painter.drawImage(QtCore.QRect(x, y, size, size), image)
        font.setPixelSize(15)
        font.setBold(True)
        painter.setFont(font)
        painter.setPen(QtGui.QColor(255, 120, 110) if cell["errors"] else QtGui.QColor(235, 235, 235))
        painter.drawText(QtCore.QRect(x + 6, y + size, size - 12, 20), QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter,
                         "%s  (%s)" % (cell["name"], cell["category"]))
        font.setPixelSize(12)
        font.setBold(False)
        painter.setFont(font)
        painter.setPen(QtGui.QColor(160, 160, 165))
        info = "%d strands, %.1f s%s" % (cell["strands"], cell["seconds"],
                                         (", errors: " + cell["errors"][:60]) if cell["errors"] else "")
        painter.drawText(QtCore.QRect(x + 6, y + size + 19, size - 12, 18), QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter, info)
    painter.end()
    sheet.save(path)
    return path


# -- The run --------------------------------------------------------------------------------------

def run():
    import groomlab
    from groomlab import presets

    folder = os.environ.get("GROOMLAB_PRESET_SHEET_DIR") or os.path.join(tempfile.gettempdir(), "groomlab_preset_sheet")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    scenes = _env_list("GROOMLAB_PRESET_SHEET_SCENES", ("head", "creature"))
    wanted = _env_list("GROOMLAB_PRESET_SHEET_NAMES", ())
    tag = os.environ.get("GROOMLAB_PRESET_SHEET_TAG", "")
    size = int(os.environ.get("GROOMLAB_PRESET_SHEET_SIZE", "420"))
    names = sorted(f[:-5] for f in os.listdir(presets.SHIPPED_DIR) if f.endswith(".json"))
    order = {c: i for i, c in enumerate(presets.CATEGORIES)}
    infos = {n: presets.preset_info(n) for n in names}
    names.sort(key=lambda n: (order.get(infos[n].get("category") or "", 99), n))
    if wanted:
        names = [n for n in names if n in wanted]
    panel = persp_panel()
    cmds.modelEditor(panel, edit=True, grid=False, headsUpDisplay=False, displayAppearance="smoothShaded",
                     displayLights="default", displayTextures=True)
    api = cmds.groomLabGpuPreview(drawApi=True)
    say("preset sheets on %s into %s: %s x %d presets" % (api, folder, ",".join(scenes), len(names)))
    sheets = []
    for scene in scenes:
        cmds.file(new=True, force=True)
        cmds.setAttr("persp.nearClipPlane", 0.01)
        cmds.setAttr("persp.farClipPlane", 100000)
        if scene == "head":
            scalp, angles = head_scene()
        elif scene == "small":
            scalp, angles = head_scene(scale=0.1)
        elif scene == "creature":
            scalp, angles = creature_scene()
        else:
            check("known scene %r" % scene, False)
            continue
        groom = groomlab.create_groom(scalp)
        groomlab.set_follow_scalp(groom, False)
        colliders = ()
        if scene in ("head", "small"):
            # The head and neck under the scalp cap keep the hair off the face and out of the neck,
            # and a center part, as a groom of a head has.
            colliders = tuple(groomlab.add_collider(groom, m, wire=False) for m in ("headMesh", "neckMesh"))
            center_part(groom, 0.1 if scene == "small" else 1.0)
        cmds.setAttr(groom + ".displayGuides", False)
        if os.environ.get("GROOMLAB_PRESET_SHEET_DENSITY"):
            cmds.setAttr(groom + ".previewDensity", float(os.environ["GROOMLAB_PRESET_SHEET_DENSITY"]))
        scalp_size = presets.scalp_size(groom)
        say("scene %s: scalp size %.3g" % (scene, scalp_size))
        cells = []
        for name in names:
            preset = presets.read_preset(name)
            started = time.time()
            guides = give_guides(groom, preset, scalp_size, colliders)
            presets.apply_preset(groom, name)
            override_params(groom)
            strands = cmds.getAttr(groom + ".outStrandCount")
            errors = "; ".join(groomlab.errors(groom))
            frame(angles, [scalp, cmds.listRelatives(groom, parent=True)[0]] +
                  (cmds.ls("headMesh", "neckMesh") if scene != "creature" else []))
            path = shoot(os.path.join(folder, "%s%s_%s_%s.png" % (tag + "_" if tag else "", scene, name, api)), size)
            seconds = time.time() - started
            info = infos[name]
            cells.append({"name": name, "category": info.get("category") or "-", "path": path, "strands": strands,
                          "seconds": seconds, "errors": errors})
            say("%-10s %-24s %7d strands %3d guides %5.1f s %s" % (scene, name, strands, guides, seconds,
                                                                   ("ERRORS " + errors) if errors else ""))
            check("%s on the %s draws without errors" % (name, scene), not errors)
        title = "GroomLab presets%s, %s scalp (size %.3g), %s, preview density %.2g" % (
            " (%s)" % tag if tag else "", scene, scalp_size, api, cmds.getAttr(groom + ".previewDensity"))
        sheet = contact_sheet(cells, os.path.join(folder, "%s%s_%s_sheet.png" % (tag + "_" if tag else "", scene, api)),
                              title, size)
        sheets.append(sheet)
        say("sheet:", sheet)
    cmds.file(new=True, force=True)
    finish()

