# SPDX-License-Identifier: Apache-2.0
"""Guides: made from curves or an Alembic file, their hierarchy and groups, selection, the hierarchy
tools, the sculpting display, resampling and per-guide attributes."""

import json

from maya import cmds

from groomlab.base import ensure_plugin, undo_chunk, _shape_of
from groomlab.params import get_graph, set_graph
from groomlab.paths import portable_path


def add_guides(groom, curves):
    """Connect curves (transforms or nurbsCurve shapes) as guides of a groom."""
    for curve in curves:
        cmds.connectAttr(_shape_of(curve, "nurbsCurve") + ".worldSpace[0]", groom + ".guideCurves",
                         nextAvailable=True)


# ---------------------------------------------------------------------------
# Guide hierarchy (stored on the groom, edited through groomLabGuides; every
# edit is undoable)


def guides_from_curves(groom, curves, group="primary", tube_radius=0.1, tube_tip_radius=None, disconnect=True):
    """Add curves as primary guides of a root group; returns the new guide IDs.

    The curves are copied, so they can be deleted afterwards. With
    `disconnect`, curves also connected to the groom's guideCurves are
    disconnected so they are not used twice.
    """
    shape_names = [_shape_of(c, "nurbsCurve") for c in curves]
    kwargs = {"fromCurves": shape_names, "group": group, "tubeRadius": tube_radius}
    if tube_tip_radius is not None:
        kwargs["tubeTipRadius"] = tube_tip_radius
    ids = cmds.groomLabGuides(groom, **kwargs)

    if disconnect:
        for shape in shape_names:
            for dst in cmds.listConnections(shape + ".worldSpace", plugs=True, source=False) or []:
                if dst.split(".")[0] == groom.split("|")[-1] and ".guideCurves" in dst:
                    cmds.disconnectAttr(shape + ".worldSpace[0]", dst)
    return ids


def generate_children(groom, parent_group=None, name=None, count=5, root_spread=0.8, tip_spread=1.0, twist=0.0,
                      jitter=0.1, tube_scale=0.35, seed=0, guides=None, selected=False):
    """Give guides `count` children each, in the child group `name` (default "<parent group>_children").

    Parents: `guides` (IDs), or the selected guides with `selected=True`, else
    every guide of `parent_group`. The child group is created if missing and
    added to otherwise; a parent that already has children there gets new
    ones. So you choose, parent by parent, which guides have children, at any
    level (children of children too). Spreads and jitter are in units of the
    parent tube radius; twist is in radians. Returns the size of the child group.
    """
    kwargs = {"generateChildren": parent_group or "", "count": count, "rootSpread": root_spread,
              "tipSpread": tip_spread, "twist": twist, "jitter": jitter, "tubeScale": tube_scale, "seed": seed}
    if name:
        kwargs["name"] = name
    if guides:
        kwargs["guide"] = [str(g) for g in guides]
    elif selected:
        kwargs["onlySelected"] = True
    elif not parent_group:
        raise ValueError("give a parent group, guides, or selected=True")
    return cmds.groomLabGuides(groom, **kwargs)


def list_guide_groups(groom):
    """Return the groom's guide groups as dicts: name, parent, depth, count, and color: the color it
    draws in without one of the groom's own (guide_group_colors): its own (a ToGuides node's),
    else its automatic one, "#rrggbb"."""
    groups = []
    for row in cmds.groomLabGuides(groom, listGroups=True) or []:
        name, parent, depth, count, color = row.split("|")
        groups.append({"name": name, "parent": parent or None, "depth": int(depth), "count": int(count),
                       "color": color})
    return groups


def guide_ids(groom, group):
    """Return the IDs (as strings) of the guides in a group."""
    return cmds.groomLabGuides(groom, listGuides=group) or []


def lock_guides(groom, ids):
    """Detach child guides from their parents; they keep their current shape."""
    cmds.groomLabGuides(groom, lock=True, guide=list(ids))


def unlock_guides(groom, ids):
    """Re-attach locked child guides to their parents without moving them."""
    cmds.groomLabGuides(groom, unlock=True, guide=list(ids))


