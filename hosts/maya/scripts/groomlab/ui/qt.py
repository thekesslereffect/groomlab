# SPDX-License-Identifier: Apache-2.0
"""Qt for GroomLab's UI in every Maya: one set of names over PySide2 (Qt 5: Maya 2022 to 2024) and
PySide6 (Qt 6: Maya 2025 and later). Every UI module imports Qt from here, never PySide2 or PySide6:

    from groomlab.ui.qt import QtCore, QtGui, QtWidgets

The binding is the one the running Maya's Qt needs (cmds.about(qtVersion=True)); outside Maya the
first of PySide6 and PySide2 that imports, or GROOMLAB_QT_API ("PySide2" or "PySide6") when set
(the offscreen checks of both: tests/hosts/maya/qt_compat_check.py).

What moved or changed between the two, as the UI uses it:
    QAction, QActionGroup, QShortcut   QtWidgets in Qt 5, QtGui in Qt 6: use qt.QAction...
    exec_(obj, *args)                  a dialog's or a menu's exec (exec_ in PySide2, exec in PySide6)
    event_pos(e), event_global_pos(e)  a QMouseEvent's or QHelpEvent's position as a QPoint (pos() and
                                       globalPos() became position() and globalPosition() in Qt 6)
    to_int(value)                      an enum or flags value as an int (PySide6's are Python enums)
    QtSvg()                            the QtSvg module (QSvgRenderer), imported when first needed
    shiboken                           shiboken2 or shiboken6 (isValid, delete, wrapInstance)
"""

import os
import sys

API = ""


def _wanted():
    """The binding to import: GROOMLAB_QT_API, else the running Maya's Qt, else "" (try both)."""
    forced = os.environ.get("GROOMLAB_QT_API", "").strip()
    if forced:
        return forced
    if "maya.cmds" in sys.modules or "maya" in sys.modules:
        try:
            from maya import cmds
            version = str(cmds.about(qtVersion=True) or "")
            if version[:1] in ("5", "6"):
                return "PySide2" if version.startswith("5") else "PySide6"
        except Exception:
            pass
    return ""


_api = _wanted()
if _api == "PySide2":
    from PySide2 import QtCore, QtGui, QtWidgets
    import shiboken2 as shiboken
elif _api == "PySide6":
    from PySide6 import QtCore, QtGui, QtWidgets
    import shiboken6 as shiboken
else:
    try:
        from PySide6 import QtCore, QtGui, QtWidgets
        import shiboken6 as shiboken
        _api = "PySide6"
    except ImportError:
        from PySide2 import QtCore, QtGui, QtWidgets
        import shiboken2 as shiboken
        _api = "PySide2"
API = _api
QT6 = API == "PySide6"

if QT6:
    QAction = QtGui.QAction
    QActionGroup = QtGui.QActionGroup
    QShortcut = QtGui.QShortcut
else:
    QAction = QtWidgets.QAction
    QActionGroup = QtWidgets.QActionGroup
    QShortcut = QtWidgets.QShortcut


def QtSvg():
    """The QtSvg module of the binding (QSvgRenderer is there in both)."""
    if QT6:
        from PySide6 import QtSvg as module
    else:
        from PySide2 import QtSvg as module
    return module


def QtTest():
    """The QtTest module of the binding (the tests' QTest), else (Maya 2027's PySide6 ships none)
    _EventTest: the same calls made of plain events."""
    try:
        if QT6:
            from PySide6 import QtTest as module
        else:
            from PySide2 import QtTest as module
    except ImportError:
        return _EventTest
    return module


