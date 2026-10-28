# SPDX-License-Identifier: Apache-2.0
"""Game cards: the groom as ribbon cards with an atlas, live; their presets, bakes, material and
scalp cap."""

import json
import os

from maya import cmds

import groomlab_core

from groomlab.base import undoable, _scalp_shape
from groomlab.paths import asset_folder, asset_name, resolve_path


# Game cards: the groom as ribbon cards with an atlas, live


@undoable("cards")
def create_cards(groom, segments=None, width_scale=None, density=None, clump_channel=None,
                 width_segments=None, curvature=None, twist=None, taper=None, before="output",
                 lod=None, cards_per_clump=None, layers=None, flyaway_cards=None, fit_twist=None, ruffle=None,
                 margin=None, max_cards=None, braids=None, node=None, from_guides=None):
    """Game cards for the groom, shown in its cards view (displayCards); returns the Cards node
    (`node`, else the first in the graph).

    `from_guides` True lays the hair's cards along the guides (the node's cardsFrom guides): a
    card, or a cross, along each guide from where it grows, as wide as the strands nearest it and
    baked from them, the guides input linked to the graph's Guides node (the one feeding an
    Interpolate); False lays them along the strands again (clumps, cardsPerClump and the rest).

    `braids` (on by default): strands a Braid node wove (its braidId channel) make one card
    set per braid, crossed along it (a cornrow one card on the scalp), instead of being carded
    by the clumps their strands came from; False cards them by clump like the rest. Feathers
    reaching the node (a Propagate upstream) get a card per feather (the node's featherMode;
    create_feather_cards gives them a Cards node of their own).

    A Cards node goes into the graph just before `before` (the output), so the
    cards are made from the groom as it is there; move it in the Graph Editor
    to card an earlier stage, or add more Cards nodes (they share one atlas).
    Cards follow the groom's clumps (the `clump_channel` Clump writes,
    "clumpId"), else spread-out groups of strands, as many as `max_cards`
    allows. Each clump gets
    `cards_per_clump` x `layers` cards (1 x 1: one flat card per clump; a hero
    groom wants 4-8 x 2): its strands split into groups lying together, in
    inner and outer depth layers, each group a card along its own mean curve,
    turned the way its strands lie (`fit_twist`, 1), as wide as they spread
    plus a `margin` kept clear of the texture's edges (x `width_scale`, x the
    `taper` ramp root to tip), as long as the longest reaches; `twist` degrees
    more round its length, bowed by `curvature` over `width_segments` quads
    across, alternate cards pushed in and out by `ruffle`. Strands no card holds get thin cards up to `flyaway_cards`, the
    rest join the nearest card. `max_cards` is the budget (sub-cards give way
    first, then neighbouring clumps merge). Alike cards share texture variations in a 0-1 UV atlas (the node's
    `variations`). `lod` ("hero", "mid", "low") applies a budget preset first
    (cards_preset). The groom draws the cards itself (with the baked maps once
    baked); a mesh is made only to export them (export_cards_unreal,
    create_card_mesh). They follow any grooming change. Strands are read at
    `density` (cardDensity, 1.0: the whole groom, never the viewport's preview
    density or point budget, so every strand of a clump goes into its card).
    With Cards nodes already in the graph, the settings given are changed on
    all of them (or only on `node`, a Cards node's name) and the rest kept;
    one is made only when there is none. Cards nodes inside compounds are left
    alone. All the Cards nodes share one atlas and one bake: its resolution is
    the largest atlasSize any of them asks for, the padding the smallest.
    """
    from groomlab import graph as gm
    if node is not None and node not in _cards_nodes(gm.GraphModel.from_groom(groom)):
        raise ValueError("%s has no Cards node %r" % (groom, node))
    if density is not None or not cmds.listConnections(groom + ".outCards", destination=True, source=False):
        cmds.setAttr(groom + ".cardDensity", 1.0 if density is None else density)
    if lod:
        cards_preset(groom, lod, node=node)
    given = {"clumpChannel": clump_channel, "segments": segments, "widthSegments": width_segments,
             "widthScale": width_scale, "curvature": curvature, "twist": twist,
             "cardsPerClump": cards_per_clump, "layers": layers, "flyawayCards": flyaway_cards,
             "fitTwist": fit_twist, "ruffle": ruffle, "margin": margin, "maxCards": max_cards, "braids": braids}
    kinds = {"clumpChannel": str, "segments": int, "widthSegments": int, "cardsPerClump": int,
             "layers": int, "flyawayCards": int, "maxCards": int, "braids": bool}
    params = {k: kinds.get(k, float)(v) for k, v in given.items() if v is not None}
    if taper is not None:
        params["widthProfile"] = taper if isinstance(taper, (list, dict)) else [[0.0, 1.0], [1.0, float(taper)]]
    model = gm.GraphModel.from_groom(groom)
    targets = [node] if node is not None else _cards_nodes(model)
    if not targets:
        targets = [model.add_node("Cards", name=model.unique_name("cards"), params=params)]
        model.insert_before(before, targets[0])
    else:
        for target in targets:
            for key, value in params.items():
                model.set_param(target, key, value)
    if from_guides is not None:
        guides = None
        if from_guides:
            feeding = [model.source_of(n, "guides") for n in model.order if model.node_type(n) == "Interpolate"]
            guides = next((g for g in feeding if g), None) or next(
                (n for n in model.order if model.node_type(n) == "Guides"), None)
            if guides is None:
                raise ValueError("%s has no Guides node to lay the cards along" % groom)
        for target in targets:
            model.set_param(target, "cardsFrom", "guides" if from_guides else "strands")
            if guides:
                model.connect(guides, target, "guides")
    model.save(groom)
    compare_cards(groom, True)  # show them
    return targets[0]


