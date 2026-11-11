# SPDX-License-Identifier: Apache-2.0
"""Braids (paths drawn from the scalp) and instancing (geometry on the strands)."""

import json

from maya import cmds

from groomlab.base import undoable
from groomlab.guides import add_guide_group, guides_from_curves, list_guide_groups


def _exclude_from_hair_guides(model, group):
    """Braid paths and feather shafts are not hair: Guides nodes reading all guides leave `group`
    out (every host's: graph_inserts.h excludeFromHairGuides). Returns True when the model changed."""
    from groomlab import graph as gm
    edit = model._edit()
    with gm._core_errors():
        changed = edit.exclude_from_hair_guides(group)
    if changed:
        model._take(edit)
    return changed


def _ensure_guide_group(groom, group):
    if group not in [g["name"] for g in list_guide_groups(groom)]:
        add_guide_group(groom, group)


def braid_paths_from_curves(groom, braid, curves, model=None):
    """Add curves (drawn from the scalp along the braids) as paths of the Braid node `braid`.

    They go into the guide group of the Guides node feeding its "braids"
    input; without one, a Guides node for group "braids" is made and connected.
    Returns the group. With `model` (a GraphModel being edited, e.g. the
    Graph Editor's), the graph change goes there and is not saved.
    """
    from groomlab import graph as gm
    own = model is None
    model = model or gm.GraphModel.from_groom(groom)
    if braid not in model.order or model.node_type(braid) != "Braid":
        raise ValueError("%s is not a Braid node" % braid)
    paths = model.source_of(braid, "braids")
    changed = False
    if paths is None:
        paths = model.add_node("Guides", name=model.unique_name("braidPaths"), params={"group": "braids"})
        model.connect(paths, braid, "braids")
        x, y = model.position(braid)
        model.set_position(paths, x, y + gm.LAYOUT_DY)
        changed = True
    group = model.param(paths, "group") or "primary"
    if group != "primary":
        changed = _exclude_from_hair_guides(model, group) or changed
    if changed and own:
        model.save(groom)
    guides_from_curves(groom, curves, group=group)
    return group


def set_instance_prototype(groom, node, mesh, model=None):
    """Instance `mesh` (a transform or mesh shape) with the Instance node `node` (in `model` when
    given, unsaved)."""
    from groomlab import graph as gm
    own = model is None
    model = model or gm.GraphModel.from_groom(groom)
    if node not in model.order or model.node_type(node) not in ("Instance", "Scales"):
        raise ValueError("%s is not an Instance or Scales node" % node)
    shapes = [mesh] if cmds.nodeType(mesh) == "mesh" else cmds.listRelatives(mesh, shapes=True, type="mesh") or []
    if not shapes:
        raise ValueError("%s is not a mesh" % mesh)
    name = cmds.listRelatives(shapes[0], parent=True)[0] if cmds.nodeType(mesh) == "mesh" else mesh
    name = name.split("|")[-1]
    model.set_param(node, "prototype", name)
    if own:
        model.save(groom)
    return name


