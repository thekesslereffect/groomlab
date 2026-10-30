# SPDX-License-Identifier: Apache-2.0
"""The Length Keys tool's options in Groom Tools (the catalog's "lengthKeys" setting): the
selected key's scale and the length it gives the guide nearest it, how guides take their new
length, Delete Key, Clear, Apply and Cancel. Edits go to the brush context
(groomLabBrushCtx -lengthEdit) and the box reads its state back (-lengthState), lengths in the
scene's units. The viewport adds keys and drags their scales; refresh() follows it."""

import json

from groomlab.ui.qt import QtCore, QtWidgets
from groomlab.ui import theme, widgets

import groomlab


def _ctx_ready():
    from maya import cmds
    ctx = groomlab.BRUSH_CONTEXT
    return cmds.contextInfo(ctx, exists=True) and cmds.currentCtx() == ctx


def length_state():
    """The tool's keys and selection (-lengthState), {} without the tool."""
    from maya import cmds
    if not _ctx_ready():
        return {}
    try:
        return json.loads(cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, query=True, lengthState=True) or "{}")
    except (RuntimeError, ValueError):
        return {}


def length_edit(words):
    """One edit (-lengthEdit: "scale 1.5", "apply"...); a refusal comes back as its message, else ""."""
    from maya import cmds
    if not _ctx_ready():
        return "Pick the Length Keys tool first."
    try:
        cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, lengthEdit=words)
    except RuntimeError as e:
        return str(e).strip()
    return ""


class LengthKeysBox(QtWidgets.QWidget):
    """The options, a column under its own heading."""

    def __init__(self, parent=None):
        super(LengthKeysBox, self).__init__(parent)
        self.setProperty("role", "options")
        self.setObjectName("lengthKeysBox")
        column = QtWidgets.QVBoxLayout(self)
        column.setContentsMargins(0, theme.px(4), 0, 0)
        column.setSpacing(theme.px(6))
        heading = QtWidgets.QLabel("KEYS")
        heading.setProperty("role", "subheading")
        column.addWidget(heading)

        form = QtWidgets.QFormLayout()
        form.setContentsMargins(0, 0, 0, 0)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        row = QtWidgets.QHBoxLayout()
        self.scale = widgets.CompactSpinBox()
        self.scale.setObjectName("lengthScale")
        self.scale.setRange(0.001, 1000.0)
        self.scale.setDecimals(3)
        self.scale.display_decimals = 2
        self.scale.setSingleStep(0.1)
        self.scale.setKeyboardTracking(False)
        self.scale.setToolTip("The selected key's scale: guides at it take their length times this (1: kept as "
                              "they are, a hold), blending smoothly toward the other keys (middle-drag the key up "
                              "or down in the viewport to set it there)")
        self.scale.valueChanged.connect(lambda v: self.edit("scale %r" % v))
        row.addWidget(self.scale, 1)
        self.delete_key = QtWidgets.QPushButton("Delete Key")
        self.delete_key.setObjectName("lengthDeleteKey")
        self.delete_key.setToolTip("Delete the selected key (or Ctrl+click it in the viewport)")
        self.delete_key.clicked.connect(lambda: self.edit("deleteKey"))
        row.addWidget(self.delete_key)
        form.addRow("Key scale", row)

        self.length = widgets.CompactSpinBox()
        self.length.setObjectName("lengthKey")
        self.length.setRange(0.0001, 1e6)
        self.length.setDecimals(4)
        self.length.display_decimals = 3
        self.length.setKeyboardTracking(False)
        self.length.setToolTip("The length the selected key gives the guide nearest it (its scale set to match)")
        self.length.valueChanged.connect(lambda v: self.edit("length %r" % v))
        form.addRow("Key length", self.length)

        row = QtWidgets.QHBoxLayout()
        self.change = QtWidgets.QComboBox()
        self.change.setObjectName("lengthChange")
        self.change.addItem("Scale", "scale")
        self.change.addItem("Trim", "trim")
        self.change.setToolTip("How a guide takes its new length: Scale (the whole guide scaled about its root: "
                               "curls and bends keep their shape) or Trim (cut at the tip, or extended along its "
                               "last direction: curls keep their size)")
        self.change.currentIndexChanged.connect(lambda _i: self.edit("change %s" % self.change.currentData()))
        row.addWidget(self.change, 1)
        self.clear = QtWidgets.QPushButton("Clear")
        self.clear.setObjectName("lengthClear")
        self.clear.setToolTip("Delete every key")
        self.clear.clicked.connect(lambda: self.edit("clear"))
        row.addWidget(self.clear)
        form.addRow("Guides", row)
        column.addLayout(form)

        row = QtWidgets.QHBoxLayout()
        self.status = QtWidgets.QLabel()
        self.status.setObjectName("lengthStatus")
        self.status.setProperty("role", "hint")
        self.status.setWordWrap(True)
        row.addWidget(self.status, 1)
        self.cancel = QtWidgets.QPushButton("Cancel")
        self.cancel.setObjectName("lengthCancel")
        self.cancel.setToolTip("Drop the changes since the last Apply: the guides and keys as they were")
        self.cancel.clicked.connect(lambda: self.edit("cancel"))
        row.addWidget(self.cancel)
        self.apply = QtWidgets.QPushButton("Apply")
        self.apply.setObjectName("lengthApply")
        self.apply.setProperty("role", "primary")
        self.apply.setToolTip("Set the guides to the lengths shown (one undo step); the keys stay with the groom "
                              "at x1 to place again")
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

    def refresh(self):
        """The fields from the tool's state (the viewport's clicks and drags); nothing sent back."""
        state = length_state()
        if state == self._state:
            return
        self._state = state
        self._filling = True
        try:
            scales = state.get("scales", [])
            key = state.get("activeKey", -1)
            has_key = 0 <= key < len(scales)
            if has_key:
                self.scale.setValue(scales[key])
                self.length.setValue(state["lengths"][key])
            self.change.setCurrentIndex(max(0, self.change.findData(state.get("change", "scale"))))
            for w in (self.scale, self.length, self.delete_key):
                w.setEnabled(has_key)
            self.clear.setEnabled(bool(scales))
            edited = bool(state.get("edited"))
            self.apply.setEnabled(edited)
            self.cancel.setEnabled(edited)
            if not scales:
                self.status.setText("Click on the scalp to place a key.")
            elif edited:
                self.status.setText("%d keys. Previewing: Apply sets the guides." % len(scales))
            else:
                self.status.setText("%d keys. Middle-drag a key to scale the guides round it." % len(scales))
        finally:
            self._filling = False
