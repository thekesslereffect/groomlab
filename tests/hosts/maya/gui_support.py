# SPDX-License-Identifier: Apache-2.0
"""The GUI session's harness: runs the GUI suites (gui_<name>.py) one after another in one Maya
window, puts the session back between them, and gives the suites what they share.

gui_runner.py (exec'd by the generated gui.mel) calls run(). Each suite module has start(), which
runs its checks through later() or soon() steps and ends with finish(). Checks write
"[suite] PASS|FAIL label" lines to the log (GROOMLAB_GUI_LOG); a unittest suite's report goes there
too, each line marked with its suite. The log ends with a summary naming every failure by suite and
test, and Maya quits with exit code 0 when everything passed. A GroomLab warning that one of its
shaders did not compile, or one of its shading fragments did not register, fails the suite it came
in (FAILING_WARNINGS), on whatever device.

GROOMLAB_GUI_SUITES (a comma-separated list: brush,viewport) runs only those suites;
GROOMLAB_GUI_CASES (parts of test names: ctrl_z,set_key) only those editor cases (gui_graph_editor) and
cached playback scenes (gui_cached_playback: simulate,background).

Nothing here moves the real mouse or needs the window to have the OS focus: hover() and drag() send
Qt events to the widgets, and activate() makes a window Qt's active one without asking Windows.
"""

import importlib
import os
import re
import time
import traceback

from maya import cmds


# The default graph before it took a look (Noise, HairColor, uneven Length): the cases that edit
# that chain by its node names (length -> width, a node named color) start from it.
CLASSIC_GRAPH = {
    "version": 1, "output": "output",
    "nodes": [{"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
              {"name": "scatter", "type": "Scatter"}, {"name": "interpolate", "type": "Interpolate"},
              {"name": "length", "type": "Length"}, {"name": "width", "type": "Width"},
              {"name": "resample", "type": "Resample"}, {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "interpolate", "input": "strands"},
                    {"from": "guides", "to": "interpolate", "input": "guides"},
                    {"from": "scalp", "to": "interpolate", "input": "scalp"},
                    {"from": "interpolate", "to": "length", "input": "strands"},
                    {"from": "length", "to": "width", "input": "strands"},
                    {"from": "width", "to": "resample", "input": "strands"},
                    {"from": "resample", "to": "output", "input": "strands"}],
}

# The viewport's frame budget first, on a fresh session; then the graph editor's cases; quitting
# with the panels open last (the session's quit is its final check).
SUITES = ("viewport", "brush", "select", "reference", "graph_editor", "cached_playback", "updates", "render", "quit")
# Suites that run only when GROOMLAB_GUI_SUITES names them (look-over tools, not checks): the
# presets' contact sheets (gui_presets.py, ctest maya.preset_sheet), and the viewport benchmark
# (gui_bench.py, ctest maya.viewport_bench), and the viewport's pictures before and after a display change
# (gui_pictures.py).
OPT_IN_SUITES = ("presets", "bench", "pictures", "hair")


class _Session(object):
    log = None
    suite = None        # the suite running now
    failures = []       # (suite, what) for the summary
    finished = set()    # suites that called finish()
    on_finish = None
    started = 0.0
    baseline = {}


S = _Session()


# -- Writing the log ----------------------------------------------------------------------------

def say(*args):
    text = " ".join(str(a) for a in args)
    prefix = "[%s] " % S.suite if S.suite else ""
    for line in text.splitlines() or [""]:
        S.log.write(prefix + line + "\n")
    S.log.flush()


def check(label, ok):
    say(("PASS " if ok else "FAIL ") + label)
    if not ok:
        S.failures.append((S.suite or "session", label))
    return ok


def fail(what):
    """A failure that is not a check: an exception, a unittest failure."""
    S.failures.append((S.suite or "session", what))


class LogStream(object):
    """A stream for unittest's report: each line marked with the suite."""

    def __init__(self):
        self._pending = ""

    def write(self, text):
        self._pending += text
        while "\n" in self._pending:
            line, self._pending = self._pending.split("\n", 1)
            say(line)

    def flush(self):
        pass  # unittest flushes after "test ... ": the line ends with its result

    def close(self):
        if self._pending:
            say(self._pending)
            self._pending = ""


# -- Steps --------------------------------------------------------------------------------------

def finish():
    """The running suite is done: the session moves on to the next one (after this call returns)."""
    suite = S.suite
    if suite is None or suite in S.finished:
        return
    S.finished.add(suite)
    say("done in %.1f s, %d failures" % (time.time() - S.started,
                                          sum(1 for s, _ in S.failures if s == suite)))
    cmds.evalDeferred(S.on_finish, lowestPriority=True)


def guarded(fn):
    """fn as a deferred step of the running suite: an exception fails the suite and ends it."""
    suite = S.suite

    def run():
        if S.suite != suite or suite in S.finished:
            return  # the suite ended already
        try:
            fn()
        except Exception:
            say(traceback.format_exc())
            fail("exception in %s: %s" % (getattr(fn, "__name__", "a step"),
                                          traceback.format_exc().strip().splitlines()[-1]))
            finish()
    return run


def later(fn):
    """Run fn once Maya's idle work is done. All of fn is one undo chunk (as a user's next action),
    open while fn runs: fn cannot undo its own steps one by one."""
    cmds.evalDeferred(guarded(fn), lowestPriority=True)


def soon(fn):
    """Run fn on Maya's next idle (maya.utils.executeDeferred), outside any undo chunk: each of its
    commands is an undo step of its own, as when a script runs them (the unittest suites)."""
    import maya.utils
    maya.utils.executeDeferred(guarded(fn))


# -- Shared helpers -----------------------------------------------------------------------------

def to_view(point):
    """View coordinates of a world point in the active 3D view."""
    import maya.OpenMaya as om
    import maya.OpenMayaUI as omui
    view = omui.M3dView.active3dView()
    x, y = om.MScriptUtil(), om.MScriptUtil()
    x.createFromInt(0)
    y.createFromInt(0)
    xp, yp = x.asShortPtr(), y.asShortPtr()
    view.worldToView(om.MPoint(*point), xp, yp)
    return om.MScriptUtil.getShort(xp), om.MScriptUtil.getShort(yp)


def persp_panel():
    return next(p for p in cmds.getPanel(type="modelPanel") if cmds.modelPanel(p, query=True, camera=True) == "persp")


def process_events(ms=0):
    """Let Qt run events and timers for `ms` milliseconds (PySide2 has no QTest.qWait)."""
    from PySide2 import QtWidgets
    end = time.time() + ms / 1000.0
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.005)
    QtWidgets.QApplication.processEvents()


