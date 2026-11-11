# SPDX-License-Identifier: Apache-2.0
"""Feathers: barbs grown off shafts (the Propagate node), feather cards, and the rachis mesh."""

import json
import os

from maya import cmds, mel

from groomlab.base import undoable
from groomlab.guides import guides_from_curves, list_guide_groups
from groomlab.cards import cards_preset, compare_cards, _cards_nodes, _strands_reaching
from groomlab.braids import _ensure_guide_group


# Feathers: barbs grown off shafts (the Propagate node), designs from the feathers_* presets


def feather_designs():
    """The feather designs there are: the presets named feathers_<design> ("contour", "down",
    "flight", "afterfeather", and any saved as feathers_<name>)."""
    from groomlab import presets
    return [p[len("feathers_"):] for p in presets.list_presets() if p.startswith("feathers_")]


def _feather_design(groom, design):
    """(design, the preset feathers_<design>'s graph as JSON, fitted to the groom's scalp): what
    the core's insertFeathers takes."""
    from groomlab import presets
    return design, json.dumps(presets.preset_for(groom, "feathers_" + design))


def _propagates_in(params):
    """The params of the Propagate nodes inside a Compound's params (its definition, and the
    compounds in it), for editing in place."""
    graph = (params.get("definition") or {}).get("graph") or {}
    for node in graph.get("nodes", []):
        if node.get("type") == "Propagate":
            yield node.setdefault("params", {})
        elif node.get("type") == "Compound":
            for inner in _propagates_in(node.setdefault("params", {})):
                yield inner


def _graph_propagates(model):
    """[(node, params)] of every Propagate in the graph: (its name, its params) for one at the
    top, (the Compound's name, the inner node's params) for one inside a compound."""
    found = []
    for name in model.order:
        node = model.nodes[name]
        if node["type"] == "Propagate":
            found.append((name, node["params"]))
        elif node["type"] == "Compound":
            found += [(name, p) for p in _propagates_in(node["params"])]
    return found


def feather_guide_groups(groom):
    """The guide groups of `groom` whose guides grow one feather each (add_feathers with source
    "guides", the hand_placed_feathers preset): flight_left, tail... A "feathers" group the graph
    does not read yet is one too (the group the hand-placed feathers go into by default)."""
    from groomlab import graph as gm
    groups = list(gm.GraphModel.from_groom(groom)._edit().feather_guide_groups())  # every host's (graph_inserts.h)
    if "feathers" not in groups and "feathers" in [g["name"] for g in list_guide_groups(groom)]:
        groups.append("feathers")
    return groups


def feather_groups(groom):
    """{groupId: groupName} of the groom's feather designs: the groom groups the Unreal export
    writes besides the hair (group 0)."""
    from groomlab import graph as gm
    groups = gm.GraphModel.from_groom(groom)._edit().feather_groups()  # every host's (graph_inserts.h)
    return {gid: name for name, gid in groups.items()}


