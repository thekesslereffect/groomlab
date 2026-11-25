# SPDX-License-Identifier: Apache-2.0
"""The Graph Editor's parameter panel (the node's params, their maps, keys and actions) and its
mappable value editor, on their own beside graph_editor.py (E6). graph_editor imports them at its
end, so these read its helpers; import groomlab.ui.graph_editor, not this module, first."""

import json
import math
import os
import sys
from groomlab.base import no_undo
from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtGui, QtWidgets
from groomlab import graph as gm
from groomlab.ui import definitions, theme, widgets
from groomlab.ui.messages import plain as plain_message
from groomlab.ui.shortcuts import keys as shortcut_keys
from groomlab.ui.widgets import shrinkable

from groomlab.ui import graph_editor as _ge  # (the theme it applies: its colors)
from groomlab.ui.graph_editor import (ADVANCED, ATTRIBUTE_TIPS, CATEGORY_COLORS, KEYABLE_KINDS,
    MeshCombo, PLUMBING, SCATTER_LENGTH_TIP, category_of, describe, file_filter, guides_shape,
    key_state, later, library_category, names_editor, paint_key_state, param_sections, pretty,
    readable_on, show, show_value, shown_when, suggest_box, unit_of)


class ParamPanel(QtWidgets.QScrollArea):
    """Widgets for the selected node's parameters, generated from its spec."""

    def __init__(self, editor):
        super(ParamPanel, self).__init__()
        self.editor = editor
        self.setWidgetResizable(True)
        self.setMinimumWidth(theme.px(300))
        # The params shrink to the panel's width (shrinkable()); a scroll bar only if something can't.
        self.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAsNeeded)
        self.show_node(None)

    # Folded sections by (operator type, section) for the session: a section folded on one Clump
    # is folded on every Clump shown after. Unset: Advanced folded, the groups open.
    folded = {}

    editing_script = None  # the script node whose editor the panel shows (show_script_editor)
    script_panel = None

    def show_node(self, name):
        if name is not None and name == self.editing_script:
            # Its editor stays while it is edited (a rebuild, an apply, the groom evaluating).
            panel = self.script_panel
            if panel is None or not widgets.alive(panel) or panel.node != name or self.widget() is not panel:
                self.show_script_editor(name)
            return
        self.editing_script = None
        # The same node shown again (an action's reload, a param's note or warning, keys): the params
        # stay where they were scrolled to; another node starts at its top (issue #47).
        keep = self.verticalScrollBar().value() if name is not None and name == getattr(self, "node", None) else 0
        self.node = name
        self.key_widgets = {}  # param -> its number field(s): keyed params show their state and value per frame
        self.sections = []  # [(section, its header or None, [(spec, row)])]: update_visibility's
        self.row_labels = []
        self.row_label_of = {}   # param -> its label (hidden while its fill field shows the name)
        self.inside_labels = {}  # param -> (name, tip) its fill field shows (a number's row)
        self.inside_fields = {}  # param -> the fill field showing its name
        self.built_units = {}  # param -> the unit its row shows, for a param whose unit depends on another
        body = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(body)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        form.setVerticalSpacing(theme.px(8))
        form.setContentsMargins(theme.px(10), theme.px(4), theme.px(10), theme.px(10))
        self.error_line = None
        actions = None
        if name is None:
            empty = QtWidgets.QLabel("Select a node to edit it.\n\nTab or right-click the canvas to add one; "
                                     "drag from a port to wire it." if self.editor.model is not None else
                                     "Select a groom in the viewport (or pick one above) to edit its graph.")
            empty.setWordWrap(True)
            empty.setProperty("role", "about")
            form.addRow(empty)
        else:
            model = self.editor.model
            op_type = model.node_type(name)
            # The node's actions (graph_ui.json's nodeActions and Maya's own: bake, export, simulate...)
            # pinned over the params with its name, in view however far they scroll.
            actions = self.node_actions(name)
            if actions is not None:
                actions.layout().insertWidget(0, self.title_row(name, op_type))
            form.addRow(self.header(name, op_type, title=actions is None))
            # The node's error (or warning) from the last evaluation, in plain words under its header.
            self.error_line = QtWidgets.QLabel()
            self.error_line.setObjectName("nodeError")
            self.error_line.setWordWrap(True)
            self.error_line.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
            form.addRow(self.error_line)
            self.update_error()
            note = self.editor.note_of(name)
            if note:  # the node does nothing as set (a Clump with no centers): a grey line, not an error
                line = QtWidgets.QLabel(u"ⓘ " + note)
                line.setObjectName("nodeNote")
                line.setWordWrap(True)
                line.setStyleSheet("color: %s;" % _ge.NOTE.name())
                form.addRow(line)
            resolution = self.editor.resolution_note(name)
            if resolution:  # a Scatter's or Resample's: which Resample sets its strands' resolution
                line = QtWidgets.QLabel(u"ⓘ " + resolution)
                line.setObjectName("resolutionNote")
                line.setWordWrap(True)
                line.setStyleSheet("color: rgb(150, 154, 160);")
                form.addRow(line)
            self.expression = None
            if op_type == "Expression":  # a code block, its variables and the library, not text fields
                from groomlab.ui import expression_editor
                self.expression = expression_editor.ExpressionPanel(
                    self.editor, name, lambda p, v, n=name: self.set_expression_param(n, p, v))
                form.addRow(self.expression)
            scene = self.scene_inputs(op_type)
            if scene:
                form.addRow(scene)
            rows = []  # (spec, label, widget), laid out by section below
            saved_version = model.node_version(name)[0]
            for spec in model.node_spec(name)["params"]:
                if spec.get("addedIn", 0) > saved_version:
                    continue  # added by a later version of the operator: nothing here until Upgrade (the core's paramInVersion)
                if op_type in ("Compound", "Script") and spec["name"] == "definition":
                    continue  # edited by grouping and expose() (a script node: Edit Node...), not as text
                if op_type == "Expression" and spec["name"] in ("expression", "variables"):
                    continue  # in the expression panel above
                if (op_type, spec["name"]) in PLUMBING and model.param(name, spec["name"]) == spec["default"]:
                    continue  # names of the host's inputs: only shown once changed
                driver = model.driver_of(name, spec["name"])
                if driver and len(model.output_names(driver)) > 1:  # which output: "mask.r"
                    driver = "%s.%s" % (driver, model.output_of(name, spec["name"]))
                text, tip = self.param_label_and_tip(name, spec)
                field_driven = driver and (spec.get("mappable") or spec.get("colorField")) and not gm.value_port(spec)
                if spec["kind"] in ("int", "float") and (field_driven or not driver):
                    # A number: a fill field with the name inside it (plain_editor), no label column.
                    self.inside_labels[spec["name"]] = (text, tip)
                if field_driven:
                    # Driven by a field node: the param's own value multiplies the field (a 0-1
                    # mask on a density of 100 gives 0-100), so it stays editable, with the driver named.
                    widget = QtWidgets.QWidget()
                    row = QtWidgets.QHBoxLayout(widget)
                    row.setContentsMargins(0, 0, 0, 0)
                    row.addWidget(self.editor_for(name, spec), 1)
                    times = QtWidgets.QLabel('<span style="color:%s">× <b>%s</b></span>' % (_ge.DRIVEN.name(), driver))
                    times.setToolTip("Multiplied by the field node %s: this value where the field is 1, 0 where it "
                                     "is 0" % driver)
                    row.addWidget(times)
                elif driver:
                    # A ramp driven by a Curve node (its shape comes from there), or any param a
                    # Value input sets (its value comes from there).
                    widget = QtWidgets.QLabel('<span style="color:%s">← driven by <b>%s</b></span>'
                                              % (_ge.DRIVEN.name(), driver))
                elif op_type == "Scatter" and spec["name"] == "length" and guides_shape(model, name):
                    # Interpolate gives the strands the guides' shapes and lengths: say so beside it,
                    # or it reads as a second length control beside the Length node's.
                    widget = QtWidgets.QWidget()
                    row = QtWidgets.QHBoxLayout(widget)
                    row.setContentsMargins(0, 0, 0, 0)
                    row.addWidget(self.editor_for(name, spec), 1)
                    unused = QtWidgets.QLabel('<span style="color:%s">guides set it</span>' % _ge.MUTED.name())
                    unused.setToolTip(SCATTER_LENGTH_TIP)
                    row.addWidget(unused)
                elif (spec.get("mappable") or spec.get("colorField")) and self.editor.groom and gm.linkable(spec):
                    # A mappable param (or a color a map colors: a ColorLayers layer's): its field and the
                    # Paint chip (as in the app), right-click > Paint's choices.
                    widget = QtWidgets.QWidget()
                    row = QtWidgets.QHBoxLayout(widget)
                    row.setContentsMargins(0, 0, 0, 0)
                    row.setSpacing(theme.px(4))
                    row.addWidget(self.editor_for(name, spec), 1)
                    row.addWidget(self.paint_chip(name, spec["name"]), 0, QtCore.Qt.AlignTop)
                else:
                    widget = self.editor_for(name, spec)
                label = QtWidgets.QLabel(text)
                label.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
                label.customContextMenuRequested.connect(
                    lambda pos, p=spec["name"], l=label: self.param_menu(name, p, l.mapToGlobal(pos)))
                for w in (label, widget):
                    w.setProperty("glParam", spec["name"])  # S over it keys this param
                label.setToolTip(tip)
                widget.setToolTip(tip)
                self.row_label_of[spec["name"]] = label
                # The fill field carries the name (and the ◆ of an exposed param): the label column
                # stays only for the rows without one (a map's fields, a driven param, a checkbox...).
                if self.label_inside(spec["name"]):
                    label.hide()
                rows.append((spec, label, widget))
            self.add_sections(form, op_type, rows)
            if not model.node_spec(name)["params"]:
                form.addRow(QtWidgets.QLabel("No parameters."))
        # The old panel may hold the button or menu whose click got us here:
        # it is deleted once that event is over, not now.
        old = self.takeWidget()
        if old is not None:
            old.hide()
            old.deleteLater()
        self.setWidget(body)
        self.set_pinned(actions)
        self.editor.watch_text_edits(body)
        shrinkable(body)
        self.align_labels()
        self.update_visibility()
        self.refresh_keys()
        self.keep_scroll(body, keep)

    def keep_scroll(self, body, value):
        """The params of `body` (just shown) scrolled to `value` once they are laid out: the scroll
        bar's range only grows to it when Qt lays the new widgets out, after this event."""
        widgets.scroll_when_laid_out(self.verticalScrollBar(), value,
                                     still=lambda: widgets.alive(body) and self.widget() is body)

    def update_error(self):
        """The red line under the node's header: its error from the last evaluation (or an amber
        warning), in plain words; hidden when it has none. Called again after each evaluation."""
        line = self.error_line
        if line is None or not widgets.alive(line):
            return
        node = self.node
        error = self.editor.node_errors.get(node) if node else None
        warning = self.editor.node_warnings.get(node) if node else None
        text = error or warning
        line.setVisible(bool(text))
        if not text:
            return
        line.setText((u"⚠ Error: " if error else u"⚠ ") + plain_message(text))
        line.setToolTip(text)
        line.setStyleSheet("color: %s; font-weight: bold;" % (_ge.ERROR if error else _ge.WARNING).name())

    def param_label_and_tip(self, name, spec):
        """A param's row name (with a ◆ when exposed as an input) and its tooltip: what it does and
        what its right-click offers."""
        model = self.editor.model
        text = (spec.get("label") or pretty(spec["name"])) + (" ◆" if spec["name"] in model.param_inputs(name) else "")
        tip = "%s: %s" % (spec["name"], spec["description"])
        if spec.get("channel") in ATTRIBUTE_TIPS:
            tip = ATTRIBUTE_TIPS[spec["channel"]] + "\n" + tip
        if spec.get("colorField"):
            tip += "\nRight-click: expose as an input, so a Map node's rgb output colors it per strand."
        elif gm.value_port(spec):
            tip += ("\nRight-click: expose as an input, so a Value node (or an Expression set to give a value) "
                    "sets it for the whole node.")
        elif gm.linkable(spec):
            tip += "\nRight-click: expose as an input, so a field node (Field, FieldNoise, ...) can drive it."
        if spec.get("kind") in KEYABLE_KINDS:
            why = model.keyable(name, spec["name"])
            tip += ("\nRight-click > Set Key, or S over it: key it (red: a key on this frame, pink: animated, "
                    "yellow: driven)" if why is None else "\nNot keyable: " + why)
        return text, tip

    def label_inside(self, param):
        """Whether the param's fill field shows its name now (a mappable param's does while it is a
        plain value, not while a map drives it)."""
        field = getattr(self, "inside_fields", {}).get(param)
        return field is not None and widgets.alive(field) and bool(field.label())

    def update_row_label(self, param):
        """Show the row's label again when its fill field is gone (a map picked), hide it when back."""
        label = getattr(self, "row_label_of", {}).get(param)
        if label is not None and widgets.alive(label) and label.parent() is not None:
            label.setVisible(not self.label_inside(param))
            self.align_labels()

    def align_labels(self):
        """The param labels in one column: each as wide as the widest, styled as the panel styles them
        (the labels shown: a fill field holds its own name)."""
        labels = [l for l in getattr(self, "row_labels", []) if widgets.alive(l) and not l.isHidden()]
        for label in labels:
            label.ensurePolished()
        width = max([l.sizeHint().width() for l in labels] or [0])
        for label in labels:
            label.setMinimumWidth(width)

    def add_sections(self, form, op_type, rows):
        """The param rows by section (param_sections): the main params, each group under a header
        that folds it, Advanced (folded unless unfolded this session) last.

        The rows are a column of their own (a label right-aligned in one width, its field beside it),
        not form rows: a hidden row then takes no room at all (a form keeps its spacing)."""
        if not rows:
            return
        box = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(box)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(form.verticalSpacing())
        form.addRow(box)
        specs = [spec for spec, _, _ in rows]
        by_name = {spec["name"]: (spec, label, widget) for spec, label, widget in rows}
        # A param on another's row (its spec's "row": a falloff beside its mode) goes after that
        # one's field, when both are in the same section.
        section_of = {spec["name"]: section for section, members in param_sections(specs) for spec in members}
        inline = {spec["name"] for spec in specs
                  if spec.get("row") in by_name and section_of.get(spec.get("row")) == section_of[spec["name"]]}
        # One width once styled (align_labels): the labels starting rows.
        self.row_labels = [label for spec, label, _ in rows if spec["name"] not in inline]
        lines = {}  # param -> its row's layout
        for section, members in param_sections(specs):
            header = None
            if section:
                header = QtWidgets.QPushButton()
                header.setObjectName("paramSection_" + section)
                header.setCheckable(True)
                header.setFlat(True)
                header.setProperty("role", "section")
                header.setChecked(not self.is_folded(op_type, section))
                self._title_section(header, section)
                header.toggled.connect(lambda on, s=section, h=header, t=op_type: self.fold(t, s, not on, h))
                column.addWidget(header)
            entries = []
            for spec in members:
                _, label, widget = by_name[spec["name"]]
                if spec["name"] in inline and spec["row"] in lines:
                    piece = QtWidgets.QWidget()  # its label and field, shown and hidden together
                    part = QtWidgets.QHBoxLayout(piece)
                    part.setContentsMargins(0, 0, 0, 0)
                    part.setSpacing(theme.px(6))
                    part.addWidget(label, 0, QtCore.Qt.AlignVCenter)
                    part.addWidget(widget, 1)
                    lines[spec["row"]].addWidget(piece, 1)
                    entries.append((spec, piece))
                    continue
                row = QtWidgets.QWidget()
                line = QtWidgets.QHBoxLayout(row)
                line.setContentsMargins(0, 0, 0, 0)
                line.setSpacing(theme.px(6))
                label.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
                label.setMinimumHeight(min(widget.sizeHint().height(), theme.px(26)))
                line.addWidget(label, 0, QtCore.Qt.AlignTop)
                line.addWidget(widget, 1)
                lines[spec["name"]] = line
                column.addWidget(row)
                entries.append((spec, row))
            self.sections.append((section, header, entries))

    @staticmethod
    def _title_section(header, section):
        header.setText(("▾  " if header.isChecked() else "▸  ") + section.upper().replace("&", "&&"))

    def is_folded(self, op_type, section):
        return ParamPanel.folded.get((op_type, section), section == ADVANCED)

    def fold(self, op_type, section, folded, header=None):
        """Fold (or open) a section of every node of this type, for the session."""
        ParamPanel.folded[(op_type, section)] = folded
        if header is not None and widgets.alive(header):
            self._title_section(header, section)
        self.update_visibility()

    def update_visibility(self):
        """Show the rows whose showWhen holds (in an open section), and the headers of sections with
        a row shown: after the panel is built and after each edit (a mode switched), no rebuild."""
        model, node = self.editor.model, self.node
        if node is None or model is None or node not in model.nodes or not getattr(self, "sections", None):
            return
        op_type = model.node_type(node)

        def value_of(param):
            try:
                return model.param(node, param)
            except gm.GraphError:
                return None

        specs = {spec["name"]: spec for spec in model.node_spec(node)["params"]}  # what a showWhen names
        # A param whose unit follows another (Noise's frequency by its space): built anew when it changed.
        for section, header, entries in self.sections:
            for spec, row in entries:
                if spec.get("unitWhen") and self.built_units.get(spec["name"]) != unit_of(spec, value_of):
                    QtCore.QTimer.singleShot(0, lambda n=node: self.node == n and self.show_node(n))
                    return
        for section, header, entries in self.sections:
            open_ = not section or not self.is_folded(op_type, section)
            any_shown = False
            for spec, row in entries:
                shown = shown_when(spec, value_of, specs.get)
                any_shown = any_shown or shown
                if widgets.alive(row):
                    row.setVisible(shown and open_)
            if header is not None and widgets.alive(header):
                header.setVisible(any_shown)

    def show_script_editor(self, name):
        """A script node's editor in the panel (script_editor.ScriptNodePanel): kept while the node is
        the one shown, until Done or another node. Returns it."""
        from groomlab.ui import script_editor
        self.node = name
        self.editing_script = name
        self.key_widgets = {}
        self.sections = []
        self.row_labels = []
        self.row_label_of, self.inside_labels, self.inside_fields = {}, {}, {}
        self.built_units = {}
        self.error_line = None
        self.expression = None
        self.script_panel = script_editor.ScriptNodePanel(self.editor, name)
        self.setWidget(self.script_panel)
        self.set_pinned(None)
        return self.script_panel

    def show_save_node(self, name):
        """Save as Node: a form in the panel (label, category, description, author, id, version,
        license, the exposed params to keep). Save writes the file and shows the node again; Cancel
        shows the node. Returns the form."""
        import groomlab_core
        model = self.editor.model
        self.node = name
        info = model.library_info(name) or {}
        definition = (model.nodes[name].get("params") or {}).get("definition") or {}
        exposed = [p["name"] for p in model.node_spec(name)["params"] if p["name"] in (definition.get("expose") or {})]
        body = QtWidgets.QWidget()
        body.setObjectName("saveNodeForm")
        form = QtWidgets.QFormLayout(body)
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setContentsMargins(theme.px(10), theme.px(4), theme.px(10), theme.px(10))
        title = QtWidgets.QLabel("Save %s as a node" % name)
        title.setProperty("role", "title")
        form.addRow(title)
        about = QtWidgets.QLabel("It goes to your node library (%s) and shows in Add Node and Tab search in every "
                                 "groom, the standalone app and Blender. Grooms that use it keep a copy, so they open "
                                 "anywhere." % groomlab_core.node_user_dir())
        about.setWordWrap(True)
        about.setProperty("role", "about")
        form.addRow(about)
        import re
        words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", name).replace("_", " ")
        label = QtWidgets.QLineEdit(definition.get("label") or " ".join(w[:1].upper() + w[1:] for w in words.split()))
        label.setObjectName("saveNodeLabel")
        category = QtWidgets.QComboBox()
        category.setObjectName("saveNodeCategory")
        category.setEditable(True)
        titles = [c for c in CATEGORY_COLORS if c not in ("Output", "Compound")]
        for entry in groomlab_core.library_nodes()["nodes"]:
            if entry.get("categoryTitle") not in titles and entry.get("categoryTitle") != "My Nodes":
                titles.append(entry["categoryTitle"])
        category.addItems(["My Nodes"] + titles)
        category.setCurrentText(definition.get("category") or "My Nodes")
        category.setToolTip("Where Add Node lists it: a built-in category, or type your own")
        description = QtWidgets.QPlainTextEdit(definition.get("description") or "")
        description.setObjectName("saveNodeDescription")
        description.setPlaceholderText("What it does, in a line (the menus show it)")
        description.setFixedHeight(theme.px(60))
        author = QtWidgets.QLineEdit(definition.get("author") or "")
        author.setObjectName("saveNodeAuthor")
        node_id = QtWidgets.QLineEdit(info.get("id") or "")
        node_id.setObjectName("saveNodeId")
        node_id.setToolTip("Never changes once shared: a newer version of a node keeps its id (yourname.node_name)")
        version = QtWidgets.QSpinBox()
        version.setObjectName("saveNodeVersion")
        version.setRange(1, 1000000)
        license_ = QtWidgets.QLineEdit(definition.get("license") or "")
        license_.setObjectName("saveNodeLicense")
        license_.setPlaceholderText("Optional (CC0-1.0, CC-BY-4.0, your own words)")
        typed = {"id": bool(info)}

        def suggest():
            if not typed["id"]:
                node_id.blockSignals(True)
                node_id.setText(groomlab_core.suggest_node_id(label.text(), author.text()))
                node_id.blockSignals(False)
            versions = dict((e["id"], e["version"]) for e in groomlab_core.library_nodes()["nodes"])
            version.setValue(max(versions.get(node_id.text().strip(), 0), info.get("version", 0)) + 1
                             if node_id.text().strip() in versions or info else 1)
        node_id.textEdited.connect(lambda _t: typed.__setitem__("id", True))
        node_id.editingFinished.connect(suggest)
        label.textEdited.connect(lambda _t: suggest())
        author.textEdited.connect(lambda _t: suggest())
        suggest()
        form.addRow("Label", label)
        form.addRow("Category", category)
        form.addRow("Description", description)
        form.addRow("Author", author)
        form.addRow("Id", node_id)
        form.addRow("Version", version)
        form.addRow("License", license_)
        checks = []
        if exposed:
            heading = QtWidgets.QLabel("Params it shows")
            heading.setProperty("role", "about")
            form.addRow(heading)
            for param in exposed:
                box = QtWidgets.QCheckBox(pretty(param))
                box.setObjectName("saveNodeParam_" + param)
                box.setChecked(True)
                box.setToolTip("Unchecked: its value now is kept inside the node")
                checks.append((param, box))
                form.addRow(box)
        error = QtWidgets.QLabel()
        error.setObjectName("saveNodeError")
        error.setWordWrap(True)
        error.setStyleSheet("color: %s;" % _ge.ERROR.name())
        form.addRow(error)
        buttons = QtWidgets.QHBoxLayout()
        save = QtWidgets.QPushButton("Save")
        save.setObjectName("saveNodeSave")
        cancel = QtWidgets.QPushButton("Cancel")
        cancel.setObjectName("saveNodeCancel")
        buttons.addStretch(1)
        buttons.addWidget(cancel)
        buttons.addWidget(save)
        form.addRow(buttons)

        def accept():
            header = {"label": label.text().strip(), "category": category.currentText().strip(),
                      "description": description.toPlainText().strip(), "author": author.text().strip(),
                      "id": node_id.text().strip(), "version": version.value(), "license": license_.text().strip(),
                      "params": [p for p, box in checks if box.isChecked()]}
            if header["category"] == "My Nodes":
                header["category"] = ""
            why = groomlab_core.node_id_error(header["id"])
            if why:
                error.setText(why)
                return
            if self.editor.save_as_node(name, header) is None:
                error.setText(self.editor.status.text())
        save.clicked.connect(accept)
        cancel.clicked.connect(lambda _=False: self.show_node(name))
        self.setWidget(body)
        self.set_pinned(None)
        body.fields = {"label": label, "category": category, "description": description, "author": author,
                       "id": node_id, "version": version, "license": license_, "params": dict(checks),
                       "save": save, "cancel": cancel, "error": error}
        self.save_form = body
        return body

    def header(self, name, op_type, title=True):
        """The node's name (title_row; `title` False: pinned with its actions instead) and what it does."""
        box = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(box)
        column.setContentsMargins(0, 0, 0, theme.px(6))
        column.setSpacing(theme.px(4))
        if title:
            column.addWidget(self.title_row(name, op_type))
        library = self.editor.model.library_info(name) if op_type in ("Compound", "Script") else None
        about = library["description"] if library else describe(op_type, self.editor.catalog)
        if about:
            text = QtWidgets.QLabel(about)
            text.setWordWrap(True)
            text.setProperty("role", "about")
            column.addWidget(text)
        # Its own height, never squeezed when the params below overflow the panel.
        box.setSizePolicy(QtWidgets.QSizePolicy.Preferred, QtWidgets.QSizePolicy.Fixed)
        return box

    def title_row(self, name, op_type):
        """The node's name, its type in its category's color, Upgrade and Update when they apply."""
        box = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(box)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(8))
        title = QtWidgets.QLabel(name)
        title.setProperty("role", "title")
        title.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        row.addWidget(title)
        library = self.editor.model.library_info(name) if op_type in ("Compound", "Script") else None
        color = CATEGORY_COLORS.get(library_category(library) if library else category_of(op_type, self.editor.catalog), _ge.MUTED)
        pill = QtWidgets.QLabel(library["label"] if library else op_type)
        if library:
            pill.setToolTip("Library node %s, version %d%s" % (library["id"], library["version"],
                            "" if library.get("embedded", True) else " (this groom holds no copy of it)"))
        pill.setStyleSheet("QLabel { background: %s; color: %s; border-radius: %dpx; padding: %dpx %dpx; "
                           "font-weight: bold; }" % (color.name(), readable_on(color).name(), theme.px(8),
                                                     theme.px(2), theme.px(8)))
        row.addWidget(pill)
        row.addStretch(1)
        saved, newest = self.editor.model.node_version(name)
        if saved < newest:  # saved with an older behaviour of its operator: offer the newest
            upgrade = QtWidgets.QToolButton()
            upgrade.setObjectName("upgradeNode")
            upgrade.setText("Upgrade")
            upgrade.setProperty("role", "chip")
            upgrade.setToolTip(
                "This %s keeps the behaviour of version %d, as it was saved; %s nodes are at version %d now.\n"
                "Upgrade: it evaluates as new %s nodes do (its result may change). One undo step."
                % (op_type, saved, op_type, newest, op_type))
            upgrade.clicked.connect(lambda _=False, n=name: self.editor.upgrade_node(n))
            row.addWidget(upgrade)
        if library and library.get("updateAvailable"):  # the library has a newer copy: offer it
            update = QtWidgets.QToolButton()
            update.setObjectName("updateLibraryNode")
            update.setText("Update to v%d" % library["latest"])
            update.setProperty("role", "chip")
            update.setToolTip("Your node library has version %d of %s (this groom holds version %d).\n"
                              "Update: take it (its values kept where it still has them). One undo step."
                              % (library["latest"], library["label"], library["version"]))
            update.clicked.connect(lambda _=False, n=name: self.editor.update_library_node(n))
            row.addWidget(update)
        return box

    def set_expression_param(self, node, param, value):
        """An edit from the expression panel: into the graph, then the node's error (if any) under the code."""
        self.editor.set_param(node, param, value)
        panel = getattr(self, "expression", None)
        if panel is not None and widgets.alive(panel):
            panel.show_error()

    # -- What a source node reads from the scene ---------------------------------

    def scene_inputs(self, op_type):
        """For Scalp, ColliderMesh and Guides nodes: the mesh or guides they read in the scene, and
        a mesh field (mesh_field) or buttons to change them."""
        import groomlab
        from maya import cmds
        groom = self.editor.groom
        if op_type not in ("Scalp", "Guides", "ColliderMesh") or not groom or not cmds.objExists(groom):
            return None
        model, node = self.editor.model, self.node
        other_scalp = None  # a Scalp node reading another of the groom's scalps: its name
        if op_type == "Scalp" and model.param(node, "input") != "scalp":
            other_scalp = model.param(node, "input")
            if not groomlab.valid_scalp_name(other_scalp):
                return None  # another of the host's inputs: no Maya connection to show
        self.mesh_combo = self.mesh_pick = None
        box = QtWidgets.QFrame()
        t = theme.current()
        box.setStyleSheet("QFrame { background: %s; border-radius: %dpx; }"
                          % (t.color("ui.panel"), int(t.get("ui.radius", 8))))
        grid = QtWidgets.QGridLayout(box)
        grid.setContentsMargins(6, 6, 6, 6)
        s = groomlab.inputs_summary(groom)

        def line(row, label, value, buttons):
            grid.addWidget(QtWidgets.QLabel(label), row, 0)
            text = QtWidgets.QLabel("<b>%s</b>" % value)
            text.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
            grid.addWidget(text, row, 1)
            for i, (caption, slot, tip) in enumerate(buttons):
                b = QtWidgets.QPushButton(caption)
                b.setToolTip(tip)
                b.clicked.connect(lambda _=False, f=slot: f())  # not clicked's `checked` as an argument
                grid.addWidget(b, row, 2 + i)

        top = 0  # the mesh row's
        if op_type == "Scalp" and (other_scalp or s.get("scalps")):
            # Several scalps on the groom: which one the node reads (its `input`).
            grid.addWidget(QtWidgets.QLabel("Scalp"), 0, 0)
            grid.addWidget(self.scalp_choice(model.param(node, "input"), [o["name"] for o in s.get("scalps", [])]),
                           0, 1, 1, 3)
            top = 1
        if op_type == "ColliderMesh":
            name = model.param(node, "name")
            current = next((c["mesh"] for c in s["colliders"] if c["name"] == name), None)
            grid.addWidget(QtWidgets.QLabel("Mesh"), 0, 0)
            grid.addWidget(self.mesh_field("collider", current, name), 0, 1, 1, 3)
        elif other_scalp:
            # Another scalp of the groom (extraScalps): the mesh under its name, or none yet.
            current = next((o["mesh"] for o in s.get("scalps", []) if o["name"] == other_scalp), None)
            grid.addWidget(QtWidgets.QLabel("Mesh"), top, 0)
            grid.addWidget(self.mesh_field("extraScalp", current, other_scalp), top, 1, 1, 3)
        elif op_type == "Scalp":
            short = lambda n: n.split("|")[-1] if n else "none"
            grid.addWidget(QtWidgets.QLabel("Mesh"), top, 0)
            grid.addWidget(self.mesh_field("scalp", s["scalp"]), top, 1, 1, 3)
            # A rest pose only matters for a scalp that moves (skinned, blend shapes, a cache):
            # strands are placed on it so the same strands stay put while the scalp deforms.
            # Grooms on deformed scalps get one on their own; the row shows when it applies.
            # (By node type, not cmds.ls(type=...): AlembicNode is AbcImport's, an unknown
            # type, and a warning, while that plug-in is not loaded.)
            history = (cmds.listHistory(s["scalp"]) or []) if s["scalp"] else []
            moving = bool(cmds.ls(history, type="geometryFilter")) or any(
                cmds.nodeType(n) in ("AlembicNode", "cacheFile") for n in history)
            if moving or s["rest"]:
                orig = s["scalp"] and groomlab._original_shape(s["scalp"])
                value = "auto (the undeformed scalp)" if s["rest"] and s["rest"] == orig else short(s["rest"])
                line(top + 1, "Rest pose", value, [
                    ("Set", lambda: self._scene_run(groomlab.set_rest_mesh, groom),
                     "The scalp before it deforms: strands are placed on it, so the same strands stay put "
                     "while the scalp moves. Without deformers (an Alembic cache, say): a copy at this frame"),
                    ("Clear", lambda: self._scene_run(groomlab.clear_rest_mesh, groom),
                     "No rest pose: strands are placed on the scalp as it is on each frame")])
        else:
            text = groomlab.guides_text(s) if s["groups"] else "no guides yet"
            if s["curves"]:
                text += " + %d curves" % s["curves"]
            line(0, "Groom has", text, [("Groups...", self.open_groups, "See the groups in the Groom Tools")])
        return box

    def scalp_choice(self, current, others):
        """A Scalp node's menu of the groom's scalps (scalp: its own, then the others by name):
        choosing one sets the node's input, and the panel shows that scalp's mesh."""
        combo = QtWidgets.QComboBox()
        combo.setObjectName("scalpChoice")
        combo.setToolTip("Which of the groom's scalps this node reads: a Scatter fed it grows hair on that mesh alone")
        names = ["scalp"] + [n for n in others if n != "scalp"]
        if current not in names:
            names.append(current)
        for n in names:
            combo.addItem("scalp (the groom's own)" if n == "scalp" else n, n)
        combo.setCurrentIndex(names.index(current))

        def chosen(index):
            value = combo.itemData(index)
            if value != current:
                self.editor.set_param(self.node, "input", value)
                later(lambda: self.node and self.show_node(self.node))
        combo.activated.connect(chosen)
        return combo

    def scene_soon(self):
        """The groom's scalp or colliders were rewired: a Scalp or ColliderMesh node's panel is
        shown again once the change is over (once for many: a collider's mesh and rest mesh)."""
        if getattr(self, "_scene_pending", False):
            return
        self._scene_pending = True

        def refresh():
            self._scene_pending = False
            if (widgets.alive(self) and self.node and self.node in self.editor.model.order
                    and self.editor.model.node_type(self.node) in ("Scalp", "ColliderMesh")):  # (any scalp)
                self.show_node(self.node)
        QtCore.QTimer.singleShot(0, refresh)

    def _scene_run(self, fn, *args):
        try:
            fn(*args)
        except (RuntimeError, ValueError) as e:
            self.editor.report(str(e))
            return
        self.show_node(self.node)

    def mesh_field(self, kind, current, name=None):
        """The scene mesh a Scalp or ColliderMesh node reads (the groom's inMesh, or the collider
        `name`'s colliderMeshes element): a menu of the scene's meshes (listed again each time it
        opens), an eyedropper that picks the next mesh selected in the viewport or the Outliner
        (groomlab.ui.pick, as the Attribute Editor's Pick), and Select. Choosing one rewires the
        connection through the same calls as the Attribute Editor (pick.set_input)."""
        from groomlab.ui import pick
        from maya import cmds
        groom = self.editor.groom
        current = (cmds.ls(current, long=True) or [None])[0] if current else None
        box = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(box)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(4))
        combo = MeshCombo(lambda: self._mesh_choices(kind, current, name), current)
        combo.setObjectName("meshField")
        combo.setToolTip("The mesh this node reads: choose another scene mesh (the menu lists them as it opens)")
        combo.activated.connect(lambda index: later(lambda: self.choose_mesh(kind, combo.itemData(index), name)))
        row.addWidget(combo, 1)
        dropper = QtWidgets.QToolButton()
        dropper.setObjectName("meshPick")
        dropper.setCheckable(True)
        dropper.setIcon(widgets.glyph("eyedropper"))
        dropper.setToolTip("Pick the mesh: click it in the viewport or the Outliner (Esc, or this again, cancels)")
        dropper.setChecked(bool(pick.armed(groom, pick.input_key(kind, name))))
        dropper.clicked.connect(lambda _=False: self.pick_mesh(kind, name))
        row.addWidget(dropper)
        select = QtWidgets.QPushButton("Select")
        select.setToolTip("Select the mesh in the scene")
        select.setEnabled(bool(current))
        select.clicked.connect(lambda _=False: current and cmds.objExists(current) and cmds.select(current))
        row.addWidget(select)
        self.mesh_combo, self.mesh_pick = combo, dropper
        return box

    def _mesh_choices(self, kind, current, name):
        """[(label, mesh shape)] for a mesh field: every scene mesh the input takes, and the one it has."""
        from groomlab.ui import pick
        groom = self.editor.groom
        out = []
        for mesh in pick.scene_meshes():
            if pick._same(mesh, current) or not pick.problem(groom, kind, mesh, name):
                out.append((pick.label(mesh), mesh))
        return out

    def choose_mesh(self, kind, mesh, name=None, ask=True):
        """A mesh chosen in a mesh field: onto the groom (pick.set_input), the panel shown again."""
        from groomlab.ui import pick
        if not mesh or not self.editor.groom:
            return
        current = self.mesh_combo.current if self.mesh_combo is not None and widgets.alive(self.mesh_combo) else None
        if pick._same(mesh, current) or not self._node_on_groom():
            return
        try:
            self.editor.report(pick.set_input(self.editor.groom, kind, mesh, name, ask=ask))
        except (RuntimeError, ValueError) as e:
            self.editor.report(str(e))
        self.show_node(self.node)

    def _node_on_groom(self):
        """The shown node on the groom (a ColliderMesh just added, edits not applied yet: the graph
        applied first), so the collider calls find it there. False when the graph can't be applied."""
        import groomlab
        stored = [n["name"] for n in groomlab.get_graph(self.editor.groom, expand_default=True).get("nodes", [])]
        return self.node in stored or self.editor.apply()

    def pick_mesh(self, kind, name=None, ask=True):
        """The mesh field's eyedropper (pick.pick_input): the panel shows the result when it ends."""
        from groomlab.ui import pick

        def ended(applied):
            later(lambda: self.node and self.show_node(self.node))
        if not self._node_on_groom():
            return "failed"
        result = pick.pick_input(self.editor.groom, kind, name, on_end=ended, ask=ask)
        if self.mesh_pick is not None and widgets.alive(self.mesh_pick):
            self.mesh_pick.setChecked(result == "armed")
        self.editor.report({"armed": "click %s in the viewport or the Outliner (Esc cancels)" % pick.WHAT[kind],
                            "cancelled": "pick cancelled"}.get(result, ""))
        return result

    def pick_position(self, name, param):
        """A position field's Pick (Transform's pivot): where the selected vertices, locator or transform
        are (pick.selected_position, world space as groom data), set as any edit of the field is.
        Returns the position, or None when nothing selected has one (said in the status line)."""
        from groomlab.ui import pick
        try:
            position, what = pick.selected_position(self.editor.groom)
        except ValueError as e:
            self.editor.report(str(e))
            return None
        value = [float(c) for c in position]
        self.editor.set_param(name, param, value)
        self.editor.report("%s.%s: (%.4g, %.4g, %.4g) from %s" % ((name, param) + tuple(value) + (what,)))
        later(lambda: self.node == name and self.show_node(name))  # the fields show it
        return value

    # -- Node actions: what a node needs from the scene, on the node itself -------

    # -- The pinned actions: over the params, in view however far they scroll ---------------

    pinned = None  # the node's actions (set_pinned), or None

    def set_pinned(self, widget):
        """`widget` (the node's actions) pinned over the params, in the scroll area's top margin; None
        takes the old one away (deleted once the click that got here is over)."""
        old = self.pinned
        if old is not None and widgets.alive(old) and old is not widget:
            # The focus off the old bar first (issue #47): a hidden focus widget hands it to the
            # next one in the chain, a param in the scrolled body, and the scroll area scrolls to
            # show it, so the params jumped after a click on Bake Maps or an export.
            focus = QtWidgets.QApplication.focusWidget()
            if focus is not None and (focus is old or old.isAncestorOf(focus)):
                self.setFocus(QtCore.Qt.OtherFocusReason)
            old.hide()
            old.setParent(None)  # (out of the panel now, as takeWidget does with the params)
            old.deleteLater()
            self._retired = old  # kept until then: its button may be the one clicked
        self.pinned = widget
        if widget is not None:
            widget.setParent(self)
            shrinkable(widget)
            # Its buttons take no focus from a click: the keyboard stays where it was, and nothing
            # hands focus on into the params when the bar is rebuilt.
            for button in widget.findChildren(QtWidgets.QAbstractButton):
                button.setFocusPolicy(QtCore.Qt.NoFocus)
            widget.show()
        self.place_pinned()

    def place_pinned(self):
        """The pinned actions across the top, as tall as they need at the panel's width; the params
        below them (the viewport's top margin)."""
        w = self.pinned
        if w is None or not widgets.alive(w):
            self.setViewportMargins(0, 0, 0, 0)
            return
        frame = self.frameWidth()
        # Clear of the vertical scroll bar's place whether it shows or not (no width that changes as
        # the params' length does).
        width = max(1, self.width() - 2 * frame - self.verticalScrollBar().sizeHint().width())
        height = w.heightForWidth(width) if w.hasHeightForWidth() else -1
        if height <= 0:
            height = w.sizeHint().height()
        w.setGeometry(frame, frame, width, height)
        self.setViewportMargins(0, height, 0, 0)

    def resizeEvent(self, event):
        super(ParamPanel, self).resizeEvent(event)
        self.place_pinned()

    _scroll_hold = None  # (node, scroll) taken as an action began (action_runner)

    def restore_scroll_hold(self):
        """The params scrolled back to where they were as the action began, when the same node shows."""
        hold, self._scroll_hold = self._scroll_hold, None
        if hold and hold[0] is not None and hold[0] == self.node and widgets.alive(self.widget()):
            self.keep_scroll(self.widget(), hold[1])

    def action_runner(self, fn, done=None, busy=None):
        """A button's click: `fn` run once the click is over, its changes into the editor's graph,
        then onto the groom (reloading rebuilds this panel, and the button that was clicked with it).
        `done`: what to report (a text, or a function of the result); `busy`: what a long one says
        while it runs (the panel disabled, the wait cursor)."""
        def act():
            # Where the params are scrolled to, taken before anything moves them (issue #47): the
            # panel disabled while a long one runs passes the focus on, and the scroll area scrolls
            # to where it lands, before the reload that would keep the scroll.
            self._scroll_hold = (self.node, self.verticalScrollBar().value())
            focus = QtWidgets.QApplication.focusWidget()
            if busy and focus is not None and (focus is self or self.isAncestorOf(focus)):
                focus.clearFocus()  # (no focus in the panel: disabling it hands none on into the params)
            try:
                if busy:
                    with widgets.busy([self], self.editor.status, busy):
                        result = fn()
                else:
                    result = fn()
            except (RuntimeError, ValueError) as e:
                self.editor.report(plain_message(e), trouble=True)
                self.restore_scroll_hold()
                return
            except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
                import traceback
                traceback.print_exc()  # the Script Editor: the details
                self.editor.report("%s (details in the Script Editor)" % plain_message(e), trouble=True)
                return
            if not self.editor.apply():
                self.restore_scroll_hold()
                return
            self.editor.reload()
            self.restore_scroll_hold()
            if done:
                self.editor.report(done(result) if callable(done) else done)

        def go(_=False):
            later(act)
        return go

    def node_action_table(self, name):
        """What graph_ui.json's nodeActions run in Maya for node `name`, on the editor's groom
        ({action id: a callable}): the Cards exports, Simulate's range, bake and clear, Bake Node to
        Guides, a Sculpt node's Clear, a compound's Ungroup and saves."""
        from groomlab.ui import menu
        editor = self.editor
        run = self.action_runner

        def card_maps():
            from groomlab.ui import card_maps_window
            try:
                card_maps_window.show(editor.groom)
            except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
                import traceback
                traceback.print_exc()
                editor.report("%s (details in the Script Editor)" % plain_message(e), trouble=True)

        # The menu's exports (their dialogs, defaults and messages), for this groom.
        def unreal():
            return menu.export_unreal_cards_dialog(groom=editor.groom)

        def unity():
            return menu.export_unity_cards_dialog(groom=editor.groom)

        def gltf():
            return menu.export_gltf_cards_dialog(groom=editor.groom)

        return {
            "export.cardMaps": lambda: later(card_maps),
            "export.unrealCards": run(unreal, lambda r: "wrote %s" % r["fbx"] if r else ""),
            "export.unityCards": run(unity, lambda r: "wrote %s" % r["fbx"] if r else ""),
            "export.gltfCards": run(gltf, lambda r: "wrote %s" % r[0] if r else ""),
            "sim.range": lambda: later(lambda: menu.simulate_range(groom=editor.groom)),
            "sim.bake": lambda: later(lambda: menu.bake_core_simulation(groom=editor.groom)),
            "sim.clear": lambda: later(lambda: menu.clear_core_simulation(groom=editor.groom)),
            "node.bake": lambda: later(lambda: editor.bake_to_guides(name)),
            "sculpt.clearEdits": lambda: later(lambda: (editor.clear_sculpt_edits(name), editor.reload())),
            "braid.sculptSections": lambda: later(lambda: editor.sculpt_braid_sections(name)),
            "braid.doneSculptSections": lambda: later(lambda: editor.done_sculpt_sections(name)),
            "compound.ungroup": lambda: later(lambda: editor.expand([name])),
            "compound.saveAsNode": lambda: later(lambda: editor.show_save_node(name)),
            "compound.saveToLibrary": lambda: later(lambda: editor.save_to_library(name)),
        }

    def node_action_state(self, name):
        """The conditions graph_ui.json's nodeActions need for node `name`: "type.<its type>",
        "node.library" for a node from the library, and "braid.sculpting" for a Braid whose sections
        the brushes shape (Sculpt Sections: Done Sculpting Sections in its place)."""
        model = self.editor.model
        node_type = model.node_type(name)
        state = {"type." + node_type}
        if model.library_info(name):
            state.add("node.library")
        if node_type == "Braid":
            target = self.editor.sculpt_target()
            if target and model.braid_sculpt_node(name) == target:
                state.add("braid.sculpting")
        return state

    def node_actions(self, name):
        """A node's actions, pinned over its params (node_actions.py)."""
        from groomlab.ui import node_actions as _m
        return _m.node_actions(self, name)

    def relink_map_image(self, map_name, path=None):
        """Point the groom's map `map_name` at an image (groomlab.set_map_texture): `path`, else picked
        in a file dialog (only a click gets there; scripts pass the path). Returns the stored path."""
        import groomlab
        groom = self.editor.groom
        if not groom:
            return None
        if path is None:
            from groomlab.ui import tool_tabs
            expected = groomlab.map_texture(groom, map_name)
            folder = os.path.dirname(expected) if expected else ""
            if not folder or not os.path.isdir(folder):
                folder = groomlab.scene_folder() or groomlab.project_root()
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Relink Image: %s" % map_name, folder,
                                                            tool_tabs.IMAGE_FILTER)
            if not path:
                return None
        try:
            stored = groomlab.set_map_texture(groom, map_name, path)
        except (RuntimeError, ValueError) as e:
            self.editor.report(str(e).strip())
            return None
        self.editor.reload()
        self.editor.report("%s reads %s" % (map_name, stored))
        return stored

    def param_menu(self, name, param, global_pos):
        menu = self.build_param_menu(name, param)
        if not menu.isEmpty():  # text params have nothing to offer
            qt.exec_(menu, global_pos)

    def param_actions(self, node, param):
        """What a param's menus run (engine/ui/graph_ui.json's paramMenu, keyMenu, paintMenu) for node.param."""
        b = definitions.bound
        return {"param.setKey": b(lambda: self.set_key(node, param)),
                "param.deleteKey": b(lambda: self.delete_key(node, param)),
                "param.removeAnimation": b(lambda: self.remove_animation(node, param)),
                "param.selectAttribute": b(lambda: self.select_attribute(node, param)),
                "param.expose": b(lambda on: self.editor.set_param_exposed(node, param, on)),
                "param.paintGuides": b(lambda: self.paint_param(node, param, "guides")),
                "param.paintVertices": b(lambda: self.paint_param(node, param, "vertices")),
                "param.paintTexture": b(lambda: self.paint_param(node, param, "texture")),
                "param.driveWithSlider": b(lambda: self.add_control(node, param)),
                "param.promote": b(lambda on: self.editor.promote_param(node, param, on))}

    def build_param_menu(self, name, param):
        menu = QtWidgets.QMenu(self)
        menu.setToolTipsVisible(True)  # why a param cannot be keyed
        self.fill_param_menu(menu, name, param)
        return menu

    def fill_param_menu(self, menu, name, param):
        """The param's menu (graph_ui.json's paramMenu: keys, expose, paint, drive) into `menu`: the
        label's, and atop a fill field's own (Reset to Default, its range)."""
        exposed = param in self.editor.model.param_inputs(name)
        spec = self.editor.model.param_spec(name, param)
        state, values = set(), {}
        if spec and spec.get("kind") in KEYABLE_KINDS:
            state.add("param.keyable")
            state, values = self.key_state(name, param, state)
        if gm.linkable(spec):
            state.add("param.linkable")
            if spec.get("mappable") or spec.get("colorField"):  # (a color: painted as a color map)
                state.add("param.mappable")
        if self.editor.groom:
            state.add("groom")
        promoted = False
        if self.editor.param_promotable(name, param):  # inside a compound: Promote to Compound
            state.add("param.promotable")
            promoted = self.editor.param_promoted(name, param)
        widgets.fill_menu(menu, definitions.get("graph/paramMenu"), definitions.binder(self.param_actions(name, param)),
                          state, values, generators=self.key_generators(name, param),
                          checked=lambda item: promoted if item.get("id") == "param.promote" else exposed)

    PAINT_TIP = definitions.get("graph/paintMenu")["tip"]

    def paint_menu(self, name, param, parent):
        """Paint > On Guides / On Scalp Vertices / As Texture: the param's map, painted (the param
        menu's and the Paint chip's, one list: graph_ui.json's paintMenu)."""
        container = definitions.get("graph/paintMenu")
        paint = QtWidgets.QMenu(container["label"], parent)  # parented: it outlives the caller's references
        paint.setToolTipsVisible(True)
        paint.setToolTip(container["tip"])
        widgets.fill_menu(paint, container["items"], definitions.binder(self.param_actions(name, param)))
        return paint

    def paint_chip(self, name, param):
        """The yellow Paint chip beside a mappable param, a paintbrush (the app's Paint button): a click
        offers where to paint its map, as right-click > Paint does."""
        chip = QtWidgets.QToolButton()
        chip.setObjectName("paintChip")
        chip.setIcon(widgets.icon("paint", theme.px(32)))
        chip.setIconSize(QtCore.QSize(theme.px(16), theme.px(16)))
        chip.setAccessibleName("Paint")
        chip.setToolTip(self.PAINT_TIP)
        chip.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        chip.setMenu(self.paint_menu(name, param, chip))
        t = theme.current()
        yellow = t.qcolor("yellow")
        chip.setStyleSheet(
            "QToolButton#paintChip { color: %s; background: transparent; border: 1px solid %s; border-radius: %dpx;"
            " padding: 1px %dpx; font-weight: bold; }"
            " QToolButton#paintChip:hover { background: %s; color: %s; }"
            " QToolButton#paintChip::menu-indicator { image: none; width: 0px; }"
            % (yellow.name(), yellow.name(), theme.px(4), theme.px(6), yellow.name(), readable_on(yellow).name()))
        return chip

    def add_control(self, node, param):
        import groomlab
        if not self.editor.apply():
            return
        try:
            attr = groomlab.add_control(self.editor.groom, node, param)
        except (RuntimeError, ValueError, gm.GraphError) as e:
            self.editor.report(str(e))
            return
        self.editor.reload()
        self.editor.report("%s.%s is driven by %s (keyable in the channel box)" % (node, param, attr))

    # -- Keyed params: Set Key on a param, its state in the Channel Box's colors ---------------------

    def key_state(self, node, param, state=()):
        """The key menu's conditions for a param (keyable here, keyed on this frame, linked to its
        attribute) and the values its texts name (why it cannot be keyed)."""
        model, groom = self.editor.model, self.editor.groom
        why = model.keyable(node, param) if groom else "no groom"
        linked = model.control_of(node, param) is not None
        state = set(state)
        if linked or why is None:
            state.add("param.canKey")
        if linked:
            state.add("param.linked")
            if key_state(groom, node, param, model) == "keyed":
                state.add("param.keyed")
        return state, {"why": why or ""}

    def key_generators(self, node, param):
        """The key menu's last entry: the attribute a linked param is set by (disabled, to read)."""
        def control(menu, _item):
            import groomlab
            menu.addAction(groomlab.param_control(self.editor.groom, node, param) or "").setEnabled(False)
        return {"paramControl": widgets.menu_filler(control)}

    def add_key_actions(self, menu, node, param):
        """Set Key, Delete Key, Remove Animation and Select Attribute for a param (in its menus:
        graph_ui.json's keyMenu)."""
        state, values = self.key_state(node, param)
        widgets.fill_menu(menu, definitions.get("graph/keyMenu"), definitions.binder(self.param_actions(node, param)),
                          state, values, generators=self.key_generators(node, param))

    def _key_edit(self, fn, *args):
        """Run a key edit on the groom (the editor's graph applied first), then show the result."""
        if not self.editor.groom or not self.editor.apply():
            return None
        try:
            result = fn(self.editor.groom, *args)
        except (RuntimeError, ValueError, gm.GraphError) as e:
            self.editor.report(str(e))
            return None
        self.editor.sync_now()
        self.refresh_keys()
        return result if result is not None else True

    def set_key(self, node, param):
        """Key the param at the current frame (the first key links it to an attribute of its own)."""
        import groomlab
        from maya import cmds
        why = self.editor.model.keyable(node, param)
        if why and self.editor.model.control_of(node, param) is None:
            self.editor.report("%s.%s cannot be keyed: %s" % (node, param, why))
            return None
        attr = self._key_edit(groomlab.set_param_key, node, param)
        if attr:
            self.editor.report("keyed %s.%s at frame %g (%s)" % (node, param, cmds.currentTime(query=True), attr))
        return attr

    def delete_key(self, node, param):
        import groomlab
        if self._key_edit(groomlab.delete_param_key, node, param):
            self.editor.report("deleted the key of %s.%s" % (node, param))

    def remove_animation(self, node, param):
        import groomlab
        if self._key_edit(groomlab.remove_param_animation, node, param):
            self.editor.report("%s.%s is a plain value again" % (node, param))

    def select_attribute(self, node, param):
        import groomlab
        attr = groomlab.select_param_attribute(self.editor.groom, node, param)
        self.editor.report("selected %s" % attr if attr else "%s.%s is not keyed" % (node, param))

    def hovered_param(self):
        """The param whose row is under the mouse (the label or its field), or None. Qt's own record
        of what the mouse is over (the enter and leave events the widgets got), not the cursor's
        position: a window covering the panel hides it, as it hides the panel's tooltips."""
        body = self.widget()
        if body is None or not self.viewport().underMouse():
            return None
        for widget in body.findChildren(QtWidgets.QWidget):
            param = widget.property("glParam")
            if param and widget.isVisible() and widget.underMouse():
                return param
        return None

    def refresh_keys(self):
        """Each number field's state (red: a key on this frame, pink: animated, yellow: driven) and, for
        a keyed param, its value on this frame: after edits and on every time change."""
        groom, model, node = self.editor.groom, self.editor.model, self.node
        if node is None or model is None or node not in model.nodes or not getattr(self, "key_widgets", None):
            return
        import groomlab
        for param, field in list(self.key_widgets.items()):
            if not widgets.alive(field):
                del self.key_widgets[param]
                continue
            if groom and model.control_of(node, param) is not None:
                try:
                    show_value(field, groomlab.param_value(groom, node, param, model))
                except (RuntimeError, ValueError):
                    pass
            paint_key_state(field, key_state(groom, node, param, model))

    def paint_param(self, node, param, kind):
        """A map (guides, vertices or texture) the param reads (as every host links it), painted in
        Groom Tools > Paint (the map it reads already, when it reads one): Ctrl lowers the param
        toward 0, strokes paint it back to its value."""
        import groomlab
        from groomlab.ui import menu
        groom = self.editor.groom
        if not self.editor.apply():  # the editor's graph first
            return
        if kind == "texture" and not menu.ensure_scene_saved("texture maps"):
            return
        # Faces of the scalp selected: a map made here is white on them only (the Paint tab's New Map too).
        faces = groomlab.selected_scalp_faces(groom)
        try:
            name = groomlab.paint_param(groom, node, param, kind=kind, faces=faces)
            from groomlab.ui import palette
            palette.show(tab="paint")
            palette.select_map(name)
            where = {"guides": "the guides", "vertices": "the scalp's vertices", "texture": "a texture"}[kind]
            message = "painting %s.%s on %s (map %s): Ctrl lowers it, strokes paint it back" % (node, param, where, name)
            if faces:
                message += " (a map made now is white on the %d selected faces only)" % len(faces)
        except (RuntimeError, ValueError, gm.GraphError) as e:
            self.editor.report(str(e))
            return
        self.editor.reload()
        self.editor.report(message)

    # -- Widgets per kind --------------------------------------------------

    def _value_of(self, name):
        model = self.editor.model

        def value_of(param):
            try:
                return model.param(name, param)
            except gm.GraphError:
                return None
        return value_of

    def editor_for(self, name, spec):
        value = self.editor.model.param(name, spec["name"])
        if spec.get("unitWhen"):  # its unit as the node's other params make it (shown, and its sliders')
            spec = dict(spec, unit=unit_of(spec, self._value_of(name)))
            self.built_units[spec["name"]] = spec["unit"]
        if self.editor.groom and self.editor.model.control_of(name, spec["name"]) is not None:
            import groomlab  # keyed: its value on this frame
            try:
                value = groomlab.param_value(self.editor.groom, name, spec["name"], self.editor.model)
            except (RuntimeError, ValueError):
                pass
        if spec["mappable"]:
            return MappableEditor(self, name, spec, value)
        return self.plain_editor(name, spec, value)

    def vector_expression_editor(self, spec, value, set_value):
        """A direction: its three numbers, or (f(x) on) an expression giving a vector per strand,
        e.g. normalize(N + [0, -1, 0])."""
        w = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(w)
        row.setContentsMargins(0, 0, 0, 0)
        expression = value.get("expression") if isinstance(value, dict) else None
        vector = [float(c) for c in (value if isinstance(value, (list, tuple)) else spec["default"])]
        if expression is None:
            boxes = []
            for c in vector:
                b = widgets.CompactSpinBox()  # the spec's decimals, trailing zeros dropped
                b.setRange(-1e9, 1e9)
                b.setDecimals(6)
                b.display_decimals = spec.get("decimals", 3)
                b.setValue(c)
                row.addWidget(b)
                boxes.append(b)
            for b in boxes:
                b.editingFinished.connect(lambda: set_value([x.value() for x in boxes]))
            w.boxes = boxes
        else:
            field = QtWidgets.QLineEdit(expression)
            field.setObjectName("vectorExpression")
            field.setToolTip("A vector per strand: [x, y, z], P, N, tip, normalize(...), rotate(...), vnoise(P)... "
                             "(the Expression node's language)")
            field.editingFinished.connect(lambda: set_value({"expression": field.text().strip() or "[0, 0, 0]"}))
            row.addWidget(field, 1)
            w.field = field
        toggle = QtWidgets.QToolButton()
        toggle.setObjectName("vectorExpressionToggle")
        toggle.setText("f(x)")
        toggle.setCheckable(True)
        toggle.setChecked(expression is not None)
        toggle.setToolTip("An expression giving a vector per strand instead of one direction for all")

        def switch(on):
            set_value({"expression": "[%g, %g, %g]" % tuple(vector)} if on else [float(c) for c in spec["default"]])
            self.show_node(self.node)  # the row as the other kind: the expression's field, or three numbers
        toggle.toggled.connect(lambda on: later(lambda: switch(on)))
        row.addWidget(toggle)
        w.toggle = toggle
        return w

    def plain_editor(self, name, spec, value, on_change=None, on_preview=None):
        set_value = on_change or (lambda v, p=spec["name"]: self.editor.set_param(name, p, v))
        preview = on_preview or (lambda v, p=spec["name"]: self.editor.preview_param(name, p, v))
        kind = spec["kind"]
        if kind == "bool":
            w = QtWidgets.QCheckBox()
            w.setChecked(bool(value))
            w.toggled.connect(set_value)
            if name == self.node:
                self.key_widgets[spec["name"]] = w
        elif kind in ("int", "float"):
            # One slider range per operator param (and display unit), shared by every node of that type.
            from groomlab import units
            kind_of_unit = spec.get("unit") or ""
            f = units.factor(kind_of_unit) if kind == "float" else 1.0
            key = "%s.%s" % (self.editor.model.node_type(name), spec["name"])
            if f != 1.0:
                key += "@" + units.unit(kind_of_unit)[0]
            default = spec["default"] if isinstance(spec["default"], (int, float)) else value
            lo, hi = units.soft_range(kind_of_unit) or (None, None)  # density: around real hair's
            if lo is None and spec.get("softMin") is not None and spec.get("softMax") is not None:  # the spec's
                lo, hi = spec["softMin"] * f, spec["softMax"] * f
            hard = [spec[k] * f if spec.get(k) is not None else None for k in ("min", "max")]  # the core clamps there
            slider = widgets.SoftSlider(value * f, lo, hi, integer=kind == "int", minimum=hard[0], maximum=hard[1],
                                        decimals=4, key=key, default=default * f,
                                        shown_decimals=widgets.display_decimals(spec.get("decimals"), f))
            # The slider gives floats: an int param stores a whole number (Layers' count, which the
            # panel's showWhen and the GPU preview compare as one).
            stored = (lambda v: int(round(v))) if kind == "int" else (lambda v: v / f)
            slider.dragged.connect(lambda v: preview(stored(v)))
            slider.committed.connect(lambda v: set_value(stored(v)))
            slider.unit_factor = f  # what show_value multiplies a keyed value by
            if name == self.node:
                self.key_widgets[spec["name"]] = slider
                slider.menu_hook = lambda menu, p=spec["name"]: self.add_key_actions(menu, name, p)
                inside = getattr(self, "inside_labels", {}).get(spec["name"])
                if inside:  # the row's name inside the field; its right-click the label's menu, then the range
                    slider.setLabel(inside[0])
                    slider.setToolTip(inside[1])
                    slider.setProperty("glParam", spec["name"])  # S over it keys this param
                    slider.menu_hook = lambda menu, p=spec["name"]: self.fill_param_menu(menu, name, p)
                    self.inside_fields[spec["name"]] = slider
            w = slider
            unit_text = units.param_label(kind_of_unit, self.editor.model.node_type(name), spec["name"])
            if unit_text:  # the unit after the value: cm, %, °, turns, s, px, cm/s²...
                if kind_of_unit == "fraction":
                    unit_tip = "A percentage: 100% is 1 (the node stores 0 to 1)"
                elif kind_of_unit not in units.FIXED:
                    unit_tip = "Units: View > Units in the graph editor"
                else:
                    unit_tip = ""
                slider.setSuffix(unit_text, unit_tip)
        elif kind == "vec3" and (spec.get("vectorExpression") or isinstance(value, dict)):
            w = self.vector_expression_editor(spec, value, set_value)
            if name == self.node and getattr(w, "boxes", None):
                self.key_widgets[spec["name"]] = w
        elif kind == "vec3":
            w = QtWidgets.QWidget()
            row = QtWidgets.QHBoxLayout(w)
            row.setContentsMargins(0, 0, 0, 0)
            boxes = []
            for c in value:
                b = widgets.CompactSpinBox()  # the spec's decimals, trailing zeros dropped
                b.setRange(-1e9, 1e9)
                b.setDecimals(6)
                b.display_decimals = spec.get("decimals", 3)
                b.setValue(float(c))
                row.addWidget(b)
                boxes.append(b)
            w.boxes = boxes
            w.swatch = None
            if spec.get("unit") == "color":
                # A color: its swatch as Maya shows it, a click to pick (widgets.ColorSwatch).
                swatch = widgets.ColorSwatch([float(c) for c in value])

                def picked(rgb, boxes=boxes):
                    for b, c in zip(boxes, rgb):
                        b.blockSignals(True)
                        b.setValue(float(c))
                        b.blockSignals(False)
                    set_value([float(c) for c in rgb])

                swatch.picked.connect(picked)
                row.insertWidget(0, swatch)
                w.swatch = swatch

            def typed(boxes=boxes, w=w):
                rgb = [x.value() for x in boxes]
                if w.swatch is not None:
                    w.swatch.set_color(rgb)
                set_value(rgb)

            for b in boxes:
                b.editingFinished.connect(typed)
            if spec.get("worldDirection") and spec.get("unit") == "length" and on_change is None:
                # A world position (Transform's pivot): Pick takes the selection's.
                picker = QtWidgets.QToolButton()
                picker.setObjectName("positionPick")
                picker.setText("Pick")
                picker.setToolTip("Set it to where the selected vertices, locator or transform are "
                                  "(select them in the viewport or the Outliner first)")
                picker.clicked.connect(lambda _=False, p=spec["name"]: self.pick_position(name, p))
                row.addWidget(picker)
                w.picker = picker
            if name == self.node:
                self.key_widgets[spec["name"]] = w
        elif kind == "ramp":
            keys = value if isinstance(value, (list, dict)) else [[0.0, float(value)], [1.0, float(value)]]
            w = widgets.RampEditor(keys)
            w.dragged.connect(preview)
            w.committed.connect(set_value)
        elif spec.get("choices"):  # a fixed set of values
            w = QtWidgets.QComboBox()
            shown = spec.get("choiceLabels") or spec["choices"]  # what each reads (else the value)
            for choice, text in zip(spec["choices"], shown):
                w.addItem(text, choice)
            if str(value) not in spec["choices"]:
                w.addItem(str(value), str(value))
            w.setCurrentIndex(max(w.findData(str(value)), 0))
            w.activated.connect(lambda i, w=w: set_value(w.itemData(i)))
        elif spec.get("channel") in ("writes", "reads") and on_change is None:
            w = self.attribute_editor(name, spec, value, set_value)
        elif spec["name"] in ("group", "exclude") and self.editor.model.node_type(name) == "Guides":
            w = self.group_editor(spec["name"], value, set_value)
        elif self.suggestions(name, spec["name"]) is not None:  # free text, with what exists offered
            options, placeholder = self.suggestions(name, spec["name"])
            w = suggest_box(value, options, set_value, placeholder)
            if spec["name"] == "prototype" and self.editor.model.node_type(name) in ("Instance", "Scales"):
                w = self.objects_field(w, set_value)
        elif file_filter(self.editor.model.node_type(name), spec["name"]) is not None:
            w = self.path_field(value, file_filter(self.editor.model.node_type(name), spec["name"]), set_value)
        else:  # string
            w = QtWidgets.QLineEdit(str(value))
            w.editingFinished.connect(lambda w=w: set_value(w.text()))
        return w

    def attribute_editor(self, node, spec, value, set_value):
        """A param naming an attribute (a strand channel). Writes: a plain field, and a warning when
        the name is one a node upstream writes (this node overwrites it). Reads: the attributes
        reaching the node to pick from, each with the node that wrote it last (any name can be
        typed), and a note when the one named does not reach it."""
        editor = self.editor
        writes = spec.get("channel") == "writes"
        info = editor.channels().get(node, {})
        holder = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(holder)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(theme.px(2))
        row = QtWidgets.QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(6))
        chip = QtWidgets.QLabel("Writes" if writes else "Reads")
        chip.setObjectName("attributeUse")
        color = QtGui.QColor(_ge.WRITES if writes else _ge.READS)
        chip.setStyleSheet("QLabel { background: %s; color: %s; border-radius: %dpx; padding: %dpx %dpx; font-weight: bold; }"
                           % (color.name(), readable_on(color).name(), theme.px(6), theme.px(1), theme.px(6)))
        chip.setToolTip(ATTRIBUTE_TIPS["writes" if writes else "reads"])
        row.addWidget(chip)

        def commit(v):
            set_value(v)
            later(lambda: self.node == node and self.show_node(node))  # the warning or note as it is now

        if writes:
            field = QtWidgets.QLineEdit(str(value))
            field.setObjectName("attributeName")
            field.setPlaceholderText("none (no attribute)")
            field.editingFinished.connect(lambda: field.text().strip() != str(value) and commit(field.text().strip()))
            row.addWidget(field, 1)
            holder.field = field
        else:
            sources = editor.channel_sources(node)
            box = QtWidgets.QComboBox()
            box.setObjectName("attributeName")
            box.setEditable(True)
            box.setInsertPolicy(QtWidgets.QComboBox.NoInsert)
            box.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
            box.setMinimumContentsLength(12)
            box.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            for source in sources:
                label = "%s (from %s)" % (source["name"], source["node"]) if source.get("node") else source["name"]
                box.addItem(label, source["name"])
            box.lineEdit().setPlaceholderText("none")
            box.setCurrentIndex(-1)
            box.setEditText(str(value))
            names = {box.itemText(i): box.itemData(i) for i in range(box.count())}

            def picked(i):
                name = box.itemData(i)
                box.setEditText(name)
                if name != str(value):
                    commit(name)

            def typed():
                text = box.currentText().strip()
                text = names.get(text, text)
                if text != str(value):
                    commit(text)
            box.activated.connect(picked)
            box.lineEdit().editingFinished.connect(typed)
            box.setToolTip("Attributes reaching this node, each with the node that writes it last; type any other name")
            row.addWidget(box, 1)
            holder.field = box
        column.addLayout(row)

        line = None
        if writes:
            over = [o for o in info.get("overwrites", []) if o["param"] == spec["name"]]
            if over:
                line = QtWidgets.QLabel(u"\u26a0 overwrites " + ", ".join("%s from %s" % (o["name"], o["node"]) for o in over))
                line.setObjectName("attributeOverwrite")
                line.setStyleSheet("color: %s;" % _ge.WARNING.name())
                line.setToolTip("A node upstream already writes this attribute: from here on the strands carry this "
                                "node's, so nodes after it read this one. Give each node its own name (chained "
                                "Clumps: lockId, then clumpId) unless replacing it is what you want.")
        else:
            read = next((r for r in info.get("reads", []) if r["param"] == spec["name"]), None)
            if read is not None and not read["reaches"] and not read.get("quiet"):  # (the core's: its user at its default too)
                why = ("none applies" if read.get("optional")
                       else "this node ignores it")
                line = QtWidgets.QLabel(u"\u24d8 no node upstream writes %s: %s" % (read["name"], why))
                line.setObjectName("attributeMissing")
                line.setStyleSheet("color: %s;" % _ge.NOTE.name())
                line.setToolTip("Attributes reaching this node: %s" % (", ".join(c["name"] for c in info.get("incoming", []))
                                                                        or "none"))
        if line is not None:
            line.setWordWrap(True)
            column.addWidget(line)
        holder.note = line
        return holder

    def path_field(self, value, file_filter, set_value):
        """A file param (Reference's groom file, ImportCurves' Alembic file): its path, typed or
        browsed for (stored as the scene stores paths: groomlab.portable_path)."""
        holder = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(holder)
        row.setContentsMargins(0, 0, 0, 0)
        line = QtWidgets.QLineEdit(str(value))
        line.setObjectName("pathField")
        line.editingFinished.connect(lambda: set_value(line.text()))
        browse = QtWidgets.QPushButton("...")
        browse.setFixedWidth(theme.px(28))
        browse.setToolTip("Browse for the file")

        def pick():
            import groomlab
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self.window(), "File", groomlab.resolve_path(line.text())
                                                            if line.text() else "", file_filter)
            if path:
                stored = groomlab.portable_path(path)
                line.setText(stored)
                set_value(stored)
        browse.clicked.connect(lambda _=False: pick())
        row.addWidget(line)
        row.addWidget(browse)
        holder.line = line
        return holder

    def suggestions(self, node, param):
        """([names], placeholder) to offer in a text param naming something that exists, else None."""
        model = self.editor.model
        op_type = model.node_type(node)
        import groomlab
        from maya import cmds
        groom = self.editor.groom
        channel = (model.param_spec(node, param) or {}).get("channel")
        if channel == "reads":  # the attributes reaching this node
            return [c["name"] for c in self.editor.channel_sources(node)], "none"
        if channel == "writes":  # a name of its own: nothing to pick
            return [], "none (no attribute)"
        if op_type == "Part" and param == "part":
            try:
                return (groomlab.list_parts(groom) if groom else []), "every part"
            except RuntimeError:
                return [], "every part"
        if (op_type == "Map" and param == "name") or (op_type == "HairColor" and param == "map") or \
                (op_type == "Interpolate" and param == "regionMap"):
            if op_type in ("HairColor", "Interpolate"):
                try:
                    # Color maps first: they paint hair color and regions (a gray map reads as gray).
                    entries = groomlab.maps(groom) if groom else []
                except RuntimeError:
                    entries = []
                if op_type == "HairColor":  # (guide layers lie on the guides, not at the scalp's UVs)
                    entries = [m for m in entries if m.get("kind") != "guides"]
                # Regions: guide maps first (each value a region, each strand its nearest guide's).
                guide_maps = [m["name"] for m in entries if m.get("kind") == "guides"] if op_type == "Interpolate" else []
                names = guide_maps + [m["name"] for m in entries if m.get("channels") == 4 and m["name"] not in guide_maps] + \
                    [m["name"] for m in entries if m.get("channels") != 4 and m["name"] not in guide_maps]
                # Then the scalp's own color sets (an image file's path can be typed in).
                names += [s for s in self.editor.color_sets() if s not in names]
                return names, "none (a color map from Groom Tools > Paint, a color set, or an image file)"
            try:
                names = groomlab.list_maps(groom) if groom else []
            except RuntimeError:
                names = []
            return names, "a map (Groom Tools > Paint > New...)"
        if op_type in ("Instance", "Scales") and param == "prototype":
            meshes = cmds.ls(type="mesh", noIntermediate=True, long=False) or []
            return sorted({(cmds.listRelatives(m, parent=True) or [m])[0] for m in meshes})[:200], "a mesh"
        if op_type == "Control" and param == "name":
            names = []
            if groom:
                names = [a[4:] for a in (cmds.listAttr(groom, userDefined=True) or []) if a.startswith("ctl_")]
            return names, "the slider's name"
        return None

    def objects_field(self, box, set_value):
        """An Instance node's Object(s) field (`box`: one mesh picked from its dropdown, or names
        typed) with a button filling it with the meshes selected in the scene, separated by commas."""
        import groomlab
        w = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(w)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(theme.px(4))
        row.addWidget(box, 1)
        pick = QtWidgets.QToolButton()
        pick.setObjectName("objectsFromSelection")
        pick.setProperty("role", "bordered")  # a light border: plainly a button beside the field
        pick.setText("Selected")
        pick.setToolTip("Fill it with the meshes selected in the scene, separated by commas (several: variants, "
                        "one per instance)")

        def from_selection():
            picked = groomlab.selected_meshes()  # (meshes only: not the groom selected with them)
            if not picked:
                self.editor.report("Select the mesh(es) to instance first")
                return
            names = groomlab.instance_object_names(picked)
            box.setCurrentText(names)
            set_value(names)
        pick.clicked.connect(lambda _=False: later(from_selection))
        row.addWidget(pick)
        w.box, w.pick = box, pick
        return w

    def group_editor(self, param, value, set_value):
        """The groom's guide groups to pick from, one or several: which to read (group) or leave out (exclude)."""
        import groomlab
        groups = groomlab.list_guide_groups(self.editor.groom) if self.editor.groom else []
        tip = ("Guide groups of this groom: %s.\nSee them (counts, levels, where they come from) under Guide "
               "groups & edits in the Groom Tools." % (", ".join(g["name"] for g in groups) or "none yet"))
        details = {g["name"]: "%d guides, level %d" % (g["count"], g["depth"]) for g in groups}
        placeholder = "every group at the node's level" if param == "group" else "none"
        w = names_editor(value, [g["name"] for g in groups], set_value, placeholder,
                         "Pick groups to read" if param == "group" else "Pick groups to leave out", details)
        panel = QtWidgets.QToolButton()
        panel.setText("...")
        panel.setToolTip("Open the guide groups in the Groom Tools")
        panel.clicked.connect(self.open_groups)
        w.layout().addWidget(panel)
        w.setToolTip(tip)
        return w

    def open_groups(self):
        from groomlab.ui import palette
        palette.show(tab="guides")