_marks = [0]


def settle_undo(rounds=20):
    """Run Maya's deferred idle work (File New's callbacks, selection mask resets, ...) until it
    adds no more undo steps, so that the steps a test makes next are the only ones on top of the
    queue: a test's undo then takes back its own step, and its redo is not cleared by a stray one.

    Lays an undoable marker step and lets idle work and Qt's events run; when the marker is still
    on top, nothing came after it. Returns the marker's name (on top of the queue)."""
    from PySide2 import QtWidgets
    for _ in range(rounds):
        _marks[0] += 1
        name = "glTestMark%d" % _marks[0]
        cmds.undoInfo(openChunk=True, chunkName=name)
        try:
            cmds.setAttr("time1.outTime", lock=False)  # an undoable step that changes nothing
        finally:
            cmds.undoInfo(closeChunk=True)
        for _ in range(3):
            cmds.flushIdleQueue()
            QtWidgets.QApplication.processEvents()
        if cmds.undoInfo(query=True, undoName=True) == name:
            return name
    return None


def activate(widget):
    """Make widget's window Qt's active window and give widget the keyboard focus, without asking
    the OS: shortcuts and key events then work while the user is busy in another application."""
    from PySide2 import QtCore, QtWidgets
    QtWidgets.QApplication.setActiveWindow(widget.window())
    widget.setFocus(QtCore.Qt.OtherFocusReason)
    QtWidgets.QApplication.processEvents()
    return QtWidgets.QApplication.focusWidget() is widget


