# SPDX-License-Identifier: Apache-2.0
"""Viewport benchmark (opt-in GUI suite "bench": numbers for the performance audit, no checks).

Run alone in the GUI session: GROOMLAB_GUI_SUITES=bench (ctest maya.viewport_bench, label "bench";
maya.viewport_bench_dx11 on DirectX 11). Each line it says starts "[ bench ]" and holds JSON; with
GROOMLAB_BENCH_OUT set they are appended there too.

Per strand count (GROOMLAB_BENCH_COUNTS, default 50000,200000,1000000; the default graph on a
radius-10 sphere, every strand shown, no point budget), per display mode (lit lines with width as
coverage, the same without coverage, unlit lines, colored lines, ribbons, lit lines casting and
receiving shadows with a shadowing light), with no evaluation in between:
  - orbit: frames of `orbit` + `refresh -cv -f` pipelined as Maya draws them (fps);
  - synced: each frame's refresh then the GPU waited for (groomLabGpuPreview -sync): CPU and GPU
    of one frame, not overlapped;
  - the frame time at half the view's size (pixel-bound or not);
  - the display update of a Noise tweak (compute, build, upload, MB);
  - playback frames of a deforming scalp (a bend keyed), each evaluated on the CPU (the same);
  - the same with Follow Scalp on (a rest mesh: evaluated at rest once, the strands carried by their
    roots, on the GPU where the viewport can), in GROOMLAB_BENCH_FOLLOW_MODES' display modes;
  - a Noise drag on the GPU preview, synced (where the preview takes it);
then, at the largest count, MSAA on against off for the lit lines; and, asked for alone (part "lod"), the
screen-space level of detail (displayClumpLod) off and on at near, mid, far and very far framings of a
clumped groom (lod_bench), and (part "lodorbit") its index rebuilds over full tumbles about several pivots
(lod_orbit_bench); and, asked for, a GPU drag's release (part "commit": the perceived latency, the
UI responsive with the final-looking frame drawn, then the commit waited for, drag_commit) and a comb
stroke's release on hair_basics (part "stroke", stroke_bench, per GROOMLAB_BENCH_STROKE_SCULPT); and, asked for alone
(part "grid"), GroomLab's hair self-shadowing grid written again by GPU drag steps (grid_bench); and strokes chained
(part "chain", chain_bench, on GROOMLAB_BENCH_CHAIN's scenes: the commit, the next stroke's press). GROOMLAB_BENCH_PARTS
picks the parts (modes, tweak, playback, follow, drag, msaa, lod, lodorbit, commit, stroke, grid, chain), GROOMLAB_BENCH_MODES the display modes drawn (default all),
GROOMLAB_BENCH_REPEAT how many times they are each drawn first (default 1: a median of several),
GROOMLAB_BENCH_TWEAK_MODES the display modes a tweak is timed in and GROOMLAB_BENCH_DRAG_MODES those a
GPU drag is (default lit_coverage alone).
"""

import json
import os
import time

from maya import cmds

from gui_support import finish, persp_panel, say, soon

FRAMES = 40


def start():
    soon(run)


def record(row):
    say("[ bench ] " + json.dumps(row, sort_keys=True))
    out = os.environ.get("GROOMLAB_BENCH_OUT")
    if out:
        with open(out, "a") as f:
            f.write(json.dumps(row, sort_keys=True) + "\n")


def counts():
    text = os.environ.get("GROOMLAB_BENCH_COUNTS", "50000,200000,1000000")
    return [int(float(c)) for c in text.split(",") if c.strip()]


def parts():
    """GROOMLAB_BENCH_PARTS: the parts to run, of modes, tweak, playback, follow, drag, msaa (default all of them)."""
    text = os.environ.get("GROOMLAB_BENCH_PARTS", "modes,tweak,playback,follow,drag,msaa")
    return set(p.strip() for p in text.split(",") if p.strip())


def named_modes(variable, default):
    text = os.environ.get(variable, default)
    return [m.strip() for m in text.split(",") if m.strip()]


def drawn_modes():
    """GROOMLAB_BENCH_MODES: the display modes (MODES' names) drawn; default all of them."""
    names = named_modes("GROOMLAB_BENCH_MODES", ",".join(name for name, _ in MODES))
    return [(name, attrs) for name, attrs in MODES if name in names]


def repeats():
    """GROOMLAB_BENCH_REPEAT: how many times each display mode is drawn first (default 1)."""
    return max(1, int(os.environ.get("GROOMLAB_BENCH_REPEAT", "1")))


def drag_modes():
    """GROOMLAB_BENCH_DRAG_MODES: the display modes a GPU drag is timed in; default lit_coverage alone."""
    return named_modes("GROOMLAB_BENCH_DRAG_MODES", "lit_coverage")


def tweak_modes():
    """GROOMLAB_BENCH_TWEAK_MODES: the display modes (MODES' names) a Noise tweak is timed in; default
    lit_coverage alone (the shape's defaults)."""
    text = os.environ.get("GROOMLAB_BENCH_TWEAK_MODES", "lit_coverage")
    return [m.strip() for m in text.split(",") if m.strip()]


def sync():
    try:
        cmds.groomLabGpuPreview(sync=True)
    except Exception:
        pass


def orbit_fps(frames=FRAMES):
    cmds.refresh(currentView=True, force=True)
    sync()
    t = time.time()
    for _ in range(frames):
        cmds.orbit("persp", rotationAngles=(0, 360.0 / frames))
        cmds.refresh(currentView=True, force=True)
    sync()
    return frames / (time.time() - t)