def remove_guide_group(groom, group):
    """Remove a guide group with its sub-groups and guides."""
    cmds.groomLabGuides(groom, removeGroup=group)


def _group_list(text):
    return [g for g in (text or "").replace(",", " ").split() if g]


def group_users(groom, group):
    """[(node, param)] of the graph params naming a guide group: Guides nodes
    reading it (group) or leaving it out (exclude)."""
    users = []
    for node in get_graph(groom, expand_default=True).get("nodes", []):
        params = node.get("params", {})
        if node.get("type") != "Guides":
            continue
        if params.get("group") == group:
            users.append((node["name"], "group"))
        if group in _group_list(params.get("exclude")):
            users.append((node["name"], "exclude"))
    return users


def add_guide_group(groom, name, parent=None):
    """A new, empty guide group: top-level, or with `parent` a child group of it
    (guides placed in it attach to the nearest guide of the parent group)."""
    kwargs = {"under": parent} if parent else {}
    cmds.groomLabGuides(groom, addGroup=name, **kwargs)
    return name


def rename_guide_group(groom, old, new):
    """Rename a guide group, and the graph params that name it (one undo step)."""
    with undo_chunk("rename guide group"):
        cmds.groomLabGuides(groom, renameGroup=(old, new))
        graph = get_graph(groom)
        touched = False
        for node in graph.get("nodes", []):
            params = node.get("params", {})
            if node.get("type") != "Guides":
                continue
            if params.get("group") == old:
                params["group"] = new
                touched = True
            excluded = _group_list(params.get("exclude"))
            if old in excluded:
                params["exclude"] = " ".join(new if g == old else g for g in excluded)
                touched = True
        if touched:
            set_graph(groom, graph)
        colors = guide_group_colors(groom)
        if old in colors:  # and keeps its color
            set_guide_group_color(groom, old, None)
            set_guide_group_color(groom, new, colors[old])
        hidden = hidden_guide_groups(groom)
        if old in hidden:  # a hidden group stays hidden
            cmds.setAttr(groom + ".hiddenGuideGroups", " ".join(new if g == old else g for g in hidden), type="string")
    return new


def guide_group_colors(groom):
    """{group: "#rrggbb"} of the guide groups given their own viewport color."""
    colors = {}
    for word in (cmds.getAttr(groom + ".guideGroupColors") or "").split():
        name, _, color = word.rpartition(":")
        if name and len(color) == 7 and color.startswith("#"):
            colors[name] = color
    return colors


def set_guide_group_color(groom, group, color):
    """Give a guide group its own viewport color: "#rrggbb", or (r, g, b) in 0-1; None goes back to
    its automatic color."""
    if isinstance(color, (tuple, list)):
        color = "#%02x%02x%02x" % tuple(int(round(max(0.0, min(1.0, float(c))) * 255)) for c in color)
    colors = guide_group_colors(groom)
    colors.pop(group, None)
    if color:
        colors[group] = color.lower()
    cmds.setAttr(groom + ".guideGroupColors", " ".join("%s:%s" % kv for kv in sorted(colors.items())),
                 type="string")


def hidden_guide_groups(groom):
    """Names of the guide groups hidden in the viewport."""
    return _group_list(cmds.getAttr(groom + ".hiddenGuideGroups"))


def show_guide_group(groom, group, visible=True):
    """Show or hide a guide group in the viewport. Hidden groups aren't drawn,
    brushed or picked by the select tool; the strands they drive stay."""
    hidden = [g for g in hidden_guide_groups(groom) if g != group]
    if not visible:
        hidden.append(group)
    cmds.setAttr(groom + ".hiddenGuideGroups", " ".join(hidden), type="string")


def clear_guides(groom):
    """Remove every guide group and guide."""
    cmds.groomLabGuides(groom, clear=True)


# ---------------------------------------------------------------------------
# Guide selection (GT-5; stored on the groom, every change undoable)

SELECT_CONTEXT = "groomLabSelectCtx1"