def _cards_nodes(model):
    """The Cards nodes of a GraphModel (its own, not those inside compounds), in its order."""
    return [n for n in model.order if model.node_type(n) == "Cards"]


def _follow_cards_view(groom, node):
    """`node` (the card mesh or hair cap transform) shows exactly in the groom's cards view."""
    plug = node + ".visibility"
    if cmds.listConnections(plug, source=True, destination=False, plugs=True) != [groom + ".displayCards"]:
        for source in cmds.listConnections(plug, source=True, destination=False, plugs=True) or []:
            cmds.disconnectAttr(source, plug)
        cmds.connectAttr(groom + ".displayCards", plug, force=True)


def create_card_mesh(groom):
    """A live Maya mesh of the groom's cards, connected to outCards, for exporting them yourself;
    returns its transform. Reuses the one already there. Not needed to see the cards (the groom
    draws them in its cards view, with the baked maps) or to bake or export them (Export for
    Unreal makes one of its own and removes it). While it exists it shows the cards in the cards
    view (its visibility follows displayCards) and the groom draws none, so they never show twice.
    Once baked it gets a material from the maps (card_material, as bake_cards gives it later):
    Maya's own transparency, so Viewport 2.0's ambient occlusion, which leaves the groom's own
    textured cards out, takes it as Maya's materials (Hardware 2.0's transparency settings)."""
    transform = cmds.listRelatives(groom, parent=True)[0]
    existing = cmds.listConnections(groom + ".outCards", destination=True, source=False, shapes=True) or []
    if existing:
        return cmds.listRelatives(existing[0], parent=True)[0]
    cards = cmds.createNode("transform", name=transform + "Cards")
    mesh = cmds.createNode("mesh", name=cards + "Shape", parent=cards)
    cmds.connectAttr(groom + ".outCards", mesh + ".inMesh")
    material = cmds.shadingNode("lambert", asShader=True, name=transform + "CardsMaterial")
    cmds.setAttr(material + ".color", 0.35, 0.25, 0.15, type="double3")
    group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=material + "SG")
    cmds.connectAttr(material + ".outColor", group + ".surfaceShader")
    cmds.sets(mesh, edit=True, forceElement=group)
    _follow_cards_view(groom, cards)
    maps = {m: resolve_path(p, groom) for m, p in _card_maps(groom).items() if m in CARD_MAPS and p}
    maps = {m: p for m, p in maps.items() if os.path.isfile(p)}
    if maps.get("Coverage"):
        card_material(cards, maps)
    return cards


# Card budgets per LOD (the core's table, card_presets.h, which the standalone app's Cards panel
# shares): triangles, cards per clump (x depth layers), how each card is built, and the atlas.
CARD_PRESETS = groomlab_core.card_lod_presets()

# Feather card budgets per LOD, for Cards nodes the feathers feed (a card per feather: a strip
# of featherSegments segments along the shaft, with featherSpine two quads each, a column on the
# rachis and each half folded to its vane; a down feather a cross of two flat strips): triangles,
# the segments along each feather's card, the spine, feathers shorter than featherMinLength x the
# longest left out (small down feathers first go), shared feather textures and the atlas. A card
# costs 2 x featherSegments triangles, 4 x with the spine: hero 24, mid 12, low 4 (a V).
FEATHER_CARD_PRESETS = groomlab_core.feather_card_lod_presets()


