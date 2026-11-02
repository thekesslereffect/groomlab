# SPDX-License-Identifier: Apache-2.0
"""Builds the skinned bust sample: a head and shoulders skinned to a joint chain and animated over 156
frames (a move forward with a bob, head turns, a nod and a quick shake), groomed with a bob that
follows the skinned scalp.

With --head, the bust is that OBJ (cm, y up, facing +z, head and neck down to the shoulders, one
closed mesh, as Nika_LowPoly.obj is). Without, a stylized bust is made from primitives. Writes, beside
--out:

    skinned_bust_hair.ma   the groom follows the scalp (Follow Scalp on, no simulation)
    skinned_bust_sim.ma    the same groom with a Simulate node: the hair swings as the head moves
    preview_f001.usda / preview_f060.usda   USD exports of two frames (a check that the hair moves)

    mayapy samples/skinned_bust/make_scene.py --out build/samples/skinned_bust [--head Nika_LowPoly.obj]

Needs GroomLab installed for Maya (the plugin loads from the module path).
"""

import argparse
import math
import os
import sys

import maya.standalone

# The skull as an ellipsoid (cm, y up, the face toward +z): guides are kept outside it. BROW_Y: bangs
# stop above it; TIP_Y: no tip falls below it. Measured again from a --head mesh.
HEAD_CENTER = (0.0, 44.0, 0.0)
HEAD_RADII = (8.5, 10.5, 9.5)
BROW_Y = 46.0
TIP_Y = 22.0
FRAMES = (1, 156)


def build_body(cmds):
    """The character's meshes, at the bind pose. Returns (meshes by name, scalp transform)."""
    cx, cy, cz = HEAD_CENTER
    rx, ry, rz = HEAD_RADII
    head = cmds.polySphere(name="head_geo", radius=1, subdivisionsX=48, subdivisionsY=36)[0]
    cmds.setAttr(head + ".scale", rx, ry, rz)
    cmds.setAttr(head + ".translate", cx, cy, cz)

    # The scalp: the head's cap down to a hairline lower at the nape than over the brow and
    # above the ears. Hidden: the groom grows from it, the head mesh is what renders.
    scalp = cmds.duplicate(head, name="scalp_geo")[0]
    drop = []
    count = cmds.polyEvaluate(scalp, face=True)
    for f in range(count):
        verts = cmds.polyInfo("%s.f[%d]" % (scalp, f), faceToVertex=True)[0].split(":")[1].split()
        pts = [cmds.pointPosition("%s.vtx[%s]" % (scalp, v), local=True) for v in verts]
        x = sum(p[0] for p in pts) / len(pts)
        y = sum(p[1] for p in pts) / len(pts)
        z = sum(p[2] for p in pts) / len(pts)
        polar = math.degrees(math.acos(max(-1.0, min(1.0, y))))
        azimuth = math.atan2(x, z)                       # 0 at the face, pi at the nape
        back = 0.5 - 0.5 * math.cos(azimuth)
        side = abs(math.sin(azimuth))
        cut = 62.0 + 50.0 * back - 12.0 * side * (1.0 - back)   # higher over the ears
        if polar > cut:
            drop.append("%s.f[%d]" % (scalp, f))
    cmds.delete(drop)
    cmds.delete(scalp, constructionHistory=True)
    cmds.setAttr(scalp + ".visibility", False)

    nose = cmds.polyCone(name="nose_geo", radius=1.6, height=4.0, subdivisionsX=16)[0]
    cmds.setAttr(nose + ".rotateX", 90)
    cmds.setAttr(nose + ".translate", cx, cy - 1.5, cz + rz + 0.6)
    ears = []
    for side in (-1, 1):
        ear = cmds.polySphere(name="ear%s_geo" % ("L" if side > 0 else "R"), radius=1, subdivisionsX=16,
                              subdivisionsY=12)[0]
        cmds.setAttr(ear + ".scale", 1.0, 3.0, 2.0)
        cmds.setAttr(ear + ".translate", cx + side * (rx - 0.3), cy - 0.5, cz - 0.5)
        ears.append(ear)
    neck = cmds.polyCylinder(name="neck_geo", radius=5.0, height=14.0, subdivisionsX=24, subdivisionsY=6)[0]
    cmds.setAttr(neck + ".translate", 0, 30.0, -0.5)
    torso = cmds.polyCube(name="torso_geo", width=38, height=28, depth=20, subdivisionsX=8, subdivisionsY=6,
                          subdivisionsZ=4)[0]
    cmds.polySmooth(torso, divisions=1)
    cmds.setAttr(torso + ".translate", 0, 10.0, -1.0)
    meshes = {"head": head, "nose": nose, "ears": ears, "neck": neck, "torso": torso}
    for m in [head, nose, neck, torso] + ears:
        cmds.makeIdentity(m, apply=True, translate=True, rotate=True, scale=True)
        cmds.delete(m, constructionHistory=True)
    cmds.makeIdentity(scalp, apply=True, translate=True, rotate=True, scale=True)

    skin = cmds.shadingNode("lambert", asShader=True, name="skin_mtl")
    cmds.setAttr(skin + ".color", 0.72, 0.52, 0.42, type="double3")
    sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="skin_mtlSG")
    cmds.connectAttr(skin + ".outColor", sg + ".surfaceShader")
    cmds.sets([head, nose, neck, torso] + ears, edit=True, forceElement=sg)
    return meshes, scalp


