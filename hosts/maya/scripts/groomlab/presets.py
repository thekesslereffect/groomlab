# SPDX-License-Identifier: Apache-2.0
"""Graph presets (PG-5): graphs saved as JSON files, independent of any scene.

Presets are looked up, in order, in:
  1. every directory on GROOMLAB_PRESET_PATH (os.pathsep-separated),
  2. the user's preset folder (<Maya user app dir>/groomlab/presets), where
     save_preset writes,
  3. the presets shipped with GroomLab (<module>/presets).
A preset in an earlier directory hides one of the same name in a later one.

A preset is a graph (graph_io.h) plus a few fields of its own, all optional:
  "size"       the scalp size it was made on (the largest extent of the scalp's
               bounding box, scene units). Applied to a bigger or smaller scalp, it
               is fitted: lengths and widths scale with the size, densities with its
               square, per-length rates (curls, braid crossings...) with its inverse,
               so a groom made on a head looks the same on a 1 cm test sphere.
               save_preset records it.
  "about"      {"title", "description", "category"}: what the Graph Editor's Presets
               menu shows (the category as a submenu, CATEGORIES; the description as
               the item's tooltip). Absent: the file's name, no description, no category.
  "requires"   {"maps": [{"name", "kind", "value", "color"}]}: painted maps the graph
               reads, made (as add_map makes them) when the groom has none of that name;
               "groups": [{"name", "draw"}]: guide groups the graph reads, made empty when
               missing ("draw": the Graph Editor then picks the Draw tool drawing into it).
  "backdrops"  Graph Editor backdrops framing parts of the graph (layout only):
               [{"title", "x", "y", "w", "h"}].
  "controls"   {"ctl_<name>": value or [x, y, z]}: the values of the groom's controls (Control
               nodes' sliders, keyable Expression variables, keyed params' attributes), set
               after the graph is applied. Copy Graph writes them; keys never travel.
  "exposed"    [{"node", "param", "name"}]: the look's key params, which the Compound made by
               Insert after selected node shows on its own panel (the core's insertPresetJson;
               without the list, a default pick).

A preset is also how a look travels as text: copy_text gives the groom's graph as the preset
Save as Preset writes, with its control values, for the clipboard (a forum post, a chat, a .json
file); paste_text checks such a text and puts it on a groom, fitted to its scalp, in one undo step.
The text is only ever read as JSON and checked against the operators before anything changes.
"""

import json
import math
import os

import groomlab_core

SHIPPED_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "presets"))

# The Presets menu's submenus, in this order (a category not listed here follows them); a
# preset's own fields (not part of the graph a groom stores): the core's (presets.h), which the
# standalone app reads the same files through.
CATEGORIES = tuple(groomlab_core.preset_categories())
PRESET_FIELDS = tuple(groomlab_core.preset_fields())


def user_dir():
    """Where presets are saved: the core's user folder (presets.h presetUserDir), every host's."""
    return groomlab_core.preset_user_dir()


def search_dirs():
    """$GROOMLAB_PRESET_PATH's folders, the user's, the folder earlier GroomLabs saved to (Maya's
    user folder), the shipped: the core's order (presets.h presetSearchDirs), every host's."""
    return list(groomlab_core.preset_search_dirs(user_dir(), SHIPPED_DIR))


def list_presets(details=False):
    """Names of every available preset, sorted. With `details`, a dict per preset instead:
    {"name", "title", "description", "category", "path"} (preset_info): the core's (presets.h)."""
    found = groomlab_core.list_presets(search_dirs())
    return [dict(p) for p in found] if details else [p["name"] for p in found]


def preset_path(name):
    path = groomlab_core.preset_path(name, search_dirs())
    if not path:
        raise ValueError("no preset named %r (looked in %s)" % (name, ", ".join(search_dirs())))
    return path


def read_preset(name):
    with open(preset_path(name), encoding="utf-8") as f:
        return json.load(f)


def title_of(name):
    """A preset's title when its file gives none: its name in words ("wet_hair" -> "Wet hair")."""
    return groomlab_core.preset_title(name)