def feather_card_triangles(segments, spine=True):
    """Triangles one vaned feather's card costs: 2 a segment, 4 with the spine (featherSpine)."""
    return groomlab_core.feather_card_triangles(int(segments), bool(spine))

# The maps a card bake writes, in Unreal's hair card layout (the core's, card_files.h: the app's too).
CARD_MAPS = tuple(groomlab_core.card_map_names())

# What the card exports' manifests say about the cards' vertex colors and normals (the card mesh's
# color set groomLabTint, its locked normals; the glTF export's _TINT and NORMAL).
CARD_TINT_NOTE = (
    "Color set groomLabTint (glTF: _TINT, the tint itself): each card's tint, half its value (0.5 none, 1 twice "
    "as light), linear: its own strands' color over the color its shared texture was baked from, root to tip "
    "(textures are shared by shape; the tint gives each card its own color, tips and bands included). "
    "In the material, multiply the Color map's RGB by 2 x the vertex color's RGB (read unconverted, as linear).")
CARD_NORMALS_NOTE = (
    "Normals: the strands' own (locked), out of the scalp at the card's roots and leaning along the card as "
    "the groom's lit strands are drawn: shade the cards with them, the same from either side.")

# What the card exports' manifests say about feather cards with a spine (featherSpine).
FEATHER_CARD_NOTE = (
    "A vaned feather's card with featherSpine has three vertices per station, root to tip: the left edge, the "
    "spine (on the rachis, at the baked shaft's U) and the right edge; each half folds out of the vane plane to "
    "follow its vane and has its own normals (the spine's between them). The maps are baked onto the flat card "
    "(each vertex keeps the flat card's UV), with Depth and Tangent in each half's own frame. For wind or "
    "stiffness, the spine is the middle vertex of each station (glTF: TEXCOORD_1 x is 0.5 on it, 0 and 1 at the "
    "edges); down feathers' crossed cards are flat strips of two vertices a station.")


def cards_preset(groom, lod="mid", triangles=None, node=None, model=None):
    """Set the groom's Cards nodes (`node`, a Cards node's name, else every Cards node of the
    graph, one made if there is none) for a level of detail: "hero", "mid" or "low".

    Each has a triangle budget (`triangles` overrides it); the card count follows
    from it (maxCards: clumps give up sub-cards first, then more clumps than that
    share wider cards), with the preset's cards per clump and depth layers (hero
    6 x 2 with flyaway cards, mid 2 x 1, low one flat card per clump), segments,
    width segments, curvature, ruffle, texture variations and atlas size. A Cards
    node only feathers feed (a Propagate upstream, no hair; or featherMode
    "perFeather") takes FEATHER_CARD_PRESETS instead, all with the spine (featherSpine: a
    column down the rachis, each half folded to its vane, 4 triangles a segment): hero 6
    segments along each feather, mid 3, low 1 (a simple V) with the smallest (down)
    feathers left out, maxCards from its triangle budget (the shortest feathers give way
    past it); one fed by both gets the hair preset plus the feather preset's
    featherSegments, featherSpine and featherMinLength, and both budgets. With `model` (a GraphModel being edited, e.g. the Graph
    Editor's) the params go there and nothing is saved. Every Cards node of a kind gets the same
    budget (maxCards each), and they share one atlas (the bake's size: the largest atlasSize).
    Returns the params set (on the last node)."""
    if lod not in CARD_PRESETS:
        raise ValueError("lod must be one of %s" % (sorted(CARD_PRESETS),))
    if model is None:  # saved here: one undo step
        return undoable("cards preset")(_cards_preset)(groom, lod, triangles, node, None)
    return _cards_preset(groom, lod, triangles, node, model)


def _preset_params(lod, kind, triangles):
    """The params cards_preset sets on a Cards node of `kind` ("hair", "feathers" or "both")."""
    return groomlab_core.card_preset_params(lod, kind, None if triangles is None else int(triangles))


def _strands_reaching(model, name):
    """(feathers, hair): whether the strands reaching `name` (itself included) hold feathers (a
    node on the way grows them: a Propagate, or a compound holding one) and hair (a strand source
    reached without passing one). The core's walk (card_presets.h)."""
    model.node_type(name)  # GraphError for a node it does not have
    return groomlab_core.strands_reaching(model.to_json(), name)