def _targets(guides=None, group=None, level=None):
    kwargs = {}
    if guides:
        kwargs["guide"] = [str(g) for g in guides]
    elif group:
        kwargs["group"] = group
    elif level is not None:
        kwargs["level"] = level
    return kwargs


def select_guides(groom, guides=None, group=None, level=None, mode="replace"):
    """Select guides by ID, group or level. mode: replace, add, deselect or toggle. Returns the selection."""
    if mode not in ("replace", "add", "deselect", "toggle"):
        raise ValueError("mode must be replace, add, deselect or toggle")
    kwargs = _targets(guides, group, level)
    if not kwargs:
        raise ValueError("give guides, a group or a level")
    if mode != "replace":
        kwargs["append" if mode == "add" else mode] = True
    return cmds.groomLabGuides(groom, select=True, **kwargs) or []


def selected_guides(groom):
    return cmds.groomLabGuides(groom, listSelected=True) or []


def clear_guide_selection(groom):
    cmds.groomLabGuides(groom, clearSelection=True)


def mirror_guide_selection(groom, axis="x", center=0.0, add=False):
    """Select the mirror images of the selected guides (nearest guide in the same group)."""
    kwargs = {"append": True} if add else {}
    return cmds.groomLabGuides(groom, mirrorSelection=axis, center=center, **kwargs) or []


def set_soft_selection(groom, radius):
    """Soft-select guides within `radius` (scene units) of the selection; 0 turns it off."""
    cmds.setAttr(groom + ".softSelectRadius", radius)


def select_tool(level=0, group="", roots_only=False, solo=True, symmetry=None, activate=True):
    """The lasso tool for guides of the selected groom (Shift: toggle, Ctrl: deselect, Ctrl+Shift: add).
    With `solo` every pick also freezes the guides not selected, in the same undo step, so the
    brushes work on the selection; a pick of nothing unfreezes every guide. `symmetry` ("x" or
    "topology", as the brushes take it) picks each guide's mirror guide too."""
    ensure_plugin()
    if not cmds.contextInfo(SELECT_CONTEXT, exists=True):
        cmds.groomLabSelectCtx(SELECT_CONTEXT)
    cmds.groomLabSelectCtx(SELECT_CONTEXT, edit=True, level=level, group=group, rootsOnly=roots_only,
                           soloSelection=bool(solo), symmetry=symmetry or "none")
    if activate:
        cmds.setToolTo(SELECT_CONTEXT)
    return SELECT_CONTEXT


# ---------------------------------------------------------------------------
# Hierarchy tools (shapes kept; every edit undoable)


def reparent_guides(groom, parent, guides=None):
    """Make guides (default: the selected ones) follow another parent guide in their parent group."""
    return cmds.groomLabGuides(groom, reparent=str(parent), **_targets(guides))


def promote_group(groom, group):
    """Move a group one level up the hierarchy (a group reaching the top becomes primaries)."""
    cmds.groomLabGuides(groom, promote=group)


def demote_group(groom, group, under):
    """Move a group one level down, under a sibling group; each guide follows the nearest guide there."""
    cmds.groomLabGuides(groom, demote=group, under=under)


def mirror_group(groom, group="primary", axis="x", center=0.0, from_negative=False):
    """Copy a root group's guides from one side of a plane to the other, replacing what is there."""
    kwargs = {"fromNegative": True} if from_negative else {}
    return cmds.groomLabGuides(groom, mirror=group, axis=axis, center=center, **kwargs) or []


def symmetry_sync(groom, axis="x", center=0.0, from_negative=False):
    """Make the whole groom symmetric: every guide on one side, children included, copied to the other.

    Hand-edited and locked children and painted attributes come across too. One undo step.
    Returns the new guide IDs.
    """
    return mirror_group(groom, "*", axis, center, from_negative)


# ---------------------------------------------------------------------------
# The sculpting display


