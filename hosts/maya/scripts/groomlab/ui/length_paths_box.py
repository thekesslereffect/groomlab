# SPDX-License-Identifier: Apache-2.0
"""The Length Paths tool's options in Groom Tools (the catalog's "lengthPaths" setting): the
selected key's length and reach, the path's smoothness, how guides take their new length, New
Path, Delete Key, Delete Path, Apply and Cancel. Edits go to the brush context
(groomLabBrushCtx -lengthEdit) and the box reads its state back (-lengthState), lengths in the
scene's units. The viewport adds keys and drags their lengths; refresh() follows it."""

import json

from groomlab.ui.qt import QtCore, QtWidgets
from groomlab.ui import theme, widgets

import groomlab


def _ctx_ready():
    from maya import cmds
    ctx = groomlab.BRUSH_CONTEXT
    return cmds.contextInfo(ctx, exists=True) and cmds.currentCtx() == ctx


def length_state():
    """The tool's paths and selection (-lengthState), {} without the tool."""
    from maya import cmds
    if not _ctx_ready():
        return {}
    try:
        return json.loads(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, lengthState=True) or "{}")
    except (RuntimeError, ValueError):
        return {}


def length_edit(words):
    """One edit (-lengthEdit: "key 2.5", "apply"...); a refusal comes back as its message, else ""."""
    from maya import cmds
    if not _ctx_ready():
        return "Pick the Length Paths tool first."
    try:
        cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, lengthEdit=words)
    except RuntimeError as e:
        return str(e).strip()
    return ""