def hover(widget, pos=None):
    """Put the (Qt) mouse over widget at pos (its center by default) without moving the real
    cursor: the window gets the enter and move events the window system would send, so Qt
    dispatches enter and leave to the widgets under that point (QWidget.underMouse())."""
    from PySide2 import QtCore, QtGui, QtWidgets
    pos = widget.rect().center() if pos is None else pos
    window = widget.window()
    handle = window.windowHandle()
    local = QtCore.QPointF(widget.mapTo(window, pos))
    screen = QtCore.QPointF(widget.mapToGlobal(pos))
    app = QtWidgets.QApplication
    app.sendEvent(handle, QtCore.QEvent(QtCore.QEvent.Leave))
    app.sendEvent(handle, QtGui.QEnterEvent(local, local, screen))
    app.sendEvent(handle, QtGui.QMouseEvent(QtCore.QEvent.MouseMove, local, local, screen, QtCore.Qt.NoButton,
                                            QtCore.Qt.NoButton, QtCore.Qt.NoModifier))
    app.processEvents()
    return widget.underMouse()


def clear_of_cursor(window):
    """Move a test's window off the real cursor (the user's own mouse moves would otherwise reach
    it between hover()'s events and the checks): to the side of its screen the cursor is not on."""
    from PySide2 import QtGui, QtWidgets
    cursor = QtGui.QCursor.pos()
    frame = window.frameGeometry()
    if not frame.contains(cursor):
        return
    screen = QtWidgets.QApplication.desktop().availableGeometry(window)
    x = screen.left() if cursor.x() > screen.center().x() else max(screen.left(), screen.right() - frame.width())
    window.move(x, window.y())
    QtWidgets.QApplication.processEvents()


def under_point(widget, pos=None):
    """Why hover(widget) may have missed, for an assertion's message: what the window has at the
    widget's point, a popup taking the mouse, the real cursor over the window (its moves win)."""
    from PySide2 import QtGui, QtWidgets
    pos = widget.rect().center() if pos is None else pos
    window = widget.window()
    local = widget.mapTo(window, pos)

    def name(w):
        if w is None:
            return "nothing"
        return "%s %r glParam=%r" % (type(w).__name__, w.objectName(), w.property("glParam"))
    cursor = QtGui.QCursor.pos()
    return ("%s (visible %s, %dx%d at %s): the window has %s there; popup %s; real cursor %s, over the "
            "window: %s; under the cursor: %s"
            % (name(widget), widget.isVisible(), widget.width(), widget.height(), local, name(window.childAt(local)),
               name(QtWidgets.QApplication.activePopupWidget()), cursor,
               window.frameGeometry().contains(cursor), name(QtWidgets.QApplication.widgetAt(cursor))))


def drag(widget, start, end, steps=2, button=None, modifiers=None):
    """A left-button drag on widget from start to end (widget coordinates), sent as events (QTest's
    mouseMove would move the real cursor)."""
    from PySide2 import QtCore, QtGui, QtTest, QtWidgets
    button = QtCore.Qt.LeftButton if button is None else button
    modifiers = QtCore.Qt.NoModifier if modifiers is None else modifiers
    QtTest.QTest.mousePress(widget, button, modifiers, start)
    for k in range(1, steps + 1):
        at = QtCore.QPoint(int(round(start.x() + (end.x() - start.x()) * k / float(steps))),
                           int(round(start.y() + (end.y() - start.y()) * k / float(steps))))
        QtWidgets.QApplication.sendEvent(widget, QtGui.QMouseEvent(QtCore.QEvent.MouseMove, QtCore.QPointF(at),
                                                                   QtCore.QPointF(widget.mapToGlobal(at)),
                                                                   button, button, modifiers))
    QtTest.QTest.mouseRelease(widget, button, modifiers, end)


# -- Between suites -----------------------------------------------------------------------------

def _job_ids():
    ids = set()
    for job in cmds.scriptJob(listJobs=True) or []:
        head = job.split(":", 1)[0].strip()
        if head.isdigit():
            ids.add(int(head))
    return ids


