# SPDX-License-Identifier: Apache-2.0
"""Card Atlas: hair card textures authored block by block (as FiberShop does), each block a groom
of its own on a root band, all baked into one texture set (card_blocks.h).

An atlas is a group in the scene (its glCardAtlas attribute set) holding its blocks: each a
root band mesh and the groom grown on it, side by side. The groom keeps its block (cardBlock:
its card's width, length and thickness, the band's rotation, its rectangle of the atlas, the
atlas' settings with it), so a groom file of the blocks is the atlas in every host.

    atlas = card_atlas.create_atlas("bangs")
    hair = card_atlas.add_block(atlas, width=2.0, length=15.0)
    card_atlas.set_block(hair, root_rotation=(35, 0, 0))   # the roots tilted: depth in the texture
    card_atlas.export_maps(atlas, "C:/textures")            # the preset's files, one texture set
    card_atlas.export_shapes(atlas, "C:/textures/bangs_cards.fbx", bend=20)
"""

import json
import os
import tempfile

from maya import cmds

GROOM_TYPE = "groomLabShape"
ATLAS_ATTR = "glCardAtlas"


def _core():
    import groomlab_core
    return groomlab_core


def _groom_shape(node):
    if cmds.nodeType(node) == GROOM_TYPE:
        return cmds.ls(node, long=True)[0]
    shapes = cmds.listRelatives(node, shapes=True, type=GROOM_TYPE, fullPath=True) or []
    if not shapes:
        raise ValueError("%s is not a groom" % node)
    return shapes[0]


def _transform(shape):
    return cmds.listRelatives(shape, parent=True, fullPath=True)[0]


def _short(node):
    return node.split("|")[-1]


# ---- Atlases ----------------------------------------------------------------------------------

def create_atlas(name="hairCards", size=2048, padding=8, preset="Unreal Hair Cards", root_at_top=True):
    """A new, empty Card Atlas: a group named `name` (its maps take the name). Its settings live
    on its blocks; until it has one, on the group. `root_at_top`: each block's root at the top of
    its rectangle and its tip at the bottom (as hand-made card atlases lay hair out), else at the
    bottom. Returns the group."""
    from groomlab import ensure_plugin
    ensure_plugin()
    group = cmds.createNode("transform", name=name)
    cmds.addAttr(group, longName=ATLAS_ATTR, dataType="string")
    cmds.setAttr(group + "." + ATLAS_ATTR, json.dumps({"size": int(size), "padding": int(padding), "preset": preset,
                                                        "name": _short(group), "rootAtTop": bool(root_at_top)}), type="string")
    return cmds.ls(group, long=True)[0]


def atlases():
    """Every Card Atlas group in the scene."""
    return [n for n in (cmds.ls(type="transform", long=True) or []) if cmds.attributeQuery(ATLAS_ATTR, node=n, exists=True)]


def atlas_of(node):
    """The atlas `node` (a block's groom, band or the group) belongs to; None when none."""
    for n in [node] + (cmds.listRelatives(node, allParents=True, fullPath=True) or []):
        if cmds.objExists(n) and cmds.nodeType(n) == "transform" and cmds.attributeQuery(ATLAS_ATTR, node=n, exists=True):
            return cmds.ls(n, long=True)[0]
        for parent in cmds.listRelatives(n, parent=True, fullPath=True) or []:
            if cmds.attributeQuery(ATLAS_ATTR, node=parent, exists=True):
                return cmds.ls(parent, long=True)[0]
    return None


def blocks(atlas):
    """The atlas' block grooms (shapes), in the group's order."""
    out = []
    for child in cmds.listRelatives(atlas, children=True, type="transform", fullPath=True) or []:
        for shape in cmds.listRelatives(child, shapes=True, type=GROOM_TYPE, fullPath=True) or []:
            if (cmds.getAttr(shape + ".cardBlock") or "").strip():
                out.append(shape)
    return out


def settings(atlas):
    """The atlas' settings: {size, padding, preset, name}."""
    return json.loads(atlas_json(atlas))["settings"]


