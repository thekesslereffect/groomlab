# SPDX-License-Identifier: Apache-2.0
"""The Expression node's editor (Qt: groomlab.ui.qt): a code block with completion and help, the node's
own variables as sliders, and the expression library (examples and saved expressions).

    CodeEdit          a small code editor: $names and functions complete as you type (or on
                      Ctrl+Space), hovering a name says what it is; it commits after a pause,
                      on Ctrl+Enter and when it loses focus
    VariablesEditor   numbers and vectors the expression reads as $name and $name.x
    ExpressionPanel   the two, the library menu and the node's last error, for one node
"""

import json
import re

from groomlab.ui.qt import QtCore, QtGui, QtWidgets

from groomlab import expressions
from groomlab.base import no_undo
from groomlab.ui import theme, widgets

WORD = re.compile(r"\$?[A-Za-z_][A-Za-z0-9_]*(\.[xyzrgb])?$")


class Highlighter(QtGui.QSyntaxHighlighter):
    """$names, functions, numbers and comments in the theme's colors."""

    def __init__(self, document):
        super(Highlighter, self).__init__(document)
        self.variables = set()
        self.restyle()

    def restyle(self):
        t = theme.current()

        def fmt(color, bold=False, italic=False):
            f = QtGui.QTextCharFormat()
            f.setForeground(t.qcolor(color))
            if bold:
                f.setFontWeight(QtGui.QFont.Bold)
            f.setFontItalic(italic)
            return f
        self.name = fmt(t.get("graph.ports.Guides", "blue"))
        self.own = fmt(t.get("graph.ports.Mesh", "emerald"), bold=True)
        self.function = fmt(t.get("graph.ports.Strands", "yellow"))
        self.number = fmt(t.get("graph.ports.Field", "purple"))
        self.comment = fmt("ui.textDim", italic=True)
        self.rehighlight()

    def highlightBlock(self, text):
        for m in re.finditer(r"\d+\.?\d*|\.\d+", text):
            self.setFormat(m.start(), m.end() - m.start(), self.number)
        for m in re.finditer(r"\$?([A-Za-z_][A-Za-z0-9_]*)(\.[xyzrgb])?", text):
            if text[m.end():m.end() + 1] == "(":
                self.setFormat(m.start(), m.end() - m.start(), self.function)
            elif m.group(0).startswith("$"):
                own = m.group(1) in self.variables
                self.setFormat(m.start(), m.end() - m.start(), self.own if own else self.name)
        comment = re.search(r"#|//", text)
        if comment:
            self.setFormat(comment.start(), len(text) - comment.start(), self.comment)


