# SPDX-License-Identifier: Apache-2.0
"""Cached Playback: grooms played with the Evaluation Manager's cache on (docs/DEVELOPING.md,
Evaluation Manager and Cached Playback).

A suite of the GUI session (gui_runner.py, ctest maya.gui and maya.gui_dx11; harness: gui_support.py).

Each scene is played twice with cached playback on: the first pass fills the cache, the second
draws the groom from it without evaluating it (its frames are the shape's outFrame, kept by the
cache). Cached playback must never enter its safe mode; a param edit invalidates the cached frames
(they evaluate again); what the viewport draws from the cache is what an uncached evaluation draws,
picture for picture. Scenes: a static scalp, a scalp cache followed (followScalp), a deforming
scalp, the frames filled in the background (another thread, other times, in any order), and the
Simulate node: live (left out of the cache, evaluated as it plays) and baked (cached).
"""

import hashlib
import json
import math
import os
import tempfile
import time

from maya import cmds

from gui_support import check, finish, say, soon

FIRST, LAST = 1, 24
SHOTS = (4, 13, 24)  # frames drawn from the cache and compared with an uncached evaluation


def start():
    soon(run)


# -- Cached playback ------------------------------------------------------------------------------

def cache_manager():
    if not cmds.pluginInfo("cacheEvaluator", query=True, loaded=True):
        cmds.loadPlugin("cacheEvaluator")
    from maya.plugin.evaluator.CacheEvaluatorManager import CacheEvaluatorManager
    return CacheEvaluatorManager()


def cache_on(fill="syncOnly"):
    """Parallel evaluation with cached playback on (the Evaluation Cache mode, Maya's default), the
    cache empty. syncOnly fills it as it plays (what the checks count on); syncAsync also fills
    it in the background when idle."""
    from maya.plugin.evaluator.CacheEvaluatorManager import CACHE_STANDARD_MODE_EVAL
    cmds.evaluationManager(mode="parallel")
    manager = cache_manager()
    manager.cache_mode = CACHE_STANDARD_MODE_EVAL
    manager.enabled = True
    cmds.cacheEvaluator(cacheFillMode=fill)
    cmds.cacheEvaluator(flushCache="destroy")


def cache_off():
    """The groom evaluated by the dependency graph alone, cached playback off: the reference."""
    cache_manager().enabled = False
    cmds.evaluationManager(mode="off")


def safe_mode():
    """'' while cached playback never entered its safe mode, else why."""
    if not int(cmds.cacheEvaluator(query=True, safeModeTriggered=True)):
        return ""
    return cmds.cacheEvaluator(query=True, safeModeMessages=True) or "(no message)"


def cached_nodes():
    return set(cmds.cacheEvaluator(query=True, cachingPoints=True) or [])


def frame_info(groom):
    return json.loads(cmds.groomLabGraphStats(groom, frameInfo=True))


def evaluations(groom):
    """The groom's evaluations so far: live and in the background (pulls nothing that is clean)."""
    info = frame_info(groom)
    return info["evaluations"] + info["backgroundEvaluations"]


def play(groom):
    """Plays FIRST..LAST (every frame); (evaluations it made, frames per second)."""
    cmds.currentTime(FIRST)
    before = evaluations(groom)
    started = time.time()
    cmds.play(forward=True, wait=True)
    seconds = time.time() - started
    return evaluations(groom) - before, (LAST - FIRST) / max(seconds, 1e-6)


def digest(groom):
    """The strands and guides the viewport draws now, rounded: (points, hash of the strands, hash of
    the guides)."""
    import groomlab
    values = cmds.groomLabGraphStats(groom, points=True) or []
    guides = []
    for g in groomlab.get_hierarchy(groom).get("guides", []):
        guides += cmds.groomLabGuides(groom, guidePoints=str(g["id"])) or []
    rounded = lambda vs: hashlib.md5(",".join("%.5f" % v for v in vs).encode()).hexdigest()
    return len(values) // 3, rounded(values), rounded(guides)


def shot(folder, name, frame):
    path = os.path.join(folder, "%s_%d.png" % (name, frame))
    cmds.playblast(frame=[frame], format="image", compression="png", viewer=False, showOrnaments=False,
                   percent=100, widthHeight=(320, 240), completeFilename=path)
    return path