@undoable("feathers")
def add_feathers(groom, design="contour", source="scatter", weights=None, density=None, curves=None,
                 group="feathers", keep_hair=True, cards=False, cards_lod=None):
    """Grow feathers; returns {"shafts", "split", "designs", "merge", "cards"}: the node names made.

    Every shaft grows barbs (a Propagate node inside the design, a Compound from the preset
    feathers_<design>: "contour", "down", "flight", "afterfeather" or "barred", fitted to the scalp's size). `source` says
    where the shafts come from:
      "scatter": a new Scatter (`density` shafts per unit area; default: the design's) shaped by
                 the groom's guides through an Interpolate, so the feathers follow the groom;
      "guides":  the guide group `group` (default "feathers"): each guide is a hand-placed
                 feather. `curves` become guides of that group first; the group is left out of
                 the Guides nodes that grow the hair;
      "node" or "node.output": what an existing node gives (a Split's "b", say).
    Several designs (a list) share the shafts through a Split node, `weights` their shares
    (default: equal). The feathers are merged with the hair before the output (`keep_hair`
    False: they replace it). With `cards`, the feathers get game cards of their own
    (create_feather_cards: a Cards node on the feather branch, `cards_lod` its preset).
    """
    from groomlab import graph as gm
    designs = [design] if isinstance(design, str) else list(design)
    parts = [_feather_design(groom, d) for d in designs]
    model = gm.GraphModel.from_groom(groom)
    # Every host's (graph_inserts.h insertFeathers): the shafts, the split, the designs (each a
    # groom group of its own: Unreal gives it its own material, LODs and physics), their merges.
    edit = model._edit()
    with gm._core_errors():
        made = edit.insert_feathers(parts, [] if weights is None else [float(w) for w in weights], str(source), group,
                                    bool(keep_hair), None if density is None else float(density), model.groom_size)
    model._take(edit)
    # The guides before the graph reads them (a Guides node of a group there is not fails).
    if curves:
        guides_from_curves(groom, curves, group=group)
    elif source == "guides":
        _ensure_guide_group(groom, group)
    model.save(groom)
    feathers = (made["merges"] or made["designs"])[-1]
    made_cards = create_feather_cards(groom, feathers=feathers, lod=cards_lod) if cards else None
    return {"shafts": made["shafts"], "split": made["split"], "designs": list(made["designs"]), "merge": made["merge"],
            "cards": made_cards}


def _feather_branch_end(model):
    """The node giving a graph's feathers: the last node of a chain only feathers reach (a feather
    design, the Merge of several) before they meet hair or the output; None without feathers."""
    only = {n for n in model.order
            if model.node_type(n) not in ("Output", "Cards") and _strands_reaching(model, n) == (True, False)}
    for n in model.order:
        if n in only and any(dst not in only for dst, _ in model.downstream_of(n)):
            return n
    return next((n for n in model.order if n in only), None)


