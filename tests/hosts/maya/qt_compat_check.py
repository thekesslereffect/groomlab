# SPDX-License-Identifier: Apache-2.0
"""GroomLab's Maya UI under PySide2 and PySide6, without Maya: every module imports and the main
windows build, show, paint and take a few events, offscreen. Maya 2022 to 2024 run Qt 5 (PySide2),
2025 and later Qt 6 (PySide6); the UI reaches Qt only through groomlab.ui.qt. The GUI session
(maya.gui) tests the UI for real in Maya 2022; this is the check for the bindings no Maya here runs.

    python tests/hosts/maya/qt_compat_check.py --api PySide6 --core <folder of groomlab_core*.pyd>
                                               [--dll-dir <folder of tbb.dll / tbb12.dll>]...

Run it with a CPython of the Maya's own minor version, PySide installed (pip: PySide2 5.15.2.1 under
3.9 or 3.10, PySide6-Essentials 6.5.3 under 3.11 for Maya 2025 and 2026, 6.8.3 under 3.13 for Maya
2027), and that Maya's groomlab_core module (build/ninja-maya<year>/engine/python/RelWithDebInfo; it
needs the TBB of that Maya: --dll-dir). Maya itself is a stand-in (maya.cmds answers every command
with an empty result): what is checked is Qt, not Maya. Exit code 0 when all of it passed.
"""

import argparse
import os
import sys
import tempfile
import traceback
import types
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SCRIPTS = os.path.join(ROOT, "hosts", "maya", "scripts")

LISTS = ("ls", "listRelatives", "listConnections", "listAttr", "listHistory", "lsUI", "getPanel", "keyframe",
         "listSets", "scriptJob", "pluginInfo")


def _fake_maya(api, prefs):
    """maya, maya.cmds, maya.mel, maya.utils, maya.api.OpenMaya, maya.OpenMayaUI, maya.app...: every
    command answers with an empty result; the ones the UI reads a value of answer one."""
    import groomlab_core

    def answer(name, args, kwargs):
        if name == "about":
            if kwargs.get("qtVersion") or kwargs.get("qt"):
                return "6.5.3" if api == "PySide6" else "5.15.2"
            if kwargs.get("apiVersion") or kwargs.get("api"):
                return 20250000 if api == "PySide6" else 20240000
            if kwargs.get("batch") or kwargs.get("b"):
                return False
            if kwargs.get("version") or kwargs.get("v"):
                return "2025" if api == "PySide6" else "2024"
            return ""
        if name == "groomLabOperators":
            return groomlab_core.operator_specs_json()
        if name == "groomLabDefaultGraph":
            return groomlab_core.default_graph_json()
        if name == "groomLabVersion":
            return "0.0.0"
        if name == "internalVar":
            return prefs + "/"
        if name == "optionVar":
            return False if kwargs.get("exists") else 0
        if name in ("objExists", "workspaceControl", "window", "control", "about"):
            return False
        if name in LISTS:
            return []
        return None

    class Commands(types.ModuleType):
        def __getattr__(self, name):
            if name.startswith("__"):
                raise AttributeError(name)
            return lambda *args, **kwargs: answer(name, args, kwargs)

    maya = types.ModuleType("maya")
    maya.__path__ = []
    cmds = Commands("maya.cmds")
    mel = types.ModuleType("maya.mel")
    mel.eval = lambda *args, **kwargs: ""
    utils = types.ModuleType("maya.utils")
    utils.executeDeferred = lambda fn, *a, **k: None
    utils.executeInMainThreadWithResult = lambda fn, *a, **k: fn(*a, **k)
    api_pkg = types.ModuleType("maya.api")
    api_pkg.__path__ = []
    om = mock.MagicMock(name="maya.api.OpenMaya")
    omui = mock.MagicMock(name="maya.OpenMayaUI")
    omui.MQtUtil.dpiScale.side_effect = lambda value: value
    omui.MQtUtil.mainWindow.return_value = None
    app = types.ModuleType("maya.app")
    app.__path__ = []
    general = types.ModuleType("maya.app.general")
    general.__path__ = []
    mixin = types.ModuleType("maya.app.general.mayaMixin")
    mixin.MayaQWidgetBaseMixin = type("MayaQWidgetBaseMixin", (object,), {})
    mixin.MayaQWidgetDockableMixin = type("MayaQWidgetDockableMixin", (object,), {})
    maya.cmds, maya.mel, maya.utils, maya.api, maya.OpenMayaUI, maya.app = cmds, mel, utils, api_pkg, omui, app
    maya.OpenMaya = om
    api_pkg.OpenMaya = om
    app.general = general
    general.mayaMixin = mixin
    sys.modules.update({"maya": maya, "maya.cmds": cmds, "maya.mel": mel, "maya.utils": utils, "maya.api": api_pkg,
                        "maya.api.OpenMaya": om, "maya.OpenMaya": om, "maya.OpenMayaUI": omui, "maya.app": app,
                        "maya.app.general": general, "maya.app.general.mayaMixin": mixin})


