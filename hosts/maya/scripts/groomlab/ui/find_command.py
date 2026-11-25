# SPDX-License-Identifier: Apache-2.0
"""Find Command (GroomLab > Find Command..., the Groom Tools' search): one box over every GroomLab
command (the GroomLab menu's, engine/ui/menus.json), every brush and every graph preset. Type to
filter (words in any order; letters in order match too), Up / Down to move, Enter to run, Esc to
close. It is how the menus and panels can stay short: anything not on screen is a few keys away.

    from groomlab.ui import find_command
    find_command.show()
"""

from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtWidgets

from groomlab.ui import definitions, theme, widgets

_window = None


def entries():
    """[(label, detail, kind, fn)]: the brushes, the GroomLab menu's commands (by their submenu),
    the graph presets."""
    from groomlab.ui import menu, palette
    found = []
    for tool in palette.CATALOG:
        found.append((tool["label"], tool["hint"], "Brush",
                      lambda t=tool["id"]: _brush(t)))
    bind = definitions.binder(menu.ACTIONS)

    def walk(items, where):
        for item in items:
            if item["kind"] == "submenu":
                walk(item["items"], item["label"])
            elif item["kind"] == "action" and item["id"] != "commands.find":
                try:
                    fn = bind(item)
                except KeyError:  # (an entry the menu binds by hand, as the theme's: left out)
                    continue
                found.append((item["label"].replace("...", ""), item["tip"], where or "GroomLab", fn))
    try:
        walk(definitions.resolve(definitions.get("menus/main"), values=menu.VALUES, generators=menu.GENERATORS), "")
    except Exception:  # (a menu generator that needs a scene)
        walk(definitions.resolve(definitions.get("menus/main")), "")
    try:
        from groomlab import presets
        for name in presets.list_presets():
            found.append(("Preset: " + presets.title_of(name), "Apply the graph preset to the current groom",
                          "Graph", lambda n=name: _preset(n)))
    except Exception:
        pass
    return found


def _brush(tool):
    from groomlab.ui import palette
    tools = palette.show(tab="tools")
    if tools is not None:
        tools.activate(tool)


def _preset(name):
    import groomlab
    from groomlab import presets
    groom = groomlab.active_groom()
    if not groom:
        raise RuntimeError("select a groom first")
    presets.apply_preset(groom, name)


def score(query, text):
    """How well `text` matches `query` (0: not at all): a word start best, then anywhere, then the
    query's letters in order."""
    if not query:
        return 1.0
    t = text.lower()
    q = query.lower()
    words = q.split()
    if len(words) > 1:
        return min(score(w, text) for w in words)
    i = t.find(q)
    if i == 0:
        return 3.0
    if i > 0:
        return 2.5 if not t[i - 1].isalnum() else 2.0
    k = 0
    for ch in t:
        if k < len(q) and ch == q[k]:
            k += 1
    return 1.0 if k == len(q) else 0.0


class FindCommand(QtWidgets.QFrame):
    """The search box and its results, a popup over the window that opened it."""

    MAX = 12

    def __init__(self, parent=None):
        super(FindCommand, self).__init__(parent, QtCore.Qt.Popup | QtCore.Qt.FramelessWindowHint)
        self.setObjectName("findCommand")
        self.setProperty("role", "card")
        self.setAttribute(QtCore.Qt.WA_StyledBackground, True)
        self.setFixedWidth(theme.px(460))
        column = QtWidgets.QVBoxLayout(self)
        pad = theme.px(8)
        column.setContentsMargins(pad, pad, pad, pad)
        column.setSpacing(theme.px(6))
        self.box = QtWidgets.QLineEdit()
        self.box.setObjectName("findCommandBox")
        self.box.setPlaceholderText("Find a GroomLab command: alembic, comb, bake, preset...")
        self.box.textChanged.connect(self.filter)
        self.box.installEventFilter(self)
        column.addWidget(self.box)
        self.list = QtWidgets.QListWidget()
        self.list.setObjectName("findCommandList")
        self.list.itemActivated.connect(lambda item: self.run(item))
        self.list.itemClicked.connect(lambda item: self.run(item))
        column.addWidget(self.list)
        self.foot = widgets.small_label("")
        column.addWidget(self.foot)
        self._entries = entries()
        theme.style(self, surface="panel")
        self.filter("")

    def filter(self, text):
        ranked = sorted(((max(score(text, e[0]) * 2.0, score(text, e[1]) * 0.6), i, e)
                         for i, e in enumerate(self._entries)), key=lambda x: (-x[0], x[1]))
        self.list.clear()
        shown = [e for v, _i, e in ranked if v > 0][:self.MAX] if text.strip() else \
            [e for e in self._entries if e[2] == "Brush"][:4] + [e for e in self._entries if e[2] != "Brush"][:6]
        for label, detail, kind, fn in shown:
            item = QtWidgets.QListWidgetItem("%s    · %s" % (label, kind))
            item.setToolTip(detail)
            item.setData(QtCore.Qt.UserRole, fn)
            self.list.addItem(item)
        if self.list.count():
            self.list.setCurrentRow(0)
        rows = min(max(self.list.count(), 1), self.MAX)
        self.list.setFixedHeight(rows * max(self.list.sizeHintForRow(0), theme.px(22)) + theme.px(4))
        self.foot.setText("%d commands, brushes and presets · Enter runs · Esc closes" % len(self._entries)
                          if not text.strip() else "%d found" % self.list.count())
        self.adjustSize()

    def eventFilter(self, obj, event):
        if obj is self.box and event.type() == QtCore.QEvent.KeyPress:
            key = event.key()
            if key in (QtCore.Qt.Key_Down, QtCore.Qt.Key_Up):
                row = self.list.currentRow() + (1 if key == QtCore.Qt.Key_Down else -1)
                self.list.setCurrentRow(max(0, min(self.list.count() - 1, row)))
                return True
            if key in (QtCore.Qt.Key_Return, QtCore.Qt.Key_Enter):
                self.run(self.list.currentItem())
                return True
            if key == QtCore.Qt.Key_Escape:
                self.close()
                return True
        return super(FindCommand, self).eventFilter(obj, event)

    def run(self, item):
        if item is None:
            return
        fn = item.data(QtCore.Qt.UserRole)
        self.close()
        try:
            fn()
        except Exception as e:  # (a command run from here says why it couldn't, as the menu does)
            try:
                from maya import cmds
                cmds.warning("GroomLab: %s" % e)
            except Exception:
                pass

    def open_over(self, widget=None):
        anchor = widget.window() if widget is not None else None
        if anchor is not None:
            point = anchor.mapToGlobal(QtCore.QPoint((anchor.width() - self.width()) // 2, theme.px(60)))
            self.move(point)
        self.show()
        self.box.setFocus()


def show(widget=None):
    """Open Find Command over `widget`'s window (else Maya's main window)."""
    global _window
    if widget is None:
        try:
            widget = widgets._maya_main_window()
        except Exception:
            widget = None
    _window = FindCommand(widget)
    _window.open_over(widget)
    return _window
