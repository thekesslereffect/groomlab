# SPDX-License-Identifier: Apache-2.0
"""The Length Keys tool's options in Groom Tools (the catalog's "lengthKeys" setting): the
selected key's scale and the length it gives the guide nearest it, how guides take their new
length, Delete Key, Clear, Apply and Cancel. Edits go to the brush context
(groomLabBrushCtx -lengthEdit) and the box reads its state back (-lengthState), lengths in the
scene's units (shown in the length unit, View > Units). The viewport adds keys and drags their
scales; refresh() follows it."""

import json

from groomlab.ui.qt import QtWidgets
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
    """The options, the rows of the tool card's Keys block (its Clear goes in the block's header):
    Key scale and Key length as fill fields, how guides take a new length as chips, Delete key,
    Cancel and Apply."""

    def __init__(self, parent=None):
        super(LengthKeysBox, self).__init__(parent)
        from groomlab import units
        self.setProperty("role", "options")
        self.setObjectName("lengthKeysBox")
        self.length_factor = units.factor("length")  # shown in real units, sent in scene units
        column = QtWidgets.QVBoxLayout(self)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(theme.px(4))

        self.scale = widgets.SoftSlider(1.0, 0.0, 3.0, minimum=0.001, maximum=1000.0, key=None, label="Key scale",
                                        shown_decimals=2)
        self.scale.setObjectName("lengthScale")
        self.scale.setToolTip("The selected key's scale: guides at it take their length times this (1: kept as "
                              "they are, a hold), blending smoothly toward the other keys (middle-drag the key up "
                              "or down in the viewport to set it there)")
        self.scale.committed.connect(lambda v: self.edit("scale %r" % float(v)))
        column.addWidget(self.scale)
        self.length = widgets.SoftSlider(1.0 * self.length_factor, minimum=0.0001 * self.length_factor, key=None,
                                         label="Key length", suffix=units.label("length"))
        self.length.setObjectName("lengthKey")
        self.length.setToolTip("The length the selected key gives the guide nearest it (its scale set to match)")
        self.length.committed.connect(lambda v: self.edit("length %r" % (float(v) / self.length_factor)))
        column.addWidget(self.length)

        guides = QtWidgets.QLabel("Guides")
        guides.setProperty("role", "muted")
        column.addWidget(guides)
        self.change_group = QtWidgets.QButtonGroup(self)
        self.change_group.setExclusive(True)
        self.change_buttons = {}
        for value, label, tip in (("scale", "Scale", "The whole guide scaled about its root: curls and bends keep "
                                   "their shape"),
                                  ("trim", "Trim", "Cut at the tip, or extended along its last direction: curls keep "
                                   "their size")):
            b = widgets.chip(label, "How a guide takes its new length: " + tip, value == "scale", "lengthChange_" + value)
            b.clicked.connect(lambda _=False, v=value: self.edit("change %s" % v))
            self.change_group.addButton(b)
            self.change_buttons[value] = b
        column.addWidget(widgets.chip_row(self.change_buttons.values()))

        self.clear = QtWidgets.QToolButton()  # (in the Keys block's header)
        self.clear.setText("Clear")
        self.clear.setObjectName("lengthClear")
        self.clear.setAutoRaise(True)
        self.clear.setProperty("role", "ghost")
        self.clear.setToolTip("Delete every key")
        self.clear.clicked.connect(lambda: self.edit("clear"))

        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, theme.px(2), 0, 0)
        row.setSpacing(theme.px(4))
        self.delete_key = QtWidgets.QToolButton()
        self.delete_key.setText("Delete key")
        self.delete_key.setObjectName("lengthDeleteKey")
        self.delete_key.setProperty("role", "ghost")
        self.delete_key.setToolTip("Delete the selected key (or Ctrl+click it in the viewport)")
        self.delete_key.clicked.connect(lambda: self.edit("deleteKey"))
        row.addWidget(self.delete_key)
        row.addStretch(1)
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
        self.status = QtWidgets.QLabel()
        self.status.setObjectName("lengthStatus")
        self.status.setProperty("role", "muted")
        self.status.setWordWrap(True)
        column.addWidget(self.status)
        self._state = None

    def change_name(self):
        """How guides take a new length: "scale" or "trim"."""
        return next((v for v, b in self.change_buttons.items() if b.isChecked()), "scale")

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
                if not self.scale.busy():
                    self.scale.setValue(scales[key])
                if not self.length.busy():
                    self.length.setValue(state["lengths"][key] * self.length_factor)
            change = state.get("change", "scale")
            if change in self.change_buttons:
                self.change_buttons[change].setChecked(True)
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
