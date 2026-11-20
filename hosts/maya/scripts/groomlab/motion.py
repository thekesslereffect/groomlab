# SPDX-License-Identifier: Apache-2.0
"""Motion without a cache: Wind and Jiggle nodes."""

import json

from groomlab.base import undoable


def motion_anchor(model):
    """Where a motion node (Wind, Jiggle) goes in a graph model: just before the output, but before
    the Collide and Cards nodes that end its chain, so the strands collide where they moved to and
    the cards (and every export and render) take the motion: the core's (graph_inserts.h
    motionAnchor), every host's. Returns the node to insert before."""
    anchor = model._edit().motion_anchor()
    if not anchor:
        raise ValueError("the graph has no output")
    return anchor


def _add_motion_node(groom, op_type, params):
    """The node added as every host adds it (graph_inserts.h insertMotion), its params over the
    spec's defaults fitted to the scalp."""
    from groomlab import graph as gm
    model = gm.GraphModel.from_groom(groom)
    given = {k: v for k, v in params.items() if v is not None}
    edit = model._edit()
    with gm._core_errors():
        node = edit.insert_motion(op_type, json.dumps(given), model.groom_size)
    model._take(edit)
    model.save(groom)
    return node


def _loop(loop, start, end):
    """A motion node's loop params (None: the node's defaults)."""
    return {"loop": None if loop is None else bool(loop), "loopStart": None if start is None else int(start),
            "loopEnd": None if end is None else int(end)}


@undoable("add wind")
def add_wind(groom, direction=(1.0, 0.0, 0.0), strength=0.3, gust=None, frequency=None, speed=None,
             turbulence=None, profile=None, seed=None, loop=None, loop_start=None, loop_end=None):
    """Add a Wind node: strands bend along a wind that varies over space and time (play the timeline
    to see it). It goes just before the output (before a Collide or Cards ending the chain:
    motion_anchor), so collisions, cards, exports and renders all see it. `direction` is where the
    wind blows to (world space); `strength` 0-1 how far strands bend; `gust` 0-1 how much it
    varies; `frequency` the gusts' size (features per scene unit); `speed` how fast they travel
    (scene units per second); `turbulence` a fast flutter toward the tips; `profile` a ramp of how
    much each part bends, root to tip; `loop` repeats the wind every `loop_end` - `loop_start`
    frames, seamlessly (a looping animation). Params left None keep the node's defaults. Bypass or
    delete the node and the groom is exactly as it was. Returns the node's name."""
    direction = [float(v) for v in direction]
    return _add_motion_node(groom, "Wind", {"direction": direction, "strength": strength, "gust": gust,
                                            "frequency": frequency, "speed": speed, "turbulence": turbulence,
                                            "profile": profile, "seed": seed, **_loop(loop, loop_start, loop_end)})


@undoable("add jiggle")
def add_jiggle(groom, strength=1.0, frequency=None, damping=None, history=None, profile=None, loop=None,
               loop_start=None, loop_end=None):
    """Add a Jiggle node: strands lag behind the scalp's motion, overshoot when it stops and
    settle, worked out on each frame from the scalp's last `history` frames (no simulation
    cache: it scrubs, and renders on a farm the same). It goes where add_wind puts Wind.
    `strength` scales the motion (mappable); `frequency` is the swing's frequency in cycles per
    second; `damping` 0 swings on, 1 settles without overshoot; `profile` how much each part lags,
    root to tip; `loop` reads the frames before `loop_start` from the end of the range, so a scalp
    animation looping from `loop_start` to `loop_end` jiggles seamlessly. On a scalp that does not
    move it changes nothing, and bypassed or deleted the groom is exactly as it was. Returns the
    node's name."""
    return _add_motion_node(groom, "Jiggle", {"strength": strength, "frequency": frequency, "damping": damping,
                                              "history": None if history is None else int(history),
                                              "profile": profile, **_loop(loop, loop_start, loop_end)})