class Check(object):
    def __init__(self):
        self.failures = []
        self.passed = 0

    def run(self, label, fn):
        try:
            fn()
        except Exception:
            self.failures.append((label, traceback.format_exc()))
            print("FAIL  %s\n%s" % (label, traceback.format_exc()))
        else:
            self.passed += 1
            print("ok    %s" % label)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--api", choices=("PySide2", "PySide6"), required=True)
    parser.add_argument("--core", required=True, help="the folder of the groomlab_core module for this Python")
    parser.add_argument("--dll-dir", action="append", default=[], help="a folder of DLLs the module needs (TBB)")
    parser.add_argument("--shots", help="a folder for PNGs of the fill fields (fill_field_check)")
    args = parser.parse_args()

    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    # Qt 5's offscreen platform finds no fonts by itself (text then isn't drawn): the system's.
    fonts = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
    if os.path.isdir(fonts):
        os.environ.setdefault("QT_QPA_FONTDIR", fonts)
    os.environ["GROOMLAB_QT_API"] = args.api
    os.environ["GROOMLAB_NO_UPDATES"] = "1"
    prefs = tempfile.mkdtemp(prefix="groomlab-qt-check-")
    os.environ["GROOMLAB_UPDATES_DIR"] = prefs
    for folder in args.dll_dir + [args.core]:
        os.add_dll_directory(os.path.abspath(folder))
    sys.path[:0] = [SCRIPTS, os.path.abspath(args.core)]
    _fake_maya(args.api, prefs)

    check = Check()
    print("Python %s, %s" % (sys.version.split()[0], args.api))

    from groomlab.ui import qt
    check.run("groomlab.ui.qt picks %s" % args.api, lambda: _expect(qt.API == args.api, qt.API))
    QtCore, QtGui, QtWidgets = qt.QtCore, qt.QtGui, qt.QtWidgets
    print("Qt %s, %s %s" % (QtCore.qVersion(), qt.API, getattr(__import__(args.api), "__version__", "?")))
    application = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])

    # Every module of the package imports.
    import importlib
    import pkgutil
    import groomlab
    names = ["groomlab." + m.name for m in pkgutil.iter_modules(groomlab.__path__)]
    names += ["groomlab.ui." + m.name for m in pkgutil.iter_modules([os.path.join(SCRIPTS, "groomlab", "ui")])]
    for name in sorted(names):
        check.run("import " + name, lambda n=name: importlib.import_module(n))

    from groomlab import graph as gm
    from groomlab.ui import graph_editor, palette, updates_window, expression_editor, widgets
    from groomlab.ui import tool_tabs
    import groomlab_core

    def settle(ms=30):
        timer = QtCore.QElapsedTimer()
        timer.start()
        while timer.elapsed() < ms:
            application.processEvents()

    def shown(widget, size=(900, 600)):
        widget.resize(*size)
        widget.show()
        settle()
        image = widget.grab()  # paints every visible child
        _expect(not image.isNull(), "grab")
        return widget

    QTest = qt.QtTest().QTest
    editor_box = {}

    def graph_editor_builds():
        editor = graph_editor.GraphEditor()
        editor.catalog = gm.OperatorCatalog.from_json(groomlab_core.operator_specs_json())
        editor.model = gm.GraphModel(editor.catalog, groomlab_core.default_graph())
        editor.rebuild()
        shown(editor, (1200, 600))
        editor_box["editor"] = editor

    def graph_editor_params():
        editor = editor_box["editor"]
        for name in list(editor.items):
            editor.scene.clearSelection()
            editor.items[name].setSelected(True)
            settle(5)
            editor.params.grab()

    def graph_editor_events():
        editor = editor_box["editor"]
        view = editor.view
        view.setFocus()
        QTest.keyClick(view.viewport(), QtCore.Qt.Key_F)
        QTest.keyClick(view, QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)  # Maya's undo (stand-in)
        QTest.keyClick(view, QtCore.Qt.Key_Y, QtCore.Qt.ControlModifier)
        middle = QtCore.QPoint(200, 200)
        QTest.mousePress(view.viewport(), QtCore.Qt.MiddleButton, QtCore.Qt.NoModifier, middle)
        QTest.mouseMove(view.viewport(), QtCore.QPoint(260, 240))
        QTest.mouseRelease(view.viewport(), QtCore.Qt.MiddleButton, QtCore.Qt.NoModifier, QtCore.QPoint(260, 240))
        wheel = QtGui.QWheelEvent(QtCore.QPointF(200, 200), QtCore.QPointF(200, 200), QtCore.QPoint(0, 0),
                                  QtCore.QPoint(0, 120), QtCore.Qt.NoButton, QtCore.Qt.NoModifier,
                                  QtCore.Qt.NoScrollPhase, False)
        QtWidgets.QApplication.sendEvent(view.viewport(), wheel)
        settle()

        class Key(object):
            def __init__(self, key, mods):
                self._key, self._mods = key, mods

            def key(self):
                return self._key

            def modifiers(self):
                return self._mods

        _expect(graph_editor.undo_key(Key(QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier)), "Ctrl+Z is undo")
        _expect(graph_editor.redo_key(Key(QtCore.Qt.Key_Z, QtCore.Qt.ControlModifier | QtCore.Qt.ShiftModifier)),
                "Ctrl+Shift+Z is redo")
        _expect(not graph_editor.undo_key(Key(QtCore.Qt.Key_Z, QtCore.Qt.NoModifier)), "Z alone is not undo")

    def palette_builds():
        shown(palette.ToolPalette(), (420, 900))

    def palette_cards():
        """Every tool's card and every tab painted; chips wrap (FlowLayout), tiles keep their size
        (ToolGrid's height for a width), the binding's virtuals reached from Python."""
        tools = shown(palette.ToolPalette(), (380, 900))
        tools.empty.setVisible(False)
        tools.work.setVisible(True)
        for tool in [None] + list(palette.TOOL_SETTINGS):
            tools.tool = tool
            tools.show_tool_settings()
            settle(5)
            tools.grab()
        for tab in palette.TABS:
            tools.tabs.setCurrentIndex(palette.TABS.index(tab))
            settle(5)
            tools.grab()
        tools.tabs.setCurrentIndex(0)
        row = widgets.chip_row([widgets.chip("Chip %d" % i, checked=False) for i in range(6)])
        _expect(row.hasHeightForWidth() and row.heightForWidth(80) > row.heightForWidth(800), "chips wrap")
        tiles = [QtWidgets.QToolButton() for _ in range(10)]
        for b in tiles:
            b.setFixedSize(34, 34)
        grid = widgets.ToolGrid(tiles)
        _expect(grid.hasHeightForWidth() and grid.heightForWidth(38 * 5) == 2 * 34 + 4 and grid.heightForWidth(400) == 34,
                "tiles: %d, %d" % (grid.heightForWidth(38 * 5), grid.heightForWidth(400)))
        shown(grid, (200, 80))
        _expect(tiles[0].size() == QtCore.QSize(34, 34) and grid.columns == 5, "five to a 200 px row: %r" % grid.columns)

    def updates_builds():
        window = updates_window.UpdatesWindow()
        shown(window, (520, 420))

    def expression_builds():
        edit = expression_editor.CodeEdit("width = 0.1 * u\n", lines=4)
        shown(edit, (400, 200))
        shown(expression_editor.VariablesEditor({"a": 1.0}), (400, 300))
        from groomlab.ui import script_editor  # the script node editor's code field and params table
        code = script_editor.ScriptEdit("per point {\n    P += N * t;\n}\n")
        shown(code, (400, 200))
        code.mark_errors([{"part": "script", "line": 2, "column": 5, "message": "x"}])
        table = script_editor.ParamsTable([{"name": "amount", "type": "length", "default": 0.5, "mappable": True}])
        shown(table, (500, 160))
        assert table.params()[0]["mappable"] and table.params()[0]["default"] == 0.5

    def ramp_events():
        ramp = widgets.RampEditor([[0.0, 0.0], [1.0, 1.0]])
        shown(ramp, (300, 160))
        QTest.mouseMove(ramp, QtCore.QPoint(150, 80))
        QTest.mousePress(ramp, QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, QtCore.QPoint(150, 80))
        QTest.mouseMove(ramp, QtCore.QPoint(160, 60))
        QTest.mouseRelease(ramp, QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, QtCore.QPoint(160, 60))
        _expect(len(ramp.keys) == 3, "a key added: %r" % ramp.keys)

    def slider_builds():
        slider = shown(widgets.SoftSlider(0.5, 0.0, 1.0), (300, 26))
        QTest.mouseClick(slider, QtCore.Qt.LeftButton, QtCore.Qt.NoModifier, QtCore.QPoint(150, 13))
        _expect(slider.isEditing(), "a click types")
        slider.editor.setText("0.25*2+0.25")
        QTest.keyClick(slider.editor, QtCore.Qt.Key_Return)
        _expect(slider.value() == 0.75, "typed arithmetic: %r" % slider.value())

    def new_map_dialog():
        dialog = tool_tabs.NewMapDialog(None)
        shown(dialog, (360, 240))
        QtCore.QTimer.singleShot(50, dialog.reject)
        _expect(qt.exec_(dialog) == QtWidgets.QDialog.Rejected, "the dialog's exec returns Rejected")

    def menu_exec():
        menu = QtWidgets.QMenu()
        action = menu.addAction("Item")
        _expect(isinstance(action, qt.QAction), "addAction returns qt.QAction")
        group = qt.QActionGroup(menu)
        group.addAction(action)
        shortcut = qt.QShortcut(QtGui.QKeySequence("S"), QtWidgets.QWidget())
        _expect(shortcut is not None, "QShortcut")
        QtCore.QTimer.singleShot(50, menu.close)
        qt.exec_(menu, QtCore.QPoint(10, 10))

    def shiboken_helpers():
        widget = QtWidgets.QWidget()
        _expect(widgets.alive(widget), "alive")
        widgets.dispose(widget)
        _expect(not widgets.alive(widget), "disposed")
        _expect(not widgets.icon("logo").isNull(), "an SVG icon renders (QtSvg)")

    for label, fn in [("Graph Editor builds and paints the default graph", graph_editor_builds),
                      ("Graph Editor's parameter panel for every node", graph_editor_params),
                      ("Graph Editor's keys, pan and zoom", graph_editor_events),
                      ("Groom Tools builds and paints", palette_builds),
                      ("Groom Tools' tool cards and tabs; chips wrap, tiles keep their size", palette_cards),
                      ("Updates window builds", updates_builds),
                      ("Expression editor builds", expression_builds),
                      ("Ramp editor takes mouse events", ramp_events),
                      ("Soft slider builds", slider_builds),
                      ("New Map dialog execs", new_map_dialog),
                      ("Menus, actions, shortcuts", menu_exec),
                      ("shiboken and QtSvg", shiboken_helpers)]:
        check.run(label, fn)
    import fill_field_check  # the fill field's behavior, in both bindings
    if args.shots and not os.path.isdir(args.shots):
        os.makedirs(args.shots)
    fill_field_check.run(check, args.shots)

    print("\n%d passed, %d failed (%s, Qt %s, Python %s)" % (check.passed, len(check.failures), args.api,
                                                           QtCore.qVersion(), sys.version.split()[0]))
    for label, _ in check.failures:
        print("FAILED: " + label)
    # The process ends without Qt's or Python's teardown of the half-mocked Maya, the windows left as
    # they are: PySide2's wheels can crash or hang in an exit that deletes them, and deleting them
    # here, or os._exit (Windows still unloads Qt's DLLs then), crashed now and then too once a
    # second Graph Editor had had a node picked (with the old sliders as well). On Windows the
    # process is ended as it is, no DLL unloaded.
    sys.stdout.flush()
    sys.stderr.flush()
    code = 1 if check.failures else 0
    if sys.platform == "win32":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        kernel32.TerminateProcess.argtypes = (ctypes.c_void_p, ctypes.c_uint)
        kernel32.TerminateProcess(kernel32.GetCurrentProcess(), code)
    os._exit(code)


def _expect(condition, what):
    if not condition:
        raise AssertionError(what)


if __name__ == "__main__":
    main()