def build_rig(cmds, meshes, scalp):
    cmds.select(clear=True)
    root = cmds.joint(name="root_jnt", position=(0, 0, 0))
    spine = cmds.joint(name="spine_jnt", position=(0, 14, 0))
    neck = cmds.joint(name="neck_jnt", position=(0, 25, -0.5))
    head = cmds.joint(name="head_jnt", position=(0, 36, 0))
    cmds.joint(name="headEnd_jnt", position=(0, 56, 0))
    cmds.skinCluster([root, spine, neck], meshes["torso"], toSelectedBones=True, maximumInfluences=2,
                     name="torso_skin")
    cmds.skinCluster([neck, head], meshes["neck"], toSelectedBones=True, maximumInfluences=2, name="neck_skin")
    for m in [meshes["head"], meshes["nose"], scalp] + meshes["ears"]:
        cmds.skinCluster([head], m, toSelectedBones=True, name=m.replace("_geo", "_skin"))
    rig = cmds.group(empty=True, name="rig")
    cmds.parent(root, rig)
    return {"mover": rig, "root": root, "spine": spine, "neck": neck, "head": head}


def build_head_mesh(cmds, path):
    """The bust from an OBJ (see the module's notes), its scalp cut from it by a hairline: above the
    brow at the front, above the ears at the sides, down to the nape at the back. Returns (meshes by
    name, scalp transform), as build_body does, and measures the skull globals on the mesh."""
    global HEAD_CENTER, HEAD_RADII, BROW_Y, TIP_Y
    before = set(cmds.ls(type="transform"))
    cmds.file(path, i=True, type="OBJ", options="mo=0", ignoreVersion=True)
    imported = [t for t in cmds.ls(type="transform") if t not in before and cmds.listRelatives(t, shapes=True)]
    head = cmds.rename(imported[0], "head_geo")
    cmds.delete(head, constructionHistory=True)

    # Landmarks: the top, the nose tip (the frontmost point), the eyes a quarter of the way from the
    # nose up to the top, and the skull's width and depth above them.
    bbox = cmds.exactWorldBoundingBox(head)
    top = bbox[4]
    count = cmds.polyEvaluate(head, vertex=True)
    points = [cmds.pointPosition("%s.vtx[%d]" % (head, v), world=True) for v in range(count)]
    nose = max(points, key=lambda p: p[2])
    eyes = nose[1] + 0.25 * (top - nose[1])
    skull = [p for p in points if p[1] > eyes + 2.0]
    xs, zs = [p[0] for p in skull], [p[2] for p in skull]
    cz = 0.5 * (max(zs) + min(zs))
    HEAD_CENTER = (0.0, eyes, cz)
    HEAD_RADII = (max(abs(x) for x in xs), top - eyes, 0.5 * (max(zs) - min(zs)))
    BROW_Y = eyes + 2.5
    TIP_Y = nose[1] - 0.45 * (top - nose[1])   # about the chin
    brow = eyes + 0.42 * (top - eyes)           # the hairline over the forehead
    ears = eyes + 0.15 * (top - eyes)           # over the ears
    nape = TIP_Y + 1.0                          # at the back

    scalp = cmds.duplicate(head, name="scalp_geo")[0]
    drop = []
    for f in range(cmds.polyEvaluate(scalp, face=True)):
        verts = cmds.polyInfo("%s.f[%d]" % (scalp, f), faceToVertex=True)[0].split(":")[1].split()
        pts = [points[int(v)] for v in verts]
        x = sum(p[0] for p in pts) / len(pts)
        y = sum(p[1] for p in pts) / len(pts)
        z = sum(p[2] for p in pts) / len(pts)
        back = 0.5 - 0.5 * math.cos(math.atan2(x, z - cz))   # 0 at the face, 1 at the nape
        if back < 0.5:
            cut = brow + (ears - brow) * back / 0.5
        else:
            cut = ears + (nape - ears) * (back - 0.5) / 0.5
        on_ear = abs(x) > 0.97 * HEAD_RADII[0] and y < ears + 1.0
        if y < cut or on_ear:
            drop.append("%s.f[%d]" % (scalp, f))
    cmds.delete(drop)
    cmds.delete(scalp, constructionHistory=True)
    cmds.setAttr(scalp + ".visibility", False)

    skin = cmds.shadingNode("lambert", asShader=True, name="skin_mtl")
    cmds.setAttr(skin + ".color", 0.72, 0.52, 0.42, type="double3")
    sg = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name="skin_mtlSG")
    cmds.connectAttr(skin + ".outColor", sg + ".surfaceShader")
    cmds.sets(head, edit=True, forceElement=sg)
    return {"head": head, "bbox": bbox, "nose": nose}, scalp