def card_node_kind(groom, node, model=None):
    """What Cards node `node` cards: "feathers" (only feathers reach it, or its featherMode is
    perFeather), "hair" (no feathers reach it, or its featherMode is off or perVane) or "both"
    (hair and feathers, featherMode auto: feather cards and hair cards from one node). The core
    decides it (card_presets.h), as the standalone app's Cards panel does."""
    from groomlab import graph as gm
    model = model or gm.GraphModel.from_groom(groom)
    model.node_type(node)
    return groomlab_core.card_node_kind(model.to_json(), node)


def _cards_preset(groom, lod, triangles, node, model):
    from groomlab import graph as gm
    own = model is None
    if own:
        model = gm.GraphModel.from_groom(groom)
    if node is not None and node not in _cards_nodes(model):
        raise ValueError("the graph has no Cards node %r" % node)
    targets = [node] if node is not None else _cards_nodes(model)
    if not targets:
        if not own:
            raise ValueError("the graph has no Cards node")
        create_cards(groom)
        model = gm.GraphModel.from_groom(groom)
        targets = _cards_nodes(model)
    preset = {}
    for target in targets:
        preset = _preset_params(lod, card_node_kind(groom, target, model), triangles)
        for key, value in preset.items():
            model.set_param(target, key, value)
    if own:
        model.save(groom)
    return preset


def card_stats(groom, node=None):
    """{"cards", "triangles", "textures", "feathers", "baked", "stale", "nodes"} of the groom's
    cards as they are now (0s without cards); feathers: feathers carded (a card each, a down
    feather's cross once); baked: there are maps; stale: some cards changed since those maps
    were baked (the strands a Cards node cards, or its params), so the maps no longer match them:
    rebake. "nodes": the same per Cards node ({name: {"cards", "triangles", "textures",
    "feathers", "stale"}}; one inside a compound is "compound/inner"): a node is stale only when
    its own cards changed (a look with no Cards node after it never makes one stale), or the bake
    never had it. With `node` (a Cards node, or a compound holding some): that node's numbers alone."""
    stats = json.loads(cmds.groomLabGraphStats(groom))
    baked = bool(stats.get("cardsBaked", False))
    nodes = {name: {"cards": n.get("cards", 0), "triangles": n.get("triangles", 0),
                    "textures": n.get("textures", 0), "feathers": n.get("feathers", 0),
                    "stale": bool(n.get("stale", False))}
             for name, n in (stats.get("cardNodes") or {}).items()}
    if node is not None:
        mine = [n for name, n in nodes.items() if name == node or name.startswith(node + "/")]
        return {"cards": sum(n["cards"] for n in mine), "triangles": sum(n["triangles"] for n in mine),
                "textures": sum(n["textures"] for n in mine), "feathers": sum(n["feathers"] for n in mine),
                "baked": baked, "stale": any(n["stale"] for n in mine)}
    return {"cards": stats.get("cards", 0), "triangles": stats.get("cardTriangles", 0),
            "textures": stats.get("cardTextures", 0), "feathers": stats.get("cardFeathers", 0), "baked": baked,
            "stale": bool(stats.get("cardsStale", False)), "nodes": nodes}


def card_mesh(groom):
    """The Maya mesh on the groom's outCards (its transform), or None."""
    shapes = cmds.listConnections(groom + ".outCards", destination=True, source=False, shapes=True) or []
    return cmds.listRelatives(shapes[0], parent=True)[0] if shapes else None


SCALP_MAPS = ("ScalpColor", "ScalpCoverage")
# A card bake's files after its prefix (the six maps, the atlas), and the scalp bake's.
_BAKE_FILES = tuple(groomlab_core.card_bake_suffixes()) + tuple("%s.tga" % m for m in SCALP_MAPS)


def _bake_folder(groom):
    """Where a groom's bakes go by default: the scene's cards folder, a folder per groom."""
    return asset_folder("cards") + "/" + asset_name(groom)


def _owned_files(groom):
    """The files the groom's own bakes use (normalized absolute paths): its card bake (every
    map of its prefix) and its hair cap's maps."""
    owned = set()
    stored = groomlab_core.card_maps_prefix(cmds.getAttr(groom + ".cardMaps") or "")
    if stored:
        prefix = resolve_path(stored + "x", groom)[:-1]
        owned.update(os.path.normcase(os.path.abspath(prefix + f)) for f in _BAKE_FILES)
    for texture in _material_textures(_existing_scalp_cap(groom)):
        owned.add(os.path.normcase(os.path.abspath(cmds.getAttr(texture + ".fileTextureName") or "")))
    return owned


