# SPDX-License-Identifier: Apache-2.0
"""Correctives: sculpt layers on top of the groom, as Yeti's layers.

A groom is a stack: the base groom (its guides) and correctives on top. Pick
what brushes edit: the base groom, or a corrective, whose strokes are stored
as corrections instead of editing the guides. Layers can be switched off,
soloed, renamed and reordered.

Each corrective's weight is a keyable attribute on the groom's transform,
named after the layer (see it in the Channel Box), driving the shape's
correctiveWeight[slot]. Keys live on the transform, so they show in the
timeline with the groom selected. Unkeyed, a corrective applies on every
frame. For a shot fix, key it: 0 before, 1 on the frame, 0 after
(`add_layer(frame=..., fade=...)` sets those keys), and edit the keys in the
timeline or Graph Editor like any animation. Corrections ride along with the
animation; renders and groom files fade them as keyed.

    import groomlab.shots as shots
    shots.add_layer(groom, frame=1012, fade=6)   # a fix keyed around frame 1012; brushes edit it
    ...brush...
    shots.set_target(groom, None)                # back to editing the base groom
"""

import json
import re

from maya import cmds, mel

from groomlab import undoable

WEIGHTS = "correctiveWeight"


def _fps():
    return float(mel.eval("currentTimeUnitToFPS"))


def _load(groom):
    text = cmds.getAttr(groom + ".correctiveData") or ""
    return json.loads(text) if text.strip() else {"layers": []}


def _store(groom, data):
    cmds.setAttr(groom + ".correctiveData", json.dumps(data), type="string")


def _find(data, name):
    layer = next((l for l in data["layers"] if l["name"] == name), None)
    if layer is None:
        raise ValueError("no corrective layer named %r" % name)
    return layer


def weight_plug(groom, name):
    """The keyable attribute holding a layer's weight ("groomTransform.<name>"); key it for fades."""
    _find(_load(groom), name)
    return _source(groom, name)


def weight_plugs(groom):
    """{layer: weight_plug} for every layer, from one read of the stack (what reads the weights
    often keeps them)."""
    return dict((l["name"], _source(groom, l["name"])) for l in _load(groom)["layers"])


def _transform(groom):
    parents = cmds.listRelatives(groom, parent=True, fullPath=True) or []
    return parents[0] if parents else groom


def _source(groom, name):
    """The keyable weight on the groom's transform (what the timeline shows keys of)."""
    return "%s.%s" % (_transform(groom), name)


def _shape_plug(groom, slot):
    return "%s.%s[%d]" % (groom, WEIGHTS, slot)


def _make_weight(groom, name, slot, value=1.0):
    """A keyable `name` on the transform driving the shape's weight slot."""
    xform = _transform(groom)
    if not cmds.attributeQuery(name, node=xform, exists=True):
        cmds.addAttr(xform, longName=name, attributeType="double", defaultValue=1.0, minValue=0.0, softMaxValue=1.0,
                     keyable=True)
    source = _source(groom, name)
    cmds.cutKey(source, clear=True)
    cmds.setAttr(source, value)
    cmds.connectAttr(source, _shape_plug(groom, slot), force=True)


def _drop_weight(groom, name, slot):
    source = _source(groom, name)
    if cmds.attributeQuery(name, node=_transform(groom), exists=True):
        cmds.cutKey(source, clear=True)
        cmds.deleteAttr(source)
    cmds.removeMultiInstance(_shape_plug(groom, slot), b=True)


def _keys(groom, name):
    plug = _source(groom, name)
    times = cmds.keyframe(plug, query=True, timeChange=True) or []
    values = cmds.keyframe(plug, query=True, valueChange=True) or []
    return list(zip(times, values))


def _free_slot(groom, data):
    used = set(cmds.getAttr(groom + "." + WEIGHTS, multiIndices=True) or [])
    used.update(l["slot"] for l in data["layers"])
    slot = 0
    while slot in used:
        slot += 1
    return slot


def _clean_name(groom, data, wanted, current=None):
    """A layer name that also works as the weight's attribute: letters, digits and _, unique."""
    base = re.sub(r"\W", "_", wanted.strip()) or "corrective"
    if base[0].isdigit():
        base = "c" + base
    taken = {l["name"] for l in data["layers"] if l["name"] != current}
    name, n = base, 1
    xform = _transform(groom)
    while name in taken or (name != current and (cmds.attributeQuery(name, node=groom, exists=True) or
                                                 cmds.attributeQuery(name, node=xform, exists=True))):
        n += 1
        name = "%s_%d" % (base, n)
    return name