class CodeEdit(QtWidgets.QPlainTextEdit):
    """A few lines of expression with completion, highlighting and hover help.

    After a pause in typing the text is checked (`validator(text)` -> "" or what is wrong,
    signal checked(message)); a text that checks out is committed (signal committed(text)).
    Ctrl+Enter and leaving the field commit whatever is there."""

    committed = QtCore.Signal(str)
    checked = QtCore.Signal(str)

    def __init__(self, text="", lines=6, parent=None, validator=None):
        super(CodeEdit, self).__init__(parent)
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)
        font.setPixelSize(max(theme.px(12), 11))
        self.setFont(font)
        self.setTabStopDistance(QtGui.QFontMetricsF(font).horizontalAdvance(" ") * 4)
        self.setLineWrapMode(QtWidgets.QPlainTextEdit.WidgetWidth)  # a side panel: long lines wrap
        self.setWordWrapMode(QtGui.QTextOption.WrapAtWordBoundaryOrAnywhere)
        self.setMinimumHeight(QtGui.QFontMetrics(font).lineSpacing() * lines + theme.px(14))
        self.setPlainText(text)
        self._last = text
        self.highlighter = Highlighter(self.document())
        self.entries = []
        self.model = QtGui.QStandardItemModel(self)
        self.completer = QtWidgets.QCompleter(self.model, self)
        self.completer.setWidget(self)
        self.completer.setCaseSensitivity(QtCore.Qt.CaseInsensitive)
        self.completer.setFilterMode(QtCore.Qt.MatchContains)
        self.completer.setCompletionMode(QtWidgets.QCompleter.PopupCompletion)
        self.completer.setMaxVisibleItems(12)
        theme.style(self.completer.popup())  # a window of its own: themed here, and on a theme change
        self.completer.activated[QtCore.QModelIndex].connect(self._insert)
        self.set_entries(expressions.completions())
        self.pause = QtCore.QTimer(self)
        self.pause.setSingleShot(True)
        self.pause.setInterval(700)
        self.validator = validator
        self.pause.timeout.connect(self.paused)
        self.textChanged.connect(self.pause.start)
        self.setMouseTracking(True)
        self.setToolTip("")

    def set_entries(self, entries, variables=()):
        """What completes: [(text, what it is)], and the node's own variable names (highlighted apart)."""
        self.entries = list(entries)
        self.model.clear()
        for text, about in self.entries:
            item = QtGui.QStandardItem(text)
            item.setData(about, QtCore.Qt.ToolTipRole)
            item.setData("%s    %s" % (text, about), QtCore.Qt.DisplayRole)
            item.setData(text, QtCore.Qt.UserRole)
            self.model.appendRow(item)
        self.completer.setCompletionRole(QtCore.Qt.UserRole)
        self.highlighter.variables = set(variables)
        self.highlighter.rehighlight()

    def text(self):
        return self.toPlainText()

    def paused(self):
        """Typing stopped: check the text, and commit it when it checks out."""
        message = self.validator(self.toPlainText()) if self.validator else ""
        self.mark(message)
        self.checked.emit(message)
        if not message:
            self.commit()

    def mark(self, message):
        """Tint the line an error points at (nothing for "")."""
        selections = []
        place = expressions.error_place(message) if message else None
        if place:
            block = self.document().findBlockByNumber(place[0] - 1)
            if block.isValid():
                selection = QtWidgets.QTextEdit.ExtraSelection()
                tint = theme.current().qcolor(theme.current().get("ui.warning", "yellow"), 60)
                selection.format.setBackground(tint)
                selection.format.setProperty(QtGui.QTextFormat.FullWidthSelection, True)
                selection.cursor = QtGui.QTextCursor(block)
                selections.append(selection)
        self.setExtraSelections(selections)

    def commit(self):
        self.pause.stop()
        text = self.toPlainText()
        if text != self._last:
            self._last = text
            self.committed.emit(text)

    def set_text(self, text):
        self.blockSignals(True)
        self.setPlainText(text)
        self.blockSignals(False)
        self._last = text

    # -- Completion ------------------------------------------------------------

    def _word(self):
        """The name being typed before the cursor ($ and a .x included), and where it starts."""
        cursor = self.textCursor()
        line = cursor.block().text()[:cursor.positionInBlock()]
        m = WORD.search(line) or (re.search(r"\$$", line))
        return (m.group(0), cursor.position() - len(m.group(0))) if m else ("", cursor.position())

    def _insert(self, index):
        text = index.data(QtCore.Qt.UserRole)
        word, start = self._word()
        cursor = self.textCursor()
        cursor.setPosition(start)
        cursor.setPosition(start + len(word), QtGui.QTextCursor.KeepAnchor)
        cursor.insertText(text)
        self.setTextCursor(cursor)

    def _show_completions(self, force=False):
        word, _ = self._word()
        if not force and (len(word) < 2 and word != "$"):
            self.completer.popup().hide()
            return
        prefix = word.lstrip("$") if word.startswith("$") else word
        self.completer.setCompletionPrefix(("$" if word.startswith("$") else "") + prefix)
        if self.completer.completionCount() == 0:
            self.completer.popup().hide()
            return
        rect = self.cursorRect()
        rect.setWidth(theme.px(380))
        self.completer.complete(rect)
        self.completer.popup().setCurrentIndex(self.completer.completionModel().index(0, 0))

    def keyPressEvent(self, event):
        popup = self.completer.popup()
        if popup.isVisible() and event.key() in (QtCore.Qt.Key_Return, QtCore.Qt.Key_Enter, QtCore.Qt.Key_Tab):
            index = popup.currentIndex()
            if index.isValid():
                self._insert(index)
            popup.hide()
            return
        if popup.isVisible() and event.key() == QtCore.Qt.Key_Escape:
            popup.hide()
            return
        if event.key() in (QtCore.Qt.Key_Return, QtCore.Qt.Key_Enter) and event.modifiers() & QtCore.Qt.ControlModifier:
            self.commit()
            return
        if event.key() == QtCore.Qt.Key_Space and event.modifiers() & QtCore.Qt.ControlModifier:
            self._show_completions(force=True)
            return
        if event.key() == QtCore.Qt.Key_Tab:
            self.insertPlainText("    ")
            return
        super(CodeEdit, self).keyPressEvent(event)
        if event.text() and (event.text().isalnum() or event.text() in "$_."):
            self._show_completions()
        elif popup.isVisible():
            popup.hide()

    def focusOutEvent(self, event):
        if not self.completer.popup().isVisible():
            self.commit()
        super(CodeEdit, self).focusOutEvent(event)

    # -- Help on hover -----------------------------------------------------------

    def help_for(self, name):
        """What a $name or function is, for the tooltip."""
        for text, about in self.entries:
            if text.rstrip("(") == name or text == "$" + name.lstrip("$"):
                return "<b>%s</b><br>%s" % (text.rstrip("("), about)
        return ""

    def event(self, event):
        if event.type() == QtCore.QEvent.ToolTip:
            cursor = self.cursorForPosition(event.pos())
            line = cursor.block().text()
            at = cursor.positionInBlock()
            for m in re.finditer(r"\$?[A-Za-z_][A-Za-z0-9_]*(\.[xyzrgb])?", line):
                if m.start() <= at <= m.end():
                    text = self.help_for(m.group(0))
                    if text:
                        QtWidgets.QToolTip.showText(event.globalPos(), text, self)
                        return True
            QtWidgets.QToolTip.hideText()
            return True
        return super(CodeEdit, self).event(event)