def preset_info(name):
    """What the Presets menu shows of a preset: {"name", "title", "description", "category",
    "path"}. A preset that cannot be read says why in its description (category ""): the core's."""
    try:
        path = preset_path(name)
    except ValueError as e:
        return {"name": name, "title": title_of(name), "description": "Cannot be read: %s" % e, "category": "", "path": ""}
    return dict(groomlab_core.preset_info(name, path))


def save_preset(groom, name, overwrite=False, about=None):
    """Save the groom's graph (the default graph if it has none) as a user preset, with its node
    layout and backdrops as the Graph Editor shows them. `about`: {"title", "description",
    "category"} for the Presets menu."""
    why = groomlab_core.preset_name_error(name or "")  # every host's (presets.h presetNameError)
    if why:
        raise ValueError(why)
    graph = preset_data(groom, about=about)
    path = os.path.join(user_dir(), name + ".json")
    if os.path.exists(path) and not overwrite:
        raise ValueError("preset %r exists (pass overwrite=True to replace it)" % name)
    if not os.path.isdir(os.path.dirname(path)):
        os.makedirs(os.path.dirname(path))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(graph, f, indent=2)
    return path


def preset_data(groom, about=None):
    """The preset save_preset writes for `groom` (a dict): its graph (the default graph if it has
    none) with "size" (its scalp's), "about" when given, the node layout ("ui") and backdrops."""
    import groomlab
    from groomlab import graph as gm
    from maya import cmds
    graph = groomlab.get_graph(groom, expand_default=True)  # the layout is in it ("ui", "backdrops")
    backdrops = graph.pop("backdrops", [])
    if not any("ui" in node for node in graph.get("nodes", [])):  # an older scene: its graphUi
        layout = json.loads(cmds.getAttr(groom + ".graphUi") or "{}")
        backdrops = backdrops or layout.pop(gm.BACKDROPS_KEY, [])
        for node in graph.get("nodes", []):
            if node.get("name") in layout:
                x, y = layout[node["name"]]
                node["ui"] = {"x": float(x), "y": float(y)}
    if backdrops:
        graph["backdrops"] = backdrops
    about = about or {}
    # The preset's fields in their order: the core's, every host's (presets.h makePresetJson).
    return json.loads(groomlab_core.make_preset_json(json.dumps(graph), scalp_size(groom) or None, about.get("title") or "",
                                                     about.get("description") or "", about.get("category") or ""))


def apply_preset(groom, name, fit=True):
    """Replace the groom's graph with a preset, fitted to the groom's scalp size, with the maps
    it reads (made when missing), its node layout and its control values: one undo step."""
    import groomlab
    with groomlab.undo_chunk("apply preset"):
        _apply(groom, read_preset(name), fit)


def _apply(groom, data, fit, catalog=None, make_maps=True):
    """Put a preset (a dict) on the groom: the maps it requires made (with `make_maps`), its
    layout and backdrops, the graph fitted to the groom's scalp, its control values. Returns
    (the graph stored, {"set": [controls set], "skipped": {control: why}}, [maps made])."""
    import groomlab
    from maya import cmds
    made = make_required_maps(groom, required_maps_of(data)) if make_maps else []
    if make_maps:
        make_required_groups(groom, required_groups_of(data))  # before the graph reads them
    graph = fitted(data, scalp_size(groom) if (fit and groom) else None, catalog)  # its control values too
    values = graph.get("controls")
    for key in PRESET_FIELDS:
        graph.pop(key, None)
    # The layout goes in the graph (each node's "ui", the backdrops), as every host keeps it.
    drops = [dict(b) for b in data.get("backdrops") or [] if isinstance(b, dict)]
    if drops:
        graph["backdrops"] = drops
    groomlab.set_graph(groom, graph)
    viewed = cmds.getAttr(groom + ".viewNode") or ""
    if viewed and viewed not in {n.get("name") for n in graph.get("nodes", [])}:
        cmds.setAttr(groom + ".viewNode", "", type="string")  # the node it viewed is gone
    controls = set_controls(groom, values, graph)
    return graph, controls, made


def graph_of(data, size=None, catalog=None):
    """A preset's graph as a groom stores it: fitted to `size` when given and the preset records
    one, without the preset's own fields (PRESET_FIELDS)."""
    graph = fitted(data, size, catalog)
    for key in PRESET_FIELDS:
        graph.pop(key, None)
    return graph