def build_head_rig(cmds, meshes, scalp):
    """Root at the shoulders, a neck joint, the head joint at the skull's base. The bust is bound
    to the three; the scalp gets the bust's weights, so it moves exactly as the skin under it."""
    bbox, nose = meshes["bbox"], meshes["nose"]
    low, top = bbox[1], bbox[4]
    z = HEAD_CENTER[2] - 1.0
    cmds.select(clear=True)
    root = cmds.joint(name="root_jnt", position=(0, low, z))
    neck = cmds.joint(name="neck_jnt", position=(0, low + 0.45 * (nose[1] - low), z - 0.5))
    head = cmds.joint(name="head_jnt", position=(0, nose[1] - 0.12 * (top - nose[1]), z))
    cmds.joint(name="headEnd_jnt", position=(0, top, z))
    cmds.skinCluster([root, neck, head], meshes["head"], toSelectedBones=True, maximumInfluences=2,
                     dropoffRate=6.0, name="head_skin")
    cmds.skinCluster([root, neck, head], scalp, toSelectedBones=True, maximumInfluences=2, name="scalp_skin")
    cmds.copySkinWeights(sourceSkin="head_skin", destinationSkin="scalp_skin", noMirror=True,
                         surfaceAssociation="closestPoint", influenceAssociation="oneToOne")
    rig = cmds.group(empty=True, name="rig")
    cmds.parent(root, rig)
    return {"mover": rig, "root": root, "spine": root, "neck": neck, "head": head}


def key(cmds, node, attr, pairs):
    for frame, value in pairs:
        cmds.setKeyframe(node, attribute=attr, time=frame, value=value, inTangentType="auto",
                         outTangentType="auto")