def _material_textures(node):
    """The file texture nodes of a mesh's materials (none for None)."""
    shapes = (cmds.listRelatives(node, shapes=True, fullPath=True) or [node]) if node else []
    found = []
    for group in (cmds.listConnections(shapes[0], type="shadingEngine") or []) if shapes else []:
        for texture in cmds.listHistory(group) or []:
            if cmds.nodeType(texture) == "file" and texture not in found:
                found.append(texture)
    return found


def _free_bake_name(groom, directory, name, files):
    """`name`, or name_2, name_3...: the first whose `files` (<directory>/<name>_<file>) are
    free or already this groom's own, so a default bake never writes over files it doesn't own."""
    owned = _owned_files(groom)
    candidate, n = name, 2
    while any(os.path.exists(path) and os.path.normcase(os.path.abspath(path)) not in owned
              for path in ("%s/%s_%s" % (directory, candidate, f) for f in files)):
        candidate, n = "%s_%d" % (name, n), n + 1
    return candidate


def _card_maps(groom):
    """The groom's cardMaps as {key: value} (card_files.h's record of its last bake)."""
    return groomlab_core.parse_card_maps(cmds.getAttr(groom + ".cardMaps") or "")


def bake_cards(groom, directory=None, resolution=None, name=None, occlusion=1.0, material=True, store=True):
    """Bake the cards' maps into `directory` (default: the groom's folder in the scene's cards
    folder, asset_folder("cards")/<asset_name>) as TGA files, in Unreal's hair card layout:
    <name>_Depth, _Coverage (opacity), _Tangent (strand direction in card space),
    _Attribute (root U, root V, seed, coordU root to tip), _Color (the strands' color,
    sRGB), _AO (ambient occlusion) and _Normal (the strands' normal in card tangent space,
    DirectX's green); `name` defaults to the groom's asset_name. By default
    no file the groom doesn't own is written over (a free name_2... instead), and never
    another groom's bake. `resolution` defaults to the Cards node's atlasSize. No card mesh is
    needed: with `store` the groom's cards view shows the cards with these maps from then on
    (cut out by the coverage; see card_display to turn the maps off); store=False bakes a copy
    (exports) and leaves the groom's maps alone. With `material` (and `store`), a live card
    mesh (create_card_mesh), if there is one, gets a material from them. Returns the paths."""
    default = directory is None and name is None
    directory = (directory or _bake_folder(groom)).replace("\\", "/")
    if not os.path.isdir(directory):
        os.makedirs(directory)
    name = name or asset_name(groom)
    if default:
        name = _free_bake_name(groom, directory, name, _BAKE_FILES)
    kwargs = {"directory": directory, "occlusion": float(occlusion), "name": name}
    if resolution:
        kwargs["resolution"] = int(resolution)
    if not store:
        kwargs["noStore"] = True
    paths = cmds.groomLabBakeCards(groom, **kwargs)  # stored: the cards view shows them from now on
    mesh = card_mesh(groom)  # a live mesh, when someone made one, gets them as its material
    if material and store and mesh:
        maps = {m: next((p for p in paths if p.endswith("_%s.tga" % m)), None) for m in CARD_MAPS}
        card_material(mesh, maps)
    return paths


# Card map exports: every map a bake gives, packed into image files as a preset says


CARD_MAP_PRESET = "Unreal Hair Cards"  # the default: Bake Maps' own files


def card_map_presets():
    """Every card map export preset: the built-in ones ("Unreal Hair Cards", "Unreal (packed)",
    "Unity HDRP Hair", "Separate maps (all)"), then the user's (JSON files in
    Documents/GroomLab/card_presets, shared by every host, and in GROOMLAB_CARD_PRESET_PATH's
    folders). Each is a dict {"name", "description", "format", "bitDepth", "normal" ("opengl" or
    "directx": the normal map's green), "builtin", "path", "files": [{"map": the file's {map} name,
    "channels": [R, G, B, A], "colorSpace": "auto" | "srgb" | "linear"}]}. A channel is a source's
    channel ("depth", "tangent.x", "color.r"; see card_map_sources), a constant ("0", "1"),
    inverted with "1-" ("1-ao"), or only there when the groom has it with "?" ("iridescence?")."""
    return groomlab_core.card_map_presets()