def atlas_json(atlas):
    """The atlas as the core reads it ({"settings", "blocks": [{name, width, length, ...}]})."""
    grooms = blocks(atlas)
    if grooms:
        return _core().card_block_atlas([(_short(_transform(g)), cmds.getAttr(g + ".cardBlock")) for g in grooms])
    own = json.loads(cmds.getAttr(atlas + "." + ATLAS_ATTR) or "{}")
    return json.dumps({"settings": own, "blocks": []})


def _write(atlas, text):
    """The atlas' JSON written back on its blocks (and its group: the settings)."""
    data = json.loads(text)
    by_name = dict((_short(_transform(g)), g) for g in blocks(atlas))
    core = _core()
    for block in data["blocks"]:
        groom = by_name.get(block["name"])
        if groom:
            stored = core.card_block_json(text, block["name"])
            if stored != (cmds.getAttr(groom + ".cardBlock") or ""):
                cmds.setAttr(groom + ".cardBlock", stored, type="string")
    cmds.setAttr(atlas + "." + ATLAS_ATTR, json.dumps(data["settings"]), type="string")


def update_layout(atlas, pack=False):
    """The atlas laid out after a change (`pack`: every block packed afresh, none placed by hand)."""
    _write(atlas, _core().card_block_layout(atlas_json(atlas), pack))


def fit(atlas):
    """Each block's card made as wide and long as its hair reaches
    (combed or noised past its width, longer or shorter than its length), the layout following.
    The bake, the exports and the edits here do it first; call it after grooming to see the
    rectangles the hair will take. True when a card changed."""
    grooms = blocks(atlas)
    if not grooms:
        return False
    texts = cmds.groomLabCardAtlas(*grooms, fitCards=True) or []
    changed = False
    for groom, text in zip(grooms, texts):
        if text != (cmds.getAttr(groom + ".cardBlock") or ""):
            cmds.setAttr(groom + ".cardBlock", text, type="string")
            changed = True
    return changed


def card_size(groom):
    """The block's card (width, length): fitted to its hair (its own width and length until fitted)."""
    b = block(groom)
    frame = b.get("frame")
    if frame:
        return frame[1] - frame[0], frame[2]
    return b["width"], b["length"]


def set_settings(atlas, size=None, padding=None, preset=None, name=None, root_at_top=None):
    """The atlas' texture size, padding (texels), map preset, file name and where the roots are
    (`root_at_top`: at the top of each rectangle); the layout follows."""
    data = json.loads(atlas_json(atlas))
    for key, value in (("size", size), ("padding", padding), ("preset", preset), ("name", name), ("rootAtTop", root_at_top)):
        if value is not None:
            data["settings"][key] = value
    _write(atlas, json.dumps(data))
    if padding is not None or size is not None:
        update_layout(atlas)


# ---- Blocks -----------------------------------------------------------------------------------

def _make_band(block_json, name, parent):
    from maya.api import OpenMaya as om
    mesh = _core().card_block_mesh(block_json)
    points = om.MPointArray([om.MPoint(*p) for p in mesh["points"]])
    transform = cmds.createNode("transform", name=name, parent=parent)
    sel = om.MSelectionList()
    sel.add(transform)
    fn = om.MFnMesh()
    fn.create(points, mesh["face_counts"], mesh["face_indices"], parent=sel.getDependNode(0))
    us, vs = mesh["uvs"][0::2], mesh["uvs"][1::2]
    fn.setUVs(us, vs)
    fn.assignUVs(mesh["face_counts"], mesh["face_indices"])
    transform = cmds.ls(transform, long=True)[0]
    shape = cmds.listRelatives(transform, shapes=True, fullPath=True)[0]
    cmds.rename(shape, _short(transform) + "Shape")
    cmds.sets(transform, edit=True, forceElement="initialShadingGroup")
    return transform


def _band_of(groom):
    from groomlab import scalp_mesh
    mesh = scalp_mesh(groom)
    return _transform(mesh) if mesh else None