@undoable("feather cards")
def create_feather_cards(groom, feathers=None, lod=None, segments=None, min_length=None, variations=None,
                         spine=None):
    """Game cards for the groom's feathers; returns the Cards node's name.

    A Cards node (featherMode "perFeather") goes right after `feathers`, the node giving them
    (default: the end of the feather branch add_feathers made, where the feathers meet the hair
    or the output), so the feathers are carded apart from the hair: one card per feather, a
    strip of `segments` segments (featherSegments) along its shaft in the vane plane, as wide as
    both vanes (the rachis and barbs baked into its texture), with `spine` (featherSpine, on by
    default) a vertex column down the rachis and each half folded to follow its vane (two quads
    a segment), a down feather two crossed cards;
    `variations` alike feathers share a texture (mirrored for the other side's asymmetry);
    feathers shorter than `min_length` x the longest make no card. `lod` ("hero", "mid", "low")
    applies the feather preset (cards_preset). Cards nodes after it (the hair's, which the
    feathers now reach too) set to featherMode "auto" become "off", so no feather is carded
    twice. Both share the groom's one atlas and bake; card_stats counts each node's cards,
    triangles and feathers. An existing feather Cards node right there is reused."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    feathers = feathers or _feather_branch_end(model)
    if feathers is None:
        raise ValueError("%s grows no feathers: add_feathers first" % groom)
    if feathers not in model.nodes or model.node_type(feathers) in ("Output", "Cards") or \
            not _strands_reaching(model, feathers)[0]:
        raise ValueError("%r does not give feathers (a feather design, or a node after one)" % feathers)
    consumers = []
    for dst, _ in model.downstream_of(feathers):
        if dst not in consumers:
            consumers.append(dst)
    node = next((d for d in consumers if model.node_type(d) == "Cards" and model.param(d, "featherMode") == "perFeather"),
                None)
    if node is None:
        node = model.add_node("Cards", name=model.unique_name("featherCards"),
                              params={"featherMode": "perFeather", "variations": 16})
        if consumers:
            for dst in consumers:
                model.insert_between(feathers, dst, node)
        else:
            model.connect(feathers, node, "strands")
        x, y = model.position(feathers)
        model.set_position(node, x + gm.LAYOUT_DX / 2, y + gm.LAYOUT_DY)
    # The hair's Cards nodes after it card the hair only.
    for other in _cards_nodes(model):
        if other != node and node in model.upstream(other) and model.param(other, "featherMode") == "auto":
            model.set_param(other, "featherMode", "off")
    if lod:
        cards_preset(groom, lod, node=node, model=model)
    kinds = {"featherSegments": int, "featherMinLength": float, "variations": int, "featherSpine": bool}
    for key, value in (("featherSegments", segments), ("featherMinLength", min_length), ("variations", variations),
                       ("featherSpine", spine)):
        if value is not None:
            model.set_param(node, key, kinds[key](value))
    if not cmds.listConnections(groom + ".outCards", destination=True, source=False):
        cmds.setAttr(groom + ".cardDensity", 1.0)
    model.save(groom)
    compare_cards(groom, True)  # show them
    return node


# ---------------------------------------------------------------------------
# Feathers in Unreal (the Meerkat eagle workflow): each feather's shaft is a rachis mesh skinned
# into the character's skeletal mesh, its barbs groom strands bound to it.


@undoable("rachis width")
def set_rachis_width(groom, width, profile=None, sides=None):
    """Give every feather design of the groom a rachis `width` (Propagate's shaftWidth, at the
    root; 0: none), optionally its `profile` (a ramp root to tip, [[pos, value], ...]) and tube
    `sides` (2: a flat ribbon). Designs inside compounds (add_feathers) take it too. With a
    width the barbs start on the rachis's surface. Returns the nodes changed."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    changed = []
    for name, params in _graph_propagates(model):
        params["shaftWidth"] = float(width)
        if profile is not None:
            params["shaftWidthProfile"] = [list(p) for p in profile]
        if sides is not None:
            params["shaftSides"] = int(sides)
        node = model.nodes[name]
        if node["type"] == "Compound" and "shaftWidth" in (node["params"].get("definition") or {}).get("expose", {}):
            node["params"]["shaftWidth"] = float(width)  # the exposed param sets the inner one
        if name not in changed:
            changed.append(name)
    if not changed:
        raise RuntimeError("%s has no feathers (Propagate nodes): add_feathers first" % groom)
    model.save(groom)
    return changed


def rachis_width(groom):
    """The largest rachis width (shaftWidth) of the groom's feather designs; 0 without a rachis."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    widths = [float(p.get("shaftWidth", 0.0) or 0.0) for _, p in _graph_propagates(model)]
    for name in model.order:  # a compound's exposed shaftWidth overrides its inner one
        node = model.nodes[name]
        if node["type"] == "Compound" and "shaftWidth" in node["params"]:
            widths.append(float(node["params"]["shaftWidth"]))
    return max(widths + [0.0])


def rachis_mesh(groom):
    """The live rachis mesh's transform (create_rachis_mesh), or None."""
    existing = cmds.listConnections(groom + ".outRachis", destination=True, source=False, shapes=True) or []
    return cmds.listRelatives(existing[0], parent=True)[0] if existing else None


