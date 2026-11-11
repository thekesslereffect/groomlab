# SPDX-License-Identifier: Apache-2.0
"""Simulation: the core's Simulate node, or a groom's hierarchy guides posed with simulated curves.

The core's simulation (every host's: Maya, the standalone app, groom files, the Arnold
procedural) is a Simulate node in the graph between the guides and Interpolate (simulation.h):
gravity, wind, the head's motion and collisions, stepped as the timeline plays and kept frame by
frame; bake it for renders and farms:
    node = add_simulate(groom, bend=0.6)
    simulate(groom, end=120)                              # Simulate Range: the frames into the cache
    bake_simulation(groom)                                # an Alembic guide cache per node (simCaches)
    invalidate_simulation(groom)                          # drop the frames (after edits it is automatic)

nHair is still here, untouched: poses replace guides by ID for evaluation only; their children
re-follow them, and the authored guides (guideData) never change. Two routes:

In Maya (nHair or any curve deformer):
    sim = create_sim_curves(groom, "primary", preset="long", collide=True, preroll=20)
    ...play the scene...
    bake_sim(groom, "shot.guides.abc", sim)              # cache it; the groom reads the cache
    remove_sim(groom, sim)                               # or back to the authored groom

Another package (Houdini, ...):
    export_guides(groom, "guides.abc", "primary", 1, 100)  # strandId = guide ID
    ...simulate, keeping strandId...
    set_guide_cache(groom, "sim.abc")

groomlab.export_groom writes the poses into the groom file's guide cache, so
groomlab-cli and the Arnold procedural render the simulated groom too.
"""

import json
import os

from maya import cmds, mel

import groomlab


# ---- The core's simulation (the Simulate node) -------------------------------------------------