def add_block(atlas, name="block", width=2.0, length=15.0, thickness=0.3, root_rotation=(0.0, 0.0, 0.0), like=None):
    """A new block in `atlas`: its root band (`width` across, `thickness` deep, turned by
    `root_rotation`), beside the others, and its groom (the card graph and guides hanging down
    `length`; `like`: another block's graph). The layout gives it a rectangle. One undo step.
    Returns the groom shape."""
    from groomlab import create_groom, undo_chunk
    core = _core()
    with undo_chunk("add card block"):
        data = json.loads(atlas_json(atlas))
        origin = core.card_block_next_origin(json.dumps(data), float(width))
        block = {"name": "", "width": float(width), "length": float(length), "thickness": float(thickness),
                 "origin": list(origin), "rootRotation": [float(v) for v in root_rotation]}
        data["blocks"].append(dict(block))
        block_json = core.card_block_json(json.dumps(data), "")
        band = _make_band(block_json, name + "_band", atlas)
        groom = create_groom(scalp=band, name=name)
        transform = cmds.parent(_transform(groom), atlas)[0]
        groom = _groom_shape(transform)
        graph = cmds.getAttr(_groom_shape(like) + ".graph") if like else core.card_block_graph_json()
        cmds.setAttr(groom + ".graph", graph, type="string")
        cmds.setAttr(groom + ".guideData", core.card_block_guides_json(block_json), type="string")
        cmds.setAttr(groom + ".previewDensity", 1.0)
        cmds.setAttr(groom + ".cardBlock", block_json, type="string")
        update_layout(atlas)
        fit(atlas)
        return groom


def duplicate_block(groom, name=None):
    """A new block of the same card and graph beside the others (its own fresh guides)."""
    groom = _groom_shape(groom)
    atlas = atlas_of(groom)
    b = block(groom)
    return add_block(atlas, name or _short(_transform(groom)) + "_copy", b["width"], b["length"], b["thickness"],
                     b.get("rootRotation", (0, 0, 0)), like=groom)


def remove_block(groom):
    """The block's groom and root band deleted; the layout follows. One undo step."""
    from groomlab import undo_chunk
    groom = _groom_shape(groom)
    atlas = atlas_of(groom)
    with undo_chunk("remove card block"):
        band = _band_of(groom)
        cmds.delete([n for n in (_transform(groom), band) if n])
        if atlas:
            update_layout(atlas)


def block(groom):
    """The block as a dict: {name, width, length, thickness, origin, rootRotation, cell, placed}."""
    groom = _groom_shape(groom)
    atlas = atlas_of(groom)
    name = _short(_transform(groom))
    for b in json.loads(atlas_json(atlas))["blocks"]:
        if b["name"] == name:
            b.setdefault("rootRotation", [0.0, 0.0, 0.0])
            return b
    raise ValueError("%s is no block of its atlas" % groom)


def set_block(groom, width=None, length=None, thickness=None, root_rotation=None, cell=None):
    """Change a block: its card's size and its root band's rotation (the band moves; each guide
    keeps its place on it and is moved, not turned: tilting the band spreads the roots, the hair
    still hangs as it was shaped), or its rectangle of the atlas (`cell`: u0, v0, u1, v1; placed by
    hand from then on). Its card is fitted to its hair again and the layout follows. One undo
    step."""
    from maya.api import OpenMaya as om
    from groomlab import undo_chunk
    groom = _groom_shape(groom)
    atlas = atlas_of(groom)
    name = _short(_transform(groom))
    with undo_chunk("edit card block"):
        data = json.loads(atlas_json(atlas))
        entry = next(b for b in data["blocks"] if b["name"] == name)
        before = dict(entry)
        for key, value in (("width", width), ("length", length), ("thickness", thickness)):
            if value is not None:
                entry[key] = float(value)
        if root_rotation is not None:
            entry["rootRotation"] = [float(v) for v in root_rotation]
        if cell is not None:
            entry["cell"] = [float(v) for v in cell]
            entry["placed"] = True
        text = json.dumps(data)
        old_json = cmds.getAttr(groom + ".cardBlock")
        block_json = _core().card_block_json(text, name)
        cmds.setAttr(groom + ".cardBlock", block_json, type="string")
        band_changed = any(entry.get(k) != before.get(k) for k in ("width", "thickness", "rootRotation"))
        if band_changed:
            # The guides moved with the band, not turned with it (set once the band has moved).
            moved = _core().move_card_block_guides(cmds.getAttr(groom + ".guideData") or "", old_json, block_json)
            band = _band_of(groom)
            mesh = _core().card_block_mesh(block_json)
            sel = om.MSelectionList()
            sel.add(band)
            fn = om.MFnMesh(sel.getDagPath(0))
            if fn.numVertices == len(mesh["points"]):  # the same band, moved: the groom follows it
                fn.setPoints(om.MPointArray([om.MPoint(*p) for p in mesh["points"]]), om.MSpace.kObject)
            else:  # another band (wider or thinner: more quads across): the groom moves onto it
                from groomlab import transfer_groom  # by the bands' UVs: across and through them alike
                new = _make_band(block_json, _short(band) + "_new", atlas)
                transfer_groom(groom, new, method="uv")
                cmds.delete(band)
                cmds.rename(new, _short(band))
            cmds.setAttr(groom + ".guideData", moved, type="string")
        update_layout(atlas)
        fit(atlas)