def preset_for(groom, name, fit=True):
    """The preset's graph as it applies to `groom`: fitted to its scalp's size when the preset
    records one, without the preset's own fields (PRESET_FIELDS)."""
    return graph_of(read_preset(name), scalp_size(groom) if (fit and groom) else None)


def backdrops(name):
    """The preset's Graph Editor backdrops (layout only), as the editor stores them."""
    return [dict(b) for b in read_preset(name).get("backdrops") or [] if isinstance(b, dict)]


def controls(name, groom=None, fit=True):
    """The control values a preset records ({"ctl_<name>": value}), for set_controls; keyed
    params' values fitted to `groom`'s scalp size as the graph is (with `fit`)."""
    data = fitted(read_preset(name), scalp_size(groom) if (fit and groom) else None)
    values = data.get("controls")
    return dict(values) if isinstance(values, dict) else {}


def required_maps(name):
    """The painted maps a preset's graph reads ("requires": {"maps": [...]})."""
    return required_maps_of(read_preset(name))


def required_maps_of(data):
    """[{name, kind, value, color}] the preset's graph reads: the core's (presets.h presetRequiredMaps)."""
    return [dict(m) for m in groomlab_core.preset_required_maps(json.dumps(data))]


def required_groups(name):
    """The guide groups a preset's graph reads ("requires": {"groups": [...]}): [{"name", "draw"}]."""
    return required_groups_of(read_preset(name))


def required_groups_of(data):
    """[{name, draw}] the preset's graph reads, each once: the core's (presets.h presetRequiredGroups)."""
    return [dict(g) for g in groomlab_core.preset_required_groups(json.dumps(data))]


def draw_group(name):
    """The guide group the preset's Draw tool draws into once it is applied ("draw": true), or None."""
    return next((g["name"] for g in required_groups(name) if g["draw"]), None)


def prepare(groom, name):
    """Give `groom` what the preset needs that it lacks: the painted maps it reads, made as
    add_map makes them (kind, fill value or color), and the guide groups it reads, made empty.
    Returns the names of the maps made."""
    make_required_groups(groom, required_groups(name))
    return make_required_maps(groom, required_maps(name))


def make_required_groups(groom, required):
    """Make the guide groups `required` ([{"name", ...}]) the groom lacks, empty. Returns the names made."""
    import groomlab
    have = [g["name"] for g in groomlab.list_guide_groups(groom)]
    made = []
    for g in required:
        if g["name"] not in have:
            groomlab.add_guide_group(groom, g["name"])
            made.append(g["name"])
    return made


def make_required_maps(groom, required):
    import groomlab
    made = []
    have = groomlab.list_maps(groom)
    for m in required:
        if m["name"] in have:
            continue
        groomlab.add_map(groom, m["name"], value=float(m.get("value", 0.0)), kind=m.get("kind", "guides"),
                         color=m.get("color"))
        made.append(m["name"])
    return made


def control_values(groom):
    """{"ctl_<name>": value} of the groom's controls now (a vector's [x, y, z]): what copy_text
    records. Only the values: keys stay on the groom."""
    import groomlab
    return {groomlab.CONTROL_PREFIX + name: value for name, value in sorted(groomlab.list_controls(groom).items())}


