# SPDX-License-Identifier: Apache-2.0
"""GroomLab Groom Tools: everything you do to a groom's guides, in four tabs (Qt: groomlab.ui.qt).

    Tools      make a groom; the brushes (create, shape, trim, freeze);
               the guide group they work in; guide groups and edits on many guides
    Paint      painted maps (0-1 values on the guides any param can read)
    Sculpt     correctives (sculpt layers on top of the groom)
    Reference  photos as cameras to groom to (look through, align by points, lock)

Pick a tool and drag in the viewport over the groom (the pinned one, else the
selected one). The brush settings under the tabs follow the tool: radius,
strength (signed for tools that work both ways), which guides, symmetry, pen
pressure, keeping off the scalp; Place and Draw add the new guide's shape.
Radius and strength changed in the viewport (middle-drag) show up here.

    import groomlab.ui.palette as palette
    palette.show()               # or show(tab="paint"), show(tab="photos")
"""

from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtWidgets

import groomlab
import groomlab_core
from groomlab.ui import definitions, length_keys_box, theme, tool_tabs, widgets

# The tools are the core's brush catalog (brush_catalog.h), the one list both hosts build their
# palettes from: id, label, category, default strength, whether the strength is signed, a hint
# and the settings its card shows. "select" is the lasso and "freeze" the mask; the rest are
# brush modes.
CATALOG = groomlab_core.brush_catalog()
CATEGORIES = groomlab_core.brush_categories()
SETTINGS = dict((s["id"], s) for s in groomlab_core.brush_settings())
# (section, [(tool, label, tooltip)]) per category, in the catalog's order.
SECTIONS = [(c, [(t["id"], t["label"], t["hint"]) for t in CATALOG if t["category"] == c]) for c in CATEGORIES]
# The settings each tool's card shows (SETTINGS' ids).
TOOL_SETTINGS = dict((t["id"], set(t["settings"])) for t in CATALOG)
SIGNED = set(t["id"] for t in CATALOG if t["signed"])
DEFAULT_STRENGTH = dict((t["id"], t["default_strength"]) for t in CATALOG)
CREATORS = set(t for t, s in TOOL_SETTINGS.items() if "newGuides" in s)
# Brushes that also shape part lines and flow arrows (the Brushes box picks what they act on).
TARGETABLE = set(t for t, s in TOOL_SETTINGS.items() if "targets" in s)
PART = "part"
# The tabs, the texts and the option lists: the UI definitions every host shares (engine/ui:
# palette.json, choices.json; groomlab.ui.definitions).
TABS = [t["id"] for t in definitions.get("palette/tabs")]
TAB_ALIASES = dict((a, t["id"]) for t in definitions.get("palette/tabs") for a in t.get("aliases", []))  # old ids
# What each tool does, shown under its name while it is active (the tiles' tooltips say the same).
HINTS = dict((t["id"], t["hint"]) for t in CATALOG)
NAMES = dict((t["id"], t["label"]) for t in CATALOG)
GESTURES = definitions.text("gestures")
SYMMETRY_TIP = definitions.text("symmetryTip")
# What the Erase brush does not do, said under its settings: the hair itself goes with the density map.
ERASE_NOTE = definitions.text("eraseNote")


def _options(name):
    """A choices.json list as (value, label, tooltip)."""
    return [(c["value"], c["label"], c.get("tip", "")) for c in definitions.choices(name)]


# The Shape brush's channels: (name, label, tooltip); a negative strength does the opposite of each.
TUBE_CHANNELS = _options("tubeChannel")
TUBE_SHAPES = _options("tubeShape")


def flow_count_text(arrows, sinks):
    """The Flow card's count line, as every host words it: "34 arrows · 2 sinks", "No flow arrows yet"."""
    if not arrows and not sinks:
        return "No flow arrows yet"
    return "%d arrow%s · %d sink%s" % (arrows, "" if arrows == 1 else "s", sinks, "" if sinks == 1 else "s")


def section_shown(section, tool, tab):
    """Whether a Groom Tools section (palette.json's) shows: its "tools" (the active tool one of
    them), its "tabs" (the current tab one of them); a section with neither always shows."""
    if section.get("tools") and tool not in section["tools"]:
        return False
    return not section.get("tabs") or tab in section["tabs"]


def tools_of(*ids):
    """(tool, label, tooltip) for the catalog's tools `ids`, in the catalog's order."""
    return [(t["id"], t["label"], t["hint"]) for t in CATALOG if t["id"] in ids]


STARTER_VAR = "groomLabHideStarter"
# A first groom, step by step: (key, what to do, the tip on how). The card ticks each off as the
# groom gets there; clicking a step still to do takes you to it.
STARTER_STEPS = [(step["id"], step["label"], step["tip"]) for step in definitions.get("palette/starter/steps")]
LOOK_TYPES = set(definitions.get("palette/starter/lookTypes"))


def starter_progress(groom):
    """{step key: done} for the first-groom card."""
    groups = groomlab.list_guide_groups(groom)
    # The default graph has a look of its own (Noise, HairColor): the step is the groom's own graph.
    types = {n.get("type") for n in groomlab.get_graph(groom).get("nodes", [])}
    return {"guides": any(g["count"] for g in groups), "look": bool(types & LOOK_TYPES)}


class StarterCard(QtWidgets.QFrame):
    """The first-groom checklist: a step per row, done ones ticked; hides once all are done
    or it is dismissed (remembered)."""

    def __init__(self, owner):
        super(StarterCard, self).__init__()
        self.owner = owner
        self.setProperty("role", "empty")
        column = QtWidgets.QVBoxLayout(self)
        pad = theme.px(12)
        column.setContentsMargins(pad, theme.px(10), theme.px(6), pad)
        column.setSpacing(theme.px(4))
        top = QtWidgets.QHBoxLayout()
        title = QtWidgets.QLabel(definitions.get("palette/starter/title"))
        title.setProperty("role", "subheading")
        top.addWidget(title, 1)
        close = QtWidgets.QToolButton()
        close.setText("\u2715")
        close.setAutoRaise(True)
        close.setToolTip(definitions.get("palette/starter/dismissTip"))
        close.clicked.connect(self.dismiss)
        top.addWidget(close)
        column.addLayout(top)
        self.rows = {}
        for key, text, tip in STARTER_STEPS:
            b = QtWidgets.QPushButton(text)
            b.setToolTip(tip)
            b.setFlat(True)
            b.setProperty("role", "step")
            b.clicked.connect(lambda _=False, k=key: self.go(k))
            column.addWidget(b)
            self.rows[key] = (b, text)
        self.tip = QtWidgets.QLabel()
        self.tip.setWordWrap(True)
        self.tip.setProperty("role", "hint")
        column.addWidget(self.tip)
        self._state = None
        self._dismissed = False

    def following(self):
        """Whether the poll should still ask the card: not once it is dismissed, nor once every
        step is done for the groom (a new groom asks again)."""
        return not self._dismissed and not (self._state and all(self._state.values()))

    @staticmethod
    def dismissed():
        try:
            from maya import cmds
            return cmds.optionVar(exists=STARTER_VAR) and bool(cmds.optionVar(query=STARTER_VAR))
        except ImportError:
            return False

    def dismiss(self):
        from maya import cmds
        cmds.optionVar(intValue=(STARTER_VAR, 1))
        self._dismissed = True
        self.setVisible(False)

    def refresh(self, groom):
        self._dismissed = self.dismissed()
        if groom != getattr(self, "_groom", None):
            self._groom = groom
            self._state = None
        if not groom or self._dismissed:
            self.setVisible(False)
            return
        try:
            done = starter_progress(groom)
        except (RuntimeError, ValueError):
            self.setVisible(False)
            return
        self.setVisible(not all(done.values()))
        if done == self._state:
            return
        self._state = done
        following = None
        for key, _, tip in STARTER_STEPS:
            button, text = self.rows[key]
            if done[key]:
                button.setText("\u2713  " + text)
                button.setProperty("done", True)
            else:
                button.setText(("\u25cb  " if following else "\u25b6  ") + text)
                button.setProperty("done", False)
                following = following or tip
            widgets.repolish(button)
        self.tip.setText(following or "")

    def go(self, key):
        if key == "guides":
            self.owner.show_tab("tools")
            self.owner.activate("place")
        else:
            self.owner.open_graph()