class MappableEditor(QtWidgets.QWidget):
    """A value, or a texture / color set / attribute / expression / painted map (PG-2)."""

    SOURCES = ["value", "texture", "colorSet", "attribute", "expression", "guide"]
    LABELS = {"value": "value", "texture": "texture", "colorSet": "color set (scalp paint)",
              "attribute": "strand attribute", "expression": "expression", "guide": "painted map"}

    def __init__(self, panel, node, spec, value):
        super(MappableEditor, self).__init__()
        self.panel, self.node, self.spec = panel, node, spec
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(4))
        self.body = QtWidgets.QWidget()
        layout.addWidget(self.body, 1)
        # Where the value comes from: a plain value most of the time, so the choice is a small
        # menu beside it rather than a dropdown row over every mappable param.
        self.source = QtWidgets.QToolButton()
        self.source.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        self.source.setToolTip("Drive this param with a map: painted map (Groom Tools > Paint; works anywhere, "
                               "including Scatter density), texture, color set, strand attribute (a channel "
                               "written upstream) or expression")
        self.source.setProperty("role", "glyph")
        menu = QtWidgets.QMenu(self.source)
        group = qt.QActionGroup(menu)
        self.source_actions = {}
        for key in self.SOURCES:
            action = menu.addAction(self.LABELS[key].capitalize())
            action.setCheckable(True)
            group.addAction(action)
            action.triggered.connect(lambda _=False, k=key: self.switch(k))
            self.source_actions[key] = action
        self.source.setMenu(menu)
        layout.addWidget(self.source, 0, QtCore.Qt.AlignTop)
        self.value = value
        # The number to come back to when switching off a map: the value now, else the map's
        # white value (what a map at 1 gave), else the default.
        if isinstance(value, dict):
            self.last_plain = value.get("max", self.spec["default"])
        else:
            self.last_plain = value
        current = "value"
        if isinstance(value, dict):
            current = gm.map_source(value) or "value"
        self.build(current)

    def switch(self, source):
        if source == "value":
            self.value = self.last_plain
        else:
            plain = self.value if not isinstance(self.value, dict) else self.last_plain
            top = float(plain) if isinstance(plain, (int, float)) and not isinstance(plain, bool) else 1.0
            self.value = {source: "", "component": "r", "min": 0.0, "max": top or 1.0}
        self.build(source)
        # The model takes the change at once, even a map without a name yet: validate() flags
        # it (the status bar says so, and nothing is applied) until a name is given.
        self.commit()

    def build(self, source):
        self.current = source
        for key, action in self.source_actions.items():
            action.setChecked(key == source)
        self.source.setText("Map" if source == "value" else "\u25c9 " + self.LABELS[source].split(" (")[0])
        old = self.body
        self.body = QtWidgets.QWidget()
        self.layout().replaceWidget(old, self.body)
        old.hide()  # gone once the event loop gets to it; until then it must not cover the row
        old.deleteLater()
        getattr(self.panel, "inside_fields", {}).pop(self.spec["name"], None)  # a value's field makes its own
        self._fill(source)
        self.panel.update_row_label(self.spec["name"])  # a map's fields need the row's label; a value's holds it
        self.panel.editor.watch_text_edits(self.body)  # Ctrl+Z in a new text field: its typing first

    def _fill(self, source):
        if source == "value":
            row = QtWidgets.QHBoxLayout(self.body)
            row.setContentsMargins(0, 0, 0, 0)
            plain = self.panel.plain_editor(self.node, self.spec, self.value if not isinstance(self.value, dict)
                                            else self.spec["default"], on_change=self.set_plain,
                                            on_preview=self.preview_plain)
            row.addWidget(plain)
            return
        form = QtWidgets.QFormLayout(self.body)
        form.setContentsMargins(0, 0, 0, 0)
        if source in ("guide", "attribute"):
            # The groom's painted layers to pick from; any other name can be typed.
            name = QtWidgets.QComboBox()
            name.setEditable(True)
            name.addItems(self.layers(source))
            name.setCurrentText(self.value.get(source, ""))
            name.lineEdit().setPlaceholderText("painted map" if source == "guide" else "strand channel or painted map")
            name.lineEdit().editingFinished.connect(lambda: self.set_field(source, name.currentText()))
            name.activated.connect(lambda _: self.set_field(source, name.currentText()))
            form.addRow("map" if source == "guide" else "name", name)
            self.add_range(form, source)
            return
        if source == "colorSet":  # the scalp's color sets to pick from
            box = suggest_box(self.value.get(source, ""), self.panel.editor.color_sets(),
                              lambda v: self.set_field(source, v), "color set on the scalp")
            form.addRow("name", box)
            component = QtWidgets.QComboBox()
            component.addItems(["r", "g", "b", "a", "luminance"])
            component.setCurrentText(self.value.get("component", "r"))
            component.currentTextChanged.connect(lambda c: self.set_field("component", c))
            form.addRow("component", component)
            self.add_range(form, source)
            return
        if source == "expression":
            from groomlab.ui import expression_editor
            code = expression_editor.CodeEdit(self.value.get(source, ""), lines=3,
                                              validator=lambda t: __import__("groomlab.expressions").expressions.check(t))
            code.setToolTip("Type $ for variables, or Ctrl+Space")
            code.committed.connect(lambda text: self.set_field(source, text))
            form.addRow(code)
            self.add_range(form, source)
            return
        name = QtWidgets.QLineEdit(self.value.get(source, ""))
        name.setPlaceholderText({"texture": "image file", "colorSet": "color set on the scalp",
                                 "attribute": "strand channel", "guide": "painted map on the guides",
                                 "expression": "e.g. 0.5 + 0.5 * sin(u * 6.28)"}[source])
        name.editingFinished.connect(lambda: self.set_field(source, name.text()))
        if source == "texture":
            holder = QtWidgets.QWidget()
            row = QtWidgets.QHBoxLayout(holder)
            row.setContentsMargins(0, 0, 0, 0)
            browse = QtWidgets.QPushButton("...")
            browse.setFixedWidth(theme.px(28))
            browse.clicked.connect(lambda: self.browse(name))
            row.addWidget(name)
            row.addWidget(browse)
            form.addRow("file", holder)
        else:
            form.addRow({"expression": "expression", "guide": "map"}.get(source, "name"), name)
        if source in ("texture", "colorSet"):
            component = QtWidgets.QComboBox()
            component.addItems(["r", "g", "b", "a", "luminance"])
            component.setCurrentText(self.value.get("component", "r"))
            component.currentTextChanged.connect(lambda c: self.set_field("component", c))
            form.addRow("component", component)
        self.add_range(form, source)

    def add_range(self, form, source):
        """min and max: what the map's 0 (black) and 1 (white) become (in the param's display units)."""
        from groomlab import units
        kind = self.spec.get("unit") or ""
        f = units.factor(kind)
        suffix = (" " + units.label(kind)) if units.label(kind) else ""
        for key in ("min", "max"):
            box = widgets.CompactSpinBox()
            box.setRange(-1e9, 1e9)
            box.setDecimals(6)
            box.setValue(float(self.value.get(key, 0.0 if key == "min" else 1.0)) * f)
            box.setToolTip("the value where the map is %s" % ("0 (black)" if key == "min" else "1 (white)"))
            box.editingFinished.connect(lambda k=key, b=box: self.set_field(k, b.value() / f))
            form.addRow("%s (%s)%s" % (key, "black" if key == "min" else "white", suffix), box)

    def layers(self, source="guide"):
        """Names to offer: painted maps, and for a strand attribute the channels reaching this node too."""
        editor = self.panel.editor
        names = editor.channels_into(self.node) if source == "attribute" else []
        if editor.groom:
            try:
                import groomlab
                names += [m for m in groomlab.list_maps(editor.groom) if m not in names]
            except Exception:
                pass
        return names

    def browse(self, line_edit):
        import groomlab
        start = groomlab.resolve_path(line_edit.text()) if line_edit.text() else ""
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self.window(), "Map image", start, "Images (*.png *.jpg *.jpeg *.tga *.bmp *.psd *.hdr *.pgm *.ppm)")
        if path:
            # Stored as the scene stores paths (relative to its folder or project): the scene moves
            # to another machine with its files.
            path = groomlab.portable_path(path)
            line_edit.setText(path)
            self.set_field("texture", path)

    def set_plain(self, v):
        self.value = v
        self.last_plain = v
        self.commit()

    def preview_plain(self, v):
        self.panel.editor.preview_param(self.node, self.spec["name"], v)

    def set_field(self, key, v):
        self.value[key] = v
        self.commit()

    def commit(self):
        self.panel.editor.set_param(self.node, self.spec["name"], self.value)


# ---------------------------------------------------------------------------
# Editor