@groomlab.undoable("add simulate")
def add_simulate(groom, name=None, preset=None, **params):
    """Add a Simulate node between the guides and every Interpolate they feed: the strands follow
    the simulated guides. `preset` ("fur", "default": simulate_presets) sets its params first;
    `params` are the node's (stretchStiffness, bend, twist, damping,
    drag, mass, strength, gravity, gravityDirection, groomedUnderGravity, windSpeed, windDirection,
    windGust, windFrequency, rootStiffness, rootStiffnessProfile, inertia, collideScalp, collisionOffset, friction, repulsion, repulsionRadius,
    substeps, iterations, startFrame, seed, cacheMode; water, waterLevel, buoyancy, waterDrag,
    currentSpeed, currentDirection, dryTime, wetWeight, rain). Bypass or delete it and the groom is
    exactly as it was. Returns the node's name."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    values = simulate_preset_params(preset) if preset else {}
    values.update({k: v for k, v in params.items() if v is not None})
    # Every host's (graph_inserts.h insertSimulate): where it goes and what it feeds.
    edit = model._edit()
    with gm._core_errors():
        node = edit.insert_simulate(json.dumps(values), model.groom_size)
        if name and name != node:
            edit.rename_node(node, name)
            node = name
    model._take(edit)
    model.save(groom)
    return node


@groomlab.undoable("load guide cache")
def add_guide_cache(groom, path, object_name=""):
    """Load Guide Cache (every host's): a GuideCache node reading `path` (Alembic curves simulated
    elsewhere, each with its guide's ID as strandId: export_guides writes the guides to simulate)
    right after the Guides node, feeding all it fed, so the guides take the cache's poses and the
    groom stays. A GuideCache already there reads the new file. Returns the node's name. (The
    groom's guideCache attribute, set_guide_cache, still poses older scenes.)"""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    # Every host's (graph_inserts.h insertGuideCache).
    edit = model._edit()
    with gm._core_errors():
        node = edit.insert_guide_cache(groomlab.portable_path(path), object_name or "", model.groom_size)
    model._take(edit)
    model.save(groom)
    return node


def simulate_presets():
    """The Simulate node's one-click settings, every host's (the core's): [{name, title,
    description}]: "default" (the node's defaults) and "fur" (springy fur that holds its groom)."""
    import groomlab_core
    return groomlab_core.simulate_presets()


def simulate_preset_params(preset):
    """The params a Simulate preset sets ({param: value}, a ramp as its keys)."""
    import groomlab_core
    return json.loads(groomlab_core.simulate_preset_params(preset))


def simulate_preset(groom, preset="fur", node=None, model=None):
    """Set Simulate node `node` (else every Simulate node of the graph) to a preset: "fur" or
    "default" (simulate_presets). Each preset sets the same params (bend, twist, damping, drag,
    rootStiffness, rootStiffnessProfile, inertia, iterations), so switching between them leaves no
    mix; gravity, wind, collisions, water and maps stay as they are. With `model` (a GraphModel
    being edited, e.g. the Graph Editor's) the params go there and nothing is saved. Returns the
    params set."""
    params = simulate_preset_params(preset)
    if model is None:
        return groomlab.undoable("simulate preset")(_simulate_preset)(groom, params, node, None)
    return _simulate_preset(groom, params, node, model)


def _simulate_preset(groom, params, node, model):
    from groomlab import graph as gm
    own = model is None
    if own:
        model = gm.GraphModel.from_groom(groom)
    nodes = model.of_type("Simulate")
    if node is not None and node not in nodes:
        raise ValueError("the graph has no Simulate node %r" % node)
    targets = [node] if node is not None else nodes
    if not targets:
        raise ValueError("%s's graph has no Simulate node (add one with add_simulate)" % groom)
    for target in targets:
        for key, value in params.items():
            model.set_param(target, key, value)
    if own:
        model.save(groom)
    return params


def simulate_nodes(groom):
    """The Simulate nodes making the groom's output, upstream first."""
    return list(cmds.groomLabSimulate(groom, nodes=True) or [])


def simulate(groom, end=None):
    """Simulate Range: every Simulate node stepped on to frame `end` (the playback range's end by
    default) from its last frame, the scalp read at each frame on the way. Scrubbing back reads
    the frames; a node in cacheMode "cached" shows only frames made this way. Returns the frames
    stepped."""
    end = cmds.playbackOptions(query=True, maxTime=True) if end is None else end
    return cmds.groomLabSimulate(groom, toFrame=int(round(end)))


def simulation_status(groom):
    """Each Simulate node: [{node, startFrame, ranges [[first, last]], frames, live, baked,
    bakeStale, bakedFirst, bakedLast, bakedFile, stepMs, bytes, stamp}]."""
    return json.loads(cmds.groomLabSimulate(groom, status=True) or "[]")


def invalidate_simulation(groom, node=""):
    """Drop a Simulate node's frames ('' every node's): the next frame shown simulates again. A
    change of the node's settings, or of what it is fed, does this by itself. Bakes stay."""
    cmds.groomLabSimulate(groom, invalidate=True, node=node)


def simulation_cache_path(groom, node):
    """Where bake_simulation writes a node's frames: the groom's own folder in the scene's caches,
    <scene>/groomlab/<scene name>/caches/<asset>/<asset>_<node>_sim.abc (as its masks and cards)."""
    asset = groomlab.asset_name(groom)
    folder = "%s/%s" % (groomlab.asset_folder("caches"), asset)
    if not os.path.isdir(folder):
        os.makedirs(folder)
    return "%s/%s_%s_sim.abc" % (folder, asset, node)


def baked_simulations(groom):
    """The bakes the groom's Simulate nodes read: {node: {"file", "object", "stamp"}}."""
    return json.loads(cmds.getAttr(groom + ".simCaches") or "{}")


@groomlab.undoable("bake simulation")
def bake_simulation(groom, node=None, start=None, end=None, path=None):
    """Simulate, bake to a cache, render: every Simulate node (or `node`) simulated to `end` (the
    playback range's end) and its frames from `start` (its start frame) written as an Alembic
    guide cache (simulation_cache_path, or `path` for one node), named in the groom's simCaches
    with the stamp of its settings. From then on the viewport, exports, groom files and Arnold
    renders read the baked frames while the node's settings are those (a node changed since
    simulates again, and says its bake is stale). One undo step (the attribute: the files stay).
    Returns {node: path}."""
    nodes = [node] if node else simulate_nodes(groom)
    if not nodes:
        raise ValueError("%s has no Simulate node to bake" % groom)
    if path and len(nodes) > 1:
        raise ValueError("one path for %d Simulate nodes: name the node" % len(nodes))
    end = int(round(cmds.playbackOptions(query=True, maxTime=True) if end is None else end))
    bakes = baked_simulations(groom)
    for n in nodes:  # the nodes simulate rather than read their old bakes (the files are written again)
        bakes.pop(n, None)
    cmds.setAttr(groom + ".simCaches", json.dumps(bakes) if bakes else "", type="string")
    simulate(groom, end)
    written = {}
    for n in nodes:
        out = path or simulation_cache_path(groom, n)
        kwargs = {"bake": out, "node": n, "endFrame": end}
        if start is not None:
            kwargs["startFrame"] = int(round(start))
        stamp = cmds.groomLabSimulate(groom, **kwargs)
        bakes[n] = {"file": groomlab.portable_path(out), "object": "", "stamp": stamp}
        written[n] = out
    cmds.setAttr(groom + ".simCaches", json.dumps(bakes), type="string")
    return written


@groomlab.undoable("clear baked simulation")
def clear_baked_simulation(groom, node=None):
    """The Simulate nodes (or `node`) simulate again instead of reading their bakes (the files stay)."""
    bakes = baked_simulations(groom) if node else {}
    bakes.pop(node, None)
    cmds.setAttr(groom + ".simCaches", json.dumps(bakes) if bakes else "", type="string")


# ---- nHair (or any curve deformer) posing the guides -------------------------------------------


def _guide_points(groom, guide_id):
    xyz = cmds.groomLabGuides(groom, guidePoints=str(guide_id))
    return [tuple(xyz[i:i + 3]) for i in range(0, len(xyz), 3)]


def connect_sim_curves(groom, curves_by_id):
    """Pose guides with curves: {guide ID: curve (transform or shape)}. Replaces earlier sim curves."""
    disconnect_sim_curves(groom)
    for index, (guide_id, curve) in enumerate(sorted(curves_by_id.items())):
        element = "%s.simGuides[%d]" % (groom, index)
        cmds.connectAttr(groomlab._shape_of(curve, "nurbsCurve") + ".worldSpace[0]", element + ".simCurve", force=True)
        cmds.setAttr(element + ".simGuideId", str(int(guide_id)), type="string")


def disconnect_sim_curves(groom):
    for index in cmds.getAttr(groom + ".simGuides", multiIndices=True) or []:
        element = "%s.simGuides[%d]" % (groom, index)
        for source in cmds.listConnections(element + ".simCurve", plugs=True, source=True, destination=False) or []:
            cmds.disconnectAttr(source, element + ".simCurve")
        cmds.removeMultiInstance(element, b=True)


# nHair settings per kind of hair. "attraction" is the start-curve attraction
# along the length (root to tip): stiff roots, freer tips.
SIM_PRESETS = {
    "fur": {"stretchResistance": 200.0, "compressionResistance": 50.0, "bendResistance": 40.0, "damp": 0.5,
            "drag": 0.05, "startCurveAttract": 0.6, "attractionDamp": 0.4, "mass": 0.3,
            "attraction": [(0.0, 1.0), (1.0, 0.6)]},
    "short": {"stretchResistance": 150.0, "compressionResistance": 30.0, "bendResistance": 10.0, "damp": 0.3,
              "drag": 0.05, "startCurveAttract": 0.25, "attractionDamp": 0.2, "mass": 0.5,
              "attraction": [(0.0, 1.0), (1.0, 0.3)]},
    "long": {"stretchResistance": 100.0, "compressionResistance": 10.0, "bendResistance": 0.5, "damp": 0.1,
             "drag": 0.05, "startCurveAttract": 0.03, "attractionDamp": 0.1, "mass": 1.0,
             "attraction": [(0.0, 1.0), (0.25, 0.3), (1.0, 0.0)]},
    "bouncy": {"stretchResistance": 120.0, "compressionResistance": 20.0, "bendResistance": 4.0, "damp": 0.02,
               "drag": 0.02, "startCurveAttract": 0.12, "attractionDamp": 0.02, "mass": 0.8,
               "attraction": [(0.0, 1.0), (1.0, 0.4)]},
    "wet": {"stretchResistance": 100.0, "compressionResistance": 10.0, "bendResistance": 0.2, "damp": 0.35,
            "drag": 0.02, "startCurveAttract": 0.01, "attractionDamp": 0.3, "mass": 3.0, "friction": 0.8,
            "stickiness": 0.4, "attraction": [(0.0, 1.0), (0.15, 0.2), (1.0, 0.0)]},
}


def apply_sim_preset(hair_system, preset, **overrides):
    """Set a hairSystem's dynamics from SIM_PRESETS (plus overrides, hairSystem attribute names)."""
    if preset not in SIM_PRESETS:
        raise ValueError("no sim preset %r (%s)" % (preset, ", ".join(sorted(SIM_PRESETS))))
    settings = dict(SIM_PRESETS[preset], **overrides)
    ramp = settings.pop("attraction", None)
    for attr, value in settings.items():
        cmds.setAttr("%s.%s" % (hair_system, attr), value)
    if ramp:
        for index in cmds.getAttr(hair_system + ".attractionScale", multiIndices=True) or []:
            cmds.removeMultiInstance("%s.attractionScale[%d]" % (hair_system, index), b=True)
        for index, (position, value) in enumerate(ramp):
            element = "%s.attractionScale[%d]" % (hair_system, index)
            cmds.setAttr(element + ".attractionScale_Position", position)
            cmds.setAttr(element + ".attractionScale_FloatValue", value)
            cmds.setAttr(element + ".attractionScale_Interp", 3)  # smooth


def configure_sim(sim, preset=None, collide=None, self_collide=None, wind=None, preroll=None):
    """Adjust a sim made by create_sim_curves.

    preset: SIM_PRESETS name. collide: the scalp becomes a passive collider
    (nRigid) and the hair collides with it. self_collide: hair against hair.
    wind: (direction xyz, speed, noise) on the nucleus. preroll: frames the
    nucleus starts before the playback start, so the hair settles first.
    """
    hair, nucleus = sim.get("hairSystem"), sim.get("nucleus")
    if not hair:
        raise ValueError("not a dynamic sim (create_sim_curves with dynamic=True)")
    if preset:
        apply_sim_preset(hair, preset)
    if collide is not None:
        cmds.setAttr(hair + ".collide", bool(collide))
        scalp = sim.get("scalp")
        if collide and scalp and not sim.get("collider"):
            before = set(cmds.ls(type="nRigid") or [])
            cmds.select(scalp, replace=True)
            mel.eval("makeCollideNCloth")
            cmds.select(clear=True)
            made = list(set(cmds.ls(type="nRigid") or []) - before)
            sim["collider"] = made[0] if made else None
            if sim["collider"]:
                cmds.setAttr(sim["collider"] + ".thickness", 0.005)
        if sim.get("collider"):
            cmds.setAttr(sim["collider"] + ".collide", bool(collide))
    if self_collide is not None:
        cmds.setAttr(hair + ".selfCollide", bool(self_collide))
    if wind is not None and nucleus:
        direction, speed, noise = wind
        cmds.setAttr(nucleus + ".windDirection", *direction)
        cmds.setAttr(nucleus + ".windSpeed", speed)
        cmds.setAttr(nucleus + ".windNoise", noise)
    if preroll is not None and nucleus:
        cmds.setAttr(nucleus + ".startFrame", cmds.playbackOptions(query=True, minTime=True) - preroll)
    return sim


def create_sim_curves(groom, group="primary", dynamic=True, preset=None, collide=False, self_collide=False,
                      wind=None, preroll=0):
    """Build a curve per guide of `group` (at the current frame) and pose the guides with them.

    With `dynamic`, the curves go through nHair (makeCurvesDynamic, follicles
    attached to the scalp) and the simulated output curves pose the guides;
    `preset`, `collide`, `self_collide`, `wind` and `preroll` then set it up
    (see configure_sim). Returns a dict of the created nodes: "curves"
    ({guide ID: input curve}), "group" (their parent transform) and, when
    dynamic, "outputs" ({guide ID: output curve}), "hairSystem", "nucleus",
    "scalp" and "collider" (the scalp's nRigid, when colliding).
    """
    ids = [int(i) for i in groomlab.guide_ids(groom, group)]
    if not ids:
        raise RuntimeError("%s has no guides in group %r" % (groom, group))
    transform = cmds.listRelatives(groom, parent=True)[0].split(":")[-1]  # namespaces: new nodes go in the root
    parent = cmds.group(empty=True, name="%s_%s_simCurves" % (transform, group))
    curves = {}
    for guide_id in ids:
        points = _guide_points(groom, guide_id)
        curve = cmds.curve(editPoint=points, degree=3 if len(points) >= 4 else 1,
                           name="%s_sim_%d" % (transform, guide_id))
        cmds.addAttr(curve, longName="glGuideId", dataType="string")
        cmds.setAttr(curve + ".glGuideId", str(guide_id), type="string")
        cmds.parent(curve, parent)
        curves[guide_id] = cmds.ls(curve, long=True)[0]
    result = {"curves": curves, "group": parent}

    if not dynamic:
        connect_sim_curves(groom, curves)
        return result

    scalp = groomlab.scalp_mesh(groom)
    scalp = cmds.listRelatives(scalp, parent=True, fullPath=True)[0] if scalp else None  # hair attaches to it
    before = set(cmds.ls(type=["hairSystem", "nucleus"]) or [])
    uuids = {guide_id: cmds.ls(curve, uuid=True)[0] for guide_id, curve in curves.items()}
    cmds.select(list(curves.values()) + ([scalp] if scalp else []), replace=True)
    # makeCurvesDynamic: attach to the selected surface, don't snap, match
    # position, output curves, no Paint Effects. It reparents the curves
    # under their follicles, so find them again by UUID.
    mel.eval('makeCurvesDynamic 2 { "1", "0", "1", "1", "0"}')
    cmds.select(clear=True)
    created = set(cmds.ls(type=["hairSystem", "nucleus"]) or []) - before
    curves = {guide_id: cmds.ls(uuid, long=True)[0] for guide_id, uuid in uuids.items()}
    result["curves"] = curves

    outputs = {}
    for guide_id, curve in curves.items():
        # makeCurvesDynamic parents each input curve under its follicle.
        parent = cmds.listRelatives(curve, parent=True, fullPath=True) or []
        follicles = cmds.listRelatives(parent, shapes=True, type="follicle", fullPath=True) if parent else []
        if not follicles:
            raise RuntimeError("makeCurvesDynamic made no follicle for " + curve)
        out = cmds.listConnections(follicles[0] + ".outCurve", shapes=True) or []
        if not out:
            raise RuntimeError("no output curve for " + curve)
        outputs[guide_id] = out[0]
    connect_sim_curves(groom, outputs)
    result["outputs"] = outputs
    result["hairSystem"] = next((n for n in created if cmds.nodeType(n) == "hairSystem"), None)
    result["nucleus"] = next((n for n in created if cmds.nodeType(n) == "nucleus"), None)
    if result["nucleus"] is None and result["hairSystem"]:  # an existing nucleus took the new hair system
        result["nucleus"] = next(iter(cmds.listConnections(result["hairSystem"], type="nucleus") or []), None)
    result["scalp"] = scalp
    result["collider"] = None
    configure_sim(result, preset=preset, collide=collide or None, self_collide=self_collide or None, wind=wind,
                  preroll=preroll or None)
    return result


def bake_sim(groom, path, sim=None, group="primary", start=None, end=None, remove=True):
    """Cache a simulation and pose the groom from the cache.

    Plays the nucleus from its start frame (pre-roll included), writes the
    posed guides over [start, end] (default: the playback range) to Alembic,
    and points the groom's guide cache at it. With `remove` and the dict from
    create_sim_curves, the live sim is deleted, so playback is fast and
    scrubbable; groom files and renders read the cache. Returns the path.
    """
    start = cmds.playbackOptions(query=True, minTime=True) if start is None else start
    end = cmds.playbackOptions(query=True, maxTime=True) if end is None else end
    nucleus = (sim or {}).get("nucleus")
    frame = min(start, cmds.getAttr(nucleus + ".startFrame")) if nucleus else start
    while frame < start:  # pre-roll: the nucleus needs every frame in order
        cmds.currentTime(frame)
        frame += 1
    export_guides(groom, path, group, start=start, end=end)
    if remove:
        remove_sim(groom, sim)
    set_guide_cache(groom, path)
    return path


def remove_sim(groom, sim=None):
    """Stop posing the guides; with the dict from create_sim_curves, delete what it made."""
    disconnect_sim_curves(groom)
    if not sim:
        return
    doomed = [sim.get("group"), sim.get("hairSystem")] + list((sim.get("curves") or {}).values())
    for out in (sim.get("outputs") or {}).values():
        doomed.append(cmds.listRelatives(out, parent=True, fullPath=True)[0])
    hair = sim.get("hairSystem")
    if hair and cmds.objExists(hair):
        doomed += cmds.listConnections(hair, type="follicle") or []
    collider = sim.get("collider")
    if collider and cmds.objExists(collider):
        doomed.append(cmds.listRelatives(collider, parent=True, fullPath=True)[0])
    doomed = [n for n in doomed if n and cmds.objExists(n)]
    if doomed:
        cmds.delete(doomed)


def export_guides(groom, path, group="*", start=None, end=None, step=1.0):
    """Write the guides (posed, as the groom sees them) to Alembic, strandId = guide ID: `group` "*"
    every guide the Guides node feeds (what add_guide_cache's GuideCache node poses), else one
    guide group's.

    The starting point for a simulation in another package; bring the
    result back with set_guide_cache.
    """
    kwargs = {"file": path, "step": step, "guideGroup": group}
    if start is not None:
        kwargs["startFrame"] = start
    if end is not None:
        kwargs["endFrame"] = end
    return cmds.groomLabExportAlembic(groom, **kwargs)


def set_guide_cache(groom, path, object_name=""):
    """Pose guides from an Alembic curves cache whose strandId is the guide ID."""
    cmds.setAttr(groom + ".guideCache", groomlab.portable_path(path), type="string")
    cmds.setAttr(groom + ".guideCacheObject", object_name or "", type="string")


def clear_guide_cache(groom):
    set_guide_cache(groom, "")


def has_poses(groom):
    """Whether sim curves or a guide cache pose the groom's guides."""
    connected = any(cmds.listConnections("%s.simGuides[%d].simCurve" % (groom, i), source=True, destination=False)
                    for i in cmds.getAttr(groom + ".simGuides", multiIndices=True) or [])
    return connected or bool(cmds.getAttr(groom + ".guideCache"))
