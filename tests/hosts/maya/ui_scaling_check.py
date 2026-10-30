# SPDX-License-Identifier: Apache-2.0
"""GroomLab's Maya windows on small screens and with large text, headless: mayapy (its PySide2),
Qt offscreen and a stand-in Maya (qt_compat_check's), no window shown on the desktop.

Maya scales its UI itself (Preferences > Interface > UI Scaling, or the display's 125-150%): every
GroomLab size goes through theme.px(), the style sheet's fonts too. Each window is built at the
scales 1.0, 1.25 and 1.5 (theme.scale(), as Maya's dpiScale would answer) and must fit the screens
people run Maya on at that scale, in the physical pixels Maya 2022 lays out in: 1920x1080 at 125%
and 150%, 1366x768 at 100% (each less a taskbar). Its minimum size fits, its default size is
fitted within the screen, and the Graph Editor opens docked where a floating one would hide the
viewport. The Theme menus (GroomLab > Theme, the windows' own) list every theme, and every shipped
theme restyles the Groom Tools and the Graph Editor.

    mayapy tests/hosts/maya/ui_scaling_check.py --core <folder of groomlab_core.pyd> [--dll-dir <dir>]...

Exit code 0 when all of it passed (ctest maya.ui_scaling).
"""

import argparse
import os
import sys
import tempfile
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import qt_compat_check  # noqa: E402  (the stand-in Maya)

# (scale, screen's usable area in Maya 2022's physical pixels, what it is)
SCREENS = (
    (1.0, (0, 0, 1366, 728), "1366x768 at 100%"),
    (1.0, (0, 0, 1920, 1040), "1920x1080 at 100%"),
    (1.25, (0, 0, 1920, 1030), "1920x1080 at 125%"),
    (1.5, (0, 0, 1920, 1020), "1920x1080 at 150%"),
)