def synced_ms(frames=20):
    times = []
    for _ in range(frames):
        cmds.orbit("persp", rotationAngles=(0, 360.0 / frames))
        t = time.time()
        cmds.refresh(currentView=True, force=True)
        sync()
        times.append((time.time() - t) * 1000.0)
    times.sort()
    return times[len(times) // 2]


def view_size():
    import maya.OpenMayaUI as omui
    view = omui.M3dView.active3dView()
    return view.portWidth(), view.portHeight()


def set_window(width, height):
    import maya.OpenMayaUI as omui
    from groomlab.ui.qt import shiboken
    from groomlab.ui.qt import QtWidgets
    window = shiboken.wrapInstance(int(omui.MQtUtil.mainWindow()), QtWidgets.QMainWindow)
    window.setGeometry(40, 40, width, height)
    QtWidgets.QApplication.processEvents()
    cmds.refresh(force=True)


# The lit modes with Maya's hair tube shading (displayHairShading 0, as before GroomLab's hair), and
# GroomLab's hair (hair_*: without and with its self-shadowing grid; ribbons).
MODES = (
    ("lit_coverage", {"displayLighting": True, "displayWidthCoverage": True, "displayShadows": 0}),
    ("lit_opaque", {"displayLighting": True, "displayWidthCoverage": False, "displayShadows": 0}),
    ("unlit_coverage", {"displayLighting": False, "displayWidthCoverage": True, "displayShadows": 0}),
    ("colored_random", {"displayLighting": False, "strandColorMode": 3, "displayShadows": 0}),
    ("ribbons_lit", {"displayLighting": True, "displayRibbons": True, "displayShadows": 0}),
    ("lit_shadows", {"displayLighting": True, "displayWidthCoverage": True, "displayShadows": 2}),
    ("hair_coverage", {"displayLighting": True, "displayWidthCoverage": True, "displayShadows": 0, "displayHairShading": 1,
                       "displaySelfShadowing": 0.0}),
    ("hair_shadows", {"displayLighting": True, "displayWidthCoverage": True, "displayShadows": 0, "displayHairShading": 1,
                      "displaySelfShadowing": 1.0}),
    ("hair_ribbons", {"displayLighting": True, "displayRibbons": True, "displayShadows": 0, "displayHairShading": 1,
                      "displaySelfShadowing": 1.0}),
)
DEFAULTS = {"displayLighting": True, "displayWidthCoverage": True, "displayShadows": 0, "strandColorMode": 0,
            "displayRibbons": False, "displayHairShading": 0, "displaySelfShadowing": 1.0}


def run():
    import maya.api.OpenMayaRender as omr
    import groomlab
    api = {omr.MRenderer.kDirectX11: "DirectX11", omr.MRenderer.kOpenGLCoreProfile: "OpenGLCoreProfile"}.get(
        omr.MRenderer.drawAPI(), str(omr.MRenderer.drawAPI()))
    panel = persp_panel()
    full = view_size()
    for count in (counts() if parts() - {"lod", "lodorbit", "stroke", "chain"} else ()):
        cmds.file(new=True, force=True)
        scalp = cmds.polySphere(radius=10, subdivisionsX=120, subdivisionsY=60)[0]
        groom = groomlab.create_groom(scalp)
        area = 4.0 * 3.14159265 * 100.0
        t = time.time()
        groomlab.set_param(groom, "scatter", "density", count / area)
        cmds.setAttr(groom + ".previewDensity", 1.0)
        cmds.setAttr(groom + ".previewPointBudget", 0.0)
        cmds.setAttr(groom + ".displayGuides", False)
        strands = cmds.getAttr(groom + ".outStrandCount")
        points = json.loads(cmds.groomLabGraphStats(groom))["points"]
        cmds.viewFit("persp", allObjects=True)
        cmds.refresh(force=True)
        sync()
        record({"bench": "create", "api": api, "strands": strands, "points": points, "seconds": time.time() - t})
        # Nothing changed: a redraw.
        t = time.time()
        for _ in range(10):
            cmds.refresh(currentView=True, force=True)
        sync()
        record({"bench": "redraw", "api": api, "strands": strands, "ms": (time.time() - t) * 100.0})
        light = None
        for repeat, (name, attrs) in [(r, m) for r in range(repeats()) for m in (drawn_modes() if "modes" in parts() else ())]:
            for attr, value in DEFAULTS.items():
                cmds.setAttr(groom + "." + attr, value)
            for attr, value in attrs.items():
                cmds.setAttr(groom + "." + attr, value)
            if name == "lit_shadows" and not light:
                light = cmds.directionalLight(intensity=1.0)
                cmds.setAttr(light + ".useDepthMapShadows", True)
                cmds.modelEditor(panel, edit=True, displayLights="all", shadows=True)
            t = time.time()
            cmds.refresh(currentView=True, force=True)
            sync()
            first = (time.time() - t) * 1000.0
            draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]
            row = {"bench": "mode", "api": api, "strands": strands, "points": points, "mode": name, "repeat": repeat,
                   "firstDrawMs": first, "orbitFps": orbit_fps(), "syncedMs": synced_ms(),
                   "items": sorted(k for k, v in draw["items"].items()),
                   "uploadMb": sum(v.get("megabytes", 0) for v in draw["items"].values()),
                   "updateMs": draw.get("updateMs"), "buildMs": draw.get("buildMs"), "uploadMs": draw.get("uploadMs")}
            if name.startswith("hair_"):
                # GroomLab's hair: its density grid (written by the GPU with the first draw) and its time there.
                row["grid"] = json.loads(cmds.groomLabGraphStats(groom, hairShading=True)).get("grid")
            if light and name != "lit_shadows":
                cmds.delete(cmds.listRelatives(light, parent=True) or light)
                cmds.modelEditor(panel, edit=True, displayLights="default", shadows=False)
                light = None
            set_window(800, 500)
            row["halfView"] = view_size()
            row["halfOrbitFps"] = orbit_fps()
            row["halfSyncedMs"] = synced_ms()
            set_window(1600, 1000)
            row["view"] = view_size()
            record(row)
        if light:
            cmds.delete(cmds.listRelatives(light, parent=True) or light)
            cmds.modelEditor(panel, edit=True, displayLights="default", shadows=False)
        for attr, value in DEFAULTS.items():
            cmds.setAttr(groom + "." + attr, value)
        # A Noise tweak on the CPU: evaluation, display update (per display mode asked for).
        for mode in (tweak_modes() if "tweak" in parts() else ()):
            for attr, value in dict(MODES)[mode].items():
                cmds.setAttr(groom + "." + attr, value)
            cmds.refresh(currentView=True, force=True)
            rows = []
            for i in range(5):
                groomlab.set_param(groom, "noise", "amplitude", 0.3 + 0.02 * i)
                t = time.time()
                cmds.refresh(currentView=True, force=True)
                sync()
                ms = (time.time() - t) * 1000.0
                stats = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))
                rows.append({"ms": ms, "computeMs": stats.get("computeMs"), "updateMs": stats["draw"]["updateMs"],
                             "buildMs": stats["draw"]["buildMs"], "uploadMs": stats["draw"]["uploadMs"],
                             "mb": sum(v.get("megabytes", 0) for v in stats["draw"]["items"].values()),
                             "items": dict((k, dict((f, v.get(f, 0)) for f in ("megabytes", "acquireMs", "writeMs", "commitMs", "bindMs", "gpuParts")))
                                           for k, v in stats["draw"]["items"].items())})
            rows.sort(key=lambda r: r["ms"])
            record(dict({"bench": "cpu_tweak", "api": api, "strands": strands, "mode": mode}, **rows[len(rows) // 2]))
            for attr, value in DEFAULTS.items():
                cmds.setAttr(groom + "." + attr, value)
        if "playback" in parts():
            playback(groom, scalp, api, strands)
        for mode in (follow_modes() if "follow" in parts() else ()):
            follow_playback(groom, scalp, api, strands, mode)
        # The same as a GPU preview drag.
        for mode in (drag_modes() if "drag" in parts() else ()):
            for attr, value in dict(MODES)[mode].items():
                cmds.setAttr(groom + "." + attr, value)
            cmds.refresh(currentView=True, force=True)
            gpu_drag(groom, api, strands, mode)
            for attr, value in DEFAULTS.items():
                cmds.setAttr(groom + "." + attr, value)
        # A GPU drag's release: the commit (perceived latency, then the commit waited for).
        if "commit" in parts():
            for attr, value in DEFAULTS.items():
                cmds.setAttr(groom + "." + attr, value)
            cmds.refresh(currentView=True, force=True)
            drag_commit(groom, api, strands)
        # GroomLab's hair: its self-shadowing grid written again by each step of a GPU drag (the
        # strands moved, nothing evaluated): the grid's GPU milliseconds (median of 9) and the frame.
        if "grid" in parts():
            grid_bench(groom, api, strands)
        # MSAA on against off (lit lines with width as coverage), at the last count.
        if count == counts()[-1] and "msaa" in parts():
            for msaa in (False, True):
                cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", msaa)
                cmds.refresh(force=True)
                record({"bench": "msaa", "api": api, "strands": strands, "msaa": msaa, "orbitFps": orbit_fps(),
                        "syncedMs": synced_ms()})
            cmds.setAttr("hardwareRenderingGlobals.multiSampleEnable", False)
    if "lod" in parts():
        for count in counts():
            lod_bench(api, count)
    if "lodorbit" in parts():
        # GROOMLAB_BENCH_ORBIT_SCENES: scenes to orbit, "hair_basics" the preset at each count (default).
        for scene in named_modes("GROOMLAB_BENCH_ORBIT_SCENES", "hair_basics"):
            for count in (counts() if scene == "hair_basics" else counts()[:1]):
                lod_orbit_bench(api, count, "" if scene == "hair_basics" else scene)
    if "stroke" in parts():
        for count in counts():
            for sculpt in stroke_sculpts():
                stroke_bench(api, count, sculpt)
    if "chain" in parts():
        for scene in chain_scenes():
            chain_bench(api, scene)
    record({"bench": "view", "api": api, "full": full})
    finish()


def frame_info(groom):
    try:
        return json.loads(cmds.groomLabGraphStats(groom, frameInfo=True))
    except Exception:
        return {}


def settle_and_draw(groom):
    """Waits for a commit still evaluating off the UI thread (groomLabGraphStats waits for it) and draws
    the committed frame: ms."""
    t = time.time()
    cmds.groomLabGraphStats(groom)
    cmds.refresh(currentView=True, force=True)
    sync()
    return (time.time() - t) * 1000.0


def drag_commit(groom, api, strands, mode="lit_coverage"):
    """A GPU drag's release (part "commit"): a Noise drag on the GPU preview, then -endDrag and the value
    committed (set_param) and the view drawn, GPU waited for: the perceived release latency (the UI
    responsive with the final-looking frame drawn). Then the commit waited for (if it evaluates off the
    UI thread) and its frame drawn. Medians of GROOMLAB_BENCH_REPEAT (default 3) releases, after one
    to warm up."""
    import groomlab
    runs = repeats() if os.environ.get("GROOMLAB_BENCH_REPEAT") else 3
    rows = []
    try:
        for i in range(runs + 1):
            value = 0.33 + 0.02 * i
            for v in (value - 0.01, value):
                if not cmds.groomLabGpuPreview(groom, node="noise", param="amplitude", value=v):
                    stats = json.loads(cmds.groomLabGpuPreview(groom, stats=True) or "{}")
                    record({"bench": "drag_commit", "api": api, "strands": strands, "mode": mode, "error": "not taken",
                            "why": stats.get("why")})
                    cmds.groomLabGpuPreview(groom, endDrag=True)
                    return
                cmds.refresh(currentView=True, force=True)
            sync()
            before = frame_info(groom)
            t = time.time()
            cmds.groomLabGpuPreview(groom, endDrag=True)
            groomlab.set_param(groom, "noise", "amplitude", value)
            t1 = time.time()
            cmds.refresh(currentView=True, force=True)
            t2 = time.time()
            sync()
            perceived = (time.time() - t) * 1000.0
            parts_ms = {"commandMs": (t1 - t) * 1000.0, "refreshMs": (t2 - t1) * 1000.0, "syncMs": (time.time() - t2) * 1000.0}
            info = frame_info(groom)
            land = settle_and_draw(groom)
            after = frame_info(groom)
            draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))
            rows.append({"ms": perceived, "pending": bool(info.get("pending")), "landMs": land, "settledBy": after.get("settledBy"), "releaseSettledBy": info.get("settledBy"),
                         "commitMs": after.get("commitMs"), "computeMs": draw.get("computeMs"),
                         "evaluations": after.get("evaluations", 0) - before.get("evaluations", 0),
                         "landUpdateMs": draw["draw"].get("updateMs"), "landUploadMs": draw["draw"].get("uploadMs"), "parts": parts_ms})
    except Exception as e:
        record({"bench": "drag_commit", "api": api, "strands": strands, "mode": mode, "error": str(e)})
        return
    rows = rows[1:]
    rows.sort(key=lambda r: r["ms"])
    record(dict({"bench": "drag_commit", "api": api, "strands": strands, "mode": mode, "runs": [r["ms"] for r in rows],
                 "landRuns": [r["landMs"] for r in rows]}, **rows[len(rows) // 2]))


def stroke_sculpts():
    """GROOMLAB_BENCH_STROKE_SCULPT: the sculpt displays a stroke is timed in: "final" (sculptDisplay
    normal, sculptDensity 1: the GPU's dabs draw the final look) and "default" (dimmed strands at
    sculptDensity 0.35); default both."""
    return named_modes("GROOMLAB_BENCH_STROKE_SCULPT", "final,default")


def hair_basics_scene(count, sculpt="final"):
    """The hair_basics preset on a radius-10 sphere at `count` strands, guides grown from curves with
    children, lit lines, framed; the groom and path(i): the i-th comb stroke's -debugStroke text."""
    import math
    import groomlab
    from groomlab import presets
    from gui_support import to_view
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=10, subdivisionsX=120, subdivisionsY=60)[0]
    groom = groomlab.create_groom(scalp)
    presets.apply_preset(groom, "hair_basics")
    curves = []
    for k in range(24):
        a = 2.0 * math.pi * k / 24.0
        for ring, lift in ((0.45, 0.89), (0.8, 0.6)):
            n = (ring * math.cos(a), lift, ring * math.sin(a))
            curves.append(cmds.curve(degree=1, point=[tuple(10.0 * c for c in n), tuple(13.0 * c for c in n),
                                                      tuple(15.0 * c for c in n)]))
    groomlab.guides_from_curves(groom, curves, tube_radius=1.0)
    cmds.delete(curves)
    groomlab.generate_children(groom, "primary", name="children", count=3)
    groomlab.set_param(groom, "scatter", "density", count / (4.0 * 3.14159265 * 100.0))
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".previewPointBudget", 0.0)
    cmds.setAttr(groom + ".displayGuides", False)
    for attr, value in DEFAULTS.items():
        cmds.setAttr(groom + "." + attr, value)
    cmds.setAttr(groom + ".sculptDisplay", 0 if sculpt == "final" else 1)
    cmds.setAttr(groom + ".sculptDensity", 1.0 if sculpt == "final" else 0.35)
    cmds.setAttr("persp.translate", 0.0, 22.0, 42.0)
    cmds.setAttr("persp.rotate", -25, 0, 0)
    cmds.select(cmds.listRelatives(groom, parent=True)[0])

    def path(i):
        x0, y0 = to_view((-4.0 + 2.0 * (i % 5), 9.2, 3.0))
        return "draw " + " ".join("%d %d" % (x0 + 12 * k, y0 + 3 * k) for k in range(10))
    return groom, path


def scene_groom(path, density):
    """A saved groom scene (a copy: never saved) opened with its Scatter's density set, every strand
    shown, framed from above its back; the groom and path(i) as hair_basics_scene's."""
    import groomlab
    from gui_support import to_view
    cmds.file(path, open=True, force=True, executeScriptNodes=False)
    shape = cmds.ls(type="groomLabShape")[0]
    groomlab.set_param(shape, "scatter", "density", density)
    cmds.setAttr(shape + ".previewDensity", 1.0)
    cmds.setAttr(shape + ".previewPointBudget", 0.0)
    cmds.setAttr(shape + ".displayGuides", False)
    transform = cmds.listRelatives(shape, parent=True)[0]
    cmds.lookThru(persp_panel(), "persp")
    lo = cmds.getAttr(transform + ".boundingBoxMin")[0]
    hi = cmds.getAttr(transform + ".boundingBoxMax")[0]
    center = [(a + b) * 0.5 for a, b in zip(lo, hi)]
    size = max(b - a for a, b in zip(lo, hi))
    # From the side and above: the brush strokes the top of the groom, along its length.
    cmds.setAttr("persp.translate", center[0] + 0.9 * size, center[1] + 0.9 * size, center[2] + 0.2 * size)
    cmds.viewPlace("persp", lookAt=center)
    cmds.select(transform)
    # Where the view's middle meets the scalp: the strokes go across it.
    import maya.api.OpenMaya as om2
    meshes = cmds.listConnections(shape, source=True, destination=False, type="mesh", shapes=True) or cmds.ls(type="mesh")
    eye = om2.MFloatPoint(*cmds.xform("persp", query=True, worldSpace=True, translation=True))
    toward = om2.MFloatVector(center[0] - eye.x, center[1] - eye.y, center[2] - eye.z).normalize()
    hit = None
    for mesh in meshes:
        dag = om2.MSelectionList().add(mesh).getDagPath(0)
        found = om2.MFnMesh(dag).closestIntersection(eye, toward, om2.MSpace.kWorld, 1e9, False)
        if found and found[0] and (hit is None or found[1] < hit[1]):
            hit = (found[0], found[1])
    at = (hit[0].x, hit[0].y, hit[0].z) if hit else tuple(center)
    mx, my = to_view(at)

    def path(i):
        x0, y0 = mx - 60 + 15 * (i % 5), my - 20 + 8 * (i % 5)
        return "draw " + " ".join("%d %d" % (x0 + 12 * k, y0 + 2 * k) for k in range(10))
    return shape, path


def chain_scenes():
    """GROOMLAB_BENCH_CHAIN: what part "chain" strokes, comma separated: hair_basics:<strands>, or
    <scene file>@<Scatter density> (a copy of a groom scene: opened, never saved)."""
    text = os.environ.get("GROOMLAB_BENCH_CHAIN", "hair_basics:500000")
    return [s.strip() for s in text.split(",") if s.strip()]


def machine_load():
    """The GPU's and CPU's busy share right now (nvidia-smi, the processors' load): a run on a
    loaded machine is labeled so."""
    import subprocess
    load = {}
    try:
        out = subprocess.check_output(["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                                      timeout=10).decode().strip()
        load["gpu"] = int(out.splitlines()[0])
    except Exception:
        pass
    try:
        out = subprocess.check_output(["powershell", "-NoProfile", "-Command",
                                       "(Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average"],
                                      timeout=20).decode().strip()
        load["cpu"] = float(out)
    except Exception:
        pass
    return load


def chain_bench(api, scene):
    """Brush strokes chained (part "chain"): on `scene` (chain_scenes), comb strokes drawn with
    -debugStroke, the GPU drawing the dabs. Per stroke: its release (perceived), the commit's
    evaluation off the UI thread (-frameInfo commitMs) and each node's time and kind in it
    (groomLabGraphStats times, runs). Then pairs of strokes, the second pressed right after the
    first's release: its press (ms to its drawn frame) is the time to the next stroke ready;
    -frameInfo says whether it chained on the first's commit (chained) or took it in (commits
    landed between), and the commit after them (pairCommitMs). Medians of
    GROOMLAB_BENCH_REPEAT (default 3) after one stroke to warm up."""
    import groomlab
    if scene.startswith("hair_basics:"):
        groom, path = hair_basics_scene(int(float(scene.split(":")[1])))
    else:
        file_path, density = scene.rsplit("@", 1)
        groom, path = scene_groom(file_path, float(density))
    label = scene if scene.startswith("hair_basics") else os.path.basename(scene)
    strands = cmds.getAttr(groom + ".outStrandCount")
    ctx = groomlab.brush_tool("comb", radius=float(os.environ.get("GROOMLAB_BENCH_CHAIN_RADIUS", "150")), strength=1.0)
    cmds.refresh(currentView=True, force=True)
    sync()
    load_before = machine_load()
    runs = repeats() if os.environ.get("GROOMLAB_BENCH_REPEAT") else 3

    def stroke(i):
        gpu_before = json.loads(cmds.groomLabGpuPreview(groom, stats=True) or "{}").get("stroke", {})
        cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=path(i))
        sync()
        times = [float(t) for t in (cmds.groomLabBrushCtx(ctx, query=True, debugStrokeTimes=True) or "").split()]
        steps = json.loads(cmds.groomLabBrushCtx(ctx, query=True, debugStrokeDraws=True) or "[]")
        gpu_after = json.loads(cmds.groomLabGpuPreview(groom, stats=True) or "{}").get("stroke", {})
        return times, steps, gpu_after.get("runs", 0) - gpu_before.get("runs", 0)

    singles, pairs = [], []
    try:
        for i in range(runs + 1):
            times, steps, dabs = stroke(i)
            info = frame_info(groom)
            land = settle_and_draw(groom)
            after = frame_info(groom)
            stats = json.loads(cmds.groomLabGraphStats(groom))
            singles.append({"release": times[-1] if times else None, "press": times[0] if times else None,
                            "pending": bool(info.get("pending")), "commitMs": after.get("commitMs"), "landMs": land,
                            "gpuDabs": dabs, "times": {k: round(v, 1) for k, v in stats.get("times", {}).items() if v > 0.05},
                            "runs": {k: v for k, v in stats.get("runs", {}).items() if v != "cached"}})
        for i in range(runs + 1):
            stroke(2 * i)
            before = frame_info(groom)
            _, steps, _ = stroke(2 * i + 1)  # pressed right after the first's release
            times = [float(t) for t in (cmds.groomLabBrushCtx(ctx, query=True, debugStrokeTimes=True) or "").split()]
            after = frame_info(groom)
            land = settle_and_draw(groom)
            landed = frame_info(groom)
            pairs.append({"nextPress": times[0] if times else None, "settledBy": after.get("settledBy"),
                          "chained": after.get("chained", 0) - before.get("chained", 0),
                          "landed": after.get("commits", 0) - before.get("commits", 0),
                          "pairLandMs": land, "pairCommitMs": landed.get("commitMs"),
                          "secondDabs": sorted(times[1:-1])[len(times[1:-1]) // 2] if len(times) > 2 else None})
    except Exception as e:
        record({"bench": "chain", "api": api, "scene": label, "strands": strands, "error": str(e)})
        cmds.setToolTo("selectSuperContext")
        return
    cmds.setToolTo("selectSuperContext")
    singles, pairs = singles[1:], pairs[1:]

    def median(rows, key):
        values = sorted(r[key] for r in rows if r.get(key) is not None)
        return values[len(values) // 2] if values else None
    by_commit = sorted(singles, key=lambda r: r["commitMs"] or 0.0)
    record({"bench": "chain", "api": api, "scene": label, "strands": strands, "load": load_before, "loadAfter": machine_load(),
            "commitMs": median(singles, "commitMs"), "commitRuns": [r["commitMs"] for r in singles],
            "releaseMs": median(singles, "release"), "landMs": median(singles, "landMs"),
            "nextPressMs": median(pairs, "nextPress"), "nextPressRuns": [r["nextPress"] for r in pairs],
            "nextPressSettledBy": [r["settledBy"] for r in pairs], "nextPressChained": [r["chained"] for r in pairs],
            "pairLandedBetween": [r["landed"] for r in pairs], "pairLandMs": median(pairs, "pairLandMs"),
            "pairCommitMs": median(pairs, "pairCommitMs"), "secondDabsMs": median(pairs, "secondDabs"),
            "gpuDabs": [r["gpuDabs"] for r in singles],
            "median": by_commit[len(by_commit) // 2]})


def stroke_bench(api, count, sculpt):
    """A comb stroke's release (part "stroke"): the hair_basics preset (its dabs on the GPU) on a
    radius-10 sphere at `count` strands, guides grown from curves with children, lit lines; a drawn
    -debugStroke (press, dabs, release, each step's ms to its redrawn frame), the release's step the
    perceived latency; then the commit waited for and drawn. Medians of GROOMLAB_BENCH_REPEAT
    (default 3) strokes after one to warm up; GROOMLAB_BENCH_STROKE_DETAIL=1 keeps the release's draw breakdown."""
    import groomlab
    groom, path = hair_basics_scene(count, sculpt)
    strands = cmds.getAttr(groom + ".outStrandCount")
    ctx = groomlab.brush_tool("comb", radius=220, strength=1.0)
    cmds.refresh(currentView=True, force=True)
    sync()
    runs = repeats() if os.environ.get("GROOMLAB_BENCH_REPEAT") else 3
    rows = []
    try:
        for i in range(runs + 1):
            stroke = path(i)
            gpu_before = json.loads(cmds.groomLabGpuPreview(groom, stats=True) or "{}").get("stroke", {})
            before = frame_info(groom)
            cmds.groomLabBrushCtx(ctx, edit=True, debugStroke=stroke)
            sync()
            times = [float(t) for t in (cmds.groomLabBrushCtx(ctx, query=True, debugStrokeTimes=True) or "").split()]
            steps = json.loads(cmds.groomLabBrushCtx(ctx, query=True, debugStrokeDraws=True) or "[]")
            release = steps[-1] if steps else {}
            info = frame_info(groom)
            land = settle_and_draw(groom)
            after = frame_info(groom)
            gpu_after = json.loads(cmds.groomLabGpuPreview(groom, stats=True) or "{}").get("stroke", {})
            draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))
            rows.append({"ms": times[-1] if times else None, "press": times[0] if times else None,
                         "dabs": sorted(times[1:-1])[len(times[1:-1]) // 2] if len(times) > 2 else None,
                         "pending": bool(release.get("pending") or info.get("pending")), "landMs": land,
                         "commitMs": after.get("commitMs"), "settledBy": after.get("settledBy"),
                         "gpuDabs": gpu_after.get("runs", 0) - gpu_before.get("runs", 0),
                         "evaluations": after.get("evaluations", 0) - before.get("evaluations", 0),
                         "landUpdateMs": draw["draw"].get("updateMs"),
                         "releaseComputeMs": release.get("computeMs"), "releaseChanged": release.get("changedStrands"),
                         "releaseUpdateMs": release.get("draw", {}).get("updateMs"),
                         "releaseBuildMs": release.get("draw", {}).get("buildMs"),
                         "releaseUploadMs": release.get("draw", {}).get("uploadMs"),
                         "releaseTimes": release.get("stats", {}).get("times"),
                         "releaseDraw": release.get("draw") if os.environ.get("GROOMLAB_BENCH_STROKE_DETAIL") else None})
    except Exception as e:
        record({"bench": "stroke", "api": api, "strands": strands, "sculpt": sculpt, "error": str(e)})
        cmds.setToolTo("selectSuperContext")
        return
    cmds.setToolTo("selectSuperContext")
    rows = [r for r in rows[1:] if r["ms"] is not None]
    if not rows:
        record({"bench": "stroke", "api": api, "strands": strands, "sculpt": sculpt, "error": "no stroke"})
        return
    rows.sort(key=lambda r: r["ms"])
    record(dict({"bench": "stroke", "api": api, "strands": strands, "sculpt": sculpt, "runs": [r["ms"] for r in rows],
                 "landRuns": [r["landMs"] for r in rows]}, **rows[len(rows) // 2]))
def grid_bench(groom, api, strands):
    """The hair_shadows mode's grid, written again by GPU drag steps: its GPU time (median of 9) and the
    synced frame (part "grid")."""
    for attr, value in DEFAULTS.items():
        cmds.setAttr(groom + "." + attr, value)
    for attr, value in dict(MODES)["hair_shadows"].items():
        cmds.setAttr(groom + "." + attr, value)
    cmds.refresh(currentView=True, force=True)
    sync()
    times, cells = [], None
    for i in range(10):
        taken = cmds.groomLabGpuPreview(groom, node="noise", param="amplitude", value=0.3 + 0.02 * i)
        if not taken:
            break
        cmds.refresh(currentView=True, force=True)
        sync()
        grid = json.loads(cmds.groomLabGraphStats(groom, hairShading=True)).get("grid", {})
        cells = grid.get("size")
        if i > 0 and grid.get("gpuMs", -1) >= 0:  # (the first: the drag's own setting up)
            times.append(grid["gpuMs"])
    frame = synced_ms()
    cmds.groomLabGpuPreview(groom, endDrag=True)
    times.sort()
    record({"bench": "grid", "api": api, "strands": strands, "cells": cells, "gridMs": times[len(times) // 2] if times else None,
            "gridMsAll": times, "syncedMs": frame})
    for attr, value in DEFAULTS.items():
        cmds.setAttr(groom + "." + attr, value)


# The framings of the "lod" part: the groom's bounding sphere across this share of the view's height
# (the research's head at 1600, 384, 96 and 32 pixels of 512: docs/research/clump_lod.md).
FRAMINGS = (("near", 3.1), ("mid", 0.75), ("far", 0.19), ("veryfar", 0.0625))


def frame_groom(groom, fill):
    """The persp camera along its view, the groom's bounding sphere across `fill` of the view's height,
    its center of interest the groom's center (an orbit keeps the distance)."""
    import math
    import maya.api.OpenMaya as om
    box = cmds.exactWorldBoundingBox(cmds.listRelatives(groom, parent=True)[0])
    center = om.MVector((box[0] + box[3]) / 2.0, (box[1] + box[4]) / 2.0, (box[2] + box[5]) / 2.0)
    radius = 0.5 * math.sqrt((box[3] - box[0]) ** 2 + (box[4] - box[1]) ** 2 + (box[5] - box[2]) ** 2)
    fov = math.radians(cmds.camera("persp", query=True, verticalFieldOfView=True))
    distance = radius / (fill * math.tan(0.5 * fov))
    eye = om.MVector(*cmds.xform("persp", query=True, worldSpace=True, translation=True))
    direction = (eye - center).normal()
    position = center + direction * distance
    cmds.xform("persp", worldSpace=True, translation=(position.x, position.y, position.z))
    cmds.viewPlace("persp", lookAt=(center.x, center.y, center.z))
    cmds.setAttr("persp.centerOfInterest", distance)
    cmds.camera("persp", edit=True, nearClipPlane=max(0.01, 0.01 * distance), farClipPlane=100.0 * distance + 10.0 * radius)


def lod_modes():
    """GROOMLAB_BENCH_LOD_MODES: the screen-space LOD settings drawn ("off", "on"); default both."""
    return named_modes("GROOMLAB_BENCH_LOD_MODES", "off,on")


def gpu_load():
    """The GPU's utilization (%) and memory used (MiB) from nvidia-smi, sampled 5 times over ~0.5 s
    while GroomLab draws nothing (the most of each): another program loading the GPU (a game) shows
    as utilization; None where nvidia-smi is not there."""
    import subprocess
    most = [0, 0]
    time.sleep(1.5)  # (nvidia-smi's utilization is over its last sample period: not GroomLab's last draws)
    try:
        for _ in range(5):
            out = subprocess.check_output(["nvidia-smi", "--query-gpu=utilization.gpu,memory.used", "--format=csv,noheader,nounits"],
                                          creationflags=0x08000000).decode().strip().splitlines()[0]
            u, m = [int(v.strip()) for v in out.split(",")]
            most = [max(most[0], u), max(most[1], m)]
            time.sleep(0.1)
    except Exception:
        return None
    return {"util": most[0], "memMiB": most[1]}


def lod_groom(count, scene=None):
    """The groom the lod parts draw: `scene` or GROOMLAB_BENCH_LOD_SCENE (a scene opened, its first
    groom, its Scatter's density GROOMLAB_BENCH_LOD_DENSITY where set), else the hair_basics preset
    on a radius-10 sphere at `count` strands. (groom, look)"""
    import groomlab
    scene = os.environ.get("GROOMLAB_BENCH_LOD_SCENE") if scene is None else scene
    if scene:
        cmds.file(scene, open=True, force=True)
        groom = cmds.ls(type="groomLabShape")[0]
        density = os.environ.get("GROOMLAB_BENCH_LOD_DENSITY")
        if density:
            groomlab.set_param(groom, "scatter", "density", float(density))
        return groom, os.path.splitext(os.path.basename(scene))[0]
    cmds.file(new=True, force=True)
    scalp = cmds.polySphere(radius=10, subdivisionsX=120, subdivisionsY=60)[0]
    groom = groomlab.create_groom(scalp)
    from groomlab import presets
    presets.apply_preset(groom, "hair_basics")
    groomlab.set_param(groom, "scatter", "density", count / (4.0 * 3.14159265 * 100.0))
    return groom, "hair_basics"


def surface_point(groom):
    """Where the persp camera's view axis (or the ray nearest it that meets one, out to 25 degrees)
    first meets a mesh (the scalp, a body: the point a user tumbling about the surface in front of
    them pivots on); the groom's center where none does."""
    import math
    import maya.api.OpenMaya as om
    matrix = om.MMatrix(cmds.xform("persp", query=True, worldSpace=True, matrix=True))
    eye = om.MFloatPoint(matrix[12], matrix[13], matrix[14])
    right = om.MFloatVector(matrix[0], matrix[1], matrix[2])
    up = om.MFloatVector(matrix[4], matrix[5], matrix[6])
    look = om.MFloatVector(-matrix[8], -matrix[9], -matrix[10])
    meshes = [om.MFnMesh(om.MSelectionList().add(m).getDagPath(0)) for m in cmds.ls(type="mesh", noIntermediate=True, long=True) or []]
    best = None
    for ring in range(26):
        for k in range(1 if ring == 0 else 12):
            a, t = math.radians(ring), 2.0 * math.pi * k / 12.0
            ray = look * math.cos(a) + (right * math.cos(t) + up * math.sin(t)) * math.sin(a)
            for mesh in meshes:
                hit = mesh.closestIntersection(eye, ray, om.MSpace.kWorld, 1e6, False)
                if hit and hit[2] >= 0 and hit[1] > 0.0 and (best is None or hit[1] < best[1]):
                    best = (hit[0], hit[1])
        if best is not None:
            break
    SURFACE_HIT[0] = best is not None
    if best is None:
        box = cmds.exactWorldBoundingBox(cmds.listRelatives(groom, parent=True)[0])
        return [(box[i] + box[i + 3]) / 2.0 for i in range(3)]
    return [best[0].x, best[0].y, best[0].z]


SURFACE_HIT = [False]  # (whether the last surface_point met a mesh)


def aim_at(point):
    """The persp camera turned in place to look at `point`, its center of interest there."""
    import maya.api.OpenMaya as om
    cmds.viewPlace("persp", lookAt=point)
    eye = om.MVector(*cmds.xform("persp", query=True, worldSpace=True, translation=True))
    cmds.setAttr("perspShape.centerOfInterest", (om.MVector(*point) - eye).length())


def coi_point():
    """Where the persp camera's center of interest is (its eye along its view)."""
    import maya.api.OpenMaya as om
    matrix = om.MMatrix(cmds.xform("persp", query=True, worldSpace=True, matrix=True))
    eye = om.MVector(matrix[12], matrix[13], matrix[14])
    point = eye + om.MVector(-matrix[8], -matrix[9], -matrix[10]) * cmds.getAttr("perspShape.centerOfInterest")
    return [point.x, point.y, point.z]


def frame_all(groom, fill):
    """Maya's own framing (viewFit: everything, its center of interest the scene's center), then the
    camera moved along its view to frame_groom's distance for `fill`, the center of interest kept."""
    import math
    cmds.viewFit("persp", allObjects=True)
    box = cmds.exactWorldBoundingBox(cmds.listRelatives(groom, parent=True)[0])
    radius = 0.5 * math.sqrt((box[3] - box[0]) ** 2 + (box[4] - box[1]) ** 2 + (box[5] - box[2]) ** 2)
    fov = math.radians(cmds.camera("persp", query=True, verticalFieldOfView=True))
    distance = radius / (fill * math.tan(0.5 * fov))
    coi = cmds.getAttr("perspShape.centerOfInterest")
    cmds.move(0, 0, distance - coi, "persp", relative=True, objectSpace=True)
    cmds.setAttr("perspShape.centerOfInterest", distance)
    cmds.camera("persp", edit=True, nearClipPlane=max(0.01, 0.01 * distance), farClipPlane=100.0 * distance + 10.0 * radius)


def lod_orbit_bench(api, count, scene=None):
    """Part "lodorbit": the screen-space level of detail's index rebuilds over a full turn (around,
    and up and down a little) at a fixed distance, near and mid (FRAMINGS), as Maya's tumble tool
    turns the camera, about (GROOMLAB_BENCH_ORBIT_PIVOTS):
      center    the groom's center (frame_groom: the center of interest there; the tool's default,
                "tumble about the center of interest");
      surface   the point on the surface the view looks at (the center of interest moved there);
      tumble    the center of interest Maya's own framing leaves (viewFit), the camera moved along
                its view to the framing's distance;
      selection "tumble on recent selection" (the tool's localTumble 0, the camera's tumble pivot on a
                point of the surface off the view's axis);
      over      the groom's center as "center", the turn over the top instead of around (`orbit`'s
                second angle: the motion the lod part's orbits make);
      free      `orbit -pivotPoint` about the surface point, the center of interest left at the
                groom's center (a pivot the viewport is not told of: no promise there);
    and the frame ms (synced, median) with the level of detail on and off. The groom is lod_groom's
    (GROOMLAB_BENCH_LOD_SCENE for the wolf)."""
    groom, look = lod_groom(count, scene)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".previewPointBudget", 0.0)
    cmds.setAttr(groom + ".displayGuides", False)
    for attr, value in DEFAULTS.items():
        cmds.setAttr(groom + "." + attr, value)
    strands = cmds.getAttr(groom + ".outStrandCount")
    local_tumble = cmds.tumbleCtx("tumbleContext", query=True, localTumble=True)
    frames = int(os.environ.get("GROOMLAB_BENCH_ORBIT_FRAMES", "60"))
    framings = [f for f in FRAMINGS if f[0] in named_modes("GROOMLAB_BENCH_ORBIT_FRAMINGS", "near,mid")]
    for framing, fill in framings:
        for pivot in named_modes("GROOMLAB_BENCH_ORBIT_PIVOTS", "center,over,surface,tumble,selection,free"):
            for mode in lod_modes():
                cmds.setAttr(groom + ".displayClumpLod", mode == "on")
                frame_groom(groom, fill)
                point = None
                if pivot in ("surface", "free"):
                    point = surface_point(groom)
                    if pivot == "surface":
                        aim_at(point)
                elif pivot == "tumble":
                    frame_all(groom, fill)
                elif pivot == "selection":
                    # A point of the surface a little off the view's axis (where the eye looks, turned
                    # a few degrees), as a selection there would set it.
                    cmds.rotate(0, 8, 0, "persp", relative=True, objectSpace=True)
                    point = surface_point(groom)
                    cmds.rotate(0, -8, 0, "persp", relative=True, objectSpace=True)
                    cmds.setAttr("perspShape.tumblePivot", *point)
                    cmds.tumbleCtx("tumbleContext", edit=True, localTumble=0)
                cmds.refresh(currentView=True, force=True)
                sync()
                load = gpu_load()  # (waits 1.5 s: the viewport asks the tumble tool's mode once a second)
                cmds.refresh(currentView=True, force=True)
                before = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["lod"]
                times, lod_ms, levels = [], [], []
                for f in range(frames):
                    step, up = 360.0 / frames, (12.0 if f < frames // 2 else -12.0) * 2.0 / frames
                    if pivot == "free":
                        cmds.orbit("persp", pivotPoint=point, rotationAngles=(step, up))
                    elif pivot == "over":
                        cmds.orbit("persp", pivotPoint=coi_point(), rotationAngles=(0.0, step))
                    else:
                        about = point if pivot == "selection" else coi_point()
                        cmds.tumble("persp", pivotPoint=about, azimuthAngle=step, elevationAngle=up)
                    t = time.time()
                    cmds.refresh(currentView=True, force=True)
                    sync()
                    times.append((time.time() - t) * 1000.0)
                    stats = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["lod"]
                    lod_ms.append(stats["ms"])
                    levels.append(stats["level"])
                after = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["lod"]
                cmds.tumbleCtx("tumbleContext", edit=True, localTumble=local_tumble)
                ordered = sorted(times)
                record({"bench": "lodorbit", "api": api, "look": look, "strands": strands, "framing": framing, "fill": fill,
                        "pivot": pivot, "pivotPoint": point, "surfaceHit": SURFACE_HIT[0] if point else None, "localTumble": local_tumble, "lod": mode, "frames": frames,
                        "rebuilds": after["rebuilds"] - before["rebuilds"], "frameMs": ordered[len(ordered) // 2],
                        "frameMsMax": ordered[-1], "lodMsMax": max(lod_ms), "levels": sorted(set(levels)),
                        "drawnStrands": after["drawnStrands"], "indicesMs": after["indicesMs"], "selectMs": after.get("selectMs"),
                        "slotsWritten": after.get("slotsWritten"), "gpuBefore": load, "loaded": bool(load and load["util"] > 15),
                        "tumblePivot": cmds.getAttr("perspShape.tumblePivot")[0],
                        "centerOfInterest": cmds.getAttr("perspShape.centerOfInterest")})


def lod_bench(api, count):
    """The hair_basics preset (clumped, tapered) on a radius-10 sphere at `count` strands, lit lines with
    width as coverage, at each framing (FRAMINGS) with the screen-space level of detail off and on
    (displayClumpLod): orbit fps and synced frame ms, the medians of GROOMLAB_BENCH_REPEAT runs (default
    3), and the strands drawn. The groom is lod_groom's (GROOMLAB_BENCH_LOD_SCENE for the wolf)."""
    groom, look = lod_groom(count)
    cmds.setAttr(groom + ".previewDensity", 1.0)
    cmds.setAttr(groom + ".previewPointBudget", 0.0)
    cmds.setAttr(groom + ".displayGuides", False)
    for attr, value in DEFAULTS.items():
        cmds.setAttr(groom + "." + attr, value)
    strands = cmds.getAttr(groom + ".outStrandCount")
    has_lod = cmds.attributeQuery("displayClumpLod", node=groom, exists=True)
    runs = max(3, repeats()) if os.environ.get("GROOMLAB_BENCH_REPEAT") is None else repeats()
    for framing, fill in FRAMINGS:
        frame_groom(groom, fill)
        for mode in lod_modes():
            if has_lod:
                cmds.setAttr(groom + ".displayClumpLod", mode == "on")
            elif mode == "on":
                continue
            cmds.refresh(currentView=True, force=True)
            sync()
            load = gpu_load()
            rebuilds = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]["lod"]["rebuilds"]
            fps, ms = [], []
            for _ in range(runs):
                fps.append(orbit_fps())
                ms.append(synced_ms())
            fps.sort()
            ms.sort()
            row = {"bench": "lod", "api": api, "look": look, "strands": strands, "framing": framing, "fill": fill, "lod": mode,
                   "orbitFps": fps[len(fps) // 2], "syncedMs": ms[len(ms) // 2], "orbitFpsRuns": fps, "syncedMsRuns": ms,
                   "view": view_size(), "gpuBefore": load, "loaded": bool(load and load["util"] > 15),
                   "orbitFrames": runs * (FRAMES + 20)}
            try:
                draw = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))["draw"]
                if "lod" in draw and mode == "on":  # (off: the last on's)
                    row["lodStats"] = draw["lod"]
                    row["orbitRebuilds"] = draw["lod"]["rebuilds"] - rebuilds
            except Exception:
                pass
            record(row)


def playback(groom, scalp, api, strands):
    """Frames of a deforming scalp (a bend keyed on it): each a CPU evaluation, new points and root
    normals (and, here, new strand counts: the scatter follows the stretched surface)."""
    deformer, handle = cmds.nonLinear(scalp, type="bend")[:2]
    cmds.setKeyframe(deformer, attribute="curvature", time=1, value=0.0)
    cmds.setKeyframe(deformer, attribute="curvature", time=20, value=40.0)
    cmds.currentTime(1)
    cmds.refresh(currentView=True, force=True)
    sync()
    rows = []
    for frame in range(2, 9):
        t = time.time()
        cmds.currentTime(frame)
        cmds.refresh(currentView=True, force=True)
        sync()
        ms = (time.time() - t) * 1000.0
        stats = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))
        rows.append({"ms": ms, "computeMs": stats.get("computeMs"), "updateMs": stats["draw"]["updateMs"],
                     "buildMs": stats["draw"]["buildMs"], "uploadMs": stats["draw"]["uploadMs"],
                     "points": json.loads(cmds.groomLabGraphStats(groom))["points"],
                     "mb": sum(v.get("megabytes", 0) for v in stats["draw"]["items"].values()),
                     "items": dict((k, dict((f, v.get(f)) for f in ("megabytes", "acquireMs", "writeMs", "commitMs", "bindMs",
                                                                     "gpuFrames", "gpuParts", "gpuMB")))
                                   for k, v in stats["draw"]["items"].items())})
    rows.sort(key=lambda r: r["ms"])
    record(dict({"bench": "playback", "api": api, "strands": strands}, **rows[len(rows) // 2]))
    cmds.delete(handle)
    cmds.currentTime(1)
    cmds.refresh(currentView=True, force=True)


def follow_modes():
    """GROOMLAB_BENCH_FOLLOW_MODES: the display modes (MODES' names) Follow Scalp playback is timed in;
    default lit_coverage and ribbons_lit."""
    return named_modes("GROOMLAB_BENCH_FOLLOW_MODES", "lit_coverage,ribbons_lit")


def follow_evaluations():
    """GROOMLAB_BENCH_FOLLOW_EVAL: how Follow Scalp playback evaluates, of live (the dependency graph,
    cached playback off: each frame evaluated) and cached (cached playback on: the first pass fills
    it, the others draw from it); default both."""
    return named_modes("GROOMLAB_BENCH_FOLLOW_EVAL", "live,cached")


def follow_playback(groom, scalp, api, strands, mode="lit_coverage"):
    """Frames of a deforming scalp (a bend keyed on it) with Follow Scalp on (a rest mesh, the groom
    evaluated at rest once and each strand carried by its root), live and with cached playback: played
    three times over, the median frame of each pass and of the passes (cached: of those drawing from
    the cache)."""
    import groomlab
    from gui_cached_playback import cache_manager, cache_off, cache_on
    was = {"enabled": cache_manager().enabled, "mode": cmds.evaluationManager(query=True, mode=True)[0]}
    for attr, value in DEFAULTS.items():
        cmds.setAttr(groom + "." + attr, value)
    for attr, value in dict(MODES)[mode].items():
        cmds.setAttr(groom + "." + attr, value)
    cmds.currentTime(1)
    groomlab.set_rest_mesh(groom)
    groomlab.set_follow_scalp(groom, True)
    deformer, handle = cmds.nonLinear(scalp, type="bend")[:2]
    cmds.setKeyframe(deformer, attribute="curvature", time=1, value=0.0)
    cmds.setKeyframe(deformer, attribute="curvature", time=20, value=40.0)
    for evaluation in follow_evaluations():
        if evaluation == "cached":
            cache_on()
        else:
            cache_off()
        cmds.currentTime(1)
        cmds.refresh(currentView=True, force=True)
        sync()
        rows, passes = [], []
        for p in range(3):
            times = []
            for frame in range(2, 9):
                info = json.loads(cmds.groomLabGraphStats(groom, frameInfo=True))
                t = time.time()
                cmds.currentTime(frame)
                t1 = time.time()
                cmds.refresh(currentView=True, force=True)
                t2 = time.time()
                sync()
                ms = (time.time() - t) * 1000.0
                times.append(ms)
                after = json.loads(cmds.groomLabGraphStats(groom, frameInfo=True))
                stats = json.loads(cmds.groomLabGraphStats(groom, drawTimes=True))
                if evaluation == "cached" and p == 0:
                    continue  # (filling the cache)
                rows.append({"ms": ms, "timeMs": (t1 - t) * 1000.0, "refreshMs": (t2 - t1) * 1000.0,
                             "evaluations": after["evaluations"] - info["evaluations"],
                             "background": after["backgroundEvaluations"] - info["backgroundEvaluations"],
                             "computeMs": stats.get("computeMs"), "updateMs": stats["draw"]["updateMs"],
                             "buildMs": stats["draw"]["buildMs"], "uploadMs": stats["draw"]["uploadMs"],
                             "mb": sum(v.get("megabytes", 0) for v in stats["draw"]["items"].values()),
                             "items": dict((k, dict((f, v.get(f)) for f in ("megabytes", "writeMs", "commitMs", "bindMs", "gpuFrames",
                                                                             "gpuParts", "gpuMB", "follow")))
                                           for k, v in stats["draw"]["items"].items())})
            times.sort()
            passes.append(times[len(times) // 2])
            cmds.currentTime(1)
            cmds.refresh(currentView=True, force=True)
            sync()
        rows.sort(key=lambda r: r["ms"])
        median = rows[len(rows) // 2]
        record(dict({"bench": "follow_playback", "api": api, "strands": strands, "mode": mode, "evaluation": evaluation,
                     "passMedians": passes, "fps": 1000.0 / max(median["ms"], 1e-6)}, **median))
    cache_manager().enabled = was["enabled"]
    cmds.evaluationManager(mode=was["mode"])
    cmds.delete(handle)
    groomlab.set_follow_scalp(groom, False)
    cmds.currentTime(1)
    cmds.refresh(currentView=True, force=True)
    for attr, value in DEFAULTS.items():
        cmds.setAttr(groom + "." + attr, value)

def gpu_drag(groom, api, strands, mode="lit_coverage"):
    try:
        took, times, gpu = [], [], []
        for i in range(8):
            t = time.time()
            took.append(bool(cmds.groomLabGpuPreview(groom, node="noise", param="amplitude", value=0.31 + 0.01 * i)))
            cmds.refresh(currentView=True, force=True)
            sync()
            times.append((time.time() - t) * 1000.0)
            gpu.append(json.loads(cmds.groomLabGpuPreview(groom, stats=True, wait=True) or "{}").get("gpu", {}))
        cmds.groomLabGpuPreview(groom, end=True)
        cmds.refresh(currentView=True, force=True)
        times = sorted(times[2:])
        stats = json.loads(cmds.groomLabGpuPreview(groom, stats=True) or "{}")
        record({"bench": "gpu_drag", "api": api, "strands": strands, "mode": mode, "taken": all(took), "ms": times[len(times) // 2],
                "gpu": gpu[-1], "why": stats.get("why")})
    except Exception as e:
        record({"bench": "gpu_drag", "api": api, "strands": strands, "mode": mode, "error": str(e)})