def set_sculpt_display(groom, mode="normal", density=1.0):
    """While a stroke is live: "normal", "dimStrands" or "guidesOnly", at `density` x the preview
    density (1.0: every strand of the preview stays up, nothing pops when the stroke ends)."""
    modes = ("normal", "dimStrands", "guidesOnly")
    cmds.setAttr(groom + ".sculptDisplay", modes.index(mode))
    cmds.setAttr(groom + ".sculptDensity", density)


def isolate_group(groom, group=None):
    """Show one guide group (with its child groups) at full color and dim the rest; None shows all."""
    cmds.setAttr(groom + ".isolateGroup", group or "", type="string")


def color_guides_by_map(groom, name=None):
    """Color guides by a painted map (black 0 to white 1); None goes back to each group's color."""
    if name:
        cmds.setAttr(groom + ".guideColorLayer", name, type="string")
    cmds.setAttr(groom + ".guideColorMode", 1 if name else 0)


# ---------------------------------------------------------------------------
# Resampling, per-guide attributes, the stored hierarchy, guides from Alembic


def resample_guides(groom, segments, guides=None, group=None, level=None):
    """Give guides (default: the selected ones, else every guide) `segments` segments evenly spaced
    along the smooth curve through their points, keeping their shape. One undo step; returns the
    number of guides resampled."""
    return cmds.groomLabGuides(groom, resample=segments, **_targets(guides, group, level))


def guide_segments(groom, guides=None, group=None, level=None):
    """The segment counts (points - 1) of guides (default: the selected ones, else every guide)."""
    return list(cmds.groomLabGuides(groom, pointCounts=True, **_targets(guides, group, level)) or [])


def preview_resample_guides(groom, segments, guides=None, group=None, level=None):
    """Show guides (as resample_guides picks them) resampled to `segments`, live, without changing
    the groom or making an undo step: always from the stored guides, so previews never compound.
    resample_guides commits; end_guide_preview drops the preview. Returns the guides previewed."""
    return cmds.groomLabGuides(groom, previewResample=segments, **_targets(guides, group, level))


def end_guide_preview(groom):
    """Drop a preview_resample_guides preview: the groom shows its stored guides again."""
    cmds.groomLabGuides(groom, endPreview=True)


def set_guide_attribute(groom, name, value, guides=None, group=None, level=None):
    """Set a per-guide attribute (children inherit it; Interpolate blends it into a strand channel)."""
    return cmds.groomLabGuides(groom, setAttribute=name, value=value, **_targets(guides, group, level))


TUBE_SHAPES = ("round", "ribbon", "teardrop", "blade")


def set_tube(groom, guides=None, group=None, level=None, width=None, aspect=None, twist=None, shape=None, samples=None):
    """Edit guides' tubes (tube guides): their cross-section along the guide and its shape.

    Targets: `guides` (IDs), else `group`, else `level`, else the selected guides. Each of `width`
    (half the section's width in scene units, as shown: a round tube's radius), `aspect` (height
    over width: 1 as drawn, toward 0 flat) and `twist` (radians, the section turned at that point)
    is a number (the same all along) or a list evenly spaced root to tip; None leaves it as it is.
    `shape`: one of TUBE_SHAPES, or a shape of the groom's library (tube_shapes). `samples` resamples all three lists to that many. Children follow
    their parent's tube; Interpolate's fillTubes fills tubes with strands. One undo step. Returns
    how many guides changed."""
    edit = {}
    for key, value in (("width", width), ("aspect", aspect), ("twist", twist)):
        if value is None:
            continue
        edit[key] = [float(v) for v in value] if isinstance(value, (list, tuple)) else float(value)
    if shape is not None:
        if shape not in TUBE_SHAPES and shape not in _library_names(groom):
            raise ValueError("shape must be one of %s or a library shape (%s)" %
                             (", ".join(TUBE_SHAPES), ", ".join(_library_names(groom)) or "the library is empty"))
        edit["shape"] = shape
    if samples is not None:
        edit["samples"] = int(samples)
    if not edit:
        raise ValueError("give a width, aspect, twist, shape or samples")
    return cmds.groomLabGuides(groom, setTube=json.dumps(edit), **_targets(guides, group, level))