def _workspace_controls():
    found = {}
    for name in cmds.lsUI(type="workspaceControl") or []:
        try:
            found[name] = cmds.workspaceControl(name, query=True, visible=True)
        except RuntimeError:
            pass
    return found


def _top_level_widgets():
    import shiboken2
    from PySide2 import QtWidgets
    return {shiboken2.getCppPointer(w)[0] for w in QtWidgets.QApplication.topLevelWidgets() if w.isVisible()}


# The size every session's Maya window takes, whatever its prefs saved: brush radii are in
# pixels and the suites frame the camera to the view, so view-dependent checks see the same
# picture on any machine and in a new build folder's first session.
WINDOW_SIZE = (1600, 1000)


def pin_view():
    """The Maya window at WINDOW_SIZE, showing the perspective view alone."""
    import maya.mel
    import maya.OpenMayaUI as omui
    import shiboken2
    from PySide2 import QtWidgets
    window = shiboken2.wrapInstance(int(omui.MQtUtil.mainWindow()), QtWidgets.QMainWindow)
    window.showNormal()
    window.setGeometry(40, 40, WINDOW_SIZE[0], WINDOW_SIZE[1])
    # The docks a prefs folder may have open take the view's room: hidden, so the view is the
    # same size in every session (a suite opening one gets it back when it asks).
    for dock in ("ChannelBoxLayerEditor", "AttributeEditor", "ToolSettings", "Outliner", "MayaHome"):
        if cmds.workspaceControl(dock, exists=True) and cmds.workspaceControl(dock, query=True, visible=True):
            cmds.workspaceControl(dock, edit=True, visible=False)
    drop_groomlab_docks()  # GroomLab's, restored from the last session's workspace
    import groomlab
    groomlab.pin_groom(None)  # no groom pinned or adopted (adopt_scene_groom) by the last session
    maya.mel.eval('setNamedPanelLayout "Single Perspective View"')
    QtWidgets.QApplication.processEvents()
    view = omui.M3dView.active3dView()
    say("view %dx%d (window %dx%d)" % (view.portWidth(), view.portHeight(), WINDOW_SIZE[0], WINDOW_SIZE[1]))


def drop_groomlab_docks():
    """Delete GroomLab's docks (the Groom Tools, the Graph Editor). They carry a uiScript, so Maya
    keeps them in the prefs folder's workspace when it quits with them open and rebuilds them at
    the next start, where they would take the view's room."""
    for name in cmds.lsUI(type="workspaceControl") or []:
        if name.startswith("groomLab"):
            try:
                cmds.deleteUI(name)
            except RuntimeError:
                pass


def take_baseline():
    """What the session looks like before the first suite: put back after each one."""
    panel = persp_panel()
    S.baseline = {
        "jobs": _job_ids(),
        "controls": _workspace_controls(),
        "widgets": _top_level_widgets(),
        "panel": panel,
        "editor": {flag: cmds.modelEditor(panel, query=True, **{flag: True})
                   for flag in ("displayLights", "shadows", "displayAppearance", "displayTextures",
                                "grid", "headsUpDisplay", "polymeshes", "useDefaultMaterial",
                                "wireframeOnShaded", "xray")},
        "playback": {flag: cmds.playbackOptions(query=True, **{flag: True})
                     for flag in ("loop", "playbackSpeed", "maxPlaybackSpeed")},
        "hotkeys": cmds.hotkeySet(query=True, current=True),
        "workspace": cmds.workspace(query=True, rootDirectory=True),
        "tool": cmds.currentCtx(),
    }