class VariablesEditor(QtWidgets.QWidget):
    """The node's own variables: a row each (name, value: a slider, or x y z for a vector),
    and buttons to add them.

    A number can be keyable (the diamond): it becomes an attribute on the groom (ctl_<node>_<name>,
    in the Channel Box and the Attribute Editor's Controls) that the slider then sets, so it can
    be keyed and animated. Signals: committed(dict) when a variable is settled, previewed(dict)
    while a slider is dragged (the groom shows it live, outside undo)."""

    committed = QtCore.Signal(object)
    previewed = QtCore.Signal(object)

    def __init__(self, variables=None, groom=None, node=None, parent=None):
        super(VariablesEditor, self).__init__(parent)
        self.variables = dict(variables or {})
        self.groom, self.node = groom, node
        self._drag_start = None  # a keyable slider's value when its drag began
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(4))
        self.rows = QtWidgets.QGridLayout()
        self.rows.setHorizontalSpacing(theme.px(6))
        self.rows.setVerticalSpacing(theme.px(4))
        self.rows.setColumnStretch(1, 1)
        layout.addLayout(self.rows)
        buttons = QtWidgets.QHBoxLayout()
        for text, kind, tip in (("+ Number", "number", "A number the expression reads as $name, with a slider"),
                                ("+ Vector", "vector", "Three numbers the expression reads as $name.x, .y and .z")):
            b = QtWidgets.QToolButton()
            b.setText(text)
            b.setProperty("role", "chip")
            b.setToolTip(tip)
            b.clicked.connect(lambda _=False, k=kind: self.add(k))
            buttons.addWidget(b)
        buttons.addStretch(1)
        layout.addLayout(buttons)
        self.fill()

    # -- Keyable numbers ------------------------------------------------------------

    @staticmethod
    def keyable(value):
        return isinstance(value, dict) and bool(value.get("control"))

    def attribute(self, name):
        """The groom attribute a keyable variable reads ("" when there is none yet)."""
        value = self.variables.get(name)
        if not (self.keyable(value) and self.groom):
            return ""
        from maya import cmds
        import groomlab
        attr = "%s.%s%s" % (self.groom, groomlab.CONTROL_PREFIX, value["control"])
        return attr if cmds.objExists(attr) else ""

    def number(self, name):
        """A number variable's value now: its attribute's for a keyable one."""
        value = self.variables.get(name)
        attr = self.attribute(name)
        if attr:
            from maya import cmds
            return float(cmds.getAttr(attr))
        return float(value["value"]) if isinstance(value, dict) else float(value)

    def set_keyable(self, name, on):
        if on:
            self.variables[name] = {"value": self.number(name),
                                    "control": expressions.control_name(self.node or "expression", name)}
        else:
            self.variables[name] = self.number(name)
        QtCore.QTimer.singleShot(0, self.fill)  # not from inside the toggle's own click
        self.changed()

    # -- Rows ---------------------------------------------------------------------------

    def fill(self):
        while self.rows.count():
            item = self.rows.takeAt(0)
            if item.widget():
                item.widget().hide()
                item.widget().deleteLater()
        for row, (name, value) in enumerate(self.variables.items()):
            vector = isinstance(value, list)
            field = QtWidgets.QLineEdit(name)
            field.setFixedWidth(theme.px(80))
            field.setToolTip("Its name in the expression: $%s%s. Edit to rename" % (name, ".x" if vector else ""))
            field.editingFinished.connect(lambda old=name, f=field: self.rename(old, f.text().strip()))
            self.rows.addWidget(field, row, 0)
            if vector:
                box = QtWidgets.QWidget()
                line = QtWidgets.QHBoxLayout(box)
                line.setContentsMargins(0, 0, 0, 0)
                line.setSpacing(theme.px(4))
                for k in range(3):
                    spin = widgets.CompactSpinBox()
                    spin.setRange(-1e9, 1e9)
                    spin.setDecimals(6)
                    spin.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
                    spin.setMinimumWidth(theme.px(40))
                    spin.setValue(float(value[k]))
                    spin.setToolTip("$%s.%s" % (name, "xyz"[k]))
                    spin.editingFinished.connect(lambda n=name, i=k, s=spin: self.set_component(n, i, s.value()))
                    line.addWidget(spin)
                self.rows.addWidget(box, row, 1)
            else:
                slider = widgets.SoftSlider(self.number(name), key=None)
                slider.committed.connect(lambda v, n=name: self.set_value(n, v))
                slider.dragged.connect(lambda v, n=name: self.drag_value(n, v))
                self.rows.addWidget(slider, row, 1)
                key = QtWidgets.QToolButton()
                key.setText("◆")
                key.setCheckable(True)
                key.setChecked(self.keyable(value))
                key.setAutoRaise(True)
                key.setObjectName("variableKey_" + name)
                key.setToolTip("Keyable: $%s becomes an attribute on the groom (Channel Box, Attribute Editor > "
                               "Controls) that you can key and animate; the slider sets it" % name)
                key.toggled.connect(lambda on, n=name: self.set_keyable(n, on))
                self.rows.addWidget(key, row, 2)
            remove = QtWidgets.QToolButton()
            remove.setIcon(widgets.glyph("delete"))
            remove.glyph = "delete"
            remove.setProperty("role", "glyph")
            remove.setAutoRaise(True)
            remove.setToolTip("Remove $%s" % name)
            remove.clicked.connect(lambda _=False, n=name: self.remove(n))
            self.rows.addWidget(remove, row, 3)

    def changed(self):
        self.committed.emit(dict(self.variables))

    def add(self, kind):
        base = "amount" if kind == "number" else "point"
        name, i = base, 1
        while name in self.variables:
            i += 1
            name = "%s%d" % (base, i)
        self.variables[name] = 0.5 if kind == "number" else [0.0, 0.0, 0.0]
        self.fill()
        self.changed()

    def remove(self, name):
        self.variables.pop(name, None)
        QtCore.QTimer.singleShot(0, self.fill)  # not from inside the button's own click
        self.changed()

    def rename(self, old, new):
        if new == old or old not in self.variables:
            return
        if not expressions.NAME.match(new) or new in self.variables:
            QtCore.QTimer.singleShot(0, self.fill)  # back to the old name
            return
        self.variables = {(new if k == old else k): v for k, v in self.variables.items()}
        QtCore.QTimer.singleShot(0, self.fill)
        self.changed()

    def drag_value(self, name, value):
        """A slider moving: a keyable variable's attribute follows outside undo; any other
        variable is previewed on the groom."""
        attr = self.attribute(name)
        if attr:
            from maya import cmds
            if self._drag_start is None:
                self._drag_start = float(cmds.getAttr(attr))
            with no_undo():
                cmds.setAttr(attr, float(value))
            return
        self.store(name, value)
        self.previewed.emit(dict(self.variables))

    def set_value(self, name, value):
        attr = self.attribute(name)
        if attr:  # one undo step from where the drag began
            from maya import cmds
            if self._drag_start is not None:
                with no_undo():
                    cmds.setAttr(attr, self._drag_start)
                self._drag_start = None
            cmds.setAttr(attr, float(value))
            self.variables[name]["value"] = float(value)  # the fallback where no host control is set
            return
        self.store(name, value)
        self.changed()

    def store(self, name, value):
        """A number's value in the variables (a keyable one keeps its control)."""
        if self.keyable(self.variables.get(name)):
            self.variables[name]["value"] = float(value)
        else:
            self.variables[name] = float(value)

    def set_component(self, name, index, value):
        vector = list(self.variables.get(name) or [0.0, 0.0, 0.0])
        vector[index] = float(value)
        self.variables[name] = vector
        self.changed()