# ---- Bake and export ----------------------------------------------------------------------------

def _block_args(atlas):
    grooms = blocks(atlas)
    if not grooms:
        raise ValueError("%s has no blocks" % atlas)
    return grooms


def preview(atlas, folder=None, resolution=512, occlusion=1.0):
    """The atlas baked small, one PNG a map: {map name: path} (Albedo, Alpha, Depth, ...)."""
    folder = folder or os.path.join(tempfile.gettempdir(), "groomlab_card_atlas", _short(atlas))
    fit(atlas)
    paths = cmds.groomLabCardAtlas(*_block_args(atlas), preview=folder, resolution=int(resolution), occlusion=float(occlusion)) or []
    return dict((os.path.splitext(os.path.basename(p))[0].split("_", 1)[-1], p) for p in paths)


def export_maps(atlas, folder, preset=None, name=None, resolution=0, file_format="", bit_depth=0, padding=-1, supersample=1,
                name_template="{name}_{map}", occlusion=1.0):
    """The atlas baked into one texture set and written as `preset` packs it (the atlas' own by
    default), named `name` (the atlas'). Returns the paths."""
    kwargs = dict(maps=folder, resolution=int(resolution), format=file_format, bitDepth=int(bit_depth), padding=int(padding),
                  supersample=int(supersample), nameTemplate=name_template, occlusion=float(occlusion))
    if preset:
        kwargs["preset"] = preset if isinstance(preset, str) else json.dumps(preset)
    if name:
        kwargs["name"] = name
    fit(atlas)
    return cmds.groomLabCardAtlas(*_block_args(atlas), **kwargs) or []


def export_shapes(atlas, path, segments=8, across=1, bend=0.0, twist=0.0, meters=False):
    """The blocks' cards as UV-ready meshes (FBX), one per block, to place by hand. Returns the path."""
    fit(atlas)
    return cmds.groomLabCardAtlas(*_block_args(atlas), shapes=path, segments=int(segments), across=int(across),
                                  bend=float(bend), twist=float(twist), meters=bool(meters))


CARDS_ATTR = "glCardAtlasCards"  # on the group of an atlas' cards in the scene: the atlas it was made from


def scene_cards(atlas):
    """The group of the atlas' cards made in the scene (cards_to_scene), or None."""
    for node in cmds.ls(type="transform", long=True) or []:
        if cmds.attributeQuery(CARDS_ATTR, node=node, exists=True) and cmds.getAttr(node + "." + CARDS_ATTR) == _short(atlas):
            return node
    return None