def reset_session():
    """Puts the session back as the baseline had it: the next suite starts as in a fresh Maya.

    Tools, pinned groom, Auto Key, GroomLab's (and any) windows, palettes and docks opened since,
    scriptJobs made since, the perspective panel's camera and display, playback options, hotkey
    set, project, then a new scene, its deferred callbacks run, and an empty undo queue."""
    import shiboken2
    from PySide2 import QtWidgets
    base = S.baseline
    try:
        import groomlab
        groomlab.pin_groom(None)
    except Exception:
        pass
    cmds.setToolTo(base["tool"] if cmds.contextInfo(base["tool"], exists=True) else "selectSuperContext")
    cmds.autoKeyframe(state=False)
    if cmds.play(query=True, state=True):
        cmds.play(state=False)
    # Docks and windows opened since the start: GroomLab's deleted, Maya's own closed again.
    for name, visible in _workspace_controls().items():
        if name not in base["controls"]:
            try:
                cmds.deleteUI(name)
            except RuntimeError:
                pass
        elif visible and not base["controls"][name]:
            try:
                cmds.workspaceControl(name, edit=True, visible=False)
            except RuntimeError:
                pass
    for widget in QtWidgets.QApplication.topLevelWidgets():
        if widget.isVisible() and shiboken2.getCppPointer(widget)[0] not in base["widgets"]:
            widget.close()
    process_events(20)
    for job in _job_ids() - base["jobs"]:
        try:
            cmds.scriptJob(kill=job, force=True)
        except RuntimeError:
            pass
    panel = base["panel"]
    if cmds.modelPanel(panel, exists=True):
        cmds.lookThru(panel, "persp")
        cmds.modelEditor(panel, edit=True, **base["editor"])
    cmds.playbackOptions(**base["playback"])
    if base["hotkeys"] and cmds.hotkeySet(base["hotkeys"], exists=True):
        cmds.hotkeySet(base["hotkeys"], edit=True, current=True)
    if base["workspace"]:
        cmds.workspace(base["workspace"], openWorkspace=True)
    cmds.file(new=True, force=True)
    cmds.flushIdleQueue()
    process_events(20)
    cmds.flushIdleQueue()
    cmds.flushUndo()
    cmds.undoInfo(state=True, infinity=True)
    if cmds.modelPanel(panel, exists=True):
        cmds.setFocus(panel)
    cmds.refresh(force=True)


# A suite still running after this long has stopped: a step that never comes (a deferred call
# lost, a wait for something that never happens) would otherwise leave Maya idle until ctest's
# timeout, with nothing in the log. GROOMLAB_GUI_SUITE_TIMEOUT (seconds) overrides it.
SUITE_TIMEOUT = float(os.environ.get("GROOMLAB_GUI_SUITE_TIMEOUT", "300"))


def watch(suite):
    """A Qt timer (it fires while Maya sits idle) that fails `suite` and moves the session on when
    it has not finished within SUITE_TIMEOUT, with the Python stacks and the time and evaluation
    state in the log."""
    from PySide2 import QtCore

    def expired():
        if S.suite != suite or suite in S.finished:
            return
        import sys
        say("the suite did not finish within %g s: stopped (current time %s, playing %s)"
            % (SUITE_TIMEOUT, cmds.currentTime(query=True), cmds.play(query=True, state=True)))
        for thread, frame in sys._current_frames().items():
            say("thread %s:\n%s" % (thread, "".join(traceback.format_stack(frame))))
        fail("timed out after %g s" % SUITE_TIMEOUT)
        finish()

    QtCore.QTimer.singleShot(int(SUITE_TIMEOUT * 1000), expired)


# GroomLab's warnings that fail the session wherever they come: a viewport shader of the plugin
# that did not compile on this device, or a shading fragment or graph of its (the cards', the lit
# strands' own colors) that did not register or could not be adapted (their items then draw with a
# stand-in, which no check of a picture may notice).
FAILING_WARNINGS = re.compile(r"GroomLab: .*(shader did not compile|did not register|could not be adapted)")


def listen_for_warnings():
    """A callback on Maya's command output failing the running suite (or the session) on any of
    FAILING_WARNINGS, the message in the log. Returns its id."""
    import maya.api.OpenMaya as om

    def heard(message, kind, _data):
        if kind in (om.MCommandMessage.kWarning, om.MCommandMessage.kError) and FAILING_WARNINGS.search(message):
            say("warning: " + message.strip())
            S.failures.append((S.suite or "session", "warning: " + message.strip().splitlines()[0]))

    return om.MCommandMessage.addCommandOutputCallback(heard)


# -- The session --------------------------------------------------------------------------------