def animate(cmds, joints):
    """Frames 1-12 hold the bind pose (a simulation settles there); then a walk forward with a bob
    and sway, the head turning left and right, a nod, a head shake, and a settle at the end. Paced
    like a person (a turn takes a second, the shake about four swings a second) with keys that
    never overshoot: hair shows every jerk of its head."""
    cmds.playbackOptions(minTime=FRAMES[0], maxTime=FRAMES[1], animationStartTime=FRAMES[0],
                         animationEndTime=FRAMES[1])
    cmds.currentUnit(time="film")
    mover, spine, neck, head = joints["mover"], joints["spine"], joints["neck"], joints["head"]
    key(cmds, mover, "translateZ", [(1, 0), (12, 0), (120, 60), (132, 62)])
    key(cmds, mover, "translateY", [(1, 0), (12, 0)] + [(12 + 12 * i, -1.0 if i % 2 else 0.0) for i in range(1, 10)])
    key(cmds, spine, "rotateY", [(1, 0), (12, 0), (32, 6), (56, -6), (80, 6), (104, -6), (120, 0)])
    key(cmds, head, "rotateY", [(1, 0), (12, 0), (36, 30), (44, 30), (72, -30), (80, -30), (96, 0),
                                (120, 0), (125, 14), (130, -14), (135, 8), (140, 0)])
    key(cmds, head, "rotateZ", [(1, 0), (12, 0), (36, -5), (72, 5), (96, 0)])
    key(cmds, neck, "rotateX", [(1, 0), (96, 0), (106, 8), (114, -3), (120, 0)])
    key(cmds, head, "rotateX", [(1, 0), (96, 0), (106, 10), (114, -5), (120, 0)])


def _inside(p, pad):
    cx, cy, cz = HEAD_CENTER
    rx, ry, rz = (r + pad for r in HEAD_RADII)
    return ((p[0] - cx) / rx) ** 2 + ((p[1] - cy) / ry) ** 2 + ((p[2] - cz) / rz) ** 2


def _push_out(p, pad):
    """The point moved out to the ellipsoid grown by pad, when it is inside."""
    d = _inside(p, pad)
    if d >= 1.0:
        return p
    cx, cy, cz = HEAD_CENTER
    s = 1.0 / math.sqrt(max(d, 1e-9))
    return [cx + (p[0] - cx) * s, cy + (p[1] - cy) * s, cz + (p[2] - cz) * s]


def _clear_of(mesh, p, pad):
    """The point moved out along the surface's normal to pad from the mesh, when it is closer or
    inside (the ellipsoid only approximates a real skull)."""
    import maya.api.OpenMaya as om
    q, n, _ = mesh.getClosestPointAndNormal(om.MPoint(*p), om.MSpace.kWorld)
    n.normalize()
    d = (om.MPoint(*p) - q) * n
    if d >= pad:
        return p
    r = q + n * pad
    return [r.x, r.y, r.z]