def card_map_sources():
    """What card map presets pack from: {"sources": [{"name", "label", "channels",
    "channelNames", "color", "description"}], "channels": every channel name a packing table
    offers ("depth", "tangent.x"... "0", "1"), "formats": [{"name", "bitDepths"}]}.

    The sources: depth, coverage (alpha), tangent (Unreal's direction in card space), flow (the
    direction as a 2-channel flow map), normal (the strands as round fibers), color (albedo), ao,
    root (0 root to 1 tip), seed (random per strand), id (random per clump), rootuv, specular,
    translucency (higher where the hair is denser) and iridescence."""
    return groomlab_core.card_map_sources()


def save_card_map_preset(name, preset):
    """Saves `preset` (a dict as card_map_presets gives, or one of their names to copy) as the
    user's preset `name` (Documents/GroomLab/card_presets/<name>.json), replacing one of that name;
    a built-in's name is refused. Returns the file."""
    if isinstance(preset, str):
        found = [p for p in card_map_presets() if p["name"] == preset]
        if not found:
            raise ValueError("no card map preset named %r" % preset)
        preset = found[0]
    preset = {k: v for k, v in dict(preset).items() if k not in ("builtin", "path")}
    preset["name"] = name
    return groomlab_core.save_card_map_preset(preset)


def delete_card_map_preset(name):
    """Deletes the user's card map preset `name`; whether there was one (built-in ones stay)."""
    return groomlab_core.delete_card_map_preset(name)


def bake_card_maps(groom, preset=CARD_MAP_PRESET, resolution=None, format=None, bit_depth=None, padding=None,
                   supersample=1, directory=None, name=None, name_template="{name}_{map}", occlusion=1.0):
    """Bakes the groom's cards and writes their maps as `preset` packs them: a preset's name (see
    card_map_presets) or a dict {"name", "files": [{"map", "channels": [R, G, B, A]}], "normal",
    "format", "bitDepth"}. An export: the groom's own bake (its cards view) is left as it was.

    resolution: texels square, up to 8192 (default: the Cards nodes' atlasSize). format: "tga",
    "png", "tiff" or "exr" (default: the preset's); bit_depth 8 or 16 (PNG, TIFF), 16 or 32 (EXR:
    half or float; TIFF 32 float) (default: the preset's, else the format's). padding: texels the
    values spread past the strands so mip maps don't bleed (default: 8 at 2K and below, scaled
    above). supersample: 1, 2 or 4 samples a texel each way (anti-aliased; the bake no bigger than
    8192). directory defaults to the scene's exports folder, name to the groom's asset_name;
    files are <name_template>.<ext> with {name}, {map} and {res}. Returns the paths written."""
    directory = (directory or asset_folder("exports")).replace("\\", "/")
    if not os.path.isdir(directory):
        os.makedirs(directory)
    if isinstance(preset, dict):
        text = json.dumps({k: v for k, v in preset.items() if k not in ("builtin", "path")})
    else:
        text = str(preset)
    kwargs = {"directory": directory, "name": name or asset_name(groom), "preset": text, "occlusion": float(occlusion),
              "supersample": int(supersample), "nameTemplate": name_template or "{name}_{map}"}
    if resolution:
        kwargs["resolution"] = int(resolution)
    if format:
        kwargs["format"] = str(format).lower()
    if bit_depth:
        kwargs["bitDepth"] = int(bit_depth)
    if padding is not None and padding >= 0:
        kwargs["padding"] = int(padding)
    return list(cmds.groomLabBakeCards(groom, **kwargs) or [])


def _file_texture(name, path, color_space):
    # Absolute: Maya 2022's file node turns a relative path into one on the spot. Save Scene As
    # repoints these with the groom's files (follow_scene_files).
    texture = cmds.shadingNode("file", asTexture=True, isColorManaged=True, name=name)
    cmds.setAttr(texture + ".fileTextureName", path, type="string")
    cmds.setAttr(texture + ".ignoreColorSpaceFileRules", True)
    try:
        cmds.setAttr(texture + ".colorSpace", color_space, type="string")
    except RuntimeError:
        pass  # no such color space in this configuration
    return texture