def run(quit_maya=True):
    """Runs the suites (all, or those GROOMLAB_GUI_SUITES names) in this Maya session."""
    S.log = open(os.environ.get("GROOMLAB_GUI_LOG", os.devnull), "w")
    wanted = [s.strip() for s in os.environ.get("GROOMLAB_GUI_SUITES", "").split(",") if s.strip()]
    unknown = [s for s in wanted if s not in SUITES + OPT_IN_SUITES]
    queue = [s for s in SUITES if not wanted or s in wanted] + [s for s in OPT_IN_SUITES if s in wanted]
    session_start = time.time()
    timings = []
    import maya.api.OpenMayaRender as omr
    # Viewport 2.0's device: OpenGL with fresh prefs, DirectX 11 under maya.gui_dx11
    # (MAYA_VP2_DEVICE_OVERRIDE); their buffers behave differently.
    say("draw API: %s" % {omr.MRenderer.kDirectX11: "DirectX 11", omr.MRenderer.kOpenGLCoreProfile: "OpenGL Core Profile",
                          omr.MRenderer.kOpenGL: "OpenGL"}.get(omr.MRenderer.drawAPI(), str(omr.MRenderer.drawAPI())))
    # OpenGL: the GPU preview's kernels compiled before the suites (a session's first groom drawn
    # starts them in the background, its drags and strokes the CPU's for the seconds they take), so
    # each suite's first drag or stroke finds them ready.
    started = time.time()
    ready = cmds.groomLabGpuPreview(kernels=True, wait=True)
    say("GPU kernels ready: %s (%.1f s)" % (ready, time.time() - started))
    warnings = listen_for_warnings()

    def end():
        import maya.api.OpenMaya as om
        om.MMessage.removeCallback(warnings)
        S.suite = None
        say("")
        for name, seconds in timings:
            say("suite %-13s %6.1f s" % (name, seconds))
        for name in unknown:
            S.failures.append((name, "no such GUI suite (known: %s)" % ", ".join(SUITES + OPT_IN_SUITES)))
        missing = [name for name, _ in timings if name not in S.finished]
        for name in missing:
            S.failures.append((name, "the suite did not finish"))
        # Cached playback (on in fresh prefs) never entered its safe mode with the suites' grooms.
        if cmds.pluginInfo("cacheEvaluator", query=True, loaded=True) and \
                int(cmds.cacheEvaluator(query=True, safeModeTriggered=True)):
            why = (cmds.cacheEvaluator(query=True, safeModeMessages=True) or "").strip()
            say("cached playback entered its safe mode: " + why)
            S.failures.append(("session", "cached playback entered its safe mode: " + why.splitlines()[0] if why else ""))
        say("%d suites in %.1f s, %d failures" % (len(timings), time.time() - session_start, len(S.failures)))
        for suite, what in S.failures:
            say("FAILED %s: %s" % (suite, what))
        say("OK" if not S.failures else "FAILED")
        S.log.close()
        drop_groomlab_docks()  # not kept in the prefs' workspace for the next session
        if quit_maya:
            cmds.quit(force=True, exitCode=1 if S.failures else 0)

    def next_suite():
        if S.suite is not None:
            timings.append((S.suite, time.time() - S.started))
        if not queue:
            end()
            return
        name = queue.pop(0)
        S.suite = None
        try:
            reset_session()
        except Exception:
            S.suite = name
            say(traceback.format_exc())
            fail("the session could not be reset before it")
        S.suite = name
        S.started = time.time()
        say("=" * 24 + " " + name + " " + "=" * 24)
        watch(name)
        try:
            module = importlib.import_module("gui_" + name)
            module.start()
        except Exception:
            say(traceback.format_exc())
            fail("exception starting the suite: " + traceback.format_exc().strip().splitlines()[-1])
            finish()

    S.on_finish = next_suite
    try:
        pin_view()
    except Exception:
        say(traceback.format_exc())
        fail("could not pin the view's size")
    try:
        take_baseline()
    except Exception:
        say(traceback.format_exc())
        fail("could not read the session's state")
    cmds.evalDeferred(next_suite, lowestPriority=True)