class ExpressionPanel(QtWidgets.QWidget):
    """An Expression node in the param panel: the library menu, the code, the node's last error
    and its variables. `set_param(param, value)` writes to the node."""

    def __init__(self, editor, node, set_param):
        super(ExpressionPanel, self).__init__()
        self.editor, self.node, self.set_param = editor, node, set_param
        model = editor.model
        text = model.param(node, "expression") or ""
        try:
            variables = json.loads(model.param(node, "variables") or "{}")
        except ValueError:
            variables = {}
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(6))

        bar = QtWidgets.QHBoxLayout()
        title = QtWidgets.QLabel("EXPRESSION")
        title.setProperty("role", "subheading")
        bar.addWidget(title, 1)
        library = QtWidgets.QToolButton()
        library.setText("Library  ▾")
        library.setProperty("role", "chip")
        library.setToolTip("Examples, and the expressions you saved (in every scene)")
        library.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        self.library_menu = QtWidgets.QMenu(library)
        self.library_menu.aboutToShow.connect(self.fill_library)
        library.setMenu(self.library_menu)
        bar.addWidget(library)
        layout.addLayout(bar)

        self.code = CodeEdit(text, validator=lambda t: expressions.check(t, self.variables.variables))
        self.code.setObjectName("expressionCode")
        self.code.setToolTip("")
        self.code.checked.connect(self.show_error)
        self.code.committed.connect(lambda t: self.set_param("expression", t))
        layout.addWidget(self.code)
        hint = QtWidgets.QLabel("Type $ for variables, or Ctrl+Space. Lines like $a = ...; make locals; # comments. "
                                "The last line is the value.")
        hint.setWordWrap(True)
        hint.setProperty("role", "hint")
        layout.addWidget(hint)
        self.error = QtWidgets.QLabel()
        self.error.setWordWrap(True)
        self.error.setProperty("role", "status")
        self.error.setObjectName("expressionError")
        layout.addWidget(self.error)

        heading = QtWidgets.QLabel("VARIABLES")
        heading.setProperty("role", "subheading")
        layout.addWidget(heading)
        self.variables = VariablesEditor(variables, groom=editor.groom, node=node)
        self.variables.committed.connect(self.variables_changed)
        self.variables.previewed.connect(
            lambda v: self.editor.preview_param(self.node, "variables", json.dumps(expressions.check_variables(v))))
        layout.addWidget(self.variables)
        self.update_completions()
        self.show_error()

    def update_completions(self):
        channels = set()
        for names in getattr(self.editor, "node_channels", {}).values():
            channels.update(names)
        if self.editor.groom:
            try:
                import groomlab
                channels.update(groomlab.list_maps(self.editor.groom))
            except RuntimeError:
                pass
        variables = self.variables.variables
        self.code.set_entries(expressions.completions(variables, sorted(channels)), variables.keys())

    def variables_changed(self, variables):
        self.update_completions()
        self.set_param("variables", json.dumps(expressions.check_variables(variables)))
        self.show_error()  # a renamed or removed variable the text still reads

    def show_error(self, message=None):
        """What is wrong: the text as typed (a check), else what the groom's evaluation said."""
        if message is None:
            message = expressions.check(self.code.text(), self.variables.variables)
            self.code.mark(message)
        if not message and hasattr(self.editor, "node_errors"):
            message = self.editor.node_errors.get(self.node, "")
        self.error.setText(message)
        self.error.setVisible(bool(message))

    # -- The library ---------------------------------------------------------------

    def fill_library(self):
        menu = self.library_menu
        menu.clear()
        entries = expressions.list_expressions()
        mine = [e for e in entries if not e["shipped"]]
        shipped = [e for e in entries if e["shipped"]]
        for title, group in (("My expressions", mine), ("Examples", shipped)):
            if not group:
                continue
            menu.addSection(title)
            for e in group:
                action = menu.addAction(e["name"], lambda e=e: self.load(e))
                action.setToolTip("%s\n\n%s" % (e["description"], e["expression"]))
        menu.setToolTipsVisible(True)
        menu.addSeparator()
        menu.addAction("Save to Library...", lambda: self.save())
        menu.addAction("Open Library Folder", self.open_folder)

    def ask(self, question):
        """Yes or no before replacing something (tests answer instead of a dialog)."""
        return QtWidgets.QMessageBox.question(self, "Expression Library", question) == QtWidgets.QMessageBox.Yes

    def load(self, entry):
        """A library entry into the node: its code, variables and range in one undo step; the node's
        own code (typed, not an untouched default or another entry) only replaced once confirmed."""
        params = expressions.params_for(entry, node=self.node)
        current = self.code.text().strip()
        default = (self.editor.model.param_spec(self.node, "expression") or {}).get("default", "")
        loaded = [e["expression"].strip() for e in expressions.list_expressions()]
        if current and current != params["expression"].strip() and current != str(default).strip() \
                and current not in loaded and not self.ask("Replace this node's expression with %s?" % entry["name"]):
            return False
        self.code.set_text(params["expression"])
        self.variables.variables = json.loads(params["variables"])
        self.variables.fill()
        self.update_completions()
        model = self.editor.model
        for key in ("expression", "variables", "min", "max"):
            model.set_param(self.node, key, params[key])
        self.editor.changed()  # one write to the groom: one undo step
        self.editor.report("expression: %s (Ctrl+Z goes back)" % entry["name"])
        # min and max have their own sliders below: show the new values.
        QtCore.QTimer.singleShot(0, lambda: self.editor.params.show_node(self.node))
        return True

    def save_form(self):
        """Save to Library's one form: the name and what it does. (name, about) or None."""
        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle("Save to Library")
        form = QtWidgets.QFormLayout(dialog)
        name = QtWidgets.QLineEdit()
        name.setObjectName("expressionName")
        name.setPlaceholderText("in the library, for every scene")
        about = QtWidgets.QLineEdit()
        about.setObjectName("expressionAbout")
        about.setPlaceholderText("optional")
        form.addRow("Name", name)
        form.addRow("What it does", about)
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Save | QtWidgets.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        form.addRow(buttons)
        theme.style(dialog)
        from groomlab.ui import qt
        if qt.exec_(dialog) != QtWidgets.QDialog.Accepted or not name.text().strip():
            return None
        return name.text().strip(), about.text().strip()

    def save(self, name=None, about=""):
        """Save to Library...: the node's code, variables and range under a name (one form); an
        existing entry of that name only replaced once confirmed. Returns the file, None if not saved."""
        if name is None:
            picked = self.save_form()
            if not picked:
                return None
            name, about = picked
        model = self.editor.model
        values = dict(expression=self.code.text(), variables=self.variables.variables, description=about,
                      minimum=model.param(self.node, "min"), maximum=model.param(self.node, "max"))
        try:
            path = expressions.save_expression(name, **values)
        except ValueError as e:
            if not self.ask("%s. Replace it?" % e):
                return None
            path = expressions.save_expression(name, overwrite=True, **values)
        self.editor.report("saved %s to %s" % (name, path))
        return path

    def open_folder(self):
        import os
        folder = expressions.user_dir()
        if not os.path.isdir(folder):
            os.makedirs(folder)
        QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(folder))