def _library(groom):
    return json.loads(cmds.groomLabGuides(groom, tubeShapes=True) or "[]")


def _library_names(groom):
    return [s["name"] for s in _library(groom)]


def _points(points):
    pts = [[float(u), float(v)] for u, v in points]
    if len(pts) < 3:
        raise ValueError("a tube shape needs at least 3 points")
    return pts


def tube_shapes(groom):
    """The tube shapes guides can take: the built-ins (round, ribbon, teardrop, blade) and the groom's
    library of custom shapes, saved with its guides. Each a dict: name, builtin (bool), points
    ((u, v) control points in the -1..1 square, counter-clockwise; a built-in's draw it closely),
    smooth (a smooth curve through them, else straight edges) and users (how many guides take it;
    None for a built-in)."""
    import groomlab_core
    out = []
    for name in TUBE_SHAPES:
        points, smooth = groomlab_core.builtin_tube_shape_points(name)
        out.append({"name": name, "builtin": True, "points": [tuple(p) for p in points], "smooth": smooth, "users": None})
    for s in _library(groom):
        out.append({"name": s["name"], "builtin": False, "points": [tuple(p) for p in s["points"]],
                    "smooth": bool(s["smooth"]), "users": int(s["users"])})
    return out


def add_tube_shape(groom, name, points, smooth=True):
    """Add a custom shape to the groom's tube shape library: `points` the outline's control points
    (u, v) in order round it (at least 3; fitted into the -1..1 square), a smooth closed curve
    through them, or straight edges with smooth=False. RuntimeError saying what to fix when the
    name is taken or not allowed, or the outline crosses itself or leaves out its center. Guides
    take it by name: set_tube(shape=name). One undo step."""
    return cmds.groomLabGuides(groom, editTubeShape=json.dumps(
        {"op": "add", "name": name, "points": _points(points), "smooth": bool(smooth)}))


def set_tube_shape_points(groom, name, points=None, smooth=None):
    """Give a library shape new control points (and/or smooth): every guide taking it takes the new
    outline, its strands, children and wires with it. One undo step. Returns how many guides take it."""
    op = {"op": "set", "name": name}
    if points is not None:
        op["points"] = _points(points)
    if smooth is not None:
        op["smooth"] = bool(smooth)
    return cmds.groomLabGuides(groom, editTubeShape=json.dumps(op))


def rename_tube_shape(groom, old, new):
    """Rename a library shape; the guides taking it keep it. One undo step."""
    return cmds.groomLabGuides(groom, editTubeShape=json.dumps({"op": "rename", "name": old, "to": new}))


def remove_tube_shape(groom, name):
    """Remove a library shape: the guides taking it become round. One undo step. Returns how many did."""
    return cmds.groomLabGuides(groom, editTubeShape=json.dumps({"op": "remove", "name": name}))


def preview_tube_shape(groom, name, points, smooth=True):
    """Show a library shape with new points on the groom, live, without changing it or making an undo
    step (the shape editor's drag): set_tube_shape_points commits, end_guide_preview drops it."""
    cmds.groomLabGuides(groom, previewTubeShape=json.dumps({"name": name, "points": _points(points),
                                                             "smooth": bool(smooth)}))


def tube_targets(groom):
    """The guides the Groom Tools' Tubes panel edits: {"source": "selection" | "solo" | "none",
    "guides": [IDs], "tube": the first one's tube (get_tube's dict) or None, "shapes": the shapes
    among them}. The selected guides; with none selected, the soloed ones (those left unfrozen
    while the rest are frozen)."""
    return json.loads(cmds.groomLabGuides(groom, tubeTargets=True))


def get_tube(groom, guide):
    """A guide's tube as a dict: shape, width (scene units, as shown), aspect and twist (radians),
    each list evenly spaced root to tip."""
    return json.loads(cmds.groomLabGuides(groom, tube=str(guide)))