def set_controls(groom, values, graph=None):
    """Set the groom's controls from {"ctl_<name>": value or [x, y, z]} (a preset's "controls"),
    for the controls `graph` (default: the groom's) has. A control keyed or driven on this groom
    keeps its keys; values of the wrong kind are left out. Returns {"set": [attributes set],
    "skipped": {attribute: why}}."""
    import groomlab
    from maya import cmds
    done = {"set": [], "skipped": {}}
    if not isinstance(values, dict):
        return done
    wanted = groomlab._controls_in(groomlab.get_graph(groom) if graph is None else graph)
    prefix = groomlab.CONTROL_PREFIX
    for attr, value in sorted(values.items(), key=lambda kv: str(kv[0])):
        name = attr[len(prefix):] if isinstance(attr, str) and attr.startswith(prefix) else None
        try:
            exists = name in wanted and cmds.attributeQuery(attr, node=groom, exists=True)
        except RuntimeError:  # not an attribute name at all
            exists = False
        if not exists:
            done["skipped"][str(attr)[:80]] = "not a control of this graph"
            continue
        plugs = groomlab._control_plugs_of(groom, attr)
        given = list(value) if isinstance(value, list) else [value]
        if len(given) != len(plugs) or not all(_is_number(v) or isinstance(v, bool) for v in given) or \
                not all(math.isfinite(float(v)) for v in given):
            done["skipped"][attr] = "the value does not fit it (%s)" % _clip(repr(value), 60)
            continue
        if cmds.listConnections(plugs + [groom + "." + attr], source=True, destination=False):
            done["skipped"][attr] = "keyed or driven on this groom: its keys stay"
            continue
        if any(cmds.getAttr(p, lock=True) for p in plugs):
            done["skipped"][attr] = "locked"
            continue
        for plug, v in zip(plugs, given):
            kind = cmds.getAttr(plug, type=True)
            if kind == "bool":
                cmds.setAttr(plug, bool(v))
            elif kind in ("long", "short", "byte", "enum"):
                cmds.setAttr(plug, int(round(float(v))))
            else:
                cmds.setAttr(plug, float(v))
        done["set"].append(attr)
    return done


def scalp_size(groom, scaled=True):
    """The largest extent of the groom's scalp (world bounding box) in centimeters, the unit the
    groom works in (Maya's internal unit, whatever the UI's linear unit: the scalp, guides and
    params are all in it), or None without a scalp. The core's presetScalpSize measures the same.
    A groom scaled with its scalp (groomScale k) stores its params at 1x: the size is divided by k,
    so a preset or a new node fits the size the params are at (and a saved preset records it).
    `scaled` False: the size as it is (a groom just made is at 1x)."""
    import groomlab
    from maya import cmds
    from maya.api import OpenMaya as om
    scalp = groomlab.scalp_mesh(groom)
    if not scalp:
        return None
    box = cmds.exactWorldBoundingBox(scalp)  # in the UI's linear unit
    size = max(box[3] - box[0], box[4] - box[1], box[5] - box[2])
    size = om.MDistance(size, om.MDistance.uiUnit()).asCentimeters()
    if scaled and cmds.attributeQuery("groomScale", node=groom, exists=True):
        k = cmds.getAttr(groom + ".groomScale")
        if k and k > 0.0:
            size /= k
    return size if size > 0 else None


def fitted(graph, size, catalog=None):
    """`graph` (a preset) with its sizes scaled from the scalp size it records to `size`: params in
    length or width units (numbers, and the min and max of maps) by size / recorded, densities by
    its inverse square, per-length rates (unit "perLength") by its inverse; inside compounds too,
    and the control values of keyed params. Unchanged without both sizes. The "size" key is
    dropped either way. The fit is the core's (presets.h: fit_preset_json), the same the
    standalone app applies; `catalog` is unused (the core reads the operators itself)."""
    del catalog
    return json.loads(groomlab_core.fit_preset_json(json.dumps(graph), None if not size else float(size)))


# ---------------------------------------------------------------------------
# Checking a preset against the operators (a renamed param must not be silently ignored)


def check(preset, catalog=None, warnings=True):
    """What is wrong with a preset (a graph dict), as a list of messages (empty: fine): unknown
    node types, params the operator does not have, values of the wrong kind or not among a
    param's choices, maps on params that take none, links to ports that do not exist or do not
    fit, required inputs left open, Expression variables that are not JSON, and the same inside
    compounds; with `warnings`, what the Graph Editor warns about too (nodes sharing a seed).
    The plugin itself ignores unknown params, so this is how renames are caught."""
    from groomlab import graph as gm
    if catalog is None:
        catalog = gm.OperatorCatalog.from_maya()
    # The params against their specs: the core's check, every host's (presets.h presetParamProblems).
    problems = list(groomlab_core.preset_param_problems(json.dumps(preset)))
    _check_graph(preset, catalog, "", problems, warnings)
    about = preset.get("about")
    if about is not None:
        if not isinstance(about, dict):
            problems.append("about: not an object")
        else:
            unknown = sorted(set(about) - {"title", "description", "category"})
            if unknown:
                problems.append("about: unknown fields %s" % ", ".join(unknown))
    return problems


