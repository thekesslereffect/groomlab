# SPDX-License-Identifier: Apache-2.0
"""The fill field (widgets.SoftSlider: the redesign's slider), offscreen: painted as the theme says,
typed into (arithmetic, Esc, focus out), dragged (the 3 px threshold, relative, Shift finer, Ctrl in
steps of 10, the range's ends), nudged with the arrows, its soft range stretched by a typed value,
the key states' colors, disabled; and in the Groom Tools and the Graph Editor, the names inside the
fields. Run by qt_compat_check (PySide2 and PySide6) and ui_scaling_check (mayapy).

    run(check, shots=None)   check: qt_compat_check.Check; shots: a folder for PNGs of the fields
"""

import os


def _expect(condition, what):
    if not condition:
        raise AssertionError(what)


# The Charcoal the redesign ships (the app's theme JSON is being updated beside this): applied in
# memory, so the check doesn't depend on that file's state.
REDESIGN_UI = {"radius": 6, "rowHeight": 26, "textMuted": "#737373", "selection": "#35435E", "accent": "#5B8DEF"}


def run(check, shots=None):
    from groomlab.ui import qt, theme, widgets
    QtCore, QtGui, QtWidgets = qt.QtCore, qt.QtGui, qt.QtWidgets
    QTest = qt.QtTest().QTest
    app = QtWidgets.QApplication.instance()
    Qt = QtCore.Qt

    def settle(ms=10):
        timer = QtCore.QElapsedTimer()
        timer.start()
        while timer.elapsed() < ms:
            app.processEvents()

    def close(a, b, tolerance=10):
        return all(abs(x - y) <= tolerance for x, y in zip(a.getRgb()[:3], b.getRgb()[:3]))

    previous = theme._current
    base = theme.load("charcoal")
    data = dict(base, ui=dict(base["ui"], **REDESIGN_UI))
    theme._current = theme.Theme("charcoal", data)
    t = theme.current()
    holder = QtWidgets.QWidget()  # a window to put fields in: a focus target besides them
    holder.setStyleSheet(theme.stylesheet(t))
    column = QtWidgets.QVBoxLayout(holder)
    other = QtWidgets.QLineEdit()
    column.addWidget(other)
    holder.resize(280, 200)
    holder.show()
    holder.activateWindow()
    settle(30)

    def field(*args, **kwargs):
        f = widgets.SoftSlider(*args, **kwargs)
        column.addWidget(f)
        f.setFixedWidth(240)
        holder.adjustSize()
        settle()
        f.got = {"dragged": [], "committed": []}
        f.dragged.connect(lambda v: f.got["dragged"].append(v))
        f.committed.connect(lambda v: f.got["committed"].append(v))
        return f

    def grab(f):
        return f.grab().toImage()

    def save(widget, name):
        if shots:
            widget.grab().save(os.path.join(shots, name + ".png"))

    def move(f, x, mods=Qt.NoModifier):
        y = f.height() // 2
        event = QtGui.QMouseEvent(QtCore.QEvent.MouseMove, QtCore.QPointF(x, y), QtCore.QPointF(f.mapToGlobal(QtCore.QPoint(x, y))),
                                  Qt.NoButton, Qt.LeftButton, mods)
        QtWidgets.QApplication.sendEvent(f, event)

    def drag(f, x0, x1, mods=Qt.NoModifier, release=True):
        y = f.height() // 2
        QTest.mousePress(f, Qt.LeftButton, mods, QtCore.QPoint(x0, y))
        move(f, x1, mods)
        if release:
            QTest.mouseRelease(f, Qt.LeftButton, mods, QtCore.QPoint(x1, y))

    try:
        def parsing():
            p = widgets.parse_number
            for text, want in (("0.5*2", 1.0), ("1/4", 0.25), ("-(2+3)", -5.0), ("1,5", 1.5), (" 3 ", 3.0),
                               ("2e-3", 0.002), ("+7", 7.0), ("10 - 2 * 3", 4.0)):
                _expect(p(text) == want, "%r reads %r, not %r" % (text, want, p(text)))
            for text in ("", "abc", "2**8", "1/0", "__import__('os')", "[1]", "True", "1;2", "a+1", "9" * 400):
                _expect(p(text) is None, "%r is not a number: %r" % (text, p(text)))
        check.run("fill field: typed values are numbers or simple arithmetic, nothing else", parsing)

        def painting():
            f = field(0.25, 0.0, 1.0, label="Strength")
            _expect(f.height() == theme.px(26), "a row tall: %d" % f.height())
            _expect(f.text() == "0.25" and f.label() == "Strength", (f.text(), f.label()))
            image = grab(f)
            y = 4
            filled = QtGui.QColor(image.pixel(int(f.width() * 0.12), y))
            empty = QtGui.QColor(image.pixel(int(f.width() * 0.6), y))
            _expect(close(filled, QtGui.QColor("#35435E")), "the filled part in ui.selection: %s" % filled.name())
            _expect(close(empty, QtGui.QColor(t.color("ui.raised"))), "the rest in ui.raised: %s" % empty.name())
            edge = QtGui.QColor(image.pixel(f.width() // 2, 0))
            _expect(close(edge, QtGui.QColor(t.color("ui.border")), 24), "a 1px border: %s" % edge.name())
            # The label on the left (textDim), the value on the right (text): some text-colored pixels there.
            right = [QtGui.QColor(image.pixel(x, yy)) for x in range(f.width() - 40, f.width() - 6)
                     for yy in range(6, f.height() - 6)]
            _expect(any(c.lightness() > 180 for c in right), "the value drawn at the right")
            left = [QtGui.QColor(image.pixel(x, yy)) for x in range(6, 60) for yy in range(6, f.height() - 6)]
            _expect(any(c.lightness() > 110 for c in left), "the label drawn at the left")
            f._hover = True
            hovered = QtGui.QColor(grab(f).pixel(int(f.width() * 0.6), y))
            _expect(close(hovered, QtGui.QColor(t.color("ui.hover"))), "hover lightens it: %s" % hovered.name())
            f._hover = False
            save(f, "field_strength")
            units = field(25, 0, 100, integer=True, label="Spacing", suffix="%")
            _expect(units.text() == "25%", units.text())
            units.setSuffix("cm")
            _expect(units.text() == "25 cm", units.text())
            save(units, "field_units")
            plain = field(0.5, 0.0, 1.0)  # no label: the value centered
            _expect(plain.text() == "0.5", plain.text())
            save(plain, "field_no_label")
        check.run("fill field paints: a row tall, filled in the selection, label left and value right", painting)

        def typing():
            f = field(0.25, 0.0, 1.0, label="Strength")
            QTest.mouseClick(f, Qt.LeftButton, Qt.NoModifier, QtCore.QPoint(60, f.height() // 2))
            settle()
            _expect(f.isEditing() and f.editor.isVisible(), "a click without a drag types")
            _expect(f.editor.selectedText() == "0.25", "the value selected: %r" % f.editor.selectedText())
            _expect(f.editor.geometry() == f.rect(), "over the field")
            save(f, "field_editing")
            QTest.keyClicks(f.editor, "0.5*2")
            QTest.keyClick(f.editor, Qt.Key_Return)
            settle()
            _expect(not f.isEditing() and not f.editor.isVisible(), "Enter closes it")
            _expect(f.value() == 1.0 and f.got["committed"] == [1.0], "0.5*2 committed once: %r" % f.got)
            _expect(f.hasFocus(), "the focus back on the field")
            # Esc puts the value back.
            f.edit()
            f.editor.selectAll()
            QTest.keyClicks(f.editor, "0.9")
            QTest.keyClick(f.editor, Qt.Key_Escape)
            settle()
            _expect(not f.isEditing() and f.value() == 1.0 and f.got["committed"] == [1.0], "Esc: nothing changed")
            # Past the range: it stretches to reach it; "," as the decimal point.
            f.edit()
            f.editor.setText("1,5")
            QTest.keyClick(f.editor, Qt.Key_Enter)
            _expect(f.value() == 1.5 and (f.lo, f.hi) == (0.0, 2.0), "1,5 stretches 0-1 to 0-2: %r" % ((f.lo, f.hi),))
            # Clicking away commits; text left as it was commits nothing.
            f.edit()
            f.editor.setText("0.3")
            other.setFocus()
            settle()
            _expect(not f.isEditing() and f.value() == 0.3, "focus out commits: %r" % f.value())
            n = len(f.got["committed"])
            f.edit()
            other.setFocus()
            settle()
            _expect(len(f.got["committed"]) == n, "an unchanged value: no commit")
            # Not a number: the value stays.
            f.edit()
            f.editor.setText("abc")
            QTest.keyClick(f.editor, Qt.Key_Return)
            _expect(f.value() == 0.3 and len(f.got["committed"]) == n, "text that isn't a number changes nothing")
            # Hard limits clamp what is typed; the unit may be typed after it.
            g = field(0.5, 0.0, 1.0, minimum=0.0, maximum=1.0, label="Opacity")
            _expect(g.commit_text("7") and g.value() == 1.0, "clamped at the hard maximum: %r" % g.value())
            h = field(50, 0, 100, integer=True, suffix="%")
            _expect(h.commit_text("40%") and h.value() == 40 and isinstance(h.value(), int), h.value())
            _expect(h.commit_text("10/3") and h.value() == 3, "a whole number: %r" % h.value())
        check.run("fill field types: click, arithmetic, Enter, Esc, focus out, the range stretched", typing)

        def dragging():
            f = field(0.25, 0.0, 1.0, label="Strength")
            w = f.width()
            drag(f, 100, 102, release=False)  # under 3 px: still a click
            _expect(f.got["dragged"] == [] and f.isSliderDown(), "under the threshold: no drag")
            move(f, 100 + w // 4)
            _expect(f.got["dragged"] and abs(f.value() - 0.5) < 0.011, "a quarter of the width: +0.25 (%r)" % f.value())
            QTest.mouseRelease(f, Qt.LeftButton, Qt.NoModifier, QtCore.QPoint(100 + w // 4, f.height() // 2))
            _expect(not f.isSliderDown() and not f.isEditing(), "a drag's release types nothing")
            _expect(f.got["committed"] == [f.value()], "one commit on release: %r" % f.got["committed"])
            # Relative to the press: pressing anywhere doesn't jump.
            before = f.value()
            drag(f, 10, 10 + w // 10)
            _expect(abs(f.value() - (before + 0.1)) < 0.011, "relative: +0.1 from %r gave %r" % (before, f.value()))
            # Shift: a tenth as fast.
            before = f.value()
            drag(f, 40, 40 + w // 2, Qt.ShiftModifier)
            _expect(abs(f.value() - (before + 0.05)) < 0.006, "Shift: +0.05, got %r" % (f.value() - before))
            # Ctrl: steps of 10x (0.1 over 0-1).
            f.setValue(0.25)
            drag(f, 40, 40 + int(w * 0.13), Qt.ControlModifier)
            _expect(abs(f.value() - 0.4) < 1e-9, "Ctrl snaps to 0.1: %r" % f.value())
            # Dragging stays inside the range.
            drag(f, 10, 10 + 3 * w)
            _expect(f.value() == 1.0, "clamped at the range's end: %r" % f.value())
            drag(f, 200, 200 - 3 * w)
            _expect(f.value() == 0.0, "and its start: %r" % f.value())
            # Whole numbers stay whole.
            n = field(12, 0, 32, integer=True, label="Segments")
            drag(n, 20, 20 + w // 8)
            _expect(n.value() == 16 and isinstance(n.value(), int), "an integer field: %r" % n.value())
            # Scripts drag it too (the Graph Editor's tests).
            n.setSliderDown(True)
            n.scrub(20)
            _expect(n.got["dragged"][-1] == 20 and n.isSliderDown(), "scrub previews while held")
            n.setSliderDown(False)
            _expect(n.got["committed"][-1] == 20, "released: committed")
        check.run("fill field drags: 3 px threshold, relative, Shift finer, Ctrl steps, inside the range", dragging)

        def keys():
            f = field(0.5, 0.0, 1.0, label="Strength")
            f.setFocus()
            settle()
            QTest.keyClick(f, Qt.Key_Right)
            _expect(abs(f.value() - 0.51) < 1e-9, "Right: +0.01 (%r)" % f.value())
            QTest.keyClick(f, Qt.Key_Right, Qt.ShiftModifier)
            _expect(abs(f.value() - 0.61) < 1e-9, "Shift+Right: +0.1 (%r)" % f.value())
            QTest.keyClick(f, Qt.Key_Down)
            QTest.keyClick(f, Qt.Key_Left)
            _expect(abs(f.value() - 0.59) < 1e-9, "Down, Left: -0.02 (%r)" % f.value())
            _expect(len(f.got["committed"]) == 4, "each nudge committed")
            save(f, "field_focused")
            QTest.keyClick(f, Qt.Key_Return)
            _expect(f.isEditing(), "Enter types")
            QTest.keyClick(f.editor, Qt.Key_Escape)
            QTest.keyClick(f, Qt.Key_F2)
            _expect(f.isEditing(), "F2 types")
            f.cancel_edit()
            big = field(200.0, 0.0, 400.0)
            _expect(big.step() == 1.0 and field(0.0, 0.0, 3.0).step() == 0.01, "a hundredth of the range, round")
        check.run("fill field keys: Enter and F2 type, the arrows nudge (Shift x10)", keys)

        def states():
            f = field(0.4, 0.0, 1.0, label="Keep")
            for state, color in (("keyed", t.color("ui.keyed")), ("animated", t.color("ui.animated")),
                                 ("driven", t.color("ui.driven"))):
                f.set_key_color(QtGui.QColor(color))
                image = grab(f)
                edge = QtGui.QColor(image.pixel(f.width() // 2, 0))
                filled = QtGui.QColor(image.pixel(int(f.width() * 0.1), 5))
                _expect(close(edge, QtGui.QColor(color), 24), "%s: the border in its color (%s)" % (state, edge.name()))
                _expect(close(filled, QtGui.QColor(color)), "%s: the fill in its color (%s)" % (state, filled.name()))
                save(f, "field_" + state)
            f.set_key_color(None)
            _expect(f.key_color() is None and close(QtGui.QColor(grab(f).pixel(int(f.width() * 0.1), 5)),
                                                    QtGui.QColor("#35435E")), "no state: the theme's look")
            f.setEnabled(False)
            dim = QtGui.QColor(grab(f).pixel(int(f.width() * 0.1), 5))
            _expect(dim.lightness() < QtGui.QColor("#35435E").lightness(), "disabled: dimmed (%s)" % dim.name())
            save(f, "field_disabled")
            QTest.mouseClick(f, Qt.LeftButton, Qt.NoModifier, QtCore.QPoint(30, 10))
            _expect(not f.isEditing() and f.edit() is None, "disabled: no typing")
        check.run("fill field states: keyed, animated and driven colors show; disabled dims", states)

        def in_windows():
            from groomlab import graph as gm
            from groomlab.ui import graph_editor, palette
            import groomlab_core
            tools = palette.ToolPalette()
            tools.resize(theme.px(380), theme.px(900))
            tools.show()
            settle(30)
            tools.tool = "comb"  # as picked (the stand-in Maya has no brush context to start)
            tools.show_tool_settings()
            tools.empty.setVisible(False)  # as with a groom (the stand-in Maya has none)
            tools.work.setVisible(True)
            settle(30)
            try:
                names = {s.label() for s in tools.findChildren(widgets.SoftSlider)}
                for name in ("Radius", "Strength", "Falloff", "Length", "Segments", "Soft radius", "Spacing"):
                    _expect(any(n.startswith(name) for n in names), "%s inside its field: %r" % (name, sorted(names)))
                _expect(tools.radius.text().endswith(" px"), "the radius in px: %r" % tools.radius.text())
                texts = [l.text() for l in tools.brush_form.findChildren(QtWidgets.QLabel)]
                _expect(not texts, "no separate labels by the brush's fields: %r" % texts)
                if shots:
                    tools.grab().save(os.path.join(shots, "groom_tools.png"))
            finally:
                tools.close()  # deleted with the other windows at the end
            catalog = gm.OperatorCatalog.from_json(groomlab_core.operator_specs_json())
            editor = graph_editor.GraphEditor()
            editor.catalog = catalog
            editor.model = gm.GraphModel(catalog, groomlab_core.default_graph())
            editor.rebuild()
            editor.resize(1200, 700)
            editor.show()
            settle(30)
            try:
                panel = editor.params
                for node in ("scatter", "length", "clump", "width"):
                    if node not in editor.items:
                        continue
                    editor.scene.clearSelection()
                    editor.items[node].setSelected(True)
                    settle(20)
                    _expect(panel.inside_fields, "%s: its numbers carry their names" % node)
                    for param, f in panel.inside_fields.items():
                        label = panel.row_label_of[param]
                        _expect(label.isHidden() and f.label() == label.text(), "%s.%s: the name inside" % (node, param))
                        _expect(f.property("glParam") == param, "%s.%s: S over the field keys it" % (node, param))
                    if shots:
                        panel.widget().grab().save(os.path.join(shots, "graph_editor_%s.png" % node))
                editor.scene.clearSelection()
                editor.items["length"].setSelected(True)
                settle(20)
                keep = panel.key_widgets["keep"]
                _expect(keep.suffix() == "%" and keep.text().endswith("%"), "keep's unit inside: %r" % keep.text())
                graph_editor.paint_key_state(keep, "keyed")
                _expect(keep.property("keyState") == "keyed" and keep.key_color() is not None, "keyed: painted")
                _expect(graph_editor.KEY_TIPS["keyed"] in keep.toolTip(), "keyed: said in the tooltip")
                if shots:
                    panel.widget().grab().save(os.path.join(shots, "graph_editor_length_keyed.png"))
                graph_editor.paint_key_state(keep, None)
                _expect(keep.key_color() is None, "a plain param again")
                # Its right-click: the label's menu (keys, expose...) atop the range's.
                menu = QtWidgets.QMenu()
                keep.menu_hook(menu)
                _expect("Set Key" in [a.text() for a in menu.actions()], "the param menu atop the field's")
                menu.deleteLater()
                # The ◆ of an exposed param stays in its name.
                editor.set_param_exposed("length", "keep", True)
                panel.show_node("length")
                settle(20)
                _expect(panel.inside_fields["keep"].label().endswith(u"◆"), panel.inside_fields["keep"].label())
                # A map picked for a mappable param: its label comes back beside the map's fields.
                editor.set_param_exposed("length", "keep", False)
                editor.scene.clearSelection()
                editor.items["scatter"].setSelected(True)
                settle(20)
                mappable = [e for e in panel.findChildren(graph_editor.MappableEditor) if e.spec["name"] == "density"]
                if mappable:
                    plain = mappable[0].value
                    mappable[0].value = {"texture": "", "component": "r", "min": 0.0, "max": 1.0}
                    mappable[0].build("texture")  # as Map > Texture shows it (switch() also writes it)
                    _expect(not panel.row_label_of["density"].isHidden(), "a map's fields: the row's label shown")
                    mappable[0].value = plain
                    mappable[0].build("value")
                    _expect(panel.row_label_of["density"].isHidden(), "a value again: the name inside the field")
            finally:
                editor.close()  # deleted with the other windows at the end (now, PySide2 can crash on its timers)
        check.run("fill fields in the windows: names inside, units inside, key states, the param menu", in_windows)
    finally:
        holder.close()
        theme._current = previous