class TubePanel(QtWidgets.QWidget):
    """The tubes as a whole, Groom Tools' Tubes section (opened by the Shape brush, Place and Draw): the targets' shape,
    width, flatness and twist (each change one undo step), the groom's shape library with its
    editor, the shape new guides take, and the groom's Fill Tubes and Tube Blend (its Interpolate
    nodes' fillTubes and tubeBlend). Follows the groom through refresh().

    The targets: the selected guides, else the soloed ones (those left unfrozen:
    groomlab.tube_targets). The shape picker lists the built-ins and the library; its choice (and
    Width, Flatness and Twist as shown) is also what Place and Draw give new guides (the brush's
    placement tube: the shape always, the sections where no guides are near to blend them from).
    The editor shows the chosen shape: a built-in read-only (Edit as Custom copies it into the
    library), a library shape editable: dragging previews live on the groom (no undo step), the
    release commits (one undo step, every guide taking it following)."""

    PREVIEW_MS = 60  # a drag's live preview at most this often

    def __init__(self, owner):
        super(TubePanel, self).__init__()
        self.owner = owner
        self.setObjectName("tubePanel")
        self.setProperty("role", "options")
        self.setToolTip(SETTINGS["tube"]["hint"])
        column = QtWidgets.QVBoxLayout(self)
        column.setContentsMargins(0, theme.px(4), 0, 0)
        column.setSpacing(theme.px(6))
        self.targets = QtWidgets.QLabel()
        self.targets.setWordWrap(True)
        self.targets.setProperty("role", "hint")
        column.addWidget(self.targets)
        form = QtWidgets.QFormLayout()
        form.setContentsMargins(0, 0, 0, 0)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        column.addLayout(form)
        self.shape = QtWidgets.QComboBox()
        self.shape.setObjectName("tubeShape")
        self.shape.setToolTip("The tube shape: picked, it goes to the targeted guides and to the guides Place and Draw "
                              "make. The built-ins, then the groom's library of custom shapes")
        self.shape.activated.connect(lambda _i: self.pick(self.shape.currentData()))
        form.addRow("Shape", self.shape)
        # The editor: the chosen shape's outline, and the library's edits.
        from groomlab.ui.tube_shape_editor import TubeShapeEditor
        self.editor = TubeShapeEditor()
        self.editor.dragged.connect(self.shape_dragged)
        self.editor.committed.connect(self.shape_committed)
        column.addWidget(self.editor)
        options = QtWidgets.QHBoxLayout()
        options.setSpacing(theme.px(6))
        self.edit_custom = QtWidgets.QPushButton("Edit as Custom")
        self.edit_custom.setObjectName("tubeEditAsCustom")
        self.edit_custom.setToolTip("Copy this built-in shape into the groom's library as a new shape you can edit "
                                    "(the targeted guides take it)")
        self.edit_custom.clicked.connect(self.edit_as_custom)
        self.mirror = QtWidgets.QCheckBox("Mirror")
        self.mirror.setObjectName("tubeShapeMirror")
        self.mirror.setChecked(True)
        self.mirror.setToolTip("Keep the outline's left and right sides alike (about the up axis): a point and its "
                               "mirror image move, come and go together")
        self.mirror.toggled.connect(self.set_mirror)
        self.smooth_shape = QtWidgets.QCheckBox("Smooth")
        self.smooth_shape.setObjectName("tubeShapeSmooth")
        self.smooth_shape.setChecked(True)
        self.smooth_shape.setToolTip("A smooth curve through the points; off: straight edges and sharp corners")
        self.smooth_shape.toggled.connect(self.set_smooth)
        options.addWidget(self.edit_custom)
        options.addStretch(1)
        options.addWidget(self.mirror)
        options.addWidget(self.smooth_shape)
        column.addLayout(options)
        library = QtWidgets.QHBoxLayout()
        library.setSpacing(theme.px(4))
        self.new_shape = QtWidgets.QPushButton("New")
        self.new_shape.setObjectName("tubeShapeNew")
        self.new_shape.setToolTip("A new library shape (a circle to start from)")
        self.new_shape.clicked.connect(self.new_library_shape)
        self.rename_shape = QtWidgets.QPushButton("Rename")
        self.rename_shape.setObjectName("tubeShapeRename")
        self.rename_shape.setToolTip("Rename this library shape (its guides keep it)")
        self.rename_shape.clicked.connect(self.start_rename)
        self.duplicate_shape = QtWidgets.QPushButton("Duplicate")
        self.duplicate_shape.setObjectName("tubeShapeDuplicate")
        self.duplicate_shape.setToolTip("A copy of this shape in the library, to change on its own")
        self.duplicate_shape.clicked.connect(self.duplicate_library_shape)
        self.delete_shape = QtWidgets.QPushButton("Delete")
        self.delete_shape.setObjectName("tubeShapeDelete")
        self.delete_shape.setToolTip("Remove this shape from the library: click twice (its guides become round)")
        self.delete_shape.clicked.connect(self.delete_library_shape)
        for b in (self.new_shape, self.rename_shape, self.duplicate_shape, self.delete_shape):
            library.addWidget(b)
        column.addLayout(library)
        self.rename_field = QtWidgets.QLineEdit()
        self.rename_field.setObjectName("tubeShapeName")
        self.rename_field.setPlaceholderText("New name, then Enter (Esc: keep it)")
        self.rename_field.returnPressed.connect(self.finish_rename)
        self.rename_field.installEventFilter(self)
        self.rename_field.hide()
        column.addWidget(self.rename_field)
        self.apply_shape = QtWidgets.QPushButton("Apply to Selected")
        self.apply_shape.setObjectName("tubeShapeApply")
        self.apply_shape.setToolTip("Give the targeted guides this shape")
        self.apply_shape.clicked.connect(lambda: self.apply(shape=self._choice))
        column.addWidget(self.apply_shape)
        self._disarm = QtCore.QTimer(self)
        self._disarm.setSingleShot(True)
        self._disarm.setInterval(4000)
        self._disarm.timeout.connect(self.disarm_delete)
        self._armed = False
        self._preview = QtCore.QTimer(self)
        self._preview.setSingleShot(True)
        self._preview.setInterval(self.PREVIEW_MS)
        self._preview.timeout.connect(self.send_preview)
        self._previewing = False
        sections = QtWidgets.QFormLayout()
        sections.setContentsMargins(0, 0, 0, 0)
        sections.setLabelAlignment(QtCore.Qt.AlignRight)
        sections.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        column.addLayout(sections)
        from groomlab import units
        self.length_factor = units.factor("length")  # shown in real units, stored in scene units
        self.width = widgets.SoftSlider(0.1 * self.length_factor, 0.0, 1.0 * self.length_factor, minimum=0.0,
                                        key="palette.tubeWidth@" + units.unit("length")[0])
        self.width.setObjectName("tubeWidth")
        self.width.setToolTip("The widest section's half-width (a round tube's radius): the targeted tubes scaled "
                              "to it, their taper and what the brush painted kept; new guides' where none are near")
        self.width.committed.connect(self.set_width)
        self.flatness = widgets.SoftSlider(0.0, 0.0, 1.0, minimum=0.0, maximum=0.99, key=None)
        self.flatness.setObjectName("tubeFlatness")
        self.flatness.setToolTip("0: as the shape is drawn; toward 1 a flat ribbon (the sections' aspect 1 - this, "
                                 "the same root to tip)")
        self.flatness.committed.connect(lambda v: self.apply(aspect=max(1.0 - v, 0.01)))
        self.twist = widgets.SoftSlider(0.0, -180.0, 180.0, key=None)
        self.twist.setObjectName("tubeTwist")
        self.twist.setToolTip("Degrees: the sections turned about the guide from lying flat on the scalp, the same "
                              "root to tip (the brush's Twist paints it along the guide)")
        self.twist.committed.connect(lambda v: self.apply(twist=v * 3.14159265358979 / 180.0))
        sections.addRow("Width (%s)" % units.label("length"), self.width)
        sections.addRow("Flatness", self.flatness)
        sections.addRow("Twist (°)", self.twist)
        # Smooth: the targeted tubes' painted sections evened out along each guide (one undo step).
        smooth_row = QtWidgets.QHBoxLayout()
        smooth_row.setSpacing(theme.px(6))
        self.smooth = QtWidgets.QPushButton("Smooth")
        self.smooth.setObjectName("tubeSmooth")
        self.smooth.setToolTip("Smooth the targeted guides' tubes along each guide: what the brush painted evened "
                               "out, the root's and tip's sections kept")
        self.smooth.clicked.connect(self.smooth_selected)
        self.smooth_what = QtWidgets.QComboBox()
        self.smooth_what.setObjectName("tubeSmoothChannels")
        for value, label, _tip in _options("tubeSmooth"):
            self.smooth_what.addItem(label, value)
        self.smooth_what.setToolTip(definitions.get("choices/tubeSmooth")["tip"])
        smooth_row.addWidget(self.smooth)
        smooth_row.addWidget(self.smooth_what, 1)
        sections.addRow("", smooth_row)
        # What needs targets (the rest also sets the placement tube, targets or not).
        self.edits = (self.smooth, self.smooth_what, self.apply_shape)
        self.fill = QtWidgets.QCheckBox("Fill Tubes")
        self.fill.setObjectName("fillTubes")
        self.fill.setToolTip("Strands rooted inside a guide's tube ride it to the tip, flattening, twisting and "
                             "tapering with it (the Interpolate nodes' fillTubes; the tubes of every guide level fill, "
                             "the deepest winning)")
        self.fill.toggled.connect(self.set_fill)
        self.blend = widgets.SoftSlider(25.0, 0.0, 100.0, minimum=0.0, maximum=100.0, key=None)
        self.blend.setObjectName("tubeBlend")
        self.blend.setToolTip("Tube Blend: the tubes' soft edge, a share of each tube's half-width. Strands rooted "
                              "just outside a tube lean toward riding it, so no seam shows round it (0: a hard edge)")
        self.blend.committed.connect(lambda v: self.set_fill_tubes(blend=v / 100.0))
        self.blend_label = QtWidgets.QLabel("Tube Blend")
        fill_form = QtWidgets.QFormLayout()
        fill_form.setContentsMargins(0, 0, 0, 0)
        fill_form.setLabelAlignment(QtCore.Qt.AlignRight)
        fill_form.addRow(self.fill)
        fill_form.addRow(self.blend_label, widgets.with_unit(self.blend, "%"))
        column.addLayout(fill_form)
        self._selection = []
        self._source = "none"
        self._reference = 0.0  # the first target's widest section (scene units)
        self._choice = "round"  # the shape picked: the targets', else the last picked
        self._library = []      # the groom's custom shapes (groomlab.tube_shapes' non-built-ins)
        self._groom = None
        self._pushed = None     # the placement tube last sent to the brush
        self._targets_key = None  # the targets and their shapes as last read
        self.fill_shapes()
        self.show_choice()

    # -- Reading the groom ---------------------------------------------------------

    def busy(self):
        """A slider held, a field being typed in or a point dragged: refresh() leaves the panel as it is."""
        for slider in (self.width, self.flatness, self.twist, self.blend):
            if slider.slider.isSliderDown() or slider.field.hasFocus():
                return True
        return self.editor.busy() or self.rename_field.isVisible()

    def refresh(self, groom, full=True, force=False):
        """The targets' tubes (each guide has its own: the panel edits the targets only), the shape
        library and, with `full`, the groom's Fill Tubes. The shape shown is the targets', "Mixed"
        when they differ; width, flatness and twist are the first target's. Left alone while the
        artist is busy in it (busy()); `force` (after the panel's own edit): only not mid-drag."""
        if self.editor.busy() if force else self.busy():
            return
        info = {"source": "none", "guides": [], "tube": None, "shapes": []}
        if groom:
            try:
                info = groomlab.tube_targets(groom)
                if full or groom != self._groom:
                    self._library = [s for s in groomlab.tube_shapes(groom) if not s["builtin"]]
            except (RuntimeError, ValueError):
                pass
        elif self._groom:
            self._library = []
        self._groom = groom
        self._selection = list(info["guides"])
        self._source = info["source"]
        tube = info["tube"]
        shapes = info["shapes"]
        self.fill_shapes()
        for w in self.edits:
            w.setEnabled(tube is not None)
        if tube is None:
            self.targets.setText("No guides selected or soloed: the shape and values here go to the guides Place and "
                                 "Draw make. Select guides (Select tool, or Shift with a brush) to set theirs.")
            self.show_mixed(False)
        else:
            count = len(self._selection)
            how = "selected" if self._source == "selection" else "soloed"
            self.targets.setText("%d guide%s %s: changes here go to %s, and to the guides Place and Draw make." %
                                 (count, "" if count == 1 else "s", how, "it" if count == 1 else "them"))
            widths, aspects, twists = tube.get("width") or [0.1], tube.get("aspect") or [1.0], tube.get("twist") or [0.0]
            self._reference = max(widths)
            for w in (self.shape, self.width, self.flatness, self.twist):
                w.blockSignals(True)
            self.show_mixed(len(shapes) > 1)
            # The choice follows the targets when they (or their shapes) change, and is left alone
            # otherwise (a shape made or picked in the editor while they stay selected).
            key = (tuple(self._selection), tuple(shapes))
            if len(shapes) == 1 and key != self._targets_key:
                self._choice = shapes[0]
            self._targets_key = key
            self.width.setValue(self._reference * self.length_factor)
            self.flatness.setValue(min(max(1.0 - sum(aspects) / len(aspects), 0.0), 0.99))
            self.twist.setValue(sum(twists) / len(twists) * 180.0 / 3.14159265358979)
            for w in (self.shape, self.width, self.flatness, self.twist):
                w.blockSignals(False)
        self.show_choice(mixed=tube is not None and len(shapes) > 1)
        self.push_placement()
        if full:
            state = None
            if groom:
                try:
                    state = groomlab.fill_tubes(groom)
                except (RuntimeError, ValueError):
                    state = None
            self.fill.blockSignals(True)
            self.fill.setEnabled(state is not None)
            self.fill.setChecked(bool(state and state["fillTubes"]))
            self.fill.blockSignals(False)
            if state is None:
                self.fill.setToolTip("The groom's graph has no Interpolate node: nothing to fill the tubes")
            self.blend.setEnabled(bool(state and state["fillTubes"]))
            self.blend_label.setEnabled(self.blend.isEnabled())
            if state is not None:
                self.blend.setValue(state["tubeBlend"] * 100.0)

    def fill_shapes(self):
        """The shape picker: the built-ins, then the library (rebuilt only when that changes)."""
        names = [name for name, _, _ in TUBE_SHAPES] + [s["name"] for s in self._library]
        have = [self.shape.itemData(i) for i in range(self.shape.count()) if self.shape.itemData(i)]
        if have == names:
            return
        mixed = self.shape.count() > 0 and self.shape.itemData(0) == ""
        self.shape.blockSignals(True)
        self.shape.clear()
        if mixed:
            self.shape.addItem("Mixed", "")
        for name, label, tip in TUBE_SHAPES:
            self.shape.addItem(label, name)
            self.shape.setItemData(self.shape.count() - 1, tip, QtCore.Qt.ToolTipRole)
        if self._library:
            self.shape.insertSeparator(self.shape.count())
        for s in self._library:
            self.shape.addItem(s["name"], s["name"])
            self.shape.setItemData(self.shape.count() - 1, "A custom shape of this groom's library (%d guide%s)" %
                                   (s["users"], "" if s["users"] == 1 else "s"), QtCore.Qt.ToolTipRole)
        self.shape.blockSignals(False)

    def library_shape(self, name):
        for s in self._library:
            if s["name"] == name:
                return s
        return None

    def show_choice(self, mixed=False):
        """The picker, the editor and the library buttons on the chosen shape."""
        if self.library_shape(self._choice) is None and self._choice not in [n for n, _, _ in TUBE_SHAPES]:
            self._choice = "round"  # (a library shape gone: renamed, deleted, another groom)
        self.shape.blockSignals(True)
        self.shape.setCurrentIndex(0 if mixed else max(0, self.shape.findData(self._choice)))
        self.shape.blockSignals(False)
        custom = self.library_shape(self._choice)
        if custom is not None:
            self.editor.set_shape(custom["points"], custom["smooth"], editable=True)
            if self.mirror.isChecked() and self.editor.set_mirror(True):
                self.shape_committed(self.editor.points)  # made symmetric: saved so
        else:
            import groomlab_core
            points, smooth = groomlab_core.builtin_tube_shape_points(self._choice)
            self.editor.set_shape(points, smooth, editable=False,
                                  note="Built in: Edit as Custom to change it")
        self.smooth_shape.blockSignals(True)
        self.smooth_shape.setChecked(self.editor.smooth)
        self.smooth_shape.blockSignals(False)
        has_groom = bool(self._groom)
        self.edit_custom.setVisible(custom is None)
        self.edit_custom.setEnabled(has_groom)
        self.smooth_shape.setEnabled(custom is not None)
        self.new_shape.setEnabled(has_groom)
        for b in (self.rename_shape, self.duplicate_shape, self.delete_shape):
            b.setEnabled(custom is not None)
        self.apply_shape.setEnabled(bool(self._selection))

    def show_mixed(self, on):
        """The shape picker's "Mixed" entry (first, no shape of its own): there while the targets'
        shapes differ."""
        has = self.shape.count() > 0 and self.shape.itemData(0) == ""
        if on and not has:
            self.shape.insertItem(0, "Mixed", "")
            self.shape.setItemData(0, "The targeted guides have different shapes: pick one to give them all",
                                   QtCore.Qt.ToolTipRole)
        elif not on and has:
            self.shape.removeItem(0)

    # -- The placement tube ------------------------------------------------------------

    def placement(self):
        """The tube Place and Draw give new guides: the chosen shape, Width, Flatness and Twist as shown."""
        return {"shape": self._choice, "width": self.width.value() / self.length_factor,
                "aspect": max(1.0 - self.flatness.value(), 0.01), "twist": self.twist.value() * 3.14159265358979 / 180.0}

    def push_placement(self):
        """The placement tube sent to the brush (when it changed)."""
        tube = self.placement()
        if tube == self._pushed:
            return
        from maya import cmds
        import json
        if not cmds.contextInfo(groomlab.BRUSH_CONTEXT, exists=True):
            return
        try:
            cmds.groomLabBrushCtx(groomlab.BRUSH_CONTEXT, edit=True, placeTube=json.dumps(tube))
            self._pushed = tube
        except RuntimeError as e:
            self.owner.status.setText(str(e).strip())

    # -- Edits -----------------------------------------------------------------------

    def run(self, fn, *args, **kwargs):
        """fn(*args, **kwargs), its error said in the status line; the panel read again after.
        Returns whether it worked."""
        ok = True
        try:
            fn(*args, **kwargs)
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            ok = False
        self.refresh(self.owner.groom(), force=True)
        return ok

    def pick(self, name):
        """A shape picked: given to the targets (one undo step) and to the guides Place and Draw make."""
        if name == "":  # "Mixed": nothing to give
            return
        self._choice = name
        if self._selection:
            self.apply(shape=name)
        self.show_choice()
        self.push_placement()

    def apply(self, **edit):
        """An edit of the targets' tubes (set_tube: one undo step); with none, the placement tube's."""
        groom = self.owner.groom()
        if edit.get("shape") == "":
            return
        if groom and self._selection:
            self.run(groomlab.set_tube, groom, guides=self._selection, **edit)
        self.push_placement()

    def set_width(self, value):
        """The width slider: the targets scaled so the first one's widest section is `value`."""
        groom = self.owner.groom()
        if groom and self._selection:
            width = value / self.length_factor
            if self._reference > 0.0:
                self.run(groomlab.scale_tube, groom, width / self._reference, guides=self._selection)
            else:
                self.run(groomlab.set_tube, groom, guides=self._selection, width=width)
        self.push_placement()

    def smooth_selected(self):
        """Smooth: the targets' channel the brush paints (or all three) evened out along each
        guide (smooth_tube: one undo step)."""
        groom = self.owner.groom()
        if not groom or not self._selection:
            return
        channel = self.owner.tube_channel_name() if self.smooth_what.currentData() == "current" else "all"
        self.run(groomlab.smooth_tube, groom, guides=self._selection, channel=channel)

    def unique_name(self, base):
        taken = set(n for n, _, _ in TUBE_SHAPES) | set(s["name"] for s in self._library)
        k = 1
        while "%s %d" % (base, k) in taken:
            k += 1
        return "%s %d" % (base, k)

    def edit_as_custom(self):
        """The chosen built-in copied into the library as a new shape, chosen (the targets take it:
        one undo step)."""
        groom = self.owner.groom()
        if not groom:
            return
        import groomlab_core
        points, smooth = groomlab_core.builtin_tube_shape_points(self._choice)
        name = self.unique_name(self._choice)
        from groomlab.base import undo_chunk
        try:
            with undo_chunk("tube shape"):
                groomlab.add_tube_shape(groom, name, points, smooth=smooth)
                if self._selection:
                    groomlab.set_tube(groom, guides=self._selection, shape=name)
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            return
        self._choice = name
        self.refresh(groom, force=True)
        self.push_placement()
        self.owner.status.setText("%s: a copy of the built-in to edit (drag its points)." % name)

    def new_library_shape(self):
        groom = self.owner.groom()
        if not groom:
            return
        import groomlab_core
        points, _ = groomlab_core.builtin_tube_shape_points("round")
        name = self.unique_name("shape")
        if self.run(groomlab.add_tube_shape, groom, name, points, smooth=True):
            self._choice = name
            self.refresh(groom, force=True)
            self.push_placement()

    def duplicate_library_shape(self):
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if not groom or custom is None:
            return
        name = self.unique_name(custom["name"])
        if self.run(groomlab.add_tube_shape, groom, name, custom["points"], smooth=custom["smooth"]):
            self._choice = name
            self.refresh(groom, force=True)
            self.push_placement()

    def start_rename(self):
        """Rename: a name field in place (Enter renames, Esc or leaving it keeps the name); no dialog."""
        if self.library_shape(self._choice) is None:
            return
        self.rename_field.setText(self._choice)
        self.rename_field.show()
        self.rename_field.setFocus()
        self.rename_field.selectAll()

    def finish_rename(self):
        groom = self.owner.groom()
        new = self.rename_field.text().strip()
        old = self._choice
        self.hide_rename()
        if not groom or not new or new == old:
            return
        if self.run(groomlab.rename_tube_shape, groom, old, new):
            self._choice = new
            self.refresh(groom, force=True)
            self.push_placement()

    def eventFilter(self, obj, event):
        if obj is self.rename_field and event.type() == QtCore.QEvent.KeyPress and event.key() == QtCore.Qt.Key_Escape:
            self.hide_rename()
            return True
        if obj is self.rename_field and event.type() == QtCore.QEvent.FocusOut and self.rename_field.isVisible():
            self.rename_field.hide()
        return super(TubePanel, self).eventFilter(obj, event)

    def hide_rename(self):
        """The name field gone, the focus on the editor (not passed on to a slider's field, which
        would read as typing there)."""
        self.editor.setFocus()
        self.rename_field.hide()

    def delete_library_shape(self):
        """Delete: the first click arms the button (it says how many guides go round), a second one
        within a few seconds deletes (one undo step); no dialog."""
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if not groom or custom is None:
            return
        if not self._armed:
            self._armed = True
            users = custom["users"]
            self.delete_shape.setText("Delete? %d go%s round" % (users, "es" if users == 1 else ""))
            self.delete_shape.setProperty("role", "danger")
            self.delete_shape.style().unpolish(self.delete_shape)
            self.delete_shape.style().polish(self.delete_shape)
            self._disarm.start()
            return
        self.disarm_delete()
        if self.run(groomlab.remove_tube_shape, groom, custom["name"]):
            self._choice = "round"
            self.refresh(groom, force=True)
            self.push_placement()

    def disarm_delete(self):
        self._armed = False
        self._disarm.stop()
        self.delete_shape.setText("Delete")
        self.delete_shape.setProperty("role", None)
        self.delete_shape.style().unpolish(self.delete_shape)
        self.delete_shape.style().polish(self.delete_shape)

    # -- The editor ------------------------------------------------------------------

    def shape_dragged(self, points):
        """A point moving: the groom shows the shape so, at most every PREVIEW_MS (no undo step)."""
        if not self._preview.isActive():
            self._preview.start()

    def send_preview(self):
        groom = self.owner.groom()
        if not groom or self.editor.problem or self.library_shape(self._choice) is None:
            return
        try:
            groomlab.preview_tube_shape(groom, self._choice, self.editor.points, smooth=self.editor.smooth)
            self._previewing = True
        except (RuntimeError, ValueError):
            pass

    def shape_committed(self, points):
        """An edit settled: the library shape takes the points (one undo step; every guide taking it
        follows). Refused (crossing itself, leaving out its center): said, the shape as it was."""
        self._preview.stop()
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if not groom or custom is None:
            return
        try:
            groomlab.set_tube_shape_points(groom, custom["name"], points, smooth=self.editor.smooth)
            self.owner.status.setText("")
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            if self._previewing:
                groomlab.end_guide_preview(groom)
        self._previewing = False
        self.refresh(groom, force=True)

    def set_mirror(self, on):
        if self.editor.set_mirror(on) and self.library_shape(self._choice) is not None:
            self.shape_committed(self.editor.points)
        self.editor.update()

    def set_smooth(self, on):
        groom = self.owner.groom()
        custom = self.library_shape(self._choice)
        if groom and custom is not None:
            self.run(groomlab.set_tube_shape_points, groom, custom["name"], smooth=on)

    # -- Fill Tubes ------------------------------------------------------------------

    def set_fill(self, on):
        self.set_fill_tubes(on=on)

    def set_fill_tubes(self, **kwargs):
        groom = self.owner.groom()
        if not groom:
            return
        try:
            groomlab.set_fill_tubes(groom, **kwargs)
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
        self.refresh(groom, force=True)