def _check_graph(graph, catalog, where, problems, warnings=True):
    from groomlab import graph as gm
    names = set()
    for node in graph.get("nodes", []):
        label = where + str(node.get("name"))
        if node.get("name") in names:
            problems.append("%s: two nodes have this name" % label)
        names.add(node.get("name"))
        op_type = node.get("type")
        if op_type not in catalog.types():
            problems.append("%s: unknown node type %r" % (label, op_type))
            continue
        if op_type == "Compound":
            definition = node.get("params", {}).get("definition")
            if isinstance(definition, dict):
                _check_graph(definition.get("graph", {}), catalog, label + "/", problems, warnings)
    try:
        model = gm.GraphModel(catalog, graph)
    except (gm.GraphError, KeyError, TypeError) as e:
        problems.append("%s%s" % (where, e))
        return
    for problem in model.validate() + model.unused_problems():  # a shipped preset: every node whole
        # Inside a compound, its Input nodes feed the graph; nothing else is exempt.
        problems.append(where + problem)
    for warning in model.warnings() if warnings else ():
        problems.append(where + warning)


def _is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


# ---------------------------------------------------------------------------
# A look as text: Copy Graph and Paste Graph (the Graph Editor), copy_graph_text and
# paste_graph_text (groomlab). The text is a preset, so it also loads as a .json preset file.

# The core's limits on a pasted text (presets.h presetFromText, every host's).
MAX_TEXT = groomlab_core.MAX_PRESET_TEXT    # characters: a big groom's graph is well under 100 KB
MAX_NODES = groomlab_core.MAX_PRESET_NODES  # nodes in all, compounds' inner nodes too
SHARED_CATEGORY = "Shared"


def copy_text(groom):
    """The groom's look as text: the preset save_preset writes (graph, scalp size, layout,
    backdrops), titled after the groom (category "Shared"), with its control values
    ("controls"). Keys and painted maps stay with the groom."""
    import groomlab
    from maya import cmds
    groom = groomlab._shape_of(groom, groomlab.GROOM_TYPE)
    title = (cmds.listRelatives(groom, parent=True) or [groom])[0].split("|")[-1].split(":")[-1]
    data = preset_data(groom, about={"title": title, "category": SHARED_CATEGORY,
                                     "description": "Copied from %s (GroomLab %s)" % (title, cmds.groomLabVersion())})
    values = control_values(groom)
    if values:
        data["controls"] = values
    return json.dumps(data, indent=2)


def _clip(message, limit=200):
    return message if len(message) <= limit else message[:limit] + "..."


def parse_text(text, removed=None):
    """The preset (a dict) a text pasted as a groom's whole graph holds: JSON only, never run as
    code. ValueError, saying why, for anything else: the core's checks every host makes (presets.h
    pastedGraphFromText: not JSON, not a GroomLab graph, too big, too deep, from a newer GroomLab,
    unknown node types, fields of the wrong kind, params against their specs, the graph whole), its
    backdrops as the Graph Editor draws them, params naming files on other machines taken out
    (`removed`, a dict: "node.param" -> path of those)."""
    if not isinstance(text, str):
        raise ValueError("cannot paste a graph: the clipboard is empty")
    try:
        body, taken = groomlab_core.pasted_graph_from_text(text)
    except ValueError as e:
        raise ValueError("cannot paste a graph: %s" % _clip(str(e), 800))
    if removed is not None:
        removed.update(taken)
    return json.loads(body)


def graph_files(graph):
    """[(label, path)] of every param of `graph` (a dict) naming a file: texture maps, ImportCurves'
    and GuideCache's path, FeatherColor's images, HairColor's map and Interpolate's regionMap when
    an image's path, inside compounds too (compound/inner.param): the core's (asset_files.h
    graphFileParams), every host's."""
    return [tuple(p) for p in groomlab_core.graph_file_params(json.dumps(graph))]