def guide_curves(cmds, scalp, body=None):
    """Guide curves grown from scalp vertices: up off the scalp, then swept back from a side part
    and falling under gravity over the head, bangs short at the brow, longer at the back."""
    cx, cy, cz = HEAD_CENTER
    rx, ry, rz = HEAD_RADII
    count = cmds.polyEvaluate(scalp, vertex=True)
    picks, seen = [], []
    for v in range(count):
        p = cmds.pointPosition("%s.vtx[%d]" % (scalp, v), world=True)
        if all((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 > 2.4 ** 2 for q in seen):
            seen.append(p)
            picks.append(p)
    if body is not None:
        import maya.api.OpenMaya as om
        sel = om.MSelectionList()
        sel.add(body)
        body = om.MFnMesh(sel.getDagPath(0))
    curves = []
    part_x = 2.5   # a side part, left of the middle
    for i, root in enumerate(picks):
        n = [(root[0] - cx) / rx ** 2, (root[1] - cy) / ry ** 2, (root[2] - cz) / rz ** 2]
        ln = math.sqrt(sum(c * c for c in n))
        n = [c / ln for c in n]
        front = max(0.0, (root[2] - cz) / rz)
        back = max(0.0, -(root[2] - cz) / rz)
        length = 9.0 + 12.0 * back + 7.0 * (1.0 - front) * (1.0 - back) - 3.0 * front
        side = 1.0 if root[0] > part_x else -1.0
        # The sweep: away from the part and back over the crown, then down.
        sweep = [side * 0.8, 0.0, -0.6 * (1.0 - front) + 0.5 * front]
        points = [list(root)]
        p = [root[0] + n[0] * 0.4, root[1] + n[1] * 0.4, root[2] + n[2] * 0.4]
        points.append(p)
        segments = 10
        step = length / segments
        d = [n[0] * 0.3 + sweep[0] * 0.5, n[1] * 0.3 + 0.3, n[2] * 0.3 + sweep[2] * 0.5]
        for s in range(segments):
            gravity = min(1.0, 0.25 + 0.12 * s)
            d = [d[0] * (1.0 - gravity) + sweep[0] * 0.15, d[1] * (1.0 - gravity) - gravity,
                 d[2] * (1.0 - gravity) + sweep[2] * 0.1]
            ld = math.sqrt(sum(c * c for c in d)) or 1.0
            d = [c / ld for c in d]
            p = _push_out([p[0] + d[0] * step, p[1] + d[1] * step, p[2] + d[2] * step], 0.6)
            if body is not None:
                p = _clear_of(body, p, 0.6)
            # Not into the face: bangs stop at the brow line.
            if front > 0.55 and p[1] < BROW_Y:
                p[1] = BROW_Y
            p[1] = max(p[1], TIP_Y)
            points.append(p)
        curves.append(cmds.curve(name="guide%03d" % i, point=points, degree=3))
    return curves


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--head", help="an OBJ bust to groom instead of the primitive one")
    args = parser.parse_args()
    out = os.path.abspath(args.out).replace("\\", "/")
    if not os.path.isdir(out):
        os.makedirs(out)

    maya.standalone.initialize(name="python")
    from maya import cmds
    cmds.loadPlugin("GroomLab", quiet=True)
    import groomlab
    from groomlab import guides as guide_api, presets, sim, exports, groom as groom_api

    cmds.file(new=True, force=True)
    cmds.currentUnit(linear="cm")
    if args.head:
        meshes, scalp = build_head_mesh(cmds, args.head)
        curves = guide_curves(cmds, scalp, body=meshes["head"])
        joints = build_head_rig(cmds, meshes, scalp)
        cmds.group([meshes["head"], scalp], name="character_geo")
    else:
        meshes, scalp = build_body(cmds)
        curves = guide_curves(cmds, scalp)
        joints = build_rig(cmds, meshes, scalp)
        cmds.group([meshes["head"], meshes["nose"], meshes["neck"], meshes["torso"], scalp] + meshes["ears"],
                   name="character_geo")
    cmds.currentTime(1)

    groom = groom_api.create_groom(scalp, name="hair", fit=True)
    presets.apply_preset(groom, "hair_basics")
    guide_api.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groom_api.set_follow_scalp(groom, True)
    animate(cmds, joints)
    cmds.currentTime(1)
    print("strands: %d" % cmds.getAttr(groom + ".outStrandCount"))

    hair = out + "/skinned_bust_hair.ma"
    cmds.file(rename=hair)
    cmds.file(save=True, type="mayaAscii", force=True)

    for frame in (1, 60):
        cmds.currentTime(frame)
        exports.export_usd(groom, "%s/preview_f%03d.usda" % (out, frame), start=frame, end=frame)

    # The simulated variant: the guides swing, the strands follow them.
    cmds.currentTime(1)
    groom_api.set_follow_scalp(groom, False)
    # Natural hair, not jelly: the groom is its shape under gravity, the hair keeping its weight
    # (groomedUnderGravity preload), held only near the root (a hold down the whole length springs
    # the bob back as one block), its form kept by a firm bend, and air drag that makes the ends
    # trail and settle without springing back.
    sim.add_simulate(groom, groomedUnderGravity="preload", bend=0.78, twist=0.6, damping=0.35, drag=0.4, inertia=0.75,
                     rootStiffnessProfile=[[0.0, 1.0], [0.25, 0.4], [0.55, 0.05], [1.0, 0.0]])
    simulated = out + "/skinned_bust_sim.ma"
    cmds.file(rename=simulated)
    cmds.file(save=True, type="mayaAscii", force=True)
    print("wrote %s and %s" % (hair, simulated))
    maya.standalone.uninitialize()


if __name__ == "__main__":
    main()