def _key(groom, name, frame, value):
    cmds.setKeyframe(_source(groom, name), time=frame, value=value, inTangentType="flat", outTangentType="flat")


_layers_cache = {}  # groom -> (state, layers): the palette asks every tick


def layers(groom):
    """The corrective stack, bottom first: [{"name", "enabled", "guides", "slot", "weight" (now),
    "keyed", "keys": [(frame, weight)], "all_frames" (not keyed), "frame" (its strongest key, or None)}]."""
    data = _load(groom)
    now = cmds.currentTime(query=True)
    result = []
    for l in data["layers"]:
        keys = _keys(groom, l["name"])
        weight = cmds.getAttr(_source(groom, l["name"]), time=now)
        peak = max(keys, key=lambda k: k[1])[0] if keys else None
        result.append({"name": l["name"], "enabled": l.get("enabled", True), "guides": len(l.get("offsets", {})),
                       "slot": l["slot"], "weight": weight, "keyed": bool(keys), "keys": keys,
                       "all_frames": not keys, "frame": peak})
    return result


def weight_at(groom, name, frame=None):
    """A layer's weight on a frame (0 when switched off), as the groom evaluates it."""
    layer = _find(_load(groom), name)
    if not layer.get("enabled", True):
        return 0.0
    frame = cmds.currentTime(query=True) if frame is None else frame
    return cmds.getAttr(_source(groom, name), time=frame)


@undoable("add corrective")
def add_layer(groom, name=None, frame=None, fade=None, all_frames=None, fade_in=None, fade_out=None):
    """A new, empty corrective on top of the stack, made the one brushes edit; returns its name.

    With `fade` (or fade_in / fade_out, frames) it is a shot fix: its weight is
    keyed 0 -> 1 -> 0 around `frame` (default: the current one). Otherwise (or
    with all_frames=True) its weight is 1 on every frame, unkeyed.
    """
    frame = cmds.currentTime(query=True) if frame is None else frame
    if all_frames is None:
        all_frames = fade is None and fade_in is None and fade_out is None
    data = _load(groom)
    base = name or ("sculpt%d" % (len(data["layers"]) + 1) if all_frames else "shot%d" % int(round(frame)))
    name = _clean_name(groom, data, base)
    slot = _free_slot(groom, data)
    _make_weight(groom, name, slot)
    fps = _fps()
    data["layers"].append({"name": name, "time": frame / fps, "fadeIn": 0.5, "fadeOut": 0.5, "holdBefore": True,
                           "holdAfter": True, "weight": 1.0, "enabled": True, "slot": slot, "offsets": {}})
    _store(groom, data)
    if not all_frames:
        before = fade_in if fade_in is not None else (fade if fade is not None else 6.0)
        after = fade_out if fade_out is not None else (fade if fade is not None else 6.0)
        _key(groom, name, frame, 1.0)
        if before > 0:
            _key(groom, name, frame - before, 0.0)
        if after > 0:
            _key(groom, name, frame + after, 0.0)
    set_target(groom, name)
    return name


@undoable("edit corrective")
def set_layer(groom, name, enabled=None, rename=None, weight=None, key=None, frame=None, fade=None,
              fade_in=None, fade_out=None, all_frames=None):
    """Change a corrective: switch it on or off, rename it, set its weight now
    (`key`: also key it here; a keyed weight is keyed anyway, as auto key does),
    or re-time it: all_frames=True drops its keys (1 everywhere); frame/fade
    re-key it as a shot fix."""
    data = _load(groom)
    layer = _find(data, name)
    if enabled is not None:
        layer["enabled"] = bool(enabled)
    if rename and rename != name:
        new = _clean_name(groom, data, rename, current=name)
        if new != rename:
            raise ValueError("%r can't name a corrective (taken, or not letters, digits and _): try %r" % (rename, new))
        cmds.renameAttr(_source(groom, name), new)
        layer["name"] = new
        if cmds.getAttr(groom + ".shotLayer") == name:
            cmds.setAttr(groom + ".shotLayer", new, type="string")
        name = new
    _store(groom, data)
    plug = _source(groom, name)
    if all_frames:
        cmds.cutKey(plug, clear=True)
        cmds.setAttr(plug, 1.0 if weight is None else weight)
    elif frame is not None or fade is not None or fade_in is not None or fade_out is not None:
        keys = _keys(groom, name)
        at = frame if frame is not None else (max(keys, key=lambda k: k[1])[0] if keys else cmds.currentTime(query=True))
        old_in = at - keys[0][0] if keys and keys[0][0] < at else 6.0
        old_out = keys[-1][0] - at if keys and keys[-1][0] > at else 6.0
        before = fade_in if fade_in is not None else (fade if fade is not None else old_in)
        after = fade_out if fade_out is not None else (fade if fade is not None else old_out)
        peak = weight if weight is not None else (max(k[1] for k in keys) if keys else cmds.getAttr(plug))
        cmds.cutKey(plug, clear=True)
        _key(groom, name, at, peak)
        if before > 0:
            _key(groom, name, at - before, 0.0)
        if after > 0:
            _key(groom, name, at + after, 0.0)
    elif weight is not None:
        if key or _keys(groom, name):
            _key(groom, name, cmds.currentTime(query=True), weight)
        else:
            cmds.setAttr(plug, weight)


