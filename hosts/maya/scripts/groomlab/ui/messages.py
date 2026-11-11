# SPDX-License-Identifier: Apache-2.0
"""Errors said in plain words with a next step: over the viewport (inViewMessage) and as a warning
on Maya's command line, the full traceback in the Script Editor for whoever wants it.

    from groomlab.ui import messages
    try:
        ...
    except Exception as e:
        messages.error("Export Alembic", e)

plain(error) turns the errors Python raises from deep inside (a missing module, a file that can't be
written, values that don't unpack) into what happened and what to do about it; anything else is
said as raised.
"""

import re
import sys
import traceback

# (pattern, words): the first match says it; {0} is the pattern's first group (a path, a name).
_PLAIN = (
    (r"No module named '?xgenm'?",
     "XGen isn't loaded: load xgenToolkit in Windows > Settings/Preferences > Plug-in Manager, then try again"),
    (r"No module named '?(pgYetiMaya\w*)'?",
     "Yeti isn't loaded: load pgYetiMaya in the Plug-in Manager, then try again"),
    (r"No module named '?([\w.]+)'?",
     "the Python module {0} isn't available in this Maya: check the installation"),
    (r"not enough values to unpack|too many values to unpack",
     "the values typed don't match what was asked: type each one asked for, separated by spaces"),
    (r"\[Errno 13\][^:]*:?\s*'?([^']*)'?",
     "can't write {0}: it may be open in another program, or the folder is read-only; close it or pick another "
     "place"),
    (r"\[Errno 2\][^:]*:?\s*'?([^']*)'?",
     "{0} doesn't exist: check the path (Utilities > Find Missing Files... relinks moved files)"),
    (r"\[Errno 28\]", "the disk is full: free some space or pick another drive"),
    (r"\[Errno 36\]|File name too long", "the file's path is too long: pick a shorter folder or name"),
)


def plain(error):
    """An error (an exception or its text) in plain words, with a next step when it is one of the
    known kinds; else its own text (or its type's name when it has none)."""
    text = str(error).strip() if error is not None else ""
    if not text and isinstance(error, BaseException):
        text = type(error).__name__
    for pattern, words in _PLAIN:
        match = re.search(pattern, text)
        if match:
            group = match.group(1) if match.groups() else ""
            return words.format(group.strip() or "the file")
    text = re.sub(r"^(RuntimeError|ValueError|OSError|IOError):\s*", "", text)
    text = re.sub(r"^GroomLab:\s*", "", text)
    return text


def say(text, warning=True, seconds=5, markup=False):
    """`text` over the viewport (not in batch) and on the command line (a warning, or printed): every
    GroomLab message over the viewport goes through here. `markup`: the text's <hl>...</hl> shows
    highlighted over the viewport (the command line gets it without the tags); else a "<" is text."""
    from maya import cmds
    line = "GroomLab: " + (re.sub(r"</?hl>", "", text) if markup else text)
    if warning:
        cmds.warning(line)
    else:
        print(line)
    try:
        if not cmds.about(batch=True):
            cmds.inViewMessage(amg="GroomLab: " + (text if markup else text.replace("<", "&lt;")), pos="topCenter",
                               fade=True, fadeStayTime=int(seconds * 1000))
    except RuntimeError:
        pass


def error(what, err, trace=True):
    """`what` (an action's name: "Export Alembic") failed with `err`: said in plain words over the
    viewport and as a warning, the traceback printed for the Script Editor. Returns the words."""
    words = plain(err)
    traced = trace and sys.exc_info()[0] is not None
    if traced:
        traceback.print_exc()  # the Script Editor: the details for a bug report
    unexpected = not isinstance(err, (RuntimeError, ValueError))  # GroomLab's own say what to do
    say("%s failed: %s%s" % (what, words, " (details in the Script Editor)" if traced and unexpected else ""))
    return words


def done(text, seconds=4):
    """A finished action said over the viewport and printed: "wrote groom.abc (24 frames)"."""
    say(text, warning=False, seconds=seconds)


def is_cancelled(err):
    """Whether `err` is a frame loop the user stopped with Esc (the export commands' "cancelled")."""
    return "cancelled (Esc)" in str(err)


class busy(object):
    """While a long action runs: the wait cursor and its status in Maya's main progress bar (with
    the steps when `steps` is given: step() moves it on; cancelled() says whether Esc was pressed).
    Nothing in batch. A frame loop inside a GroomLab command shows its own frames over it.

        with messages.busy("Exporting Alembic curves"):
            ...
    """

    def __init__(self, status, steps=0, interruptable=False):
        self.status, self.steps, self.interruptable = status, steps, interruptable
        self.bar = ""

    def __enter__(self):
        from maya import cmds, mel
        if cmds.about(batch=True):
            return self
        cmds.waitCursor(state=True)
        try:
            self.bar = mel.eval("$tmp = $gMainProgressBar")
            cmds.progressBar(self.bar, edit=True, beginProgress=True, isInterruptable=self.interruptable,
                             status="GroomLab: %s..." % self.status, minValue=0, maxValue=max(1, self.steps))
        except RuntimeError:
            self.bar = ""
        cmds.refresh(force=False)  # the cursor and the status show before the work starts
        return self

    def step(self, status=None):
        from maya import cmds
        if self.bar:
            extra = {"status": "GroomLab: %s..." % status} if status else {}
            cmds.progressBar(self.bar, edit=True, step=1, **extra)

    def cancelled(self):
        from maya import cmds
        return bool(self.bar) and cmds.progressBar(self.bar, query=True, isCancelled=True)

    def __exit__(self, *_exc):
        from maya import cmds
        if cmds.about(batch=True):
            return False
        if self.bar:
            try:
                cmds.progressBar(self.bar, edit=True, endProgress=True)
            except RuntimeError:
                pass
        try:
            if cmds.waitCursor(query=True, state=True):
                cmds.waitCursor(state=False)
        except RuntimeError:
            pass
        return False