class ToolPalette(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super(ToolPalette, self).__init__(parent)
        self.setWindowTitle("GroomLab Groom Tools")
        self.setWindowIcon(widgets.icon("logo"))
        self.tool = None
        self.strengths = dict(DEFAULT_STRENGTH)
        self.buttons = {}  # tool -> its tile (the first made; a tool with tiles on two tabs: tiles)
        self.tiles = {}    # tool -> every tile of it

        # Everything scrolls in one themed column; the window paints the theme's background.
        frame = QtWidgets.QVBoxLayout(self)
        frame.setContentsMargins(0, 0, 0, 0)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        frame.addWidget(scroll)
        content = QtWidgets.QWidget()
        content.setObjectName("groomToolsContent")
        scroll.setWidget(content)
        outer = QtWidgets.QVBoxLayout(content)
        outer.setContentsMargins(theme.px(12), theme.px(10), theme.px(12), theme.px(12))
        outer.setSpacing(theme.px(10))

        # What the tools work on: the current groom (groomlab.active_groom: the Graph Editor's too
        # unless pinned) and its group.
        top = QtWidgets.QHBoxLayout()
        top.setSpacing(theme.px(4))
        self.grooms = QtWidgets.QComboBox()
        header = definitions.get("palette/header")
        self.grooms.setToolTip(header["groom"]["tip"])
        self.grooms.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.grooms.setMinimumContentsLength(8)
        self.grooms.activated.connect(self.groom_picked)
        top.addWidget(self.grooms, 1)
        # The eyes: the groom's hair (the strands alone), and its guides (every guide group);
        # Shift+click or right-click: every groom's.
        self.visible = self.header_button("eye", header["visible"]["tip"], checkable=True)
        self.visible.setObjectName("hairVisible")
        self.visible.toggled.connect(self.hair_visibility)
        self.eye_menu(self.visible, header["visible"].get("items", []))
        top.addWidget(self.visible)
        self.guides_eye = self.header_button("guides", header["guides"]["tip"], checkable=True)
        self.guides_eye.setObjectName("guidesVisible")
        self.guides_eye.toggled.connect(self.guides_visibility)
        self.eye_menu(self.guides_eye, header["guides"].get("items", []))
        top.addWidget(self.guides_eye)
        self.header_icons = {}
        graph = self.header_button(None, header["graph"]["tip"])
        graph.setIcon(widgets.icon("graph"))
        graph.clicked.connect(self.open_graph)
        self.header_icons["graph"] = graph
        top.addWidget(graph)
        more = self.header_button("more", header["more"]["tip"])
        more.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        self.more_menu = QtWidgets.QMenu(more)
        widgets.fill_menu(self.more_menu, header["more"]["items"], definitions.binder(self.ACTIONS, self),
                          generators=self.generators())
        more.setMenu(self.more_menu)
        top.addWidget(more)
        outer.addLayout(top)
        # A newer GroomLab (the startup check's): a quiet note linking to GroomLab > Help > Check for Updates.
        from groomlab.ui import updates_window
        self.update_note = updates_window.UpdateNote()
        outer.addWidget(self.update_note)

        # The group brushes edit and Place and Draw add to (children follow their parents).
        # Two lines: the group, then Symmetry (added below, with the brush settings it belongs to).
        self.target_row = QtWidgets.QWidget()
        lines = QtWidgets.QVBoxLayout(self.target_row)
        lines.setContentsMargins(0, 0, 0, 0)
        lines.setSpacing(theme.px(6))
        row = QtWidgets.QHBoxLayout()
        lines.addLayout(row)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(8))
        group_label = QtWidgets.QLabel("Group")
        group_label.setProperty("role", "about")
        row.addWidget(group_label)
        self.level = QtWidgets.QComboBox()
        self.level.setToolTip("The guide group brushes edit and new guides go into; All groups: every guide")
        self.level.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.level.setMinimumContentsLength(8)
        self.level.currentIndexChanged.connect(self.target_changed)
        row.addWidget(self.level, 1)
        self.isolate = QtWidgets.QCheckBox("Isolate")
        self.isolate.setToolTip("Show the group (and its child groups) at full color and dim the others")
        self.isolate.toggled.connect(lambda _: self.apply_isolation())
        row.addWidget(self.isolate)
        outer.addWidget(self.target_row)

        self.status = widgets.StatusLabel()  # made early: the tabs report into it
        outer.addWidget(self.status)

        # No groom to work on: how to start one.
        self.empty = QtWidgets.QFrame()
        self.empty.setProperty("role", "empty")
        column = QtWidgets.QVBoxLayout(self.empty)
        pad = theme.px(14)
        column.setContentsMargins(pad, pad, pad, pad)
        column.setSpacing(theme.px(8))
        empty = definitions.get("palette/empty")
        title = QtWidgets.QLabel(empty["title"])
        title.setProperty("role", "title")
        column.addWidget(title)
        self.empty_text = QtWidgets.QLabel()
        self.empty_text.setWordWrap(True)
        self.empty_text.setProperty("role", "about")
        column.addWidget(self.empty_text)
        # Make one on the selected mesh, add a sample mesh, or bring one in from XGen, Yeti, Alembic
        # curves or a groom file (the chip: the Output strip's Import list).
        bind = definitions.binder(self.ACTIONS, self)
        for item in definitions.resolve(empty["items"]):
            if item["kind"] == "action":
                make = QtWidgets.QPushButton(item["label"])
                make.setProperty("role", "primary")
                make.setToolTip(item["tip"])
                make.clicked.connect(lambda _=False, fn=bind(item): fn())
                column.addWidget(make)
            elif item["id"] == "import":
                self.empty_import = self.menu_chip(item)
                column.addWidget(self.empty_import)
            else:
                sample = QtWidgets.QPushButton(item["label"])
                sample.setToolTip(item["tip"])
                samples = QtWidgets.QMenu(sample)
                widgets.fill_menu(samples, definitions.get(item["entry"]["ref"]), bind)
                sample.setMenu(samples)
                column.addWidget(sample)
        outer.addWidget(self.empty)
        self.starter = StarterCard(self)
        self.starter.setVisible(False)
        outer.addWidget(self.starter)

        # Everything that works on a groom (the tabs, the tool's settings, the sections below): hidden
        # while there is none, so the empty state shows only "Start a groom" (fill_grooms).
        self.work = QtWidgets.QWidget()
        self.work.setObjectName("groomToolsWork")
        work = QtWidgets.QVBoxLayout(self.work)
        work.setContentsMargins(0, 0, 0, 0)
        work.setSpacing(theme.px(10))
        outer.addWidget(self.work)

        self.group = QtWidgets.QButtonGroup(self)
        self.group.setExclusive(False)  # the active tool's tiles are checked by hand (one tool, maybe two tiles)
        self.tabs = widgets.Pages()
        tools_tab = QtWidgets.QWidget()
        tools_layout = QtWidgets.QVBoxLayout(tools_tab)
        tools_layout.setContentsMargins(0, 0, 0, 0)
        tools_layout.setSpacing(theme.px(6))
        for section, tools in SECTIONS:
            if section == "Paint":
                continue  # the painting brushes live in the Paint tab, beside the maps they paint
            tools_layout.addWidget(self.heading(section))
            # Beside Freeze: Unfreeze All, a button (not a tool).
            extra = [self.unfreeze_tile()] if any(t[0] == "freeze" for t in tools) else []
            tools_layout.addWidget(self.tool_grid(tools, extra=extra))
        self.paint_tab = tool_tabs.PaintTab(self)
        sculpt_tab = QtWidgets.QWidget()
        self.sculpt_layout = QtWidgets.QVBoxLayout(sculpt_tab)
        self.sculpt_layout.setContentsMargins(0, 0, 0, 0)
        self.sculpt_layout.setSpacing(theme.px(8))
        self.reference_tab = tool_tabs.ReferenceTab(self)
        pages = {"tools": tools_tab, "paint": self.paint_tab, "correctives": sculpt_tab, "photos": self.reference_tab}
        for tab in definitions.get("palette/tabs"):
            self.tabs.addTab(pages[tab["id"]], tab["label"])
        self.tabs.currentChanged.connect(self.tab_changed)
        work.addWidget(self.tabs)

        # The active tool: its name, what it does, and its settings in one card.
        self.settings_heading = self.heading("Brush")
        work.addWidget(self.settings_heading)
        self.hint = QtWidgets.QLabel()
        self.hint.setWordWrap(True)
        self.hint.setProperty("role", "about")
        work.addWidget(self.hint)
        self.settings = QtWidgets.QFrame()
        self.settings.setProperty("role", "card")
        card = QtWidgets.QVBoxLayout(self.settings)
        pad = theme.px(12)
        card.setContentsMargins(pad, pad, pad, pad)
        card.setSpacing(theme.px(10))
        self.brush_form = QtWidgets.QWidget()
        self.brush_form.setProperty("role", "options")
        form = QtWidgets.QFormLayout(self.brush_form)
        form.setContentsMargins(0, 0, 0, 0)
        form.setVerticalSpacing(theme.px(8))
        form.setHorizontalSpacing(theme.px(10))
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        self.radius = widgets.SoftSlider(60, 5, 400, integer=True, minimum=1, key="palette.radius")
        self.radius.committed.connect(lambda v: self.push(radius=v))
        self.radius.dragged.connect(lambda v: self.push(radius=v))
        self.strength = widgets.SoftSlider(1.0, 0.0, 1.0, key=None)
        self.strength.committed.connect(self.set_strength)
        self.strength.dragged.connect(self.set_strength)
        # Falloff as the catalog names it in both hosts: 0 hard (full strength to the edge), 1 soft
        # (fading from the center out); the brush's hardness is 1 - falloff.
        self.falloff = widgets.SoftSlider(1.0, 0.0, 1.0, key="palette.falloff")
        self.falloff.setToolTip(SETTINGS["falloff"]["hint"])
        self.falloff.committed.connect(lambda v: self.push(hardness=1.0 - v))
        self.falloff.dragged.connect(lambda v: self.push(hardness=1.0 - v))
        self.smooth_strength = widgets.SoftSlider(1.0, 0.0, 3.0, key="palette.smoothStrength")
        self.smooth_strength.setToolTip(SETTINGS["smoothStrength"]["hint"])
        self.smooth_strength.committed.connect(lambda v: self.push(smoothStrength=v))
        self.smooth_strength.dragged.connect(lambda v: self.push(smoothStrength=v))
        self.radius_label = QtWidgets.QLabel(SETTINGS["radius"]["label"])
        self.strength_label = QtWidgets.QLabel(SETTINGS["strength"]["label"])
        self.falloff_label = QtWidgets.QLabel(SETTINGS["falloff"]["label"])
        self.smooth_label = QtWidgets.QLabel(SETTINGS["smoothStrength"]["label"])
        self.radius_row = widgets.with_unit(self.radius, "px", "Screen pixels: the circle's radius on screen, "
                                                               "whatever the zoom")
        for label, slider in ((self.radius_label, self.radius_row), (self.strength_label, self.strength),
                              (self.falloff_label, self.falloff), (self.smooth_label, self.smooth_strength)):
            form.addRow(label, slider)
        card.addWidget(self.brush_form)
        # The brush's curves (the catalog's falloffProfile and strengthProfile): each off until its
        # box is ticked, then drawn in a curve editor (right-click: presets).
        self.falloff_curve_box, self.falloff_curve = self.curve_setting(
            "falloffProfile", [[0, 1], [0.5, 0.56], [1, 0]], ("center", "edge"))
        self.strength_curve_box, self.strength_curve = self.curve_setting(
            "strengthProfile", [[0, 0.25], [1, 1]], ("root", "tip"))
        for box in (self.falloff_curve_box, self.strength_curve_box):
            card.addWidget(box)
        self.alpha_box = self.alpha_setting()
        card.addWidget(self.alpha_box)
        # Symmetry, and across what: world X, or the scalp's own topology (a head turned, or not
        # quite symmetric).
        self.symmetry = self.setting_box("symmetry")
        self.symmetry_mode = QtWidgets.QComboBox()
        self.symmetry_mode.setObjectName("symmetryMode")
        for name, label, tip in _options("symmetry"):
            self.symmetry_mode.addItem(label, name)
            self.symmetry_mode.setItemData(self.symmetry_mode.count() - 1, tip, QtCore.Qt.ToolTipRole)
        self.symmetry_mode.setToolTip(SETTINGS["symmetry"]["hint"])
        self.symmetry_mode.setEnabled(False)
        self.symmetry.toggled.connect(lambda on: (self.symmetry_mode.setEnabled(on), self.push(symmetry=self.symmetry_name())))
        self.symmetry_mode.currentIndexChanged.connect(lambda _i: self.push(symmetry=self.symmetry_name()))
        self.symmetry_row = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(self.symmetry_row)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(6))
        row.addWidget(self.symmetry)
        row.addWidget(self.symmetry_mode)
        row.addStretch(1)
        self.symmetry.setToolTip(SETTINGS["symmetry"]["hint"] + ". " + SYMMETRY_TIP)
        # In the Group row, always shown: found before a brush is picked, its state always seen (the
        # brushes that mirror take it when they start; Alt+Shift+X flips it).
        self.symmetry_row.setObjectName("symmetryRow")
        self.target_row.layout().addWidget(self.symmetry_row)
        self.selected_only = self.setting_box("selectedOnly")
        self.selected_only.toggled.connect(lambda on: self.push(selectedOnly=on))
        self.pressure = self.setting_box("pressure")
        self.pressure.setChecked(True)
        self.pressure.toggled.connect(lambda on: self.push(pressure=on))
        self.collide = self.setting_box("collide")
        self.collide.setChecked(True)
        self.collide.toggled.connect(lambda on: self.push(collide=on))
        self.mask_scalp = self.setting_box("frontFacingOnly")  # "Front facing only" in both hosts
        self.mask_scalp.setChecked(True)
        self.mask_scalp.toggled.connect(lambda on: self.push(maskByScalp=on))
        self.checks = QtWidgets.QGridLayout()  # refilled with the tool's own (show_tool_settings)
        self.checks.setHorizontalSpacing(theme.px(10))
        self.checks.setVerticalSpacing(theme.px(6))
        self.check_boxes = (self.selected_only, self.pressure, self.collide, self.mask_scalp)
        card.addLayout(self.checks)

        # Place and Draw
        self.create_box, column = self.options("New guides")
        form = self.options_form(column)
        self.match = QtWidgets.QCheckBox("Match nearby guides")
        self.match.setChecked(True)
        self.match.setToolTip("New guides take the shape, length and painted maps of the nearest guides: the "
                              "group's own, or the groups around it while it has none. Draw takes the maps. "
                              "Off: they stand straight up, with no maps")
        self.match.toggled.connect(lambda on: self.push(matchNeighbors=on))
        from groomlab import units
        self.length_factor = units.factor("length")  # shown in real units, stored in scene units
        length_key = "palette.length@" + units.unit("length")[0]
        self.length = widgets.SoftSlider(0.5 * self.length_factor, minimum=0.001 * self.length_factor, key=length_key)
        self.length.committed.connect(lambda v: self.push(length=v / self.length_factor))
        self.segments = widgets.SoftSlider(12, 1, 32, integer=True, minimum=1, maximum=256, key="palette.segments")
        self.segments.committed.connect(lambda v: self.push(segments=v))
        self.spacing = widgets.SoftSlider(0.1 * self.length_factor, minimum=0.0,
                                          key="palette.spacing@" + units.unit("length")[0])
        self.spacing.committed.connect(lambda v: self.push(spacing=v / self.length_factor))
        self.spacing_label = QtWidgets.QLabel("Spacing (%s)" % units.label("length"))
        form.addRow(self.match)
        form.addRow("Length (%s)" % units.label("length"), self.length)
        form.addRow("Segments", self.segments)
        form.addRow(self.spacing_label, self.spacing)
        self.solo_new = self.setting_box("soloNewGuides")
        self.solo_new.setChecked(False)  # (off by default: placing freezes nothing unless asked)
        self.solo_new.toggled.connect(lambda on: self.push(soloNewGuides=on))
        form.addRow(self.solo_new)
        card.addWidget(self.create_box)

        # Parts
        self.part_box, column = self.options("Parts")
        form = self.options_form(column)
        self.part_name = QtWidgets.QLineEdit("part")
        self.part_name.setToolTip("Name for the next part (made unique)")
        self.part_name.editingFinished.connect(lambda: self.push(partName=self.part_name.text() or "part"))
        self.part_list = QtWidgets.QListWidget()
        self.part_list.setMaximumHeight(theme.px(90))
        part_buttons = QtWidgets.QHBoxLayout()
        bind = definitions.binder(self.ACTIONS, self)
        for item in definitions.resolve(definitions.get("lists/parts/bar"), None):
            b = QtWidgets.QPushButton(item["label"])
            b.setToolTip(item["tip"])
            b.clicked.connect(lambda _=False, fn=bind(item): fn())
            part_buttons.addWidget(b)
        form.addRow("Name", self.part_name)
        column.addWidget(self.part_list)
        column.addLayout(part_buttons)
        card.addWidget(self.part_box)

        # Flow arrows
        self.flow_box, column = self.options("Flow")
        row = QtWidgets.QHBoxLayout()
        self.flow_sink = QtWidgets.QComboBox()
        for kind, label, tip in _options("flowSink"):
            self.flow_sink.addItem(label, kind)
            self.flow_sink.setItemData(self.flow_sink.count() - 1, tip, QtCore.Qt.ToolTipRole)
        self.flow_sink.setToolTip(definitions.get("choices/flowSink")["tip"])
        self.flow_sink.currentIndexChanged.connect(self.flow_sink_changed)
        row.addWidget(QtWidgets.QLabel(definitions.get("choices/flowSink")["label"]))
        row.addWidget(self.flow_sink, 1)
        column.addLayout(row)
        # How many arrows and sinks the groom keeps, and Clear All (GroomLab > Utilities > Delete All
        # Flow Arrows): where the arrows live, and how to take them all off.
        row = QtWidgets.QHBoxLayout()
        self.flow_count = QtWidgets.QLabel()
        self.flow_count.setObjectName("flowCount")
        self.flow_count.setProperty("role", "hint")
        self.flow_count.setToolTip("The flow arrows and sinks kept on the groom (the Flow node lays the strands "
                                   "along them)")
        row.addWidget(self.flow_count, 1)
        clear_flow = QtWidgets.QPushButton("Clear All")
        clear_flow.setObjectName("clearFlow")
        clear_flow.setToolTip(definitions.get("actions/flow.deleteAll")["tip"])
        clear_flow.clicked.connect(self.clear_flow)
        self.clear_flow_button = clear_flow
        row.addWidget(clear_flow)
        column.addLayout(row)
        self.flow_hint = QtWidgets.QLabel()
        self.flow_hint.setWordWrap(True)
        self.flow_hint.setProperty("role", "hint")
        column.addWidget(self.flow_hint)
        self.flow_sink_changed()
        card.addWidget(self.flow_box)

        # Length Keys: the selected key, Apply and Cancel
        self.length_keys = length_keys_box.LengthKeysBox()
        card.addWidget(self.length_keys)

        # Comb: keep lengths, or stretch
        self.stretch_box, column = self.options()
        self.stretch = self.setting_box("stretch")
        self.stretch.toggled.connect(lambda on: self.push(stretch=on))
        column.addWidget(self.stretch)
        card.addWidget(self.stretch_box)

        # Shape: what it paints on the tubes (Width, Flatness, Twist), and the selected guides' tubes
        # as a whole with the groom's Fill Tubes.
        self.tube_channel_box, column = self.options()
        row = QtWidgets.QHBoxLayout()
        row.setSpacing(theme.px(6))
        paints = QtWidgets.QLabel(SETTINGS["tubeChannel"]["label"])
        paints.setToolTip(SETTINGS["tubeChannel"]["hint"])
        row.addWidget(paints)
        self.tube_channel = QtWidgets.QButtonGroup(self)  # exclusive: one channel at a time
        self.tube_channel_buttons = {}
        for name, label, tip in TUBE_CHANNELS:
            b = QtWidgets.QToolButton()
            b.setText(label)
            b.setToolTip(tip)
            b.setCheckable(True)
            b.setProperty("role", "chip")
            b.setObjectName("tubeChannel_" + name)
            b.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            self.tube_channel.addButton(b)
            self.tube_channel_buttons[name] = b
            row.addWidget(b)
        self.tube_channel_buttons["width"].setChecked(True)
        self.tube_channel.buttonClicked.connect(lambda _b: self.push(tubeChannel=self.tube_channel_name()))
        column.addLayout(row)
        card.addWidget(self.tube_channel_box)
        self.tube_panel = TubePanel(self)  # in its own Tubes section below

        # Erase: what it removes
        self.erase_box, column = self.options("Erases")
        row = QtWidgets.QHBoxLayout()
        self.erase_guides = QtWidgets.QCheckBox("Guides")
        self.erase_parts = QtWidgets.QCheckBox("Parts")
        self.erase_flow = QtWidgets.QCheckBox("Flow")
        for box, flag in ((self.erase_guides, "eraseGuides"), (self.erase_parts, "eraseParts"),
                          (self.erase_flow, "eraseFlow")):
            box.setChecked(True)
            box.toggled.connect(lambda on, f=flag: self.push(**{f: on}))
            row.addWidget(box)
        row.addStretch(1)
        column.addLayout(row)
        erase_note = QtWidgets.QLabel(ERASE_NOTE)
        erase_note.setWordWrap(True)
        erase_note.setProperty("role", "hint")
        column.addWidget(erase_note)
        self.erase_box.setToolTip("What the Erase brush removes where it passes (one undo step)")
        card.addWidget(self.erase_box)

        # Shaping brushes: what they act on (guides, part lines, flow arrows and sinks)
        self.target_box, column = self.options("Acts on")
        row = QtWidgets.QHBoxLayout()
        self.target_guides = QtWidgets.QCheckBox("Guides")
        self.target_parts = QtWidgets.QCheckBox("Parts")
        self.target_flow = QtWidgets.QCheckBox("Flow")
        for box, on in ((self.target_guides, True), (self.target_parts, False), (self.target_flow, False)):
            box.setChecked(on)
            box.toggled.connect(self.push_targets)
            row.addWidget(box)
        row.addStretch(1)
        column.addLayout(row)
        self.target_box.setToolTip("What the brush shapes where it passes: guides, part lines, flow arrows and sinks "
                                   "(Comb slides and lays arrows along the stroke; Twist turns arrows and whorls; "
                                   "Length sizes whorls; Smooth, Straighten and Attract shape both; Cut splits parts)")
        card.addWidget(self.target_box)

        # Guide selection
        self.select_box, column = self.options("Selection")
        form = self.options_form(column)
        from groomlab import units
        self.soft_factor = units.factor("length")  # shown in real units (View > Units), kept in scene units
        self.soft = widgets.SoftSlider(0.0, 0.0, 1.0 * self.soft_factor, minimum=0.0, key="palette.soft",
                                       shown_decimals=widgets.display_decimals(3, self.soft_factor))
        self.soft.setToolTip("Soft selection radius: guides near the selection are partly selected; "
                             "brushes with Selected only fade out over it")
        self.soft.committed.connect(lambda v: self.set_soft_selection(v / self.soft_factor))
        form.addRow("Soft radius", widgets.with_unit(self.soft, units.label("length")))
        card.addWidget(self.select_box)

        # Freeze: the mask as a whole
        self.freeze_box, column = self.options("Freeze mask")
        unfreeze = QtWidgets.QPushButton("Unfreeze All")
        unfreeze.setToolTip("Clear the freeze mask: every guide can be brushed again")
        unfreeze.clicked.connect(lambda: self.paint_tab.unfreeze_all())
        column.addWidget(unfreeze)
        card.addWidget(self.freeze_box)

        self.gestures = QtWidgets.QLabel(GESTURES)
        self.gestures.setWordWrap(True)
        self.gestures.setProperty("role", "hint")
        card.addWidget(self.gestures)
        work.addWidget(self.settings)

        # How the strands draw while a stroke is live: set once, rarely changed.
        display = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(display)
        form.setContentsMargins(0, 0, 0, 0)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        self.sculpt = QtWidgets.QComboBox()
        sculpt = definitions.get("choices/sculptDisplay")
        for value, label, _tip in _options("sculptDisplay"):
            self.sculpt.addItem(label, value)
        self.sculpt.setCurrentIndex(self.sculpt.findData(sculpt["default"]))
        self.sculpt.setToolTip(sculpt["tip"])
        self.sculpt.currentIndexChanged.connect(self.apply_sculpt_display)
        form.addRow(sculpt["label"], self.sculpt)
        self.full_density = QtWidgets.QCheckBox("Full density")
        self.full_density.setObjectName("sculptFullDensity")
        self.full_density.setToolTip("Keep every strand of the preview up while a stroke is live, instead of a "
                                     "lighter share of them (slower on heavy grooms, but nothing pops)")
        self.full_density.setChecked(True)  # (the groom's sculptDensity default, 1)
        self.full_density.toggled.connect(lambda _: self.apply_sculpt_display(self.sculpt.currentIndex()))
        form.addRow(self.full_density)
        # Tubes: shapes, sizes and the shape library, a section of its own (open with the Shape
        # brush, Place or Draw; any time from its header).
        sections = dict((section["id"], section) for section in definitions.get("palette/sections"))
        self.section_rules = sections  # which tool and tab each section shows with (show_sections)
        self.tubes_section = widgets.Section(sections["tubes"]["label"], self.tube_panel)
        self.tubes_section.header.toggled.connect(lambda on: on and self.tube_panel.refresh(self.groom()))
        work.addWidget(self.tubes_section)
        self.display_section = widgets.Section(sections["sculptDisplay"]["label"], display)
        work.addWidget(self.display_section)

        # Guide groups and edits on many guides (Tools tab).
        self.guides_tab = tool_tabs.GuidesTab(self)
        self.guides_section = widgets.Section(sections["groups"]["label"], self.guides_tab)
        work.addWidget(self.guides_section)

        # After grooming: simulate, export, render, transfer, import (the GroomLab menu's items).
        self.output_strip = self.output_buttons()
        self.output_section = widgets.Section(sections["output"]["label"], self.output_strip,
                                              expanded=sections["output"].get("expanded", False))
        work.addWidget(self.output_section)

        # Correctives: the groom as a stack (base groom + correctives); the picked row is what brushes edit.
        self.sculpt_layout.addWidget(self.heading("Correctives"))
        self.correctives = tool_tabs.CorrectivesBox(self)
        self.sculpt_layout.addWidget(self.correctives)
        outer.addStretch(1)

        theme.style(self)
        theme.listen(self, "apply_theme")
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(self.POLL_IDLE_MS)
        self.timer.timeout.connect(self.poll)
        self.timer.start()
        self._scene_jobs = []
        self._watch_scene()
        groomlab.on_current_groom(self.current_groom_changed)  # (a pick in the Graph Editor: at once)
        self.poll()
        self.tab_changed(self.tabs.currentIndex())
        self.show_tool_settings()

    def header_button(self, glyph, tip, checkable=False):
        b = QtWidgets.QToolButton()
        if glyph:
            b.setIcon(widgets.glyph(glyph))
            b.glyph = glyph
        b.setIconSize(QtCore.QSize(theme.px(20), theme.px(20)))
        b.setToolTip(tip)
        b.setAutoRaise(True)
        b.setCheckable(checkable)
        b.setProperty("role", "glyph")
        return b

    # What the Groom Tools' own buttons and menus run (engine/ui ids; the Output strip runs the GroomLab
    # menu's: menu.ACTIONS): method names, or functions of the palette.
    ACTIONS = {
        "groom.create": "create_groom",
        "sample.nika": lambda self: self.add_sample("nika"),
        "sample.manny": lambda self: self.add_sample("manny"),
        "sample.wolf": lambda self: self.add_sample("wolf"),
        "sample.buddy": lambda self: self.add_sample("buddy"),
        "parts.fromCurves": "parts_from_curves",
        "parts.remove": "remove_part",
        "parts.clear": "clear_parts",
    }

    @staticmethod
    def eye_menu(button, items):
        """An eye's right-click menu: the GroomLab menu's Show / Hide All (palette.json's header items)."""
        from groomlab.ui import menu
        popup = QtWidgets.QMenu(button)
        widgets.fill_menu(popup, items, definitions.binder(menu.ACTIONS))
        popup.setToolTipsVisible(True)
        button.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        button.customContextMenuRequested.connect(lambda pos: popup.exec_(button.mapToGlobal(pos)))
        button.eye_menu = popup  # kept (an unowned QMenu is garbage-collected)
        return popup

    def generators(self):
        """The menus' entries the Groom Tools fill themselves: the Theme submenu (menus/theme)."""
        return {"themeMenu": widgets.menu_filler(lambda menu, _item: menu.addMenu(widgets.theme_menu(self)))}

    @staticmethod
    def action_chip(item):
        """A chip running the GroomLab menu's action of a definitions item (guarded: errors in plain words)."""
        from groomlab.ui import menu
        chip = QtWidgets.QToolButton()
        chip.setText(item["label"])
        chip.setToolTip(item["tip"])
        chip.setProperty("role", "chip")
        chip.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        chip.clicked.connect(menu._guarded(definitions.binder(menu.ACTIONS)(item), item["label"]))
        return chip

    @staticmethod
    def menu_chip(item):
        """A chip opening a submenu item's list of the GroomLab menu's actions (the Output strip's
        Import, Export, Render and Studio: menus/output.<kind>; menu.shown_items, one table for both)."""
        from groomlab.ui import menu
        kind = item["id"]
        chip = QtWidgets.QToolButton()
        chip.setText(item["label"] + "  ▾")
        chip.setToolTip(item["tip"])
        chip.setProperty("role", "chip")
        chip.setObjectName("output_" + kind)
        chip.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        chip.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        popup = QtWidgets.QMenu(chip)
        for entry in menu.shown_items(kind, menu.TOOLS_ONLY):
            if entry is None:
                popup.addSeparator()
                continue
            text, fn, annotation = entry[:3]
            action = popup.addAction(text)
            action.setToolTip(annotation)
            action.triggered.connect(menu._guarded(fn, text))
        popup.setToolTipsVisible(True)
        chip.setMenu(popup)
        return chip

    def output_buttons(self):
        """The Output strip (palette.json's output): Simulate Range, Export, Render, Studio, Transfer... and
        Import, the steps after grooming one click away (the same actions as the GroomLab menu's)."""
        box = QtWidgets.QWidget()
        box.setObjectName("groomToolsOutput")
        grid = QtWidgets.QGridLayout(box)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setSpacing(theme.px(6))
        chips = [self.menu_chip(item) if item["kind"] == "submenu" else self.action_chip(item)
                 for item in definitions.resolve(definitions.get("palette/output"))]
        for i, chip in enumerate(chips):
            grid.addWidget(chip, i // 3, i % 3)
        self.output_chips = chips
        return box

    @staticmethod
    def setting_box(setting):
        """A checkbox for one of the catalog's settings: its label and hint."""
        box = QtWidgets.QCheckBox(SETTINGS[setting]["label"])
        box.setToolTip(SETTINGS[setting]["hint"])
        return box

    def curve_setting(self, setting, keys, ends):
        """A brush curve of the catalog's: a checkbox (its label and hint) over a curve editor shown
        while it is ticked; the curve goes to the tool as it settles ("" when unticked)."""
        box, column = self.options()
        check = self.setting_box(setting)
        column.addWidget(check)
        editor = widgets.RampEditor(keys, ends=ends)
        editor.setToolTip(SETTINGS[setting]["hint"] + " (right-click: presets)")
        editor.setVisible(False)
        column.addWidget(editor)
        flag = setting  # the tool's flag has the setting's name
        check.toggled.connect(lambda on: (editor.setVisible(on), self.push(**{flag: self.curve_of(check, editor)})))
        editor.committed.connect(lambda _keys: self.push(**{flag: self.curve_of(check, editor)}))
        editor.check = check
        return box, editor

    def alpha_setting(self):
        """Paint's alpha on a scalp map (the catalog's "alpha"): the picker, and while one is
        picked its rotation, angle, spacing and invert."""
        box, column = self.options()
        top = QtWidgets.QHBoxLayout()
        top.setSpacing(theme.px(10))
        label = QtWidgets.QLabel(SETTINGS["alpha"]["label"])
        label.setToolTip(SETTINGS["alpha"]["hint"])
        self.alpha = widgets.AlphaPicker()
        self.alpha.changed.connect(self.alpha_changed)
        top.addWidget(label)
        top.addWidget(self.alpha, 1)
        column.addLayout(top)
        self.alpha_options = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(self.alpha_options)
        form.setContentsMargins(0, 0, 0, 0)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        rotation_row = QtWidgets.QHBoxLayout()
        self.alpha_rotation = QtWidgets.QComboBox()
        self.alpha_rotation.setObjectName("alphaRotation")
        for name, label_text, tip in (("fixed", "Fixed", "Every stamp at the angle"),
                                      ("stroke", "Follow Stroke", "Each stamp turned the way the stroke goes, plus the angle"),
                                      ("random", "Random", "Each stamp at a random angle")):
            self.alpha_rotation.addItem(label_text, name)
            self.alpha_rotation.setItemData(self.alpha_rotation.count() - 1, tip, QtCore.Qt.ToolTipRole)
        self.alpha_rotation.currentIndexChanged.connect(self.alpha_rotation_changed)
        self.alpha_angle = widgets.SoftSlider(0, 0, 360, integer=True, minimum=-360, maximum=360)
        self.alpha_angle.setToolTip("The alpha's angle in degrees, counterclockwise (Follow Stroke: added to the stroke's way)")
        self.alpha_angle.committed.connect(lambda v: self.push(alphaAngle=v))
        self.alpha_angle.dragged.connect(lambda v: self.push(alphaAngle=v))
        rotation_row.addWidget(self.alpha_rotation)
        rotation_row.addWidget(self.alpha_angle, 1)
        form.addRow("Rotation", rotation_row)
        self.alpha_spacing = widgets.SoftSlider(25, 5, 100, integer=True, minimum=2, maximum=400, key="palette.alphaSpacing")
        self.alpha_spacing.committed.connect(lambda v: self.push(alphaSpacing=v / 100.0))
        spacing = widgets.with_unit(self.alpha_spacing, "%", "How far apart the stamps fall along a stroke, in percent of "
                                                             "the brush's diameter: low for a smooth trail, 100 and up "
                                                             "for separate stamps")
        form.addRow("Spacing", spacing)
        self.alpha_invert = QtWidgets.QCheckBox("Invert")
        self.alpha_invert.setToolTip("Paint where the alpha is black instead")
        self.alpha_invert.toggled.connect(lambda on: self.push(alphaInvert=on))
        form.addRow("", self.alpha_invert)
        self.alpha_options.setVisible(False)
        column.addWidget(self.alpha_options)
        return box

    def alpha_changed(self, path):
        self.alpha_options.setVisible(bool(path))
        self.push(alpha=path)

    def alpha_rotation_changed(self, _index):
        name = self.alpha_rotation.currentData()
        self.alpha_angle.setEnabled(name != "random")
        self.push(alphaRotation=name)

    def alpha_flags(self):
        """The alpha settings as brush_tool takes them (an alpha whose file is gone: none)."""
        self.alpha.set_path(self.alpha.path, emit=False)
        self.alpha_options.setVisible(bool(self.alpha.path))
        return dict(alpha=self.alpha.path, alpha_rotation=self.alpha_rotation.currentData(),
                    alpha_angle=self.alpha_angle.value(), alpha_spacing=self.alpha_spacing.value() / 100.0,
                    alpha_invert=self.alpha_invert.isChecked())

    def sync_alpha(self):
        """The alpha shows for Paint on a scalp map (vertices or texture), not on a guide map."""
        if not hasattr(self, "alpha_box"):
            return  # the Paint tab picks its map while the card is still being built
        on = "alpha" in TOOL_SETTINGS.get(self.tool, set()) and self.paint_tab.current_kind() in ("vertices", "texture")
        self.alpha_box.setVisible(on)
        self.alpha_options.setVisible(on and bool(self.alpha.path))

    @staticmethod
    def curve_of(check, editor):
        """A curve setting as the tool takes it: the editor's keys as JSON, or "" while it is off."""
        import json
        return json.dumps(editor.value()) if check.isChecked() else ""

    def options(self, title=None):
        """A tool's own options inside the settings card: a small heading (if any) over a column."""
        box = QtWidgets.QWidget()
        box.setProperty("role", "options")
        column = QtWidgets.QVBoxLayout(box)
        column.setContentsMargins(0, theme.px(4) if title else 0, 0, 0)
        column.setSpacing(theme.px(6))
        if title:
            heading = QtWidgets.QLabel(title.upper())
            heading.setProperty("role", "subheading")
            column.addWidget(heading)
        return box, column

    @staticmethod
    def options_form(column):
        form = QtWidgets.QFormLayout()
        form.setContentsMargins(0, 0, 0, 0)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        column.addLayout(form)
        return form

    def closeEvent(self, event):
        self.stop_following()
        super(ToolPalette, self).closeEvent(event)

    def stop_following(self):
        """The tabs stop following their groom (their Maya callbacks go), and the poll stops:
        closing, or Maya quitting."""
        self.timer.stop()
        self._unwatch_scene()
        groomlab.remove_current_groom_listener(self.current_groom_changed)
        for tab in (self.guides_tab, self.paint_tab, self.correctives, self.reference_tab):
            tab.unwatch()

    def _watch_scene(self):
        """File > Open: the opened scene's groom is the one worked on (groomlab.adopt_scene_groom)."""
        from maya.api import OpenMaya as om
        self._unwatch_scene()

        def opened(*_):
            if widgets.alive(self):
                QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and self.scene_opened())
            else:  # deleted without closing
                self._unwatch_scene()
        try:
            self._scene_jobs = [om.MSceneMessage.addCallback(om.MSceneMessage.kAfterOpen, opened)]
        except RuntimeError:
            self._scene_jobs = []

    def _unwatch_scene(self):
        from maya.api import OpenMaya as om
        for job in getattr(self, "_scene_jobs", []):
            try:
                om.MMessage.removeCallback(job)
            except RuntimeError:
                pass
        self._scene_jobs = []

    def scene_opened(self):
        groomlab.adopt_scene_groom()
        self._groom_rows = None
        self.poll()

    def showEvent(self, event):
        super(ToolPalette, self).showEvent(event)
        if not self.timer.isActive():  # closed and shown again: follow the groom once more
            for tab in (self.guides_tab, self.paint_tab, self.correctives, self.reference_tab):
                tab._watch()
                tab.refresh()
            self._watch_scene()
            groomlab.on_current_groom(self.current_groom_changed)
            self.timer.start()
        if not getattr(self, "_polled", False):  # the first show: the groom (or the empty state) at once
            QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and not getattr(self, "_polled", False) and self.poll())

    @staticmethod
    def heading(text):
        label = QtWidgets.QLabel(text.upper())
        label.setProperty("role", "heading")
        return label

    def apply_theme(self, t):
        """A new theme: the icons in its colors (the style sheet follows by itself)."""
        for tool, button in self.buttons.items():
            button.setIcon(widgets.icon(tool))
        for name, button in self.header_icons.items():
            button.setIcon(widgets.icon(name))
        for button in self.findChildren(QtWidgets.QToolButton):
            if getattr(button, "glyph", None):
                button.setIcon(widgets.glyph(button.glyph))

    def unfreeze_tile(self):
        """Unfreeze All as a tile beside the Freeze tool's: every guide brushable again."""
        b = QtWidgets.QToolButton()
        b.setObjectName("unfreezeAllTile")
        b.setText("Unfreeze All")
        b.setIcon(widgets.icon("unfreeze"))
        b.setIconSize(QtCore.QSize(theme.px(28), theme.px(28)))
        b.setToolButtonStyle(QtCore.Qt.ToolButtonTextUnderIcon)
        b.setAutoRaise(True)
        b.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        b.setMinimumSize(theme.px(64), theme.px(56))
        b.setProperty("role", "tool")
        b.setToolTip("<b>Unfreeze All</b><br>Clear the freeze mask: every guide can be brushed again "
                     "(a brush's Shift+click where no guide is does the same)")
        b.clicked.connect(lambda: self.paint_tab.unfreeze_all())
        self.buttons.setdefault("unfreeze", b)  # its icon follows the theme
        return b

    def tool_grid(self, tools, columns=4, extra=()):
        """Tool tiles (tool, label, tooltip) that reflow to the panel's width, and any `extra`
        buttons after them; one tool is active across every tab."""
        tiles = []
        for tool, label, tip in tools:
            b = QtWidgets.QToolButton()
            b.setText(label)
            b.setIcon(widgets.icon(tool))
            b.setIconSize(QtCore.QSize(theme.px(28), theme.px(28)))
            b.setToolButtonStyle(QtCore.Qt.ToolButtonTextUnderIcon)
            b.setCheckable(True)
            b.setAutoRaise(True)
            b.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            b.setMinimumSize(theme.px(64), theme.px(56))
            b.setProperty("role", "tool")  # a rounded tile: the theme colors it
            b.setToolTip("<b>%s</b><br>%s" % (NAMES.get(tool, label), tip))
            b.clicked.connect(lambda _=False, t=tool: self.activate(t))
            self.group.addButton(b)
            self.buttons.setdefault(tool, b)
            self.tiles.setdefault(tool, []).append(b)
            tiles.append(b)
        tiles.extend(extra)
        return widgets.ToolGrid(tiles, columns)

    def check_tiles(self, tool):
        """The active tool's tiles on, every other off."""
        for t, tiles in self.tiles.items():
            for b in tiles:
                b.setChecked(t == tool)

    def show_tab(self, name):
        """Show tab `name`; "guides" is the Tools tab with its guide groups open."""
        if name == "guides":
            self.tabs.setCurrentIndex(TABS.index("tools"))
            self.guides_section.set_expanded(True)
        else:
            self.tabs.setCurrentIndex(TABS.index(TAB_ALIASES.get(name, name)))

    def show_sections(self):
        """The sections the active tool and tab use (palette.json's "tools" and "tabs"), the rest hidden:
        Tubes with Place, Draw, Shape and Select, While sculpting with the brushes that stroke, the
        guide groups on the Tools tab; Output always."""
        tab = TABS[self.tabs.currentIndex()] if self.tabs.currentIndex() >= 0 else TABS[0]
        for key, section in (("tubes", self.tubes_section), ("sculptDisplay", self.display_section),
                             ("groups", self.guides_section), ("output", self.output_section)):
            section.setVisible(section_shown(self.section_rules[key], self.tool, tab))

    def tab_changed(self, index):
        self.show_sections()
        if TABS[index] != "paint" and self.tool == "paint":
            # Leaving the Paint tab (back to the tools) is done painting: the Paint brush stops and
            # the strands and scalp stop showing the map, as the Paint tile pressed again does.
            self.stop_tool()
            return
        self.show_tool_settings()

    # -- Groom and syncing ---------------------------------------------------

    def groom(self):
        return groomlab.active_groom()

    def fill_grooms(self, active):
        """The groom choice: every groom, the current one picked (the Graph Editor's choice too,
        unless it is pinned). The eyes show or hide the current groom's hair and its guides."""
        from maya import cmds
        shapes = sorted(cmds.ls(type=groomlab.GROOM_TYPE) or [])
        rows = [(shape, self._short((cmds.listRelatives(shape, parent=True) or [shape])[0])) for shape in shapes]
        visible = guides = None
        if active and cmds.objExists(active):
            visible, guides = groomlab.hair_visible(active), groomlab.guides_visible(active)
        current = (cmds.ls(active) or [active])[0] if active and cmds.objExists(active) else None
        state = (rows, current, visible, guides)
        if state == getattr(self, "_groom_rows", None):
            return
        self._groom_rows = state
        self.grooms.blockSignals(True)
        self.grooms.clear()
        for key, text in rows:
            self.grooms.addItem(text, key)
        self.grooms.setCurrentIndex(self.grooms.findData(current) if current else -1)
        self.grooms.blockSignals(False)
        for button, on, glyphs in ((self.visible, visible, ("eye", "eyeOff")),
                                   (self.guides_eye, guides, ("guides", "guidesOff"))):
            button.blockSignals(True)
            button.setEnabled(on is not None)
            button.setChecked(bool(on))
            button.glyph = glyphs[0] if on or on is None else glyphs[1]
            button.setIcon(widgets.glyph(button.glyph))
            button.blockSignals(False)
        # Nothing to work on: how to start a groom, in place of the group row.
        self.target_row.setVisible(bool(active))
        self.empty.setVisible(not active)
        self.work.setVisible(bool(active))  # no tabs or tools with nothing to use them on
        self.empty_text.setText(
            "Pick a groom above, or select a scalp mesh (and any guide curves) to grow a new one." if shapes else
            "Select the scalp mesh, and any curves to start the guides from, then:")

    @staticmethod
    def _short(name):
        return name.split("|")[-1]

    def groom_picked(self, index):
        """The picked groom is the current groom: the tools, and the Graph Editor unless it is
        pinned, work on it (until another groom is picked or selected)."""
        key = self.grooms.itemData(index)
        if key:
            groomlab.set_current_groom(key)
        self._groom_rows = None
        QtCore.QTimer.singleShot(0, self.poll)  # after the pick: the list may change under it

    def current_groom_changed(self, _groom):
        """groomlab.on_current_groom: a pick in the Graph Editor, a groom selected or made: the
        choice follows at once (not at the next tick)."""
        if not widgets.alive(self):  # deleted without closing
            groomlab.remove_current_groom_listener(self.current_groom_changed)
            return
        self._groom_rows = None
        QtCore.QTimer.singleShot(0, lambda: widgets.alive(self) and self.poll())

    @staticmethod
    def _every_groom():
        """Shift held on an eye's click: it is for every groom in the scene."""
        return bool(QtWidgets.QApplication.keyboardModifiers() & QtCore.Qt.ShiftModifier)

    def _eye(self, what, on, one):
        from maya import cmds
        from groomlab.ui import menu
        if self._every_groom():
            menu.show_all(what, on)
        else:
            groom = self.groom()
            if not groom or not cmds.objExists(groom):
                return
            one(groom, on)
        self._groom_rows = None
        self.fill_grooms(self.groom())  # the eyes' glyphs now

    def hair_visibility(self, on):
        """The eye: show or hide the current groom's hair, the strands alone (its displayHair;
        Shift+click: every groom's)."""
        self._eye("hair", on, groomlab.set_hair_visible)

    def guides_visibility(self, on):
        """The guides' eye: show or hide every guide group of the current groom (its displayGuides;
        Shift+click: every groom's guides)."""
        self._eye("guides", on, groomlab.set_guides_visible)

    def apply_isolation(self):
        groom = self.groom()
        if groom:
            groomlab.isolate_group(groom, self.current_group() if self.isolate.isChecked() else None)

    def apply_sculpt_display(self, index):
        groom = self.groom()
        if groom:
            cmds_mode = self.sculpt.itemData(index)
            groomlab.set_sculpt_display(groom, cmds_mode, density=1.0 if self.full_density.isChecked() else 0.35)

    def sync(self, from_negative):
        from groomlab.ui import menu
        menu.symmetry_sync(from_negative)

    # -- The target group --------------------------------------------------------

    def current_group(self):
        """The group picked for brushes and new guides ("" for all groups)."""
        return self.level.currentData(QtCore.Qt.UserRole + 1) or ""

    def target_changed(self, _index=None):
        self._target = (self.current_group(), self.level.currentData())
        self.push(level=self.level.currentData(), group=self.current_group())
        self.apply_isolation()
        self.guides_tab.sync_segments()  # Segments works on the picked group

    def pick_group(self, name):
        """Brushes and new guides use group `name` (from the groups list)."""
        index = next((i for i in range(self.level.count())
                      if self.level.itemData(i, QtCore.Qt.UserRole + 1) == name), -1)
        if index >= 0:
            self.level.setCurrentIndex(index)

    def new_group(self):
        """Ask for a name (and a parent group), make the group and work in it."""
        groom = self.groom()
        if not groom:
            self.status.setText("Select a groom first.")
            return
        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle("New Guide Group")
        form = QtWidgets.QFormLayout(dialog)
        name = QtWidgets.QLineEdit()
        name.setPlaceholderText("e.g. bangs, beard, flyaways")
        parent = QtWidgets.QComboBox()
        parent.addItem("None: a top-level group", "")
        for g in groomlab.list_guide_groups(groom):
            parent.addItem("child of %s" % g["name"], g["name"])
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        form.addRow("Name", name)
        form.addRow("Parent", parent)
        form.addRow(buttons)
        theme.style(dialog)
        if qt.exec_(dialog) != QtWidgets.QDialog.Accepted or not name.text().strip():
            return
        group = name.text().strip()
        try:
            groomlab.add_guide_group(groom, group, parent.currentData() or None)
        except (RuntimeError, ValueError) as e:
            self.status.setText(str(e).strip())
            return
        self.fill_levels(groom, select=group)
        self.guides_tab.refresh()
        self.status.setText("New guides go into %s." % group)

    def add_sample(self, sample):
        import groomlab
        try:
            part = groomlab.add_sample(sample)
        except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
            from groomlab.ui import messages
            self.status.setText("The sample wasn't added: %s" % messages.error("Add Sample Mesh", e))
            return
        self.status.setText("Added %s (selected): click Create Groom from Selection." % part)

    def create_groom(self):
        from maya import cmds
        from groomlab.ui import menu
        meshes = cmds.ls(selection=True, dag=True, type="mesh", noIntermediate=True)
        if not meshes:
            self.status.setText("Select the scalp mesh (and any guide curves) first.")
            return
        try:
            menu.create_groom_from_selection()
        except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
            from groomlab.ui import messages
            self.status.setText("The groom wasn't made: %s" % messages.error("Create Groom from Selection", e))
            return
        self.status.setText("")
        self.poll()

    def step_level(self, delta):
        """Move the Guides choice one level (Page Up / Page Down)."""
        index = self.level.currentIndex()
        levels = [self.level.itemData(i) for i in range(self.level.count())]
        current = levels[index] if index >= 0 else 0
        candidates = sorted(l for l in levels if l is not None and l >= 0)
        if not candidates:
            return
        target = min(candidates, key=lambda l: abs(l - (max(current, 0) + delta)))
        self.level.setCurrentIndex(levels.index(target))

    # The tick: quick while a GroomLab tool is on (its radius and strength follow middle-drags in the
    # viewport), slower otherwise (the selection, the grooms, edits made elsewhere).
    POLL_TOOL_MS = 400
    POLL_IDLE_MS = 1000

    def poll(self):
        """Follow the selection and the tool's settings changed in the viewport (middle-drag). The
        scene-wide reads (every groom, the photos) run when the active groom changes and every few
        ticks; the rest only reads what the current tool or tab shows."""
        if not self.isVisible():
            return
        app = QtWidgets.QApplication.instance()
        if app.activePopupWidget() or app.activeModalWidget():  # a menu or dialog is open: wait
            return
        self._polled = True
        from maya import cmds
        groom = groomlab.active_groom()
        # The node, not just its name: a new scene's groom of the same name is another groom.
        node = (groom, (cmds.ls(groom, uuid=True) or [""])[0] if groom else "")
        # Levels and layers: on a groom change, and every few seconds for edits made elsewhere.
        self._ticks = getattr(self, "_ticks", 0) + 1
        if node != getattr(self, "_groom_key", "?") or self._ticks % 3 == 0:
            self._groom_key = node
            self.fill_grooms(groom)
        if node != getattr(self, "_last_groom", "?"):
            self._last_groom = node
            for tab in (self.guides_tab, self.paint_tab, self.correctives, self.reference_tab):
                tab.set_groom(groom)
        # The group list follows the guides through the Guides tab (its callback on guideData); here
        # only a new groom. The first-groom card: now and then until it is done or dismissed.
        if groom != getattr(self, "_levels_groom", "?"):
            self._levels_groom = groom
            self.starter.refresh(groom)
            self.fill_levels(groom, select=self.current_group())
        elif self._ticks % 6 == 0 and self.starter.following():
            self.starter.refresh(groom)
        if self.tool == PART:
            self.fill_parts(groom)
        if self.tool == "flow" and (self._ticks % 2 == 0 or groom != getattr(self, "_flow_groom", None)):
            self._flow_groom = groom
            self.fill_flow_count(groom)
        if self.length_keys.isVisible():  # the keys clicked and dragged in the viewport
            self.length_keys.refresh()
        if self.tube_panel.isVisible():  # (the Shape brush, Place, Draw) every selection or solo change
            self.tube_panel.refresh(groom, full=self._ticks % 3 == 0)
        # Weights change with the frame: read them every tick, the stack itself now and then
        # (keys edited in the timeline) and whenever the groom's correctives change (its callback);
        # only while the Correctives box shows.
        self.correctives.tick(full=self._ticks % 6 == 0)
        if self.reference_tab.isVisible():  # the photos follow the scene and the viewport's camera
            self.reference_tab.poll(full=self._ticks % 5 == 0)
        ctx = groomlab.BRUSH_CONTEXT
        current = cmds.currentCtx()
        wanted = self.POLL_TOOL_MS if current in (ctx, groomlab.SELECT_CONTEXT) else self.POLL_IDLE_MS
        if self.timer.interval() != wanted:
            self.timer.setInterval(wanted)
        if current == ctx and cmds.contextInfo(ctx, exists=True):
            mode = cmds.groomLabBrushCtx(ctx, query=True, mode=True)
            if mode == "paint" and cmds.groomLabBrushCtx(ctx, query=True, attribute=True) == "freeze":
                mode = "freeze"
            if mode != self.tool and mode in self.buttons:
                if self.tool == "paint":
                    self.paint_tab.end_painting()  # the brush left Paint (a hotkey, the tool settings)
                self.tool = mode
                self.buttons[mode].setChecked(True)
                self.show_tool_settings()
            if not self.radius.slider.isSliderDown():
                self.radius.setValue(cmds.groomLabBrushCtx(ctx, query=True, radius=True))
            # The group or level picked from the viewport (the level keys) shows here too.
            if self.tool != PART:
                picked = cmds.groomLabBrushCtx(ctx, query=True, group=True)
                if picked:
                    index = next((i for i in range(self.level.count())
                                  if self.level.itemData(i, QtCore.Qt.UserRole + 1) == picked), -1)
                else:
                    index = self.level.findData(cmds.groomLabBrushCtx(ctx, query=True, level=True))
                if index >= 0 and index != self.level.currentIndex():
                    self.level.blockSignals(True)
                    self.level.setCurrentIndex(index)
                    self.level.blockSignals(False)
                    self._target = (self.current_group(), self.level.currentData())
                    self.apply_isolation()
            if not self.strength.slider.isSliderDown():
                strength = cmds.groomLabBrushCtx(ctx, query=True, strength=True)
                self.strength.setValue(strength)
                if self.tool in self.strengths:
                    self.strengths[self.tool] = strength
        elif current == groomlab.SELECT_CONTEXT and self.tool != "select":
            if self.tool == "paint":
                self.paint_tab.end_painting()  # the Select tool from elsewhere (a shelf button, a script)
            self.tool = "select"
            self.buttons["select"].setChecked(True)
            self.show_tool_settings()
        elif current not in (ctx, groomlab.SELECT_CONTEXT) and self.tool is not None:
            self.clear_tool()  # another Maya tool took over: nothing is active here

    def fill_levels(self, groom, select=None, groups=None):
        """The group choices: All groups, then each group (children under their parents).
        `groups`: groomlab.list_guide_groups(groom) when the caller has read it."""
        role = QtCore.Qt.UserRole + 1
        wanted = select if select is not None else self.current_group()
        first = getattr(self, "_target", None) is None  # a new window starts on the first group
        if groups is None:
            groups = []
            if groom:
                try:
                    groups = groomlab.list_guide_groups(groom)
                except RuntimeError:
                    groups = []
        items = [("All groups", -1, "")]
        ordered = []

        def walk(parent, depth):
            for g in groups:
                if g["parent"] == parent:
                    ordered.append(("    " * depth + g["name"], g["depth"], g["name"]))
                    walk(g["name"], depth + 1)
        walk(None, 0)
        items += ordered or [("primary", 0, "primary")]  # a new groom: Place starts the primary group
        state = [(text, level, group) for text, level, group in items]
        if state == getattr(self, "_level_items", None) and wanted == self.current_group():
            return
        self._level_items = state
        self.level.blockSignals(True)
        self.level.clear()
        for text, level, group in items:
            self.level.addItem(text, level)
            self.level.setItemData(self.level.count() - 1, group, role)
        index = -1 if first else next((i for i in range(self.level.count()) if self.level.itemData(i, role) == wanted), -1)
        if index < 0:
            index = next((i for i in range(self.level.count()) if self.level.itemData(i) == 0), 0)
        self.level.setCurrentIndex(index)
        self.level.blockSignals(False)
        if self.current_group() != getattr(self, "_target", ("?",))[0]:
            self._target = (self.current_group(), self.level.currentData())
            self.push(level=self.level.currentData(), group=self.current_group())

    # -- Tools -----------------------------------------------------------------

    def activate(self, tool):
        from maya import cmds
        if tool == "paint" and self.tool == "paint" and cmds.currentCtx() == groomlab.BRUSH_CONTEXT:
            self.stop_tool()  # the Paint tile again: done painting
            return
        if tool != "paint":
            self.paint_tab.end_painting()
        self.tool = tool
        self.check_tiles(tool)
        self.status.setText("")
        if not self.groom():
            self.status.setText("Select a groom first; tools work on the selected groom.")
        if tool == "select":
            groomlab.select_tool(level=max(self.level.currentData() or 0, 0), group=self.current_group(),
                                 symmetry=self.symmetry_name())
            self.show_tool_settings()
            return
        if tool == "freeze":
            try:
                groomlab.freeze_tool(radius=self.radius.value(), strength=self.strengths.get("freeze", 1.0),
                                     level=self.level.currentData() if self.level.currentData() is not None else 0,
                                     group=self.current_group(),
                                     symmetry=self.symmetry_name(),
                                     pressure=self.pressure.isChecked())
            except (RuntimeError, ValueError) as e:
                self.tool_failed(e)
            self.show_tool_settings()
            return
        if tool == PART:
            groom = self.groom()
            if groom:
                try:
                    groomlab.draw_part_tool(groom, name=self.part_name.text() or "part",
                                            symmetry=self.symmetry_name())
                except (RuntimeError, ValueError) as e:
                    self.tool_failed(e)
            self.fill_parts(groom)
            self.show_tool_settings()
            return
        kwargs = dict(radius=self.radius.value(), strength=self.strengths.get(tool, 1.0),
                      level=self.level.currentData() if self.level.currentData() is not None else 0,
                      group=self.current_group(), selected_only=self.selected_only.isChecked(),
                      erase_guides=self.erase_guides.isChecked(), erase_parts=self.erase_parts.isChecked(),
                      erase_flow=self.erase_flow.isChecked(), stretch=self.stretch.isChecked(),
                      tube_channel=self.tube_channel_name(),
                      flow_sink=self.flow_sink.currentData() or "",
                      symmetry=self.symmetry_name(), pressure=self.pressure.isChecked(),
                      collide=self.collide.isChecked(), mask_by_scalp=self.mask_scalp.isChecked(),
                      match_neighbors=self.match.isChecked(), solo_new_guides=self.solo_new.isChecked(),
                      length=self.length.value() / self.length_factor, segments=self.segments.value(),
                      spacing=self.spacing.value() / self.length_factor,
                      value=self.paint_tab.value.value(),
                      targets=self.targets_name(),
                      hardness=1.0 - self.falloff.value(), smooth_strength=self.smooth_strength.value(),
                      falloff_profile=self.curve_of(self.falloff_curve.check, self.falloff_curve),
                      strength_profile=self.curve_of(self.strength_curve.check, self.strength_curve),
                      tube=self.tube_panel.placement())
        if tool == "paint":
            self.show_tab("paint")
            name = self.paint_tab.current() or self.paint_tab.new_map()
            if not name:
                self.status.setText("Make a map to paint (New...).")
                return
            kwargs["attribute"] = name
            kwargs.update(self.paint_tab.paint_flags())
            kwargs.update(self.alpha_flags())
            if self.groom():
                self.paint_tab.show_while_painting(name)
        try:
            groomlab.brush_tool(tool, **kwargs)
        except (RuntimeError, ValueError) as e:
            self.tool_failed(e)
        self.show_tool_settings()
        try:
            cmds.setFocus("MayaWindow")  # keyboard shortcuts go back to Maya
        except RuntimeError:
            pass

    def clear_tool(self):
        """No tool here: every tile off, the card back to how to start."""
        self.check_tiles(None)
        self.tool = None
        self.paint_tab.end_painting()  # the map shown while painting goes back to how it was
        self.show_tool_settings()

    def stop_tool(self):
        """No GroomLab tool: Maya's Select tool, every tile off."""
        from maya import cmds
        self.clear_tool()
        cmds.setToolTo("selectSuperContext")

    def tool_failed(self, error):
        """A tool that couldn't start: say why, and leave no tool active, so a drag
        in the viewport doesn't quietly use the one picked before."""
        self.status.setText("%s: %s" % ((self.tool or "tool").capitalize(), str(error).strip()))
        self.stop_tool()

    def show_tool_settings(self):
        """The card shows what the active tool uses (the catalog's settings for it), and nothing
        else; with no tool, how to start."""
        tool = self.tool
        uses = TOOL_SETTINGS.get(tool, set())
        self.settings_heading.setText((NAMES.get(tool, tool) if tool else "Tool").upper())
        self.hint.setText(HINTS.get(tool, "Pick a tool, then drag over the groom in the viewport.")
                          if tool else "Pick a tool, then drag over the groom in the viewport.")
        self.settings.setVisible(tool is not None)
        brush = "radius" in uses
        for label, slider in ((self.radius_label, self.radius), (self.strength_label, self.strength),
                              (self.falloff_label, self.falloff)):
            label.setVisible(brush)
            slider.setVisible(brush)
            slider.setEnabled(brush)
        self.radius_row.setVisible(brush)
        self.falloff_curve_box.setVisible("falloffProfile" in uses)
        self.strength_curve_box.setVisible("strengthProfile" in uses)
        self.smooth_strength.setVisible("smoothStrength" in uses)
        self.smooth_label.setVisible("smoothStrength" in uses)
        self.sync_alpha()
        self.selected_only.setVisible("selectedOnly" in uses)
        self.pressure.setVisible("pressure" in uses)
        self.pressure.setEnabled("pressure" in uses)
        self.collide.setVisible("collide" in uses)
        self.mask_scalp.setVisible("frontFacingOnly" in uses)
        self.collide.setEnabled(tool != "select")
        self.brush_form.setVisible(brush)
        # The tool's checkboxes packed two to a row, with no holes where others are hidden.
        for box in self.check_boxes:
            self.checks.removeWidget(box)
        shown = [box for box in self.check_boxes if not box.isHidden()]
        for i, box in enumerate(shown):
            self.checks.addWidget(box, i // 2, i % 2)
        self.gestures.setVisible(brush)
        self.create_box.setVisible("newGuides" in uses)
        self.spacing.setVisible("spacing" in uses)
        self.spacing_label.setVisible("spacing" in uses)
        self.part_box.setVisible("parts" in uses)
        self.erase_box.setVisible("erases" in uses)
        self.target_box.setVisible("targets" in uses)
        self.stretch_box.setVisible("stretch" in uses)
        self.tube_channel_box.setVisible("tubeChannel" in uses)
        if "tube" in uses:
            self.tubes_section.set_expanded(True)
            self.tube_panel.refresh(self.groom())
        self.flow_box.setVisible("flow" in uses)
        if "flow" in uses:
            self.fill_flow_count(self.groom())
        self.length_keys.setVisible("lengthKeys" in uses)
        if "lengthKeys" in uses:
            self.length_keys.refresh()
        self.select_box.setVisible("selection" in uses)
        self.freeze_box.setVisible("freezeMask" in uses)
        self.level.setEnabled(tool != PART)
        self.show_sections()
        if tool in self.strengths:
            signed = tool in SIGNED
            self.strength.setRange(-1.0 if signed else 0.0, 1.0)
            self.strength.setValue(self.strengths[tool])
            self.strength_label.setText("Strength (±)" if signed else "Strength")

    def push(self, **flags):
        """Send changed settings to the active tool."""
        from maya import cmds
        ctx = groomlab.BRUSH_CONTEXT
        if self.tool in (None, "select") or not cmds.contextInfo(ctx, exists=True):
            if self.tool == "select" and ("level" in flags or "symmetry" in flags):
                groomlab.select_tool(level=max(self.level.currentData() or 0, 0), group=self.current_group(),
                                     symmetry=self.symmetry_name())
            return
        try:
            cmds.groomLabBrushCtx(ctx, edit=True, **flags)
        except RuntimeError as e:
            self.status.setText(str(e))

    def toggle_symmetry(self):
        """Symmetry on or off (Alt+Shift+X): the box, which tells the tool. Returns whether it is on."""
        self.symmetry.setChecked(not self.symmetry.isChecked())
        return self.symmetry.isChecked()

    def symmetry_name(self):
        """The tools' -symmetry: "none", "x" (world X) or "topology"."""
        return self.symmetry_mode.currentData() if self.symmetry.isChecked() else "none"

    def set_strength(self, v):
        if self.tool in self.strengths:
            self.strengths[self.tool] = v
        self.push(strength=v)

    def tube_channel_name(self):
        """The Shape brush's -tubeChannel: width, flatness or twist."""
        return next((name for name, b in self.tube_channel_buttons.items() if b.isChecked()), "width")

    def targets_name(self):
        """The Brushes checkboxes as the tool's -targets: "guides,parts,flow" (any subset) or "none"."""
        names = [name for box, name in ((self.target_guides, "guides"), (self.target_parts, "parts"),
                                        (self.target_flow, "flow")) if box.isChecked()]
        return ",".join(names) or "none"

    def push_targets(self):
        """The Brushes checkboxes: what the shaping brushes act on, sent to the active tool."""
        self.push(targets=self.targets_name())

    # -- Actions ---------------------------------------------------------------

    def fill_parts(self, groom):
        names = groomlab.list_parts(groom) if groom else []
        if [self.part_list.item(i).text() for i in range(self.part_list.count())] != names:
            self.part_list.clear()
            self.part_list.addItems(names)

    def remove_part(self):
        groom, item = self.groom(), self.part_list.currentItem()
        if groom and item:
            groomlab.remove_part(groom, item.text())
            self.fill_parts(groom)

    def parts_from_curves(self):
        from maya import cmds
        groom = self.groom()
        curves = cmds.ls(selection=True, dag=True, type="nurbsCurve")
        if not groom or not curves:
            self.status.setText("Select the part curves (and pin the groom, or select it too).")
            return
        try:
            names = groomlab.add_part(groom, [cmds.listRelatives(c, parent=True)[0] for c in curves],
                                      delete_curves=True)
            groomlab.add_part_node(groom, if_missing=True)
        except (RuntimeError, ValueError) as e:
            self.status.setText(str(e))
            return
        self.status.setText("Added %s." % ", ".join(names))
        self.fill_parts(groom)

    def flow_sink_changed(self, *_):
        kind = self.flow_sink.currentData() or ""
        self.flow_hint.setText(definitions.text("flowSinks" if kind else "flowArrows"))
        self.push(flowSink=kind)

    def clear_flow(self):
        groom = self.groom()
        if groom:
            groomlab.clear_flow(groom)
            self.fill_flow_count(groom)

    def fill_flow_count(self, groom):
        """The Flow card's count: "34 arrows · 2 sinks" kept on the groom (Clear All off with none)."""
        arrows, sinks = groomlab.flow_counts(groom) if groom else (0, 0)
        if (arrows, sinks) == getattr(self, "_flow_counted", None):
            return
        self._flow_counted = (arrows, sinks)
        self.flow_count.setText(flow_count_text(arrows, sinks))
        self.clear_flow_button.setEnabled(bool(arrows or sinks))

    def set_soft_selection(self, radius):
        groom = self.groom()
        if groom:
            groomlab.set_soft_selection(groom, radius)

    def clear_parts(self):
        groom = self.groom()
        if groom:
            groomlab.clear_parts(groom)
            self.fill_parts(groom)

    def open_graph(self):
        from groomlab.ui import graph_editor
        graph_editor.show()  # the current groom (a pinned Graph Editor stays on its own)

_window = None


def current():
    """The open palette, or None."""
    try:
        return _window if _window is not None and _window.isVisible() else None
    except RuntimeError:  # its widget was deleted
        return None


def select_map(name):
    """Pick map `name` in the Paint tab of the open Groom Tools (show() first)."""
    if _window is not None:
        _window.paint_tab.select(name)


# What Maya runs to rebuild the Groom Tools in their dock as it restores its workspace (restore).
UI_SCRIPT = "import groomlab.ui.palette; groomlab.ui.palette.restore()"


def _make():
    from maya.app.general.mayaMixin import MayaQWidgetDockableMixin

    class DockableToolPalette(MayaQWidgetDockableMixin, ToolPalette):
        pass

    window = DockableToolPalette()
    window.setObjectName("groomLabToolPalette")
    widgets.at_exit(_close_at_exit)
    return window


def show(tab=None):
    """Open (or raise) the dockable Groom Tools; `tab` picks "tools", "paint", "correctives" or "photos" (the
    old ids "sculpt" and "reference" too; "guides": the Tools tab with its guide groups open)."""
    global _window
    if _window is not None and not widgets.alive(_window):  # its dock was deleted
        _window = None
    if _window is None:
        _window = _make()
        widgets.into_dock(_window, UI_SCRIPT)  # the dock Maya kept: opens where it was
    _window.show(dockable=True, area="left", floating=False, uiScript=UI_SCRIPT)  # (a new dock: on the left)
    widgets.keep_dock_resizable(_window)
    if tab:
        _window.show_tab(tab)
    groomlab.adopt_scene_groom()  # a scene with grooms: one of them, not "Start a groom"
    _window.poll()
    return _window


def restore():
    """Maya restoring its workspace (a restart, an update's too): the Groom Tools rebuilt in their
    dock, where they were when Maya last quit."""
    global _window
    groomlab.ensure_plugin()
    if _window is not None and widgets.alive(_window):
        widgets.dispose(_window)
    _window = _make()
    widgets.into_dock(_window, UI_SCRIPT, restoring=True)
    widgets.keep_dock_resizable(_window)


def _close_at_exit():
    """Maya quits: the palette stops its poll and callbacks and its widgets go while Qt and Python
    are whole (see widgets.at_exit). Its dock stays as Maya saves it."""
    global _window
    window, _window = _window, None
    if window is not None and widgets.alive(window):
        window.stop_following()
        widgets.dispose(window)