def key_weight(groom, name, value=None, frame=None):
    """Key a corrective's weight on a frame (default: now, at its current value)."""
    frame = cmds.currentTime(query=True) if frame is None else frame
    _find(_load(groom), name)
    value = cmds.getAttr(_source(groom, name), time=frame) if value is None else value
    _key(groom, name, frame, value)


@undoable("clear corrective keys")
def clear_keys(groom, name):
    """Drop a corrective's keys: it applies on every frame at its current weight."""
    plug = weight_plug(groom, name)
    value = cmds.getAttr(plug, time=cmds.currentTime(query=True))  # the curve's value now (not a stale one)
    cmds.cutKey(plug, clear=True)
    cmds.setAttr(plug, value)


@undoable("remove corrective")
def remove_layer(groom, name):
    data = _load(groom)
    layer = _find(data, name)
    data["layers"] = [l for l in data["layers"] if l["name"] != name]
    _store(groom, data)
    _drop_weight(groom, name, layer["slot"])
    if cmds.getAttr(groom + ".shotLayer") == name:
        cmds.setAttr(groom + ".shotLayer", "", type="string")
        cmds.setAttr(groom + ".shotSculpt", False)


@undoable("move corrective")
def move_layer(groom, name, step):
    """Move a corrective up (+1: later in the stack) or down (-1)."""
    data = _load(groom)
    names = [l["name"] for l in data["layers"]]
    if name not in names:
        raise ValueError("no corrective layer named %r" % name)
    i = names.index(name)
    j = min(max(i + step, 0), len(names) - 1)
    data["layers"].insert(j, data["layers"].pop(i))
    _store(groom, data)


@undoable("solo corrective")
def solo_layer(groom, name):
    """Only this corrective on; if it already is the only one on, every one comes back on."""
    data = _load(groom)
    _find(data, name)
    alone = all(l.get("enabled", True) == (l["name"] == name) for l in data["layers"])
    for l in data["layers"]:
        l["enabled"] = True if alone else l["name"] == name
    _store(groom, data)


def target(groom):
    """What brushes edit: a corrective's name, or None for the base groom (or a Sculpt node:
    groomlab.sculpts.target)."""
    name = cmds.getAttr(groom + ".shotLayer") or None
    return name if name and cmds.getAttr(groom + ".shotSculpt") else None


def set_target(groom, name):
    """Brushes edit corrective `name` (strokes become corrections), or the base groom (None). Either
    way no Sculpt node is the target any more (groomlab.sculpts)."""
    if cmds.getAttr(groom + ".sculptNode"):
        cmds.setAttr(groom + ".sculptNode", "", type="string")
    if name is None:
        cmds.setAttr(groom + ".shotSculpt", False)
        return
    _find(_load(groom), name)
    cmds.setAttr(groom + ".shotLayer", name, type="string")
    cmds.setAttr(groom + ".shotSculpt", True)


def active_layer(groom):
    return cmds.getAttr(groom + ".shotLayer") or None


def go_to_layer(groom, name):
    """Jump to a keyed corrective's strongest frame and edit it."""
    layer = next((l for l in layers(groom) if l["name"] == name), None)
    if layer is None:
        raise ValueError("no corrective layer named %r" % name)
    set_target(groom, name)
    if layer["frame"] is not None:
        cmds.currentTime(layer["frame"])