def card_material(mesh, maps):
    """A preview material on the card mesh from baked maps ({"Color": path, "Coverage": ..., "AO": ...}):
    a lambert (it lights like the viewport's strands, so the two compare fairly) with color x AO,
    and coverage as transparency. Reuses the mesh's material when it is a lambert made here.
    Returns the material."""
    shapes = cmds.listRelatives(mesh, shapes=True, fullPath=True) or [mesh]
    groups = cmds.listConnections(shapes[0], type="shadingEngine") or []
    group = groups[0] if groups else None
    material = (cmds.listConnections(group + ".surfaceShader") or [None])[0] if group else None
    if not material or cmds.nodeType(material) != "lambert":
        material = cmds.shadingNode("lambert", asShader=True, name=mesh + "Material")
        if not group:
            group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=material + "SG")
            cmds.sets(shapes[0], edit=True, forceElement=group)
        cmds.connectAttr(material + ".outColor", group + ".surfaceShader", force=True)
    cmds.setAttr(material + ".diffuse", 1.0)
    if maps.get("Color"):
        color = _file_texture(material + "Color", maps["Color"], "sRGB")
        if maps.get("AO"):
            ao = _file_texture(material + "AO", maps["AO"], "Raw")
            # Half the occlusion: the viewport's strands have none, and the map keeps it all for engines.
            soft = cmds.shadingNode("blendColors", asUtility=True, name=material + "SoftAO")
            cmds.setAttr(soft + ".blender", 0.5)
            cmds.connectAttr(ao + ".outColor", soft + ".color1", force=True)
            cmds.setAttr(soft + ".color2", 1.0, 1.0, 1.0, type="double3")
            shade = cmds.shadingNode("multiplyDivide", asUtility=True, name=material + "ColorAO")
            cmds.connectAttr(color + ".outColor", shade + ".input1", force=True)
            cmds.connectAttr(soft + ".output", shade + ".input2", force=True)
            cmds.connectAttr(shade + ".output", material + ".color", force=True)
        else:
            cmds.connectAttr(color + ".outColor", material + ".color", force=True)
    if maps.get("Coverage"):
        coverage = _file_texture(material + "Coverage", maps["Coverage"], "Raw")
        see_through = cmds.shadingNode("reverse", asUtility=True, name=material + "Transparency")
        cmds.connectAttr(coverage + ".outColor", see_through + ".input", force=True)
        cmds.connectAttr(see_through + ".output", material + ".transparency", force=True)
        if cmds.attributeQuery("aiOpaque", node=shapes[0], exists=True):
            cmds.setAttr(shapes[0] + ".aiOpaque", False)  # Arnold: let the transparency through
    return material


def bake_scalp(groom, directory=None, resolution=2048, name=None, reach=None, density=1.0, cap=True):
    """Bake the groom onto its scalp: the hair near the skin, seen from outside, in the scalp
    mesh's UVs (it needs UVs), into `directory` (default: the groom's folder in the scene's
    cards folder, as bake_cards; `name` defaults to the groom's asset_name).
    Writes <name>_ScalpColor (the hair's color, spread over the whole
    map so the skin between roots takes it) and <name>_ScalpCoverage (how much hair covers the
    skin) as TGA; hair within `reach` of the skin counts (default 3% of the scalp's size).
    Low card LODs leave skin showing between cards: with `cap`, a hair cap mesh (the scalp just
    above itself, textured with these) shows the hair color there instead. Returns the paths."""
    default = directory is None and name is None
    directory = (directory or _bake_folder(groom)).replace("\\", "/")
    if not os.path.isdir(directory):
        os.makedirs(directory)
    name = name or asset_name(groom)
    if default:
        name = _free_bake_name(groom, directory, name, tuple("%s.tga" % m for m in SCALP_MAPS))
    kwargs = {"directory": directory, "scalp": True, "resolution": int(resolution),
              "density": float(density), "name": name}
    if reach:
        kwargs["reach"] = float(reach)
    paths = cmds.groomLabBakeCards(groom, **kwargs)
    if cap:
        maps = {m: next((p for p in paths if p.endswith("_%s.tga" % m)), None) for m in SCALP_MAPS}
        scalp_cap(groom, maps)
    return paths