class _EventTest(object):
    """A stand-in for the QtTest module: QTest's mouse, key and wait calls the tests make, sent as
    events to the widget (no real cursor or keyboard; mouseMove moves no cursor either)."""

    class QTest(object):
        _TEXT = {"Key_Return": "\r", "Key_Enter": "\r", "Key_Escape": "\x1b", "Key_Tab": "\t",
                 "Key_Backspace": "\b", "Key_Space": " "}

        @staticmethod
        def _mouse(kind, widget, button, modifiers, pos, buttons):
            pos = widget.rect().center() if pos is None else pos
            local = QtCore.QPointF(pos)
            event = QtGui.QMouseEvent(kind, local, QtCore.QPointF(widget.mapToGlobal(pos)), button, buttons,
                                      QtCore.Qt.NoModifier if modifiers is None else modifiers)
            QtWidgets.QApplication.sendEvent(widget, event)

        @classmethod
        def mousePress(cls, widget, button, modifiers=None, pos=None, delay=-1):
            cls._mouse(QtCore.QEvent.MouseButtonPress, widget, button, modifiers, pos, button)

        @classmethod
        def mouseRelease(cls, widget, button, modifiers=None, pos=None, delay=-1):
            cls._mouse(QtCore.QEvent.MouseButtonRelease, widget, button, modifiers, pos, QtCore.Qt.NoButton)

        @classmethod
        def mouseClick(cls, widget, button, modifiers=None, pos=None, delay=-1):
            cls.mousePress(widget, button, modifiers, pos)
            cls.mouseRelease(widget, button, modifiers, pos)

        @classmethod
        def mouseDClick(cls, widget, button, modifiers=None, pos=None, delay=-1):
            cls.mouseClick(widget, button, modifiers, pos)
            cls._mouse(QtCore.QEvent.MouseButtonDblClick, widget, button, modifiers, pos, button)
            cls.mouseRelease(widget, button, modifiers, pos)

        @classmethod
        def mouseMove(cls, widget, pos=None, delay=-1):
            cls._mouse(QtCore.QEvent.MouseMove, widget, QtCore.Qt.NoButton, None, pos, QtCore.Qt.NoButton)

        @classmethod
        def keyClick(cls, widget, key, modifiers=None, delay=-1):
            modifiers = QtCore.Qt.NoModifier if modifiers is None else modifiers
            if isinstance(key, str):  # a character: its key, and the character as the text
                text, code = key, ord(key.upper()) if len(key) == 1 else 0
            else:
                code = int(getattr(key, "value", key))
                name = getattr(key, "name", "")
                text = cls._TEXT.get(name if isinstance(name, str) else "", "")
                if not text and 0x20 <= code <= 0x7E:  # a letter, digit or sign: lower case unless Shift
                    text = chr(code) if modifiers & QtCore.Qt.ShiftModifier else chr(code).lower()
                if modifiers & QtCore.Qt.ControlModifier:
                    text = ""
            for kind in (QtCore.QEvent.KeyPress, QtCore.QEvent.KeyRelease):
                QtWidgets.QApplication.sendEvent(widget, QtGui.QKeyEvent(kind, code, modifiers, text))

        @classmethod
        def keyClicks(cls, widget, text, modifiers=None, delay=-1):
            for char in text:
                cls.keyClick(widget, char, modifiers)

        @staticmethod
        def qWait(ms):
            timer = QtCore.QElapsedTimer()
            timer.start()
            while timer.elapsed() < ms:
                QtWidgets.QApplication.processEvents(QtCore.QEventLoop.AllEvents, 10)


def exec_(obj, *args):
    """obj.exec(*args): a QDialog's, a QMenu's, a QMessageBox's (PySide2 names it exec_)."""
    run = getattr(obj, "exec", None) if QT6 else getattr(obj, "exec_", None)
    return (run or getattr(obj, "exec_"))(*args)


def event_pos(event):
    """A widget event's position in the widget as a QPoint (QMouseEvent, QHelpEvent, QDropEvent...)."""
    position = getattr(event, "position", None)
    if position is not None:
        return position().toPoint()
    return event.pos()


def event_global_pos(event):
    """A widget event's position on the screen as a QPoint."""
    position = getattr(event, "globalPosition", None)
    if position is not None:
        return position().toPoint()
    return event.globalPos()


def to_int(value):
    """An enum or flags value (Qt.ControlModifier, event.modifiers()...) as an int: PySide2's convert
    with int(); PySide6's are Python enums, whose number is .value."""
    try:
        return int(value)
    except TypeError:
        return int(value.value)


def wrap_instance(pointer, cls):
    """A Python wrapper of type cls for a C++ object's address (Maya's MQtUtil pointers)."""
    return shiboken.wrapInstance(int(pointer), cls)