class LengthPathsBox(QtWidgets.QWidget):
    """The options, a column under its own heading."""

    def __init__(self, parent=None):
        super(LengthPathsBox, self).__init__(parent)
        self.setProperty("role", "options")
        self.setObjectName("lengthPathsBox")
        column = QtWidgets.QVBoxLayout(self)
        column.setContentsMargins(0, theme.px(4), 0, 0)
        column.setSpacing(theme.px(6))
        heading = QtWidgets.QLabel("PATHS")
        heading.setProperty("role", "subheading")
        column.addWidget(heading)

        form = QtWidgets.QFormLayout()
        form.setContentsMargins(0, 0, 0, 0)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        row = QtWidgets.QHBoxLayout()
        self.path = QtWidgets.QComboBox()
        self.path.setObjectName("lengthPath")
        self.path.setToolTip("The path the fields edit (a key clicked in the viewport picks its path)")
        self.path.currentIndexChanged.connect(self.path_picked)
        row.addWidget(self.path, 1)
        self.new_path = QtWidgets.QPushButton("New Path")
        self.new_path.setObjectName("lengthNewPath")
        self.new_path.setToolTip("The next click on the scalp starts a new path (or Shift+click)")
        self.new_path.clicked.connect(lambda: self.edit("newPath"))
        row.addWidget(self.new_path)
        form.addRow("Path", row)

        row = QtWidgets.QHBoxLayout()
        self.key = widgets.CompactSpinBox()
        self.key.setObjectName("lengthKey")
        self.key.setRange(0.0001, 1e6)
        self.key.setDecimals(4)
        self.key.display_decimals = 3
        self.key.setKeyboardTracking(False)
        self.key.setToolTip("The selected key's length: guides at it take this length (middle-drag the key up or "
                            "down in the viewport to set it there)")
        self.key.valueChanged.connect(lambda v: self.edit("key %r" % v))
        row.addWidget(self.key, 1)
        self.delete_key = QtWidgets.QPushButton("Delete Key")
        self.delete_key.setObjectName("lengthDeleteKey")
        self.delete_key.setToolTip("Delete the selected key (or Ctrl+click it in the viewport)")
        self.delete_key.clicked.connect(lambda: self.edit("deleteKey"))
        row.addWidget(self.delete_key)
        form.addRow("Key length", row)

        self.reach = widgets.CompactSpinBox()
        self.reach.setObjectName("lengthReach")
        self.reach.setRange(0.0001, 1e6)
        self.reach.setDecimals(4)
        self.reach.display_decimals = 3
        self.reach.setKeyboardTracking(False)
        self.reach.setToolTip("How far from the path guides change at the selected key (its ring; between keys "
                              "the reach blends): full near it, fading out to their own length at the edge "
                              "(Shift+drag a key in the viewport to size it). A new path's first reach is the brush "
                              "ring's; later keys take the one before")
        self.reach.valueChanged.connect(lambda v: self.edit("reach %r" % v))
        form.addRow("Key reach", self.reach)

        row = QtWidgets.QHBoxLayout()
        self.smooth = QtWidgets.QCheckBox("Smooth")
        self.smooth.setObjectName("lengthSmooth")
        self.smooth.setToolTip("Lengths ease smoothly from key to key (never past a key); off: straight between them")
        self.smooth.toggled.connect(lambda on: self.edit("smooth %s" % ("on" if on else "off")))
        row.addWidget(self.smooth)
        self.change = QtWidgets.QComboBox()
        self.change.setObjectName("lengthChange")
        self.change.addItem("Scale", "scale")
        self.change.addItem("Trim", "trim")
        self.change.setToolTip("How a guide takes its new length: Scale (the whole guide scaled about its root: "
                               "curls and bends keep their shape) or Trim (cut at the tip, or extended along its "
                               "last direction: curls keep their size)")
        self.change.currentIndexChanged.connect(lambda _i: self.edit("change %s" % self.change.currentData()))
        row.addWidget(self.change, 1)
        self.delete_path = QtWidgets.QPushButton("Delete Path")
        self.delete_path.setObjectName("lengthDeletePath")
        self.delete_path.setToolTip("Delete the selected path and its keys")
        self.delete_path.clicked.connect(lambda: self.edit("deletePath"))
        row.addWidget(self.delete_path)
        form.addRow("", row)
        column.addLayout(form)

        row = QtWidgets.QHBoxLayout()
        self.status = QtWidgets.QLabel()
        self.status.setObjectName("lengthStatus")
        self.status.setProperty("role", "hint")
        self.status.setWordWrap(True)
        row.addWidget(self.status, 1)
        self.cancel = QtWidgets.QPushButton("Cancel")
        self.cancel.setObjectName("lengthCancel")
        self.cancel.setToolTip("Drop the changes since the last Apply: the guides and paths as they were")
        self.cancel.clicked.connect(lambda: self.edit("cancel"))
        row.addWidget(self.cancel)
        self.apply = QtWidgets.QPushButton("Apply")
        self.apply.setObjectName("lengthApply")
        self.apply.setProperty("role", "primary")
        self.apply.setToolTip("Set the guides to the lengths shown (one undo step); the paths stay with the groom "
                              "to edit and apply again")
        self.apply.clicked.connect(lambda: self.edit("apply"))
        row.addWidget(self.apply)
        column.addLayout(row)
        self._state = None

    def edit(self, words):
        if getattr(self, "_filling", False):
            return
        problem = length_edit(words)
        self.refresh()
        if problem:
            self.status.setText(problem)

    def path_picked(self, index):
        if index >= 0:
            self.edit("select %d 0" % index)

    def refresh(self):
        """The fields from the tool's state (the viewport's clicks and drags); nothing sent back."""
        state = length_state()
        if state == self._state:
            return
        self._state = state
        self._filling = True
        try:
            paths = state.get("paths", [])
            active = state.get("activePath", -1)
            key = state.get("activeKey", -1)
            self.path.clear()
            for i, p in enumerate(paths):
                self.path.addItem("Path %d (%d keys)" % (i + 1, len(p["lengths"])))
            if 0 <= active < len(paths):
                self.path.setCurrentIndex(active)
            path = paths[active] if 0 <= active < len(paths) else None
            has_key = path is not None and 0 <= key < len(path["lengths"])
            if has_key:
                self.key.setValue(path["lengths"][key])
                self.reach.setValue(path["reaches"][key])
            if path is not None:
                self.smooth.setChecked(path["smooth"])
            self.change.setCurrentIndex(max(0, self.change.findData(state.get("change", "scale"))))
            for w in (self.key, self.reach, self.delete_key):
                w.setEnabled(has_key)
            for w in (self.smooth, self.delete_path):
                w.setEnabled(path is not None)
            edited = bool(state.get("edited"))
            self.apply.setEnabled(edited)
            self.cancel.setEnabled(edited)
            if not paths:
                self.status.setText("Click on the scalp to lay a path's first key.")
            elif edited:
                self.status.setText("Previewing: Apply sets the guides.")
            else:
                self.status.setText("Applied. Drag a key or change a field to edit.")
        finally:
            self._filling = False