def _expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--core", required=True, help="the folder of the groomlab_core module for this Python")
    parser.add_argument("--dll-dir", action="append", default=[], help="a folder of DLLs the module needs (TBB)")
    args = parser.parse_args()

    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    # Qt offscreen reads fonts from a folder (no system font database): the system's, so text is
    # measured in Maya's own font on Windows (Segoe UI) rather than a stand-in twice as wide.
    fonts = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
    if os.path.isdir(fonts):
        os.environ.setdefault("QT_QPA_FONTDIR", fonts)
    api = "PySide6" if sys.version_info >= (3, 11) else "PySide2"
    os.environ["GROOMLAB_QT_API"] = api
    os.environ["GROOMLAB_NO_UPDATES"] = "1"
    prefs = tempfile.mkdtemp(prefix="groomlab-ui-scaling-")
    os.environ["GROOMLAB_UPDATES_DIR"] = prefs
    # The node library: the shipped nodes only (the user's own folder left out).
    os.environ["GROOMLAB_USER_NODES"] = os.path.join(prefs, "nodes")
    os.environ.pop("GROOMLAB_NODE_PATH", None)
    for folder in args.dll_dir + [args.core]:
        if hasattr(os, "add_dll_directory") and os.path.isdir(folder):
            os.add_dll_directory(os.path.abspath(folder))
    sys.path[:0] = [qt_compat_check.SCRIPTS, os.path.abspath(args.core)]
    for name in [n for n in sys.modules if n == "maya" or n.startswith("maya.")]:
        del sys.modules[name]  # mayapy's own: the stand-in answers instead
    qt_compat_check._fake_maya(api, prefs)

    from groomlab.ui import qt
    QtWidgets = qt.QtWidgets
    application = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    if os.path.isdir(fonts):
        application.setFont(qt.QtGui.QFont("Segoe UI"))  # as Maya on Windows
    from groomlab import graph as gm
    from groomlab.ui import graph_editor, palette, theme, transfer_window, updates_window, widgets
    from groomlab.ui import reference_window, yeti_import_window
    import groomlab_core

    check = qt_compat_check.Check()

    # The pure parts: a size fitted to a screen, when the Graph Editor docks.
    def fitting():
        _expect(widgets.fit_size(1200, 600, (0, 0, 1920, 1040)) == (1200, 600), "room enough: as asked")
        w, h = widgets.fit_size(1800, 900, (0, 0, 1920, 1020))
        _expect(w <= 1920 * 0.9 and h <= 1020 * 0.9, "fitted: %r" % ((w, h),))
        _expect(widgets.fit_size(1800, 900, (0, 0, 400, 300)) == (400, 300), "a tiny screen: all of it")
        _expect(widgets.fit_size(700, 500, None) == (700, 500), "no screen: as asked")
        _expect(not graph_editor.open_docked((1200, 600), (0, 0, 1920, 1040)), "100% on 1920x1080 floats, as before")
        _expect(graph_editor.open_docked((1500, 750), (0, 0, 1920, 1030)), "125% on 1920x1080 docks")
        _expect(graph_editor.open_docked((1200, 600), (0, 0, 1366, 728)), "1366x768 docks")
    check.run("sizes fit the screen; the Graph Editor docks on small screens", fitting)

    def settle():
        for _ in range(3):
            application.processEvents()

    # The themes: GroomLab > Theme and the windows' Theme menus list every one, the active one
    # checked; each restyles the Groom Tools and the Graph Editor.
    def theme_menus():
        from groomlab.ui import menu
        shipped = sorted(f[:-5] for f in os.listdir(theme.SHIPPED_DIR) if f.endswith(".json"))
        _expect("charcoal" in shipped and set(shipped) <= set(theme.list_themes()), "shipped: %r" % shipped)
        theme.set_theme("charcoal")
        items = []

        class Recorder(object):  # GroomLab's menu as Maya would build it
            def __getattr__(self, name):
                def call(*args, **kwargs):
                    items.append((name, kwargs))
                return call

        real = menu.cmds
        menu.cmds = Recorder()
        try:
            menu._theme_submenu()
        finally:
            menu.cmds = real
        radios = [(kw["label"], kw["radioButton"]) for name, kw in items if name == "menuItem" and "radioButton" in kw]
        _expect([label for label, _ in radios] == [theme.label(n) for n in theme.list_themes()], "GroomLab > Theme: %r" % radios)
        _expect([label for label, on in radios if on] == ["Charcoal"], "the active theme checked: %r" % radios)
        tools = palette.ToolPalette()
        menu_widget = widgets.theme_menu(tools)
        menu_widget.aboutToShow.emit()
        labels = [a.text() for a in menu_widget.actions()]
        _expect(all(theme.label(n) in labels for n in shipped), "Groom Tools' Theme menu: %r" % labels)
        _expect([a.text() for a in menu_widget.actions() if a.isChecked()] == ["Charcoal"], "checked: the active theme")
        widgets.dispose(tools)
    check.run("the Theme menus list every theme, the active one checked", theme_menus)

    # Errors in plain words with a next step; a failing menu item says so instead of raising.
    def plain_errors():
        from groomlab.ui import menu, messages
        _expect(messages.plain(ImportError("No module named 'xgenm'")).startswith("XGen isn't loaded: load xgenToolkit"),
                "xgenm")
        _expect("values typed" in messages.plain(ValueError("not enough values to unpack (expected 2, got 1)")), "unpack")
        _expect(messages.plain(IOError(13, "Permission denied", "D:/out.abc")).startswith("can't write D:/out.abc"),
                messages.plain(IOError(13, "Permission denied", "D:/out.abc")))
        _expect(messages.plain(RuntimeError("select a groom")) == "select a groom", "GroomLab's own words kept")

        def fails():
            raise OSError(28, "No space left on device")
        _expect(menu._guarded(fails, "Export Alembic...")() is None, "a guarded item never raises")
    check.run("errors in plain words; menu items guarded", plain_errors)

    # GroomLab's menu as Maya would build it (the GUI session's test_the_menu_is_short checks it in
    # Maya): one Simulate menu with nHair under Legacy, Render apart from Export, Help holding
    # Check for Updates and Support, every item's command guarded, the Review toggles checkable.
    def menu_layout():
        from groomlab.ui import menu
        tree, stack, commands = [], [[]], []

        class Recorder(object):
            def __getattr__(self, name):
                def call(*args, **kwargs):
                    if name == "menuItem" and not (kwargs.get("edit") or kwargs.get("query") or kwargs.get("exists")):
                        if kwargs.get("divider"):
                            return "divider"
                        entry = (kwargs.get("label"), [], kwargs)
                        stack[-1].append(entry)
                        if kwargs.get("subMenu"):
                            stack.append(entry[1])
                        elif "command" in kwargs:
                            commands.append(kwargs["label"])
                        return "item"
                    if name == "setParent":
                        stack.pop()
                    if name == "menu":
                        return False if kwargs.get("exists") else "menu"
                    return False if name == "about" else None
                return call
        real = menu.cmds
        menu.cmds = Recorder()
        try:
            menu.install()
        finally:
            menu.cmds = real
        tree = stack[0]
        top = [label for label, _, _ in tree]
        _expect(top == ["Groom Tools", "Graph Editor", "Create Groom from Selection", "Add Sample Mesh",
                        "Guides from Photos (experimental)...", "Groom Files", "Import", "Add", "Simulate", "Display",
                        "Review", "Studio", "Render", "Export", "Utilities", "Theme", "Install GroomLab Hotkeys", "Help"],
                "top: %r" % top)
        sub = {label: children for label, children, _ in tree}
        _expect([l for l, _, _ in sub["Help"]] == ["Docs", "Keyboard Shortcuts", "Check for Updates...",
                                                   "Discord", "Support GroomLab", "About GroomLab"], "Help")
        _expect([l for l, _, _ in sub["Simulate"]][-1] == "Legacy", "nHair under Simulate > Legacy")
        _expect("Simulation" not in [l for l, _, _ in sub["Add"]], "no second Add Simulate")
        render = [l for l, _, _ in sub["Render"]]
        _expect(render == ["Render from Groom File...", "Render as Curves", "Maya Hardware 2.0 (no renderer needed)...",
                           "RenderMan (prman)...", "V-Ray Standalone...",
                           "V-Ray for Maya Hook (2024+, experimental)", "Load as VRayScene (2024+, experimental)"],
                "Render: %r" % render)
        _expect(not {"Groom File...", "Bake Scale Maps", "Render as Curves"} & {l for l, _, _ in sub["Export"]}, "Export")
        _expect("Save Groom File As..." in [l for l, _, _ in sub["Groom Files"]], "one groom-file block")
        review = {l: kw for l, _, kw in sub["Review"]}
        _expect("checkBox" in review["Density Heatmap"] and "checkBox" in review["Strand Count HUD"], "checkable")
        _expect("Braid on Curves..." in [l for l, _, _ in sub["Add"]], "a dialog item ends in ...")
        studio_items = [l for l, _, _ in sub["Studio"]]
        _expect(studio_items[0] == "Portrait" and studio_items[-1] == "Remove Studio", "Studio: %r" % studio_items)
    check.run("GroomLab's menu: one place for each thing, Help holds the rest", menu_layout)

    # The UI definitions every host shares (engine/ui): every action id a Maya menu, bar or list shows
    # has a binding here, each binding names an action of the files and a method the owner has.
    def every_id_bound():
        from groomlab.ui import definitions as d, hotkeys, menu, tool_tabs
        tools = palette.ToolPalette()
        editor = graph_editor.GraphEditor()
        try:
            def ids(*paths, **generators):
                return set(i for p in paths for i in d.listed_ids(d.get(p), generators=generators.items()))

            theme_ids = ids("menus/theme/items")
            empty = [e for e in d.get("palette/empty/items") if not (isinstance(e, dict) and e.get("id") == "import")]
            tables = (
                ("menu.ACTIONS", menu.ACTIONS, None,
                 (ids("menus/main", studioPresets="studio.preset") - theme_ids)
                 | ids("palette/output", "menus/output.import", "menus/output.export", "menus/output.render",
                       "menus/output.studio", "palette/header/visible/items", "palette/header/guides/items")),
                ("widgets.theme_actions", widgets.theme_actions(), None, theme_ids),
                ("ToolPalette.ACTIONS", palette.ToolPalette.ACTIONS, tools,
                 ids("palette/header/more/items", "lists/parts/bar") | set(d.listed_ids(empty))),
                ("GuidesTab.ACTIONS", tool_tabs.GuidesTab.ACTIONS, tools.guides_tab,
                 ids("lists/groups/bar", "lists/groups/context", "lists/groups/edits/items")),
                ("PaintTab.ACTIONS", tool_tabs.PaintTab.ACTIONS, tools.paint_tab,
                 ids("lists/maps/bar", "lists/maps/context")),
                ("CorrectivesBox.ACTIONS", tool_tabs.CorrectivesBox.ACTIONS, tools.correctives,
                 ids("lists/correctives/bar", "lists/correctives/context", "lists/correctives/nodeContext")),
                ("ReferenceTab.ACTIONS", tool_tabs.ReferenceTab.ACTIONS, tools.reference_tab,
                 ids("lists/photos/bar", "lists/photos/context", "lists/photos/pairsBar", "lists/photos/pairsContext",
                     "lists/photos/guidesBar")),
                ("NodeItem.ACTIONS", graph_editor.NodeItem.ACTIONS, graph_editor.NodeItem,
                 ids("graph/nodeMenu", exposeParams="node.expose", innerParams="compound.showInner")),
                ("BackdropItem.ACTIONS", graph_editor.BackdropItem.ACTIONS, graph_editor.BackdropItem,
                 ids("graph/backdropMenu")),
                ("GraphEditor.ACTIONS", graph_editor.GraphEditor.ACTIONS, editor,
                 ids("graph/editMenu", "graph/addMenu", "graph/presetMenu", "graph/presetItem")),
                ("ParamPanel.param_actions", editor.params.param_actions("node", "param"), None,
                 ids("graph/paramMenu")),
                ("ParamPanel.node_action_table", editor.params.node_action_table("node"), None,
                 ids("graph/nodeActions")),
                ("hotkeys.ACTIONS", hotkeys.ACTIONS, None, set(e for e in d.get("markingMenu/tail") if isinstance(e, str))),
            )
            actions = d.get("actions")
            for name, table, owner, listed in tables:
                _expect(listed, "%s: the definitions list nothing for it" % name)
                missing = sorted(listed - set(table))
                _expect(not missing, "%s: no binding for %r" % (name, missing))
                stale = sorted(set(table) - set(actions))
                _expect(not stale, "%s: binds ids no file defines: %r" % (name, stale))
                for aid, target in table.items():
                    if isinstance(target, str):
                        _expect(hasattr(owner, target), "%s: %s binds %r, which it has not" % (name, aid, target))
            # Every Maya action of the files is shown somewhere in Maya (the rest are other hosts' only).
            shown = set()
            for _name, table, _owner, listed in tables:
                shown |= listed
            unshown = sorted(a for a, v in actions.items() if "maya" in v.get("hosts", ["maya"]) and a not in shown)
            _expect(not unshown, "actions Maya never shows: %r" % unshown)
            # The option lists Maya builds widgets from.
            _expect([tools.flow_sink.itemData(i) for i in range(tools.flow_sink.count())]
                    == [c["value"] for c in d.choices("flowSink")], "flow sinks from choices.json")
            _expect(palette.TABS == [t["id"] for t in d.get("palette/tabs")], "tabs from palette.json")
        finally:
            widgets.dispose(tools)
            widgets.dispose(editor)
    check.run("every UI definition id Maya shows has a binding", every_id_bound)

    def every_theme_styles_the_windows():
        catalog = gm.OperatorCatalog.from_json(groomlab_core.operator_specs_json())
        tools = palette.ToolPalette()
        editor = graph_editor.GraphEditor()
        editor.catalog = catalog
        editor.model = gm.GraphModel(catalog, groomlab_core.default_graph())
        editor.rebuild()
        try:
            for name in sorted(f[:-5] for f in os.listdir(theme.SHIPPED_DIR) if f.endswith(".json")):
                t = theme.set_theme(name)
                settle()
                _expect(t.color("ui.panel") in tools.styleSheet(), "%s: the Groom Tools restyled" % name)
                _expect(editor.scene.backgroundBrush().color().name().upper() == t.color("graph.canvas").upper(),
                        "%s: the Graph Editor's canvas" % name)
                _expect(graph_editor.HEADER_STYLE == t.get("graph.headerStyle"), "%s: the header style" % name)
            charcoal = theme.set_theme("charcoal")
            sheet = tools.styleSheet()
            _expect("solid %s" % charcoal.color("ui.outline") in sheet, "Charcoal: thin outlines")
            _expect("background: %s; color: %s" % (charcoal.color("ui.selection"), charcoal.color("ui.selectionText")) in sheet,
                    "Charcoal: a grey selection, not the off-white accent")
            outline = charcoal.color("ui.outline")
            playful = theme.set_theme("playful")
            _expect("solid %s" % outline not in tools.styleSheet() and not playful.get("ui.outline"), "Playful: no outlines")
        finally:
            theme.set_theme("playful")
            widgets.dispose(tools)
            widgets.dispose(editor)
    check.run("every shipped theme restyles the Groom Tools and the Graph Editor", every_theme_styles_the_windows)

    # The 2026-10-30 look (research/ui-redesign, "chosen"): square icon-only tool tiles six to a row
    # (name and hint in the tooltip, the checked one in the selection with an accent border, its icon
    # in its own colors), underlined tabs left aligned, sections under a hairline without a card, and
    # the Graph Editor's minimal headers and canvas styles painted.
    def redesign_look():
        from groomlab.ui.qt import QtCore, QtGui
        t = theme.set_theme("charcoal")
        tools = palette.ToolPalette()
        tools.resize(theme.px(380), theme.px(1000))
        tools.show()
        settle()
        tools.empty.setVisible(False)  # as with a groom (the stand-in Maya has none)
        tools.work.setVisible(True)
        settle()
        try:
            tiles = [b for b in tools.findChildren(QtWidgets.QToolButton) if b.property("role") == "tool"]
            _expect(len(tiles) > 10 and all(b.toolButtonStyle() == QtCore.Qt.ToolButtonIconOnly for b in tiles),
                    "icon-only tool tiles")
            _expect(all(b.toolTip().startswith("<b>") and "<br>" in b.toolTip() for b in tiles), "name + hint tooltips")
            shown = [b for b in tiles if b.isVisibleTo(tools)]
            _expect(shown and all(abs(b.width() - b.height()) <= 2 for b in shown),
                    "square tiles: %r" % sorted({(b.width(), b.height()) for b in shown}))
            grids = [g for g in tools.findChildren(widgets.ToolGrid) if g.isVisibleTo(tools)]
            _expect(grids and all(g.columns == 6 for g in grids), "six to a row: %r" % [g.columns for g in grids])
            unfreeze = tools.findChild(QtWidgets.QToolButton, "unfreezeAllTile")
            _expect(unfreeze in tiles and not unfreeze.isCheckable() and "Unfreeze All" in unfreeze.toolTip(),
                    "Unfreeze All: an icon tile, a button")
            comb = tools.buttons["comb"].icon()
            _expect(comb.pixmap(32, 32, QtGui.QIcon.Normal, QtGui.QIcon.On).toImage()
                    == comb.pixmap(32, 32, QtGui.QIcon.Normal, QtGui.QIcon.Off).toImage(), "a checked tile's icon: the same")
            _expect(not tools.tabs.bar.expanding(), "tabs as wide as their names")
            sheet = tools.styleSheet()
            _expect("QToolButton[role=\"tool\"]:checked { background: %s; border-color: %s;"
                    % (t.hex(t.get("ui.selection") or "accent"), t.hex("accent")) in sheet, "checked tile: selection + accent")
            _expect("border-bottom-color: %s" % t.hex("accent") in sheet, "the current tab underlined in the accent")
            _expect("QFrame[role=\"card\"] { background: transparent; border: none; }" in sheet, "no card box")
            _expect(tools.settings.layout().contentsMargins().left() == 0, "the brush settings unpadded")
            _expect(tools.settings_heading.property("rule") == "true", "the brush heading under a hairline")
        finally:
            widgets.dispose(tools)
        # The node painters: every header and canvas style, a theme's unknown ones as the defaults.
        specs = gm.OperatorCatalog.from_json(groomlab_core.operator_specs_json())
        editor = graph_editor.GraphEditor()
        editor.catalog = specs
        editor.model = gm.GraphModel(specs, groomlab_core.default_graph())
        editor.rebuild()
        node = editor.items["scatter"]
        accent = graph_editor.CATEGORY_COLORS.get(node.category)
        try:
            bars = {}
            for header in ("tinted", "filled", "minimal"):
                for canvas in ("dots", "lines", "plain"):
                    graph_editor.HEADER_STYLE, graph_editor.CANVAS_STYLE = header, canvas
                    image = QtGui.QImage(int(graph_editor.NODE_W) + 40, int(node.height) + 40, QtGui.QImage.Format_ARGB32)
                    image.fill(0)
                    painter = QtGui.QPainter(image)
                    editor.scene.drawBackground(painter, QtCore.QRectF(image.rect()))
                    painter.translate(20, 20)
                    node.paint(painter, QtWidgets.QStyleOptionGraphicsItem(), None)
                    painter.end()
                    bars[header] = QtGui.QColor(image.pixel(int(20 + graph_editor.NODE_W / 2), 22))
            _expect(bars["tinted"].name() == accent.name(), "tinted: the accent bar (%s)" % bars["tinted"].name())
            _expect(bars["minimal"].name() != accent.name() and bars["minimal"].name() != bars["tinted"].name(),
                    "minimal: no bar, no fill (%s)" % bars["minimal"].name())
            data = dict(t.data, graph=dict(t.data["graph"], headerStyle="ribbon", canvasStyle="hex"))
            graph_editor.apply_theme(theme.Theme("future", data))
            _expect((graph_editor.HEADER_STYLE, graph_editor.CANVAS_STYLE) == ("tinted", "dots"),
                    "unknown styles: tinted, dots")
        finally:
            graph_editor.apply_theme(theme.current())
            widgets.dispose(editor)
            theme.set_theme("playful")
    check.run("the new look: icon tiles, underlined tabs, plain sections, minimal node headers", redesign_look)

    def fits(widget, screen, label):
        minimum = widget.minimumSizeHint().expandedTo(widget.minimumSize())
        _expect(minimum.width() <= screen[2] and minimum.height() <= screen[3],
                "%s: its minimum %dx%d is larger than the screen %dx%d" % (label, minimum.width(), minimum.height(),
                                                                           screen[2], screen[3]))
        default = widgets.fit_size(widget.width(), widget.height(), screen)
        _expect(default[0] <= screen[2] and default[1] <= screen[3], "%s: default %r" % (label, default))
        widget.resize(*default)
        settle()
        _expect(widget.width() <= screen[2] and widget.height() <= screen[3],
                "%s: %dx%d after fitting to %r" % (label, widget.width(), widget.height(), screen))
        return minimum

    catalog = gm.OperatorCatalog.from_json(groomlab_core.operator_specs_json())

    # The Graph Editor's search and notes: name matches first, Tab with no groom says what to do,
    # the default graph opens without a note (HairColor's clumpShare at its default, no Clump).
    def graph_search_and_notes():
        from groomlab.ui.qt import QtCore
        editor = graph_editor.GraphEditor()
        editor.catalog = catalog
        editor.model = gm.GraphModel(catalog, groomlab_core.default_graph())
        editor.rebuild()
        try:
            search = graph_editor.NodeSearch(editor, (0.0, 0.0))
            for text, first in (("density", "Scatter"), ("symmetry", "Mirror"), ("hair", "HairColor"),
                                ("clump", "Clump"), ("hair color", "HairColor"), ("linked", "Reference"),
                                ("collision", "Collide"), ("jitter", "Noise"), ("deform", "Bend"),
                                ("frizz", "node:groomlab.soft_frizz")):  # a shipped compound node, by its label
                search.fill(text)
                got = search.list.item(0).data(QtCore.Qt.UserRole) if search.list.count() else None
                _expect(got == first, "search %r: %s first, not %s" % (text, first, got))
            search.close()
            widgets.dispose(search)
            editor.update_attribute_notes()
            _expect("color" not in editor.attribute_notes, "the default graph's HairColor: no note (%r)"
                    % editor.attribute_notes.get("color"))
            editor.params.show_node("color")
            _expect(not [l for l in editor.params.findChildren(QtWidgets.QLabel, "attributeMissing")],
                    "no 'no node upstream writes clumpId' line in the default HairColor's panel")
            editor.model.set_param("color", "clumpShare", 0.8)  # changed: now it says no Clump feeds it
            editor.params.show_node("color")
            notes = [l.text() for l in editor.params.findChildren(QtWidgets.QLabel, "attributeMissing")]
            _expect(any("clumpId" in n for n in notes), "a changed clumpShare notes the missing clumpId: %r" % notes)
            # A node's error under its header, in plain words.
            editor.node_errors["color"] = "not enough values to unpack (expected 2, got 1)"
            editor.params.show_node("color")
            line = editor.params.findChild(QtWidgets.QLabel, "nodeError")
            _expect(line is not None and not line.isHidden() and "values typed" in line.text(),
                    "the node's error line: %r" % (line.text() if line is not None else None))
            editor.node_errors.clear()
            editor.params.update_error()
            _expect(line.isHidden(), "no error: no line")
        finally:
            widgets.dispose(editor)
        empty = graph_editor.GraphEditor()  # no groom: Tab says to select one
        try:
            _expect(empty.show_search(QtCore.QPointF(0, 0), QtCore.QPoint(0, 0)) is None, "no popup")
            _expect("Select a groom first" in empty.status.text(), "Tab with no groom: %r" % empty.status.text())
            _expect(empty.status.wordWrap() and empty.status.toolTip() == empty.status.text(), "wrapped, its tooltip")
        finally:
            widgets.dispose(empty)
    check.run("the Graph Editor's search ranks names first; no note on the default graph", graph_search_and_notes)

    # UX batch 2: one name per thing, the core's categories and keys, units and decimals.
    def names_keys_and_units():
        from groomlab.ui import shortcuts, hotkeys
        from groomlab import units
        tools = palette.ToolPalette()
        try:
            titles = [tools.tabs.bar.tabText(i) for i in range(tools.tabs.bar.count())]
            _expect(titles == ["Tools", "Paint", "Correctives", "Photos"], "Groom Tools tabs: %r" % titles)
            _expect(tools.symmetry_row.parent() is tools.target_row, "symmetry sits in the Group row")
            tools.work.setVisible(True)
            tools.show_tool_settings()  # no tool: the symmetry row stays (it isn't the tool's)
            _expect(not tools.symmetry_row.isHidden(), "symmetry shown with no brush picked")
            _expect("Alt+Shift+X" in tools.symmetry.toolTip(), "symmetry's tooltip names its key")
        finally:
            widgets.dispose(tools)
        text = shortcuts.text()
        for want in ("M + drag", "Alt+Shift+X", "Page Up / Page Down", "Install GroomLab Hotkeys"):
            _expect(want in text, "Help > Keyboard Shortcuts lists %r" % want)
        _expect("Select guides" not in text, "Maya's list has no unbound S row")
        _expect(shortcuts.keys("graph.frame") == "F" and shortcuts.keys("graph.frameAll") == "A", "graph F / A")
        _expect(shortcuts.tip("Frame", "graph.frame") == "Frame (F)", "keys in tooltips")
        _expect("Y  " not in hotkeys.changes_text([]) and "Nothing changed" in hotkeys.changes_text([]),
                "the hotkeys note with nothing changed")
        note = hotkeys.changes_text([("Y", "(nothing)", "GroomLabMarkingMenuPress")])
        _expect("Y" in note and "GroomLabMarkingMenuPress" in note, "the hotkeys note lists each key: %r" % note)
        # Numbers as every host shows them: the spec's decimals, trailing zeros dropped.
        _expect(widgets.format_number(0.35, 3) == "0.35" and widgets.format_number(1.0, 3) == "1"
                and widgets.format_number(980.0, 1) == "980", "numbers drop trailing zeros")
        _expect(widgets.format_number(0.0347, 1) == "0.035" and widgets.format_number(0.00874, 1) == "0.0087"
                and widgets.format_number(1e-9, 3) == "0", "small numbers keep two significant digits")
        _expect(widgets.display_decimals(3, 100.0) == 1 and widgets.display_decimals(4, 1e4) == 0, "unit decimals")
        _expect(units.param_label("length", "Simulate", "gravity").endswith(u"/s²"), "gravity's unit")
        # The Add Node menu: the core's categories, spaced labels, each with what it does.
        editor = graph_editor.GraphEditor()
        editor.catalog = catalog
        try:
            menu = editor.build_add_menu()
            subs = [m.title() for m in menu.submenus]
            _expect(subs[:5] == ["Sources", "Grow", "Shape", "Deform", "Collide"], "Add Node categories: %r" % subs)
            labels = {a.text(): a.toolTip() for m in menu.submenus for a in m.actions()}
            _expect("Linked Groom" in labels and "Reference" not in labels, "the Reference node reads Linked Groom")
            _expect("Blend Neighbors" in labels, "spaced labels")
            _expect(all(labels.values()), "every Add Node entry has its description as a tooltip")
            shape = next(m for m in menu.submenus if m.title() == "Shape")
            _expect(len(shape.actions()) <= 12, "Shape is split: %d entries" % len(shape.actions()))
            widgets.dispose(menu)
        finally:
            widgets.dispose(editor)
        # A window's long action: buttons off and the wait cursor while it runs, back after.
        button = QtWidgets.QPushButton("Generate")
        label = QtWidgets.QLabel()
        with widgets.busy([button], label, "Generating..."):
            _expect(not button.isEnabled() and label.text() == "Generating...", "busy: disabled, said")
        _expect(button.isEnabled() and QtWidgets.QApplication.overrideCursor() is None, "busy: restored")
        widgets.dispose(button)
        widgets.dispose(label)
    check.run("one name per thing; the core's categories and keys; units and decimals", names_keys_and_units)

    # UX batch 3: the Groom Tools' Output strip (the menu's table), one Theme menu list, themed status
    # colors, Tab wiring after the selection, a refused link saying why, the Paint chip.
    def batch3():
        from groomlab.ui import menu
        tools = palette.ToolPalette()
        try:
            chips = [c.text().split("  ")[0] for c in tools.output_chips]
            _expect(chips == ["Simulate Range", "Export", "Render", "Studio", "Transfer...", "Import"], "Output strip: %r" % chips)
            export = tools.findChild(QtWidgets.QToolButton, "output_export").menu()
            labels = [a.text() for a in export.actions() if not a.isSeparator()]
            _expect("Alembic..." in labels and "glTF Cards..." in labels and "Scales Mesh (FBX)..." not in labels,
                    "Output > Export: %r" % labels)
            _expect(tools.empty_import.menu() is not None, "the empty state offers Import")
            imports = [a.text() for a in tools.empty_import.menu().actions() if not a.isSeparator()]
            _expect("Alembic Curves..." in imports, "Import offers Alembic Curves...: %r" % imports)
        finally:
            widgets.dispose(tools)
        _expect("Alembic Curves..." in [i[0] for i in menu.shown_items("import", menu.MENU_ONLY) if i],
                "GroomLab > Import > Alembic Curves...")
        # GroomLab > Theme and the windows' Theme menus: one list.
        qt_labels = [i[0] for i in widgets.theme_menu_items() if i]
        items = []

        class Recorder(object):
            def __getattr__(self, name):
                def call(*args, **kwargs):
                    items.append((name, kwargs))
                return call
        real = menu.cmds
        menu.cmds = Recorder()
        try:
            menu._theme_submenu()
        finally:
            menu.cmds = real
        maya_labels = [kw["label"] for name, kw in items if name == "menuItem" and "label" in kw and kw["label"] != "Theme"]
        _expect(maya_labels == qt_labels, "one Theme menu: %r vs %r" % (maya_labels, qt_labels))
        # Status colors follow the theme.
        for name in ("polar", "playful"):
            t = theme.set_theme(name)
            graph_editor.apply_theme(t)
            _expect(graph_editor.ERROR.name().upper() == t.color("graph.error").upper(), "%s: error color" % name)
            _expect(graph_editor.DRIVEN.name().upper() == t.color("graph.driven").upper(), "%s: driven color" % name)
            _expect("Deform" in t.get("graph.categories") and "Collide" in t.get("graph.categories"),
                    "%s: Deform and Collide colors" % name)
        editor = graph_editor.GraphEditor()
        editor.catalog = catalog
        editor.model = gm.GraphModel(catalog, groomlab_core.default_graph())
        editor.rebuild()
        try:
            editor.items["length"].setSelected(True)
            graph_editor.NodeSearch(editor, (100.0, 200.0)).choose("Curl")
            curl = [n for n in editor.model.order if editor.model.node_type(n) == "Curl"][0]
            _expect(editor.model.source_of(curl, "strands") == "length", "Tab wires after the selected node")
            _expect(editor.model.position(curl) == (100.0, 200.0), "where Tab was pressed")
            scene = editor.scene
            guides = editor.items["guides"].output
            why = scene.incompatible_reason(guides, editor.items["scatter"].inputs["scalp"])
            _expect(why and "Guides" in why and "Mesh" in why, "a refused link says why: %r" % why)
            _expect(scene.incompatible_reason(guides, editor.items["interpolate"].inputs["guides"]) is None, "fits")
            editor.groom = "groomLabShape1"  # a groom (the stand-in's): mappable rows get the Paint chip
            editor.params.show_node("scatter")
            chips = editor.params.widget().findChildren(QtWidgets.QToolButton, "paintChip")
            _expect(chips and [a.text() for a in chips[0].menu().actions()] == ["On Guides", "On Scalp Vertices",
                                                                                "As Texture"], "the Paint chip")
            editor.groom = None
        finally:
            widgets.dispose(editor)
    check.run("UX batch 3: Output strip, one Theme list, status colors, Tab wiring, link reasons, Paint chip",
              batch3)

    for scale, screen, about in SCREENS:
        theme._scale[0] = scale  # as Maya's dpiScale answers at this UI scaling

        def graph_editor_fits(scale=scale, screen=screen, about=about):
            editor = graph_editor.GraphEditor()
            editor.catalog = catalog
            editor.model = gm.GraphModel(catalog, groomlab_core.default_graph())
            editor.rebuild()
            editor.resize(theme.px(1200), theme.px(600))
            editor.show()
            settle()
            # The HairColor node's params (the color the user is after), in the param panel.
            if "color" in editor.items:
                editor.scene.clearSelection()
                editor.items["color"].setSelected(True)
                settle()
            minimum = fits(editor, screen, "Graph Editor")
            # The params fit the panel's width: nothing cut off on the right (shrinkable fields).
            panel = editor.params
            needed = panel.widget().minimumSizeHint().width()
            _expect(needed <= panel.viewport().width(), "Graph Editor's param panel: needs %d px, shows %d"
                    % (needed, panel.viewport().width()))
            docked =graph_editor.open_docked((theme.px(1200), theme.px(600)), screen)
            print("      Graph Editor at %s: minimum %dx%d, %s" % (about, minimum.width(), minimum.height(),
                                                                  "docked" if docked else "floating"))
            editor.close()
            widgets.dispose(editor)
        check.run("Graph Editor fits %s" % about, graph_editor_fits)

        def palette_fits(screen=screen, about=about):
            tools = palette.ToolPalette()
            tools.resize(theme.px(420), screen[3])
            tools.show()
            settle()
            # No groom (the stand-in Maya has none): only "Start a groom", no tabs or tools.
            _expect(tools.empty.isVisibleTo(tools) and not tools.work.isVisibleTo(tools),
                    "Groom Tools with no groom: the empty state alone")
            tools.work.setVisible(True)  # as with a groom: the tabs and tools must fit too
            settle()
            minimum = fits(tools, screen, "Groom Tools")
            # Docked beside the viewport: it must leave most of the screen's width to it.
            _expect(minimum.width() <= screen[2] * 0.35, "Groom Tools: %d wide at its narrowest" % minimum.width())
            tools.close()
            widgets.dispose(tools)
        check.run("Groom Tools fit %s" % about, palette_fits)

        def transfer_fits(screen=screen, about=about):
            window = transfer_window.TransferWindow(groom="groomLabShape1")
            window.show()
            settle()
            fits(window, screen, "Transfer window")
            window.close()
            widgets.dispose(window)
        check.run("Transfer window fits %s" % about, transfer_fits)

        def reference_fits(screen=screen, about=about):
            window = reference_window.ReferenceWindow(groom="groomLabShape1")
            window.show()
            settle()
            fits(window, screen, "Reference window")
            # The photo and the mesh views side by side at their minimum, the side panel beside them.
            _expect(window.photo_view.width() >= window.photo_view.minimumWidth(), "Reference window: the photo view")
            window.close()
            widgets.dispose(window)
        check.run("Reference window fits %s" % about, reference_fits)

        def yeti_import_fits(screen=screen, about=about):
            window = yeti_import_window.YetiImportWindow()
            window.show()
            settle()
            fits(window, screen, "Import from Yeti window")
            window.close()
            widgets.dispose(window)
        check.run("Import from Yeti window fits %s" % about, yeti_import_fits)

        def updates_fits(screen=screen, about=about):
            window = updates_window.UpdatesWindow()
            window.show()
            settle()
            fits(window, screen, "Updates window")
            window.close()
            widgets.dispose(window)
        check.run("Updates window fits %s" % about, updates_fits)
    theme._scale[0] = None

    print("%d passed, %d failed" % (check.passed, len(check.failures)))
    for label, trace in check.failures:
        print("FAILED: %s" % label)
    return 1 if check.failures else 0


if __name__ == "__main__":
    try:
        code = main()
    except Exception:
        traceback.print_exc()
        code = 1
    sys.stdout.flush()
    os._exit(code)  # no Qt teardown in mayapy
