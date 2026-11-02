# SPDX-License-Identifier: Apache-2.0
"""GUI test for quitting Maya with GroomLab's panels open (needs a Maya UI session).

Maya does not delete its docks' widgets when it quits, nor finalize Python, so shiboken2's static
cleanup walks every wrapper still alive as the process exits. It crashed there now and then, after
every suite had passed: a widget of the Groom Tools held a wrapper of Qt's own style sheet style
(PySide2 keeps one from a widget whose style() is called), and when the cleanup freed that wrapper
before reaching the widget, the widget's reference led it back into freed memory. This suite
checks both halves of the fix: the panels repolish without calling style() (widgets.repolish),
and the step Maya's quit runs (kMayaExiting: widgets.run_exit_steps) stops the panels' callbacks
and deletes their widgets. It then opens the panels again and is the last suite, so the session's
own quit runs that step for real (ctest checks Maya's exit code).

A suite of the GUI session (gui_runner.py, ctest maya.gui; harness: gui_support.py).
"""

from maya import cmds

from gui_support import check, finish, process_events, soon

state = {}


def start():
    state.clear()
    soon(open_panels)


def open_panels():
    """The panels open on a new groom and repolish what changes role (the first-groom steps, the
    editor's status): a dynamic property's rule applies, and no widget's style() is called."""
    import groomlab
    from groomlab.ui.qt import QtGui, QtWidgets
    from groomlab.ui import graph_editor, palette
    cmds.file(new=True, force=True)
    cmds.optionVar(intValue=(palette.STARTER_VAR, 0))  # the first-groom steps show (they repolish)
    scalp = cmds.polySphere(radius=1, subdivisionsX=20, subdivisionsY=10)[0]
    groom = groomlab.create_groom(scalp)
    cmds.select(groom)
    calls = []
    style = QtWidgets.QWidget.style

    def counted(widget):
        calls.append(type(widget).__name__)
        return style(widget)
    QtWidgets.QWidget.style = counted
    try:
        tools = palette.show()
        editor = graph_editor.show(groom)
        process_events(50)
        tools.poll()
        label = editor.status
        editor.report("cannot apply: error")
        trouble = label.palette().color(QtGui.QPalette.WindowText).name()
        editor.report("saved preset x")
        calm = label.palette().color(QtGui.QPalette.WindowText).name()
    finally:
        QtWidgets.QWidget.style = style
    state.update(groom=groom, tools=tools, editor=editor)
    check("the first-groom steps are showing", tools.starter.isVisible())
    check("the status label takes its role's color when the role changes (%s, then %s)" % (trouble, calm),
          trouble != calm)
    check("no widget's style() is called (it would keep Qt's style sheet style alive from it: %s)"
          % (", ".join(sorted(set(calls))) or "none"), not calls)
    soon(quit_step)


def quit_step():
    """What Maya's quit runs (kMayaExiting): the panels stop following Maya and their widgets go."""
    from groomlab.ui import graph_editor, palette, widgets
    editor, tools = state["editor"], state["tools"]
    jobs = [j for j in (editor._selection_job, editor._time_job) if j is not None]
    check("the editor follows the selection and the time", len(jobs) == 2)
    widgets.run_exit_steps()
    check("the Graph Editor's widget is deleted", not widgets.alive(editor))
    check("the Groom Tools' widget is deleted", not widgets.alive(tools))
    check("the panels let go of them", graph_editor._window is None and palette._window is None)
    check("the editor's scriptJobs are killed", not any(cmds.scriptJob(exists=j) for j in jobs))
    process_events(50)  # nothing fires into the deleted widgets
    cmds.currentTime(cmds.currentTime(query=True) + 1)
    cmds.select(state["groom"])
    process_events(50)
    # Open again (their docks, kept for Maya to save, are made anew): the session's quit, right
    # after this suite, runs the step for real.
    palette.show()
    graph_editor.show(state["groom"])
    process_events(50)
    check("opening the panels again registers the quit step", bool(widgets._exit_steps))
    state.clear()
    finish()