def shot_sculpt(groom, on=True, all_frames=None, fade=6.0):
    """Brushes edit a corrective (on) or the base groom (off); returns the corrective.

    On, the current one is used when it has weight on this frame; otherwise a
    new one is made: on every frame the first time (or with all_frames=True),
    else a fix keyed around this frame over `fade` frames.
    """
    if not on:
        set_target(groom, None)
        return None
    current = active_layer(groom)
    names = [l["name"] for l in layers(groom)]
    if current in names and not all_frames and weight_at(groom, current) > 1e-3:
        set_target(groom, current)
        return current
    if current not in names:
        return add_layer(groom, all_frames=True if all_frames is None else all_frames, fade=None if all_frames in (None, True) else fade)
    return add_layer(groom, all_frames=bool(all_frames), fade=None if all_frames else fade)


# ---- Sculpting on the simulation -----------------------------------------------------------------


def _motion_nodes(model, node):
    """`node` (a Simulate) and the DriveGuides and Simulate nodes feeding it guides, in a chain."""
    out, name = [], node
    while name and name not in out and model.node_type(name) in ("Simulate", "DriveGuides"):
        out.append(name)
        source = model._source(name, "guides")
        name = source[0] if source else None
    return out


@undoable("sculpt on simulation")
def sculpt_on_simulation(groom, node=None, end=None):
    """Shot sculpting on top of a simulation, as studios fix a sim: the Simulate node's frames are
    baked (bake_simulation, unless its bake is current), the guides are posed from the bake (the
    groom's guide cache) and the Simulate node, with the DriveGuides and Simulate nodes feeding it,
    is bypassed. The guides shown are then the simulated ones, and corrective layers (add_layer,
    shot_sculpt) apply on top of them: in the viewport, exports, groom files and renders.
    live_simulation goes back. `node`: the Simulate node (default: the last). Returns the bake's
    path as the groom names it."""
    from groomlab import graph as gm
    from groomlab import sim
    nodes = sim.simulate_nodes(groom)
    if not nodes:
        raise ValueError("%s has no Simulate node to sculpt on" % groom)
    node = node or nodes[-1]
    if node not in nodes:
        raise ValueError("%s has no Simulate node %r making its output" % (groom, node))
    status = next((s for s in sim.simulation_status(groom) if s.get("node") == node), {})
    bakes = sim.baked_simulations(groom)
    if not status.get("baked") or status.get("bakeStale") or node not in bakes:
        sim.bake_simulation(groom, node=node, end=end)
        bakes = sim.baked_simulations(groom)
    bake = bakes[node]
    cmds.setAttr(groom + ".guideCache", bake["file"], type="string")
    cmds.setAttr(groom + ".guideCacheObject", bake.get("object", ""), type="string")
    model = gm.GraphModel.from_groom(groom)
    for name in _motion_nodes(model, node):
        model.set_bypass(name, True)
    model.save(groom)
    return cmds.getAttr(groom + ".guideCache")


def sculpting_on_simulation(groom):
    """The Simulate node whose bake poses the guides with it bypassed (sculpt_on_simulation), or
    None: the simulation is live."""
    from groomlab import graph as gm
    from groomlab import sim
    cache = cmds.getAttr(groom + ".guideCache") or ""
    if not cache:
        return None
    model = gm.GraphModel.from_groom(groom)
    from groomlab.paths import resolve_path, _same_file
    for node, bake in sim.baked_simulations(groom).items():
        if node in model.nodes and model.bypassed(node) and bake.get("file") and                 _same_file(resolve_path(bake["file"], groom), resolve_path(cache, groom)):
            return node
    return None


@undoable("live simulation")
def live_simulation(groom):
    """Back from sculpt_on_simulation: its Simulate node and the DriveGuides and Simulate nodes
    feeding it run again, and the guide cache is cleared. The corrective layers stay (on the live
    simulation they apply before it: switch the shot fixes off, or remove them). Returns the nodes
    turned back on."""
    from groomlab import graph as gm
    from groomlab import sim
    node = sculpting_on_simulation(groom)
    if node is None:
        return []
    model = gm.GraphModel.from_groom(groom)
    names = _motion_nodes(model, node)
    for name in names:
        model.set_bypass(name, False)
    model.save(groom)
    sim.clear_guide_cache(groom)
    return names