def cards_to_scene(atlas, segments=8, across=1, bend=0.0, twist=0.0, maps=None):
    """The blocks' cards made in the scene as UV-ready meshes (<block>_card, as Export Card Shapes
    writes them), in a group <atlas>_cards beside the blocks, sharing one material (<atlas>_mat)
    whose color and transparency are `maps`' "Albedo" and "Alpha" images when given (a preview's
    or the exported maps). Made again: the group made before is replaced. One undo step. Returns
    the group."""
    from maya.api import OpenMaya as om
    from groomlab import undo_chunk
    _block_args(atlas)
    fit(atlas)
    text = atlas_json(atlas)
    name = _short(atlas)
    with undo_chunk("cards to scene"):
        old = scene_cards(atlas)
        if old:
            cmds.delete(old)
        group = cmds.createNode("transform", name=name + "_cards")
        cmds.addAttr(group, longName=CARDS_ATTR, dataType="string")
        cmds.setAttr(group + "." + CARDS_ATTR, name, type="string")
        material, shading_group = _cards_material(name + "_mat", maps or {})
        sel = om.MSelectionList()
        sel.add(group)
        parent = sel.getDependNode(0)
        for mesh in _core().card_block_shapes(text, int(segments), int(across), float(bend), float(twist)):
            transform = cmds.createNode("transform", name=mesh["name"], parent=group)
            sel = om.MSelectionList()
            sel.add(transform)
            fn = om.MFnMesh()
            fn.create(om.MPointArray([om.MPoint(*p) for p in mesh["points"]]), mesh["face_counts"], mesh["face_indices"],
                      parent=sel.getDependNode(0))
            uvs = mesh["uvs"]
            fn.setUVs(uvs[0::2], uvs[1::2])
            fn.assignUVs(mesh["face_counts"], list(range(len(uvs) // 2)))
            shape = cmds.listRelatives(transform, shapes=True, fullPath=True)[0]
            cmds.rename(shape, mesh["name"] + "Shape")
            cmds.sets(transform, edit=True, forceElement=shading_group)
        del parent
        # Beside the blocks (to their right), so the two read side by side.
        right = 0.0
        for b in json.loads(text)["blocks"]:
            frame = b.get("frame") or [-0.5 * b["width"], 0.5 * b["width"], b["length"]]
            right = max(right, b["origin"][0] + max(0.5 * b["width"], frame[1]))
        cmds.setAttr(group + ".translateX", right + 3.0)
        return cmds.ls(group, long=True)[0]


def _cards_material(name, maps):
    """The cards' material: a lambert (made once, kept), its color and transparency from `maps`'
    Albedo and Alpha images."""
    if cmds.objExists(name) and cmds.nodeType(name) == "lambert":
        material = name
    else:
        material = cmds.shadingNode("lambert", asShader=True, name=name)
    groups = cmds.listConnections(material + ".outColor", type="shadingEngine") or []
    shading_group = groups[0] if groups else cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=name + "SG")
    if not groups:
        cmds.connectAttr(material + ".outColor", shading_group + ".surfaceShader")
    for attr, key, invert in (("color", "Albedo", False), ("transparency", "Alpha", True)):
        path = maps.get(key)
        if not path:
            continue
        plug = material + "." + attr
        for src in cmds.listConnections(plug, source=True, destination=False, plugs=True) or []:
            cmds.disconnectAttr(src, plug)
        node = cmds.shadingNode("file", asTexture=True, name="%s_%s" % (name, key))
        cmds.setAttr(node + ".fileTextureName", path, type="string")
        if invert:
            cmds.setAttr(node + ".invert", 1)  # transparency: 1 - alpha
        cmds.connectAttr(node + ".outColor", plug)
    return material, shading_group


# ---- Files --------------------------------------------------------------------------------------

def save_atlas(atlas, path):
    """The atlas' blocks as one groom file (a document of named grooms: the app opens it as the
    atlas, Maya and Blender import it with open_atlas). Returns the paths written."""
    from groomlab.files import export_grooms
    return export_grooms(_block_args(atlas), path)


def open_atlas(path, name=None):
    """The Card Atlas a groom file holds: its grooms made (on meshes from their own root bands)
    and grouped as an atlas named after its settings (or `name`); grooms that are no blocks stay
    outside it. Returns the group."""
    from groomlab import undo_chunk
    from groomlab.files import import_grooms
    with undo_chunk("open card atlas"):
        grooms = import_grooms(path)
        members = [g for g in grooms if (cmds.getAttr(g + ".cardBlock") or "").strip()]
        if not members:
            raise ValueError("%s holds no Card Atlas blocks" % path)
        first = json.loads(cmds.getAttr(members[0] + ".cardBlock"))
        atlas = create_atlas(name or first.get("atlas", {}).get("name", "hairCards"))
        for groom in members:
            band = _band_of(groom)
            if band:
                cmds.parent(band, atlas)
            cmds.parent(_transform(groom), atlas)
        _write(atlas, atlas_json(atlas))  # the group's settings
        return atlas