def create_rachis_mesh(groom, width=None):
    """A live Maya mesh of the feathers' shafts as rachis tubes, connected to outRachis; returns
    its transform (reusing the one there is). It follows the groom (every feather, full density);
    skin it into the character with the body (or combine it into the skeletal mesh's geometry and
    bind it there), so the barbs, rooted on its surface, ride it in the engine.

    UV sets: map1 (u around the tube, v root to tip) and featherId (u: the feather's index, one
    per feather in the mesh; v: its groom group). `width` sets the designs' rachis width first
    (set_rachis_width); without one, a groom whose feathers have no rachis width is an error."""
    if width is not None:
        set_rachis_width(groom, width)
    elif rachis_width(groom) <= 0.0:
        raise RuntimeError("%s's feathers have no rachis: give them a width (create_rachis_mesh(groom, width=...) "
                           "or set_rachis_width)" % groom)
    existing = rachis_mesh(groom)
    if existing:
        return existing
    transform = cmds.listRelatives(groom, parent=True)[0]
    rachis = cmds.createNode("transform", name=transform + "Rachis")
    mesh = cmds.createNode("mesh", name=rachis + "Shape", parent=rachis)
    cmds.connectAttr(groom + ".outRachis", mesh + ".inMesh")
    material = cmds.shadingNode("lambert", asShader=True, name=transform + "RachisMaterial")
    cmds.setAttr(material + ".color", 0.55, 0.5, 0.42, type="double3")
    group = cmds.sets(renderable=True, noSurfaceShader=True, empty=True, name=material + "SG")
    cmds.connectAttr(material + ".outColor", group + ".surfaceShader")
    cmds.sets(mesh, edit=True, forceElement=group)
    return rachis


def export_rachis(groom, path, unity=False):
    """Write the rachis mesh to `path`: .fbx (Maya's FBX export, UV sets and all: import it with
    the skeletal mesh, or skin it in Maya first and export them together) or .abc (AbcExport, UV
    sets written). Uses the live mesh (create_rachis_mesh), or makes one and removes it after.
    With `unity` the FBX is in Unity's conventions, as the Unity card export writes it: Y up and
    converted to meters (Unity imports it at scale 1, where the strands' .abc lands with its
    Scale Factor 0.01). Returns the path."""
    ext = os.path.splitext(path)[1].lower()
    if ext not in (".fbx", ".abc"):
        raise ValueError("the rachis goes to .fbx or .abc, not %r" % ext)
    if unity and ext != ".fbx":
        raise ValueError("the rachis goes to Unity as .fbx")
    directory = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(directory):
        os.makedirs(directory)
    path = path.replace("\\", "/")
    mesh = rachis_mesh(groom)
    before = set(cmds.ls(long=True)) if mesh is None else None
    selection = cmds.ls(selection=True)
    try:
        if mesh is None:
            mesh = create_rachis_mesh(groom)
        shape = cmds.listRelatives(mesh, shapes=True, fullPath=True)[0]
        if not cmds.polyEvaluate(shape, face=True):
            raise RuntimeError("%s has no rachis to export (feathers with a rachis width)" % groom)
        if ext == ".fbx":
            if not cmds.pluginInfo("fbxmaya", query=True, loaded=True):
                cmds.loadPlugin("fbxmaya", quiet=True)
            cmds.select(mesh, replace=True)
            commands = ["FBXResetExport", "FBXExportInputConnections -v false", "FBXExportSmoothingGroups -v true",
                        "FBXExportTriangulate -v false", "FBXExportEmbeddedTextures -v false"]
            if unity:  # the Unity card export's conventions
                commands += ["FBXExportUpAxis y", "FBXExportConvertUnitString m", "FBXExportScaleFactor 1.0"]
            try:
                for command in commands:
                    mel.eval(command)
                mel.eval('FBXExport -f "%s" -s' % path)
            finally:
                mel.eval("FBXResetExport")  # the next FBX export (the user's own) starts from the defaults
        else:
            if not cmds.pluginInfo("AbcExport", query=True, loaded=True):
                cmds.loadPlugin("AbcExport", quiet=True)
            frame = cmds.currentTime(query=True)
            root = cmds.ls(mesh, long=True)[0]
            cmds.AbcExport(j='-frameRange %s %s -uvWrite -writeUVSets -worldSpace -root "%s" -file "%s"'
                           % (frame, frame, root, path))
    finally:
        cmds.select(selection, replace=True) if selection else cmds.select(clear=True)
        if before is not None:
            made = [n for n in cmds.ls(long=True) if n not in before]
            cmds.delete([n for n in made if cmds.objExists(n)])
    return path