def scalp_cap(groom, maps=None):
    """The groom's hair cap: a copy of its scalp mesh just above the skin (following it, live),
    with the baked scalp maps (bake_scalp) as color and coverage. Made once, then reused (new maps
    replace the old); returns its transform, or None when the groom has no scalp mesh."""
    source = _scalp_shape(groom)
    if not source:
        return None
    cap = _existing_scalp_cap(groom)
    if cap is None:
        transform = cmds.listRelatives(groom, parent=True)[0]
        cap = cmds.createNode("transform", name=transform + "ScalpCap")
        shape = cmds.createNode("mesh", name=cap + "Shape", parent=cap)
        # Just above the skin: pushed out along the normals by 0.4% of the scalp's size.
        box = cmds.exactWorldBoundingBox(source)
        size = max(box[3] - box[0], box[4] - box[1], box[5] - box[2])
        push = cmds.createNode("polyMoveVertex", name=cap + "Lift")
        cmds.setAttr(push + ".inputComponents", 1, "vtx[*]", type="componentList")
        cmds.setAttr(push + ".localTranslateZ", 0.004 * size)
        cmds.connectAttr(source + ".worldMesh[0]", push + ".inputPolymesh")
        cmds.connectAttr(push + ".output", shape + ".inMesh")
        cmds.addAttr(cap, longName="groomLabScalpCapOf", attributeType="message")
        cmds.connectAttr(groom + ".message", cap + ".groomLabScalpCapOf")
        material = cmds.shadingNode("lambert", asShader=True, name=cap + "Material")
        group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=material + "SG")
        cmds.connectAttr(material + ".outColor", group + ".surfaceShader")
        cmds.sets(shape, edit=True, forceElement=group)
        _follow_cards_view(groom, cap)  # it shows with the cards
    shape = cmds.listRelatives(cap, shapes=True)[0]
    if maps:
        groups = cmds.listConnections(shape, type="shadingEngine") or []
        material = (cmds.listConnections(groups[0] + ".surfaceShader") or [None])[0] if groups else None
        if material:
            if maps.get("ScalpColor"):
                color = _file_texture(material + "Color", maps["ScalpColor"], "sRGB")
                cmds.connectAttr(color + ".outColor", material + ".color", force=True)
            if maps.get("ScalpCoverage"):
                coverage = _file_texture(material + "Coverage", maps["ScalpCoverage"], "Raw")
                see_through = cmds.shadingNode("reverse", asUtility=True, name=material + "Transparency")
                cmds.connectAttr(coverage + ".outColor", see_through + ".input", force=True)
                cmds.connectAttr(see_through + ".output", material + ".transparency", force=True)
                if cmds.attributeQuery("aiOpaque", node=shape, exists=True):
                    cmds.setAttr(shape + ".aiOpaque", False)
    return cap


def compare_cards(groom, cards=None):
    """A/B the cards against the strands: the cards view (cards=True) shows the cards (the card
    mesh, textured once baked, with the hair cap) instead of the strands; False shows the strands;
    None flips. The groom stays visible (its guides too). Returns "cards" or "strands"."""
    if cards is None:  # flip
        cards = not cmds.getAttr(groom + ".displayCards")
    cmds.setAttr(groom + ".displayCards", bool(cards))  # the card mesh and the hair cap follow it
    if cards:
        # Cards are seen from both sides (as engines draw them): light their backs too.
        for panel in cmds.getPanel(type="modelPanel") or []:
            cmds.modelEditor(panel, edit=True, twoSidedLighting=True)
    return "cards" if cards else "strands"


def card_display(groom, textures=None):
    """How the groom draws its cards in the cards view: textures=True (the default on a new
    groom) shows them with their baked maps, the coverage cutting out the hair's silhouette;
    False shows them flat shaded in the theme's card color, to look at the geometry; None
    leaves it. Before a bake there are no maps and the cards draw flat either way. Returns
    {"textures": bool, "baked": bool, "stale": bool} (baked: the groom has maps to show;
    stale: the cards changed since they were baked, see card_stats)."""
    if textures is not None:
        cmds.setAttr(groom + ".displayCardTextures", bool(textures))
    stats = card_stats(groom)
    return {"textures": bool(cmds.getAttr(groom + ".displayCardTextures")),
            "baked": "Coverage=" in (cmds.getAttr(groom + ".cardMaps") or ""), "stale": stats["stale"]}


def _existing_scalp_cap(groom):
    plugs = cmds.listConnections(groom + ".message", destination=True, source=False, plugs=True) or []
    return next((p.split(".")[0] for p in plugs if p.endswith(".groomLabScalpCapOf")), None)


def _pointed_textures(node, maps):
    """Point the file nodes of `node`'s material that show baked maps (named ..._<map>.tga) at
    `maps` ({map: path}); returns [(file node, previous path)] to put back."""
    put_back = []
    for texture in _material_textures(node):
        old = cmds.getAttr(texture + ".fileTextureName") or ""
        key = next((m for m in maps if maps[m] and old.endswith("_%s.tga" % m)), None)
        if key:
            put_back.append((texture, old))
            cmds.setAttr(texture + ".fileTextureName", maps[key], type="string")
    return put_back