def differ(a, b):
    """The share of pixels that differ between two captures."""
    from groomlab.ui.qt import QtGui
    ia, ib = QtGui.QImage(a), QtGui.QImage(b)
    if ia.size() != ib.size() or ia.isNull():
        return 1.0
    ia = ia.convertToFormat(QtGui.QImage.Format_RGB32)
    ib = ib.convertToFormat(QtGui.QImage.Format_RGB32)
    da, db = bytes(ia.constBits())[:ia.byteCount()], bytes(ib.constBits())[:ib.byteCount()]
    changed = sum(1 for i in range(0, len(da), 4) if da[i:i + 3] != db[i:i + 3])
    return changed / float(ia.width() * ia.height())


# -- Scenes ---------------------------------------------------------------------------------------

def new_scene(scalp=None, density=600.0):
    """A groom grown from three guide curves (on `scalp`, else a new sphere)."""
    import groomlab
    if scalp is None:
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15, name="scalp")[0]
    cmds.playbackOptions(minTime=FIRST, maxTime=LAST, animationStartTime=FIRST, animationEndTime=LAST,
                         loop="once", playbackSpeed=0, maxPlaybackSpeed=0)
    groom = groomlab.create_groom(scalp)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    curves = []
    for angle in (0.3, 0.9, 1.4):
        n = (math.sin(angle), math.cos(angle), 0.2)
        curves.append(cmds.curve(degree=1, point=[n, tuple(1.6 * c for c in n), tuple(2.2 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves)
    cmds.delete(curves)
    groomlab.set_param(groom, "scatter", "density", density)
    cmds.currentTime(FIRST)
    cmds.select(clear=True)
    cmds.viewFit("persp", allObjects=True)
    return groom, scalp


def bend(scalp):
    deformer, _ = cmds.nonLinear(scalp, type="bend")
    cmds.setKeyframe(deformer, attribute="curvature", time=FIRST, value=0)
    cmds.setKeyframe(deformer, attribute="curvature", time=LAST, value=60)
    return deformer


def static_scene():
    groom, _ = new_scene()
    box = cmds.polyCube(name="mover")[0]  # the scene animates; the groom does not
    cmds.setKeyframe(box, attribute="translateX", time=FIRST, value=3.0)
    cmds.setKeyframe(box, attribute="translateX", time=LAST, value=5.0)
    cmds.viewFit("persp", allObjects=True)
    return groom


def deforming_scene(density=600.0):
    import groomlab
    groom, scalp = new_scene(density=density)
    groomlab.set_rest_mesh(groom)
    bend(scalp)
    return groom


def scalp_cache_scene(folder):
    """The scalp read from an Alembic cache (a bent sphere), followed: the groom grown at rest."""
    import groomlab
    cmds.file(new=True, force=True)
    source = cmds.polySphere(radius=1, subdivisionsX=30, subdivisionsY=15, name="scalpSource")[0]
    bend(source)
    path = os.path.join(folder, "scalp.abc").replace("\\", "/")
    cmds.loadPlugin("AbcExport", quiet=True)
    cmds.AbcExport(jobArg="-frameRange %d %d -worldSpace -uvWrite -dataFormat ogawa -root |%s -file \"%s\""
                   % (FIRST, LAST, source, path))
    cmds.file(new=True, force=True)
    cmds.loadPlugin("AbcImport", quiet=True)
    cmds.currentTime(FIRST)
    cmds.AbcImport(path, mode="import")
    scalp = cmds.ls("scalpSource", type="transform")[0]
    groom, _ = new_scene(scalp)
    groomlab.set_rest_mesh(groom)  # a copy at frame 1: the rest pose
    groomlab.set_follow_scalp(groom, True)
    return groom


def key_slide(scalp):
    for frame, x in ((FIRST, 0.0), (5, 0.0), (15, 1.5), (LAST, 1.5)):
        cmds.setKeyframe(scalp, attribute="translateX", time=frame, value=x)


# -- Checks ---------------------------------------------------------------------------------------

def cached_twice(name, groom, cached=True):
    """Two passes with cached playback on: the first fills it, the second draws the groom from it
    (no evaluation) when `cached`, else evaluates it live as it plays; never safe mode."""
    first, fill_fps = play(groom)
    second, fps = play(groom)
    shape = cmds.listRelatives(groom, shapes=True)[0] if cmds.nodeType(groom) != "groomLabShape" else groom
    holds = shape in cached_nodes()
    say("[ budget ] %s: pass 1 %d evaluations (%.1f fps, filling), pass 2 %d evaluations (%.1f fps); the groom %s"
        % (name, first, fill_fps, second, fps, "cached" if holds else "not cached"))
    why = safe_mode()
    check("%s: cached playback stays out of safe mode (%s)" % (name, why or "never triggered"), not why)
    if cached:
        check("%s: the first pass evaluates every frame (%d) and fills the cache" % (name, first), first >= LAST - FIRST and holds)
        check("%s: the second pass draws the groom from the cache, evaluating nothing (%d)" % (name, second), second == 0)
    else:
        check("%s: left out of the cache, the groom evaluates live on every frame (%d, %d)" % (name, first, second),
              not holds and first >= LAST - FIRST and second >= LAST - FIRST)
    return fps


def drawn_from_cache(name, groom, folder):
    """The SHOTS frames as the cache draws them: (pictures, strands) by frame, nothing evaluated."""
    before = evaluations(groom)
    pictures, strands = {}, {}
    for frame in SHOTS:
        pictures[frame] = shot(folder, name + "_cached", frame)
        cmds.currentTime(frame)
        strands[frame] = digest(groom)
    return pictures, strands, evaluations(groom) - before


def matches_uncached(name, groom, folder, pictures, strands, play_through=False):
    """With cached playback off and the Evaluation Manager off, the SHOTS frames evaluated afresh
    (played through from the first frame when the groom depends on the frames before) draw what the
    cache drew."""
    cache_off()
    if play_through:
        from groomlab import sim
        sim.invalidate_simulation(groom)
        for frame in range(FIRST, LAST + 1):
            cmds.currentTime(frame)
            cmds.groomLabGraphStats(groom, frameInfo=True)
    worst, same = 0.0, True
    for frame in SHOTS:
        reference = shot(folder, name + "_uncached", frame)
        cmds.currentTime(frame)
        fresh = digest(groom)
        changed = differ(pictures[frame], reference)
        if fresh != strands[frame] or changed >= 0.001:
            say("%s: frame %d: drawn %s, uncached %s, %.5f of the pixels differ (%s, %s)"
                % (name, frame, strands[frame], fresh, changed, pictures[frame], reference))
        same = same and fresh == strands[frame]
        worst = max(worst, changed)
    say("%s: frames %s drawn from the cache against an uncached evaluation: strands and guides %s, %.5f of the pixels "
        "differ at most" % (name, list(SHOTS), "the same" if same else "DIFFERENT", worst))
    check("%s: the frames drawn from the cache are the uncached evaluation's (strands and pixels)" % name,
          same and worst < 0.001)


def scene_check(name, groom, folder, cached=True, play_through=False):
    cache_on()
    cached_twice(name, groom, cached)
    pictures, strands, evaluated = drawn_from_cache(name, groom, folder)
    if cached:
        check("%s: stepping to cached frames draws them without evaluating (%d)" % (name, evaluated), evaluated == 0)
    matches_uncached(name, groom, folder, pictures, strands, play_through)


def edit_invalidates(folder):
    """A param edit invalidates the cached frames: the next pass evaluates them all again, the one
    after draws them from the cache, and they show the edit."""
    import groomlab
    groom = deforming_scene()
    cache_on()
    cached_twice("edit", groom)
    cmds.currentTime(13)
    before = digest(groom)
    groomlab.set_param(groom, "length", "keep", 0.6)
    edited, _ = play(groom)
    again, _ = play(groom)
    cmds.currentTime(13)
    after = digest(groom)
    why = safe_mode()
    say("edit: a pass after a length edit %d evaluations, the next %d; frame 13 %s" % (edited, again, "changed" if after != before else "UNCHANGED"))
    check("edit: a param edit invalidates the cached frames (all evaluated again, then cached), no safe mode (%s)"
          % (why or "never triggered"), edited >= LAST - FIRST and again == 0 and after != before and not why)
    pictures, strands, _ = drawn_from_cache("edit", groom, folder)
    matches_uncached("edit", groom, folder, pictures, strands)


def background_fill(folder):
    """Filled in the background (syncAsync, idle): the groom evaluates its frames off the main
    thread at their own times, in any order (the background lane); they draw as uncached ones."""
    from gui_support import process_events
    groom = deforming_scene()
    cache_on("syncAsync")
    cmds.currentTime(12)  # filled outward from here
    deadline = time.time() + 30.0
    filled = False
    while time.time() < deadline and not filled:
        process_events(100)
        cmds.flushIdleQueue()
        try:
            filled = bool(cmds.cacheEvaluator(waitForCache=1.0))
        except RuntimeError:
            filled = False
    info = frame_info(groom)
    say("background fill: %s, %d frames evaluated in the background, %d live; %s"
        % ("filled" if filled else "NOT FILLED", info["backgroundEvaluations"], info["evaluations"], info))
    check("background: the frames fill in the background, evaluated there (%d)" % info["backgroundEvaluations"],
          filled and info["backgroundEvaluations"] >= LAST - FIRST - 1)
    played, _ = play(groom)
    why = safe_mode()
    check("background: a pass after it draws from the cache (%d evaluations), no safe mode (%s)" % (played, why or "never triggered"),
          played == 0 and not why)
    pictures, strands, _ = drawn_from_cache("background", groom, folder)
    matches_uncached("background", groom, folder, pictures, strands)


def simulate_checks(folder):
    """The Simulate node: live it depends on the frames before (left out of the cache: evaluated as
    it plays, the rest of the scene cached); added to a cached groom it leaves the cache; baked it
    is cached again."""
    import groomlab
    from groomlab import sim
    groom, scalp = new_scene()
    key_slide(scalp)
    scene_check("before simulate", groom, folder)
    node = sim.add_simulate(groom, gravity=9.8, collisionOffset=0.02, damping=0.2)
    check("simulate: added (%s)" % node, not groomlab.errors(groom))
    scene_check("simulate live", groom, folder, cached=False, play_through=True)
    cmds.file(rename=os.path.join(folder, "simulate.ma"))
    sim.bake_simulation(groom, end=LAST)
    scene_check("simulate baked", groom, folder)


def wanted(name):
    """GROOMLAB_GUI_CASES (parts of names: simulate,background) runs only those scenes."""
    parts = [p.strip() for p in os.environ.get("GROOMLAB_GUI_CASES", "").split(",") if p.strip()]
    return not parts or any(p in name for p in parts)


def run():
    import groomlab  # noqa: F401
    folder = tempfile.mkdtemp(prefix="groomlab_cached_")
    manager = cache_manager()
    was = {"enabled": manager.enabled, "fill": cmds.cacheEvaluator(query=True, cacheFillMode=True),
           "mode": cmds.evaluationManager(query=True, mode=True)[0]}
    try:
        if wanted("rates"):
            rates()
        for name, check_scene in (("static", lambda: scene_check("static", static_scene(), folder)),
                                  ("scalp cache followed", lambda: scene_check("scalp cache followed", scalp_cache_scene(folder), folder)),
                                  ("deforming", lambda: scene_check("deforming", deforming_scene(), folder)),
                                  ("edit", lambda: edit_invalidates(folder)),
                                  ("background", lambda: background_fill(folder)),
                                  ("simulate", lambda: simulate_checks(folder))):
            if wanted(name):
                check_scene()
    finally:
        manager.enabled = was["enabled"]
        cmds.cacheEvaluator(cacheFillMode=was["fill"])
        cmds.evaluationManager(mode=was["mode"])
    finish()


def rates():
    """Frame rates: a heavier groom on a deforming scalp (evaluated on every frame) played without
    the cache in the dependency graph and in parallel, then filling the cache and from it."""
    groom = deforming_scene(density=4000.0)
    cache_off()
    _, dg_fps = play(groom)
    cmds.evaluationManager(mode="parallel")
    _, em_fps = play(groom)
    cache_on()
    first, fill_fps = play(groom)
    second, cached_fps = play(groom)
    say("[ budget ] a deforming scalp, %d strands: %.1f fps in the DG, %.1f in parallel without the cache, %.1f "
        "filling it (%d evaluations), %.1f from it (%d evaluations) (reported only)"
        % (cmds.getAttr(groom + ".outStrandCount"), dg_fps, em_fps, fill_fps, first, cached_fps, second))
