# SPDX-License-Identifier: Apache-2.0
"""GUI test of Check for Updates (needs a Maya UI session): GroomLab > Check for Updates... is a plain
window, never modal; this session's plugin (the build tree: a developer build) says updates are off
and asks nothing; pointed at a fake packaged install and a fake downloads site on localhost
(fake_downloads.py), the window shows the newer version and its notes, the Groom Tools show the quiet
note, and Update Now downloads, verifies and installs it beside the install, ending with "Restart
Maya to finish". Nothing goes to the real downloads domain. A suite of the GUI session
(gui_runner.py, ctest maya.gui; harness: gui_support.py).
"""

import time

from gui_support import check, finish, process_events, say, soon

state = {}


def start():
    state.clear()
    soon(setup)


def _guard_dialogs():
    """Any modal dialog that opens fails the suite and is closed: a scripted session never waits."""
    from groomlab.ui.qt import QtCore, QtWidgets

    def close_modal():
        for widget in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(widget, QtWidgets.QDialog) and widget.isVisible() and widget.isModal():
                check("no modal dialog (%s %r)" % (type(widget).__name__, widget.windowTitle()), False)
                widget.reject()

    timer = QtCore.QTimer()
    timer.timeout.connect(close_modal)
    timer.start(200)
    state["guard"] = timer


def _wait(window, then, timeout=20.0):
    """then() once the window's check or install is done (each poll on Maya's next idle)."""
    deadline = time.time() + timeout

    def poll():
        process_events(50)
        if window.busy() and time.time() < deadline:
            soon(poll)
            return
        check("the window's work finished in time", not window.busy())
        soon(then)
    soon(poll)


def setup():
    from groomlab import updates
    from groomlab.ui import menu
    _guard_dialogs()
    check("the session's plugin is a developer build", updates.off_reason() == updates.DEV_MESSAGE)
    window = menu.open_updates()
    process_events(50)
    check("the window is open and not modal", window.isVisible() and not window.isModal())
    check("it says updates are off (%r)" % window.status.text(),
          window.status.text() == "Updates are off for this install (developer build)")
    check("no Update Now, no Check Again", not window.update_button.isVisible() and not window.check_button.isEnabled())
    check("it shows the installed version (%r)" % window.installed.text(), window.installed.text().startswith("Installed:"))
    window.close()
    # A packaged install and its downloads site, both fake.
    from fake_downloads import FakeDownloads
    site = FakeDownloads()
    site.publish(site.release_zip())
    site.point(updates)
    state["site"] = site
    window = menu.open_updates()
    state["window"] = window
    _wait(window, found)


def found():
    from groomlab.ui import palette
    window = state["window"]
    check("a newer version (%r)" % window.status.text(), "GroomLab 0.2.0 is available" in window.status.text())
    check("its notes are shown", window.notes.isVisible() and "Faster brushes." in window.notes.text())
    check("Update Now is offered", window.update_button.isVisible() and window.update_button.isEnabled())
    tools = palette.show()
    process_events(50)
    check("the Groom Tools show the quiet note (%r)" % tools.update_note.text(),
          tools.update_note.isVisible() and "GroomLab 0.2.0 is available" in tools.update_note.text())
    window.update_button.click()
    _wait(window, installed)


def installed():
    from groomlab.ui import palette
    window, site = state["window"], state["site"]
    check("installed: restart to finish (%r)" % window.status.text(), "Restart Maya to finish" in window.status.text())
    check("the new version is beside the old one (%s)" % site.installed_folders(),
          site.installed_folders() == ["GroomLab-0.1.0", "GroomLab-0.2.0"])
    with open(site.mod) as f:
        check("GroomLab.mod points at it", "GroomLab-0.2.0/GroomLab" in f.read())
    check("the Groom Tools' note went", not palette.show().update_note.isVisible())
    soon(done)


def done():
    from groomlab import updates
    window = state.get("window")
    if window is not None:
        window.close()
    state["site"].close()
    updates._announce(None)
    state["guard"].stop()
    check("updates read this install again", updates.off_reason() == updates.DEV_MESSAGE)
    say("updates suite done")
    finish()