def scale_tube(groom, factor, guides=None, group=None, level=None):
    """Scale guides' tubes as a whole: every width times `factor` (their taper, and what the Shape
    brush painted, kept). Targets as in set_tube (else the selected guides). One undo step.
    Returns how many guides changed."""
    if not factor >= 0.0:
        raise ValueError("factor: 0 or more")
    return cmds.groomLabGuides(groom, setTube=json.dumps({"scaleWidth": float(factor)}), **_targets(guides, group, level))


# smooth_tube's channels: the Shape brush's names, and what the tube calls each list.
_TUBE_LISTS = {"width": "width", "flatness": "aspect", "twist": "twist", "all": "all"}


def smooth_tube(groom, guides=None, channel=None, passes=3, group=None, level=None):
    """Smooth guides' tubes along each guide (what the Shape brush painted, evened out): each
    section moved toward its neighbours' mean, one value per guide point, `passes` times, the
    root's and the tip's kept. `channel`: "width", "flatness", "twist", or "all" (None: all).
    Targets as in set_tube (else the selected guides). One undo step. Returns how many guides
    changed."""
    name = "all" if channel is None else channel
    if name not in _TUBE_LISTS:
        raise ValueError("channel must be one of width, flatness, twist or all")
    edit = {"smooth": _TUBE_LISTS[name], "passes": max(1, min(int(passes), 100))}
    return cmds.groomLabGuides(groom, setTube=json.dumps(edit), **_targets(guides, group, level))


def _interpolate_nodes(graph):
    return [n for n in graph.get("nodes", []) if n.get("type") == "Interpolate"]


def fill_tubes(groom):
    """The groom's Fill Tubes as its graph's Interpolate nodes have it: {"fillTubes": bool,
    "tubeBlend": share} (the first Interpolate's), or None when the graph has no Interpolate."""
    from groomlab.params import get_graph
    nodes = _interpolate_nodes(get_graph(groom, expand_default=True))
    if not nodes:
        return None
    params = nodes[0].get("params", {})
    return {"fillTubes": bool(params.get("fillTubes", False)), "tubeBlend": float(params.get("tubeBlend", 0.25))}


def set_fill_tubes(groom, on=None, blend=None):
    """Turn Fill Tubes on or off (`on`) and set its soft edge (`blend`, a share of each tube's
    half-width) on every Interpolate node of the groom's graph: strands rooted in a guide's tube
    ride it to the tip (the tubes of every guide level fill, the deepest winning). Adds nothing
    when the graph has no Interpolate. One undo step. Returns how many nodes changed."""
    from groomlab.params import get_graph, set_graph
    graph = get_graph(groom, expand_default=True)
    changed = 0
    for node in _interpolate_nodes(graph):
        params = node.setdefault("params", {})
        before = dict(params)
        if on is not None:
            params["fillTubes"] = bool(on)
        if blend is not None:
            params["tubeBlend"] = min(max(float(blend), 0.0), 1.0)
        changed += params != before
    if changed:
        set_graph(groom, graph)
    return changed


def get_hierarchy(groom):
    """Return the stored guide hierarchy as a dict (empty dict when none)."""
    text = cmds.getAttr(groom + ".guideData")
    return json.loads(text) if text else {}


def import_guides_from_alembic(groom, path, object_name=None, samples=24):
    """Take the groom's guides from an Alembic file instead of connected curves.

    Replaces the graph's "guides" node with an ImportCurves node reading
    `path` (all curves, or those under `object_name`) and roots them on the
    groom's scalp. Animated files are read at the scene time.
    """
    graph = get_graph(groom, expand_default=True)
    params = {"path": portable_path(path), "samples": samples}
    if object_name:
        params["object"] = object_name

    nodes = [n for n in graph["nodes"] if n["name"] != "guides"]
    nodes.append({"name": "guides", "type": "ImportCurves", "params": params})
    graph["nodes"] = nodes

    connections = [c for c in graph.get("connections", []) if c["to"] != "guides"]
    if any(n["name"] == "scalp" for n in nodes):
        connections.append({"from": "scalp", "to": "guides", "input": "scalp"})
    graph["connections"] = connections
    set_graph(groom, graph)