def with_graph_files(graph, paths):
    """`graph` (a dict) with the file params `paths` ({label: path}, graph_files' labels) naming their
    new paths (a texture map keeps its other fields): the core's (withGraphFileParams). A new dict."""
    return json.loads(groomlab_core.with_graph_file_params(json.dumps(graph), dict(paths)))


def is_image_path(name):
    """Whether a name is an image file's: the core's rule (texture.h's isImagePath: an extension it
    reads, or a UDIM set), every host's."""
    return isinstance(name, str) and groomlab_core.is_image_path(name)


def file_exists(path):
    """Whether a file is there; a UDIM tile set (any token, or a texture map's plain path standing
    for one) when any tile is: the core's check, every host's (asset_files.h fileThere)."""
    return bool(path) and groomlab_core.file_there(path)


def missing_files(groom, graph, network=False):
    """The params of `graph` naming files `groom` cannot find: {"node.param": path as stored}
    (resolved as the groom resolves them). A path on another machine (//host/share) counts as
    missing unless `network`: a pasted stranger's graph never makes Maya reach out to their
    server, but a groom's own graph may name the studio's shares."""
    import groomlab
    out = {}
    for label, path in graph_files(graph):
        resolved = None if groomlab_core.is_network_path(path) and not network else groomlab.resolve_path(path, groom)
        if not resolved or not file_exists(resolved):
            out[label] = path
    return out


def missing_inputs(groom, graph):
    """What `graph` names that `groom` does not have: {"maps": {name: how it is read},
    "colorSets": [color sets missing on the scalp], "files": {"node.param": path},
    "meshes": {"node.param": name} (Instance prototypes), "colliders": [names], "groups":
    [guide groups], "parts": [parts]}. Nodes reading what is missing use their defaults."""
    import groomlab
    from groomlab import utils
    from maya import cmds
    have = {m["name"] for m in groomlab.maps(groom)}
    scalp = groomlab.scalp_mesh(groom)
    color_sets = set(cmds.polyColorSet(scalp, query=True, allColorSets=True) or []) if scalp else set()
    missing = {"maps": {}, "colorSets": [], "files": {}, "meshes": {}, "colliders": [], "groups": [], "parts": []}
    for name, how in sorted(utils.graph_maps(graph).items()):
        if name in have or name in color_sets:
            continue
        if how == "colorSet":
            missing["colorSets"].append(name)
        else:
            missing["maps"][name] = how
    missing["files"] = missing_files(groom, graph)
    colliders = {c["name"] for c in groomlab.colliders(groom)}
    groups = {g["name"] for g in groomlab.list_guide_groups(groom)}
    parts = set(groomlab.list_parts(groom))
    for node in groomlab._graph_nodes(graph):
        params, kind = node.get("params") or {}, node.get("type")
        if kind in ("Instance", "Scales"):
            proto = params.get("prototype", "instance")
            if isinstance(proto, str) and not _mesh_exists(proto):
                missing["meshes"]["%s.prototype" % node.get("name")] = proto
        elif kind == "ColliderMesh":
            name = params.get("name", "collider")
            if isinstance(name, str) and name not in colliders and name not in missing["colliders"]:
                missing["colliders"].append(name)
        elif kind == "Guides":
            names = [params.get("group")] if isinstance(params.get("group"), str) else []
            names += groomlab._group_list(params.get("exclude")) if isinstance(params.get("exclude"), str) else []
            for g in names:
                if g and g not in groups and g not in missing["groups"]:
                    missing["groups"].append(g)
        elif kind == "Part":
            part = params.get("part")
            if isinstance(part, str) and part and part not in parts and part not in missing["parts"]:
                missing["parts"].append(part)
    return missing


def _mesh_exists(name):
    from maya import cmds
    if not name or any(c in name for c in "*?[]"):
        return False
    try:
        found = cmds.ls(name, long=True) or []
    except RuntimeError:
        return False
    for node in found:
        if cmds.nodeType(node) == "mesh" or cmds.listRelatives(node, shapes=True, type="mesh"):
            return True
    return False