@undoable("braid")
def add_braid(groom, curves=None, style="three", group="braids", width=0.25, tightness=1.0, region=0.25,
              dutch=False, after=None, feed=None, feed_until=None):
    """Braid strands along paths; returns the Braid node's name.

    `curves` (drawn from the scalp along the braid) become the guide group
    `group` (default "braids"); without curves the group's guides are used
    (draw them with the Draw tool and group "braids"). style: "three",
    "four", "five", "fishtail", "rope" or "cornrow"; dutch raises the braid;
    tightness sets how close the crossings are (1 natural). Strands rooted
    within `region` of a path's root join its braid (large, e.g. 0.45: a
    ponytail braid the hair sweeps over the scalp into). With `feed` (the
    default for cornrows) strands within `region` of anywhere along the path
    join where they grow, as in cornrows and French braids, but only along
    the first `feed_until` (0-1) of the path: past it the braid hangs free.
    The Braid node goes after `after` (default: the Interpolate), so noise
    and frizz later add flyaways. It writes the `braidId` and `braidFlat`
    strand channels the Cards node builds braid cards from.
    """
    from groomlab import graph as gm
    params = {"style": style, "width": float(width), "tightness": float(tightness), "region": float(region),
              "dutch": bool(dutch)}
    if feed is not None:
        params["feed"] = bool(feed)  # (left out: on for a cornrow)
    if feed_until is not None:
        params["feedUntil"] = float(feed_until)
    model = gm.GraphModel.from_groom(groom)
    # Every host's (graph_inserts.h insertBraid): where it goes, its paths' Guides node.
    edit = model._edit()
    with gm._core_errors():
        braid = edit.insert_braid(group, json.dumps(params), after or "", model.groom_size)
    model._take(edit)
    # The guides before the graph reads them (a Guides node of a group there is not fails).
    if curves:
        guides_from_curves(groom, curves, group=group)
    else:
        _ensure_guide_group(groom, group)  # draw the paths in it with the Draw tool
    model.save(groom)
    return braid


# ---------------------------------------------------------------------------
# Instancing: geometry on the strands (snow, dandruff, dew, beads)


@undoable("instances")
def add_instances(groom, prototype, probability=0.1, count=1, t_range=(0.6, 1.0), scale=0.02, orient="strand",
                  scale_random=0.3, spin_random=1.0, offset=0.0, seed=None, before="output",
                  scale_by_width=True, pin_to_rest=False, size_mode="fixed", fit_scale=1.0, aspect=0.1,
                  deform="none", deform_axis="y"):
    """Instance the mesh `prototype` on the strands; returns the Instance node's name.

    Each strand gets up to `count` instances, each kept with `probability`
    (a number or any map: paint where the snow falls), between t_range along
    the strand (0 root, 1 tip). orient: "strand", "up", "root" or "random"
    (what the prototype's +Y, its long axis, follows).
    scale_by_width: sizes follow the strand's width where they sit (over its
    root width), so they shrink toward thin tips. pin_to_rest: each keeps its
    distance from the root as measured at rest, so strands that stretch
    (simulated guides) don't slide it.
    size_mode "fixed": `scale` is the size in scene units. "fitStrand": each
    spans its strand (leaves, feathers): `fit_scale` of the strand's rest
    length along the prototype's +Y (1: the whole strand; model it 1 unit long
    up +Y from its base at the origin), `aspect` of that across (X and Z,
    times the width factor with scale_by_width); from the rest length, so the
    size holds while strands stretch.
    deform "alongStrand": each one bent along its strand (leaves on vines, a
    feather mesh following its shaft, ribbons): the prototype's `deform_axis`
    ("x", "y" or "z") laid onto the strand from where it sits, facing the
    feather's vane or the skin; rendered as a mesh of its own (as scales are),
    not by the instancer.
    The node goes just before `before` (the output), so the instances follow
    every shaping node; hide the prototype mesh. Arnold renders them through
    an instancer (motion blurred with the hair); USD exports write a
    PointInstancer. `seed` None: one no other Instance node has (so a second
    one picks other strands).
    """
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    params = {"prototype": prototype, "probability": probability, "count": count,
              "tMin": float(t_range[0]), "tMax": float(t_range[1]), "scale": scale,
              "orient": orient, "scaleRandom": scale_random, "spinRandom": spin_random, "offset": offset,
              "scaleByWidth": bool(scale_by_width), "pinToRest": bool(pin_to_rest)}
    if size_mode != "fixed":
        params.update({"sizeMode": size_mode, "fitScale": fit_scale, "aspect": aspect})
    if deform != "none":
        params.update({"deform": deform, "deformAxis": deform_axis})
    if seed is not None:
        params["seed"] = seed
    name = model.add_node("Instance", name=model.unique_name("instance"), params=params)
    model.insert_before(before, name)
    model.save(groom)
    return name