def paste_text(groom, text, fit=True):
    """Put a look copied as text (copy_text, or any preset's JSON) on `groom`: checked first
    (parse_text: nothing changes when it does not fit), fitted to this groom's scalp size with
    `fit`, the graph and its layout replaced, the control values set: one undo step. Params naming
    files on other machines are taken out first. What the look reads that this groom lacks
    (painted maps, color sets, files, meshes, colliders, guide groups, parts) is reported, not
    made (nor are a preset's "requires" maps: pasting writes no files); the nodes reading it use
    their defaults until it is there.

    Returns {"title", "nodes", "fit": [from size, to size] or None, "controls": [set],
    "controlsSkipped": {control: why}, "removed": {"node.param": network path}, "missing":
    missing_inputs(...), "message"}."""
    import groomlab
    from groomlab import graph as gm
    groom = groomlab._shape_of(groom, groomlab.GROOM_TYPE)
    removed = {}
    data = parse_text(text, removed)
    catalog = gm.OperatorCatalog.from_maya()
    size = scalp_size(groom) if fit else None
    recorded = data.get("size")
    try:
        stored = fitted(data, size, catalog)  # (the core refuses sizes fitted past a number)
    except RuntimeError as e:
        raise ValueError("cannot paste the graph: %s" % _clip(str(e)))
    try:
        gm.GraphModel(catalog, stored)  # as it will be stored: it loads in the Graph Editor
    except (gm.GraphError, KeyError, TypeError) as e:
        raise ValueError("cannot paste the graph: %s" % _clip(str(e)))
    from maya import cmds
    try:
        with groomlab.undo_chunk("paste graph"):
            graph, controls, _ = _apply(groom, data, fit, catalog, make_maps=False)
    except Exception as e:  # checked above, so unexpected: take back what was done
        if (cmds.undoInfo(query=True, undoName=True) or "").strip() == "groomLab paste graph":
            cmds.undo()
        raise ValueError("cannot paste the graph: %s" % _clip(str(e)))
    missing = missing_inputs(groom, graph)
    about = data.get("about") if isinstance(data.get("about"), dict) else {}
    title = about.get("title") if isinstance(about.get("title"), str) and about["title"].strip() else "the graph"
    result = {"title": title.strip()[:80], "nodes": len(graph.get("nodes", [])),
              "fit": [recorded, size] if (recorded and size and abs(size / recorded - 1.0) >= 1e-6) else None,
              "controls": controls["set"], "controlsSkipped": controls["skipped"], "removed": removed,
              "missing": missing}
    result["message"] = _paste_message(result)
    return result


def _names(items, limit=4):
    items = [_clip(str(i), 60) for i in items]
    more = " and %d more" % (len(items) - limit) if len(items) > limit else ""
    return ", ".join(items[:limit]) + more


def _paste_message(r):
    """One line for the Graph Editor's status bar."""
    text = "pasted %s: %d node%s" % (r["title"], r["nodes"], "" if r["nodes"] == 1 else "s")
    if r["fit"]:
        text += ", sizes fitted to this scalp (x%.3g)" % (r["fit"][1] / r["fit"][0])
    if r["controls"]:
        text += ", %d control value%s" % (len(r["controls"]), "" if len(r["controls"]) == 1 else "s")
    m = r["missing"]
    lacking = []
    if m["maps"]:
        lacking.append("painted maps " + _names(sorted(m["maps"])))
    if m["colorSets"]:
        lacking.append("color sets " + _names(m["colorSets"]))
    if m["files"]:
        lacking.append("files " + _names(sorted(set(os.path.basename(p.replace("\\", "/")) or p
                                                    for p in m["files"].values()))))
    if m["meshes"]:
        lacking.append("meshes " + _names(sorted(set(m["meshes"].values()))))
    for key, words in (("colliders", "colliders"), ("groups", "guide groups"), ("parts", "parts")):
        if m[key]:
            lacking.append("%s %s" % (words, _names(m[key])))
    if lacking:
        text += "; missing here (those nodes use their defaults): " + "; ".join(lacking)
    if r["removed"]:
        text += "; network paths left out: " + _names(sorted(r["removed"]))
    if r["controlsSkipped"]:
        text += "; controls not set: " + _names(sorted(r["controlsSkipped"]))
    return text
