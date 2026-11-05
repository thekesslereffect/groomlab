# SPDX-License-Identifier: Apache-2.0
"""Export Card Maps (Cards node > Export Maps..., GroomLab > Export > Card Maps...): the groom's card
maps written as a preset packs them, in one click. A plain window over Maya, never modal.

From the top: the preset (Unreal Hair Cards, Unreal (packed), Unity HDRP Hair, Separate maps (all),
the user's own, or Custom once the packing is edited), the resolution, format, bit depth, padding
and supersampling, the output folder and name, and the Export button; under it the maps written
as thumbnails. Advanced folds out the packing table: a row per output file (its name, R G B A each a
map's channel or 0 / 1 with an invert box, its color space), add and remove rows, the normal map's
green convention and the file naming, and Save as Preset (a name typed beside it: no dialog) and
Delete. The work is groomlab.bake_card_maps'.

    from groomlab.ui import card_maps_window
    card_maps_window.show(groom)
"""

import os

from groomlab.ui.qt import QtCore, QtGui, QtWidgets

import groomlab_core

from groomlab.ui import theme, widgets

_window = None

CUSTOM = "Custom"
NONE = "—"  # a channel the file leaves out
RESOLUTIONS = ("Atlas size", "512", "1024", "2048", "4096", "8192")
FORMATS = (("TGA", "tga"), ("PNG", "png"), ("TIFF", "tiff"), ("EXR", "exr"))
SUPERSAMPLING = (("1x (off)", 1), ("2x", 2), ("4x", 4))
COLOR_SPACES = (("Auto", "auto"), ("sRGB", "srgb"), ("Linear", "linear"))


def _groomlab():
    import groomlab
    return groomlab


class ChannelCell(QtWidgets.QWidget):
    """One channel of an output file: a map's channel (or 0, 1, nothing) and whether it is inverted."""

    changed = QtCore.Signal()

    def __init__(self, names, optional, parent=None):
        super(ChannelCell, self).__init__(parent)
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(2, 0, 2, 0)
        layout.setSpacing(2)
        self.combo = QtWidgets.QComboBox()
        if optional:
            self.combo.addItem(NONE)
        for name in names:
            self.combo.addItem(name)
        self.combo.addItem("iridescence?")  # only when the groom has it
        self.combo.setToolTip("The map channel this file channel holds (0 or 1: a constant; "
                              "iridescence?: only when the groom has any, else left out)")
        self.invert = QtWidgets.QCheckBox("inv")
        self.invert.setToolTip("Inverted: 1 - the value")
        layout.addWidget(self.combo, 1)
        layout.addWidget(self.invert)
        self.combo.currentIndexChanged.connect(lambda *_: self.changed.emit())
        self.invert.toggled.connect(lambda *_: self.changed.emit())

    def value(self):
        """"tangent.x", "1-ao", "0"..., or "" for none."""
        text = self.combo.currentText()
        if text == NONE:
            return ""
        return ("1-" if self.invert.isChecked() and text not in ("0", "1") else "") + text

    def set_value(self, text):
        text = text or ""
        invert = text.startswith("1-")
        name = text[2:] if invert else text
        index = self.combo.findText(name or NONE)
        if index < 0 and name:  # a constant or an alias the list doesn't name: add it
            self.combo.addItem(name)
            index = self.combo.findText(name)
        self.combo.setCurrentIndex(max(index, 0))
        self.invert.setChecked(invert)


class PackingTable(QtWidgets.QTableWidget):
    """The preset's files: a row each, its name, R G B A and color space."""

    changed = QtCore.Signal()
    COLUMNS = ("File", "R", "G", "B", "A", "Color")

    def __init__(self, names, parent=None):
        super(PackingTable, self).__init__(0, len(self.COLUMNS), parent)
        self.names = names
        self.setObjectName("cardMapsPacking")
        self.setHorizontalHeaderLabels(self.COLUMNS)
        self.verticalHeader().setVisible(False)
        self.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
        self.horizontalHeader().setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeToContents)
        self.horizontalHeader().setSectionResizeMode(5, QtWidgets.QHeaderView.ResizeToContents)
        self.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectRows)
        self.setMinimumHeight(theme.px(160))
        self._quiet = False

    def add_row(self, file=None):
        file = file or {"map": "Map%d" % (self.rowCount() + 1), "channels": ["depth"], "colorSpace": "auto"}
        row = self.rowCount()
        self.insertRow(row)
        name = QtWidgets.QLineEdit(file.get("map", ""))
        name.setToolTip("The file's {map} in the name template (hair_<this>.tga)")
        name.textEdited.connect(lambda *_: self._changed())
        self.setCellWidget(row, 0, name)
        channels = list(file.get("channels", []))
        for c in range(4):
            cell = ChannelCell(self.names, optional=c > 0)
            cell.set_value(channels[c] if c < len(channels) else "")
            cell.changed.connect(self._changed)
            self.setCellWidget(row, 1 + c, cell)
        space = QtWidgets.QComboBox()
        for label, value in COLOR_SPACES:
            space.addItem(label, value)
        space.setCurrentIndex(max(0, [v for _, v in COLOR_SPACES].index(file.get("colorSpace", "auto"))
                                  if file.get("colorSpace", "auto") in [v for _, v in COLOR_SPACES] else 0))
        space.setToolTip("How color channels are written: Auto (sRGB, linear in EXR), sRGB or Linear; data "
                         "channels are always linear")
        space.currentIndexChanged.connect(lambda *_: self._changed())
        self.setCellWidget(row, 5, space)
        self.resizeRowToContents(row)
        self._changed()

    def remove_rows(self):
        rows = sorted({i.row() for i in self.selectedIndexes()}, reverse=True) or ([self.rowCount() - 1] if self.rowCount() else [])
        for row in rows:
            self.removeRow(row)
        self._changed()

    def set_files(self, files):
        self._quiet = True
        try:
            self.setRowCount(0)
            for f in files:
                self.add_row(f)
        finally:
            self._quiet = False

    def files(self):
        out = []
        for row in range(self.rowCount()):
            channels = [self.cellWidget(row, 1 + c).value() for c in range(4)]
            while channels and not channels[-1]:
                channels.pop()
            channels = [c or "0" for c in channels]  # a gap in the middle: 0
            out.append({"map": self.cellWidget(row, 0).text().strip(), "channels": channels,
                        "colorSpace": self.cellWidget(row, 5).currentData()})
        return out

    def _changed(self):
        if not self._quiet:
            self.changed.emit()


class CardMapsPanel(QtWidgets.QWidget):
    """The Export Card Maps panel (see the module)."""

    def __init__(self, groom=None, parent=None):
        super(CardMapsPanel, self).__init__(parent)
        self.setObjectName("groomLabCardMaps")
        gl = _groomlab()
        self.groom = groom
        self.sources = gl.card_map_sources()
        self.formats = {f["name"]: f["bitDepths"] for f in self.sources["formats"]}
        self.presets = []
        self._filling = False
        outer = QtWidgets.QVBoxLayout(self)
        pad = theme.px(14)
        outer.setContentsMargins(pad, pad, pad, pad)
        outer.setSpacing(theme.px(8))

        title = QtWidgets.QLabel("Export Card Maps")
        title.setProperty("role", "title")
        outer.addWidget(title)
        self.groom_label = QtWidgets.QLabel()
        self.groom_label.setProperty("role", "about")
        outer.addWidget(self.groom_label)

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        preset_row = QtWidgets.QHBoxLayout()
        self.preset = QtWidgets.QComboBox()
        self.preset.setObjectName("cardMapsPreset")
        self.preset.setToolTip("What goes in each file: an engine's layout, every map on its own, or your own. AO World "
                               "uses the mesh's unique aoWorld UV set")
        self.preset.currentIndexChanged.connect(self._preset_picked)
        preset_row.addWidget(self.preset, 1)
        self.delete_button = QtWidgets.QPushButton("Delete")
        self.delete_button.setObjectName("cardMapsDelete")
        self.delete_button.setToolTip("Delete this preset of yours (built-in ones stay)")
        self.delete_button.clicked.connect(self.delete_preset)
        preset_row.addWidget(self.delete_button)
        form.addRow("Preset", preset_row)
        self.description = QtWidgets.QLabel()
        self.description.setWordWrap(True)
        self.description.setProperty("role", "about")
        form.addRow("", self.description)

        self.resolution = QtWidgets.QComboBox()
        self.resolution.setObjectName("cardMapsResolution")
        self.resolution.setEditable(True)
        self.resolution.addItems(RESOLUTIONS)
        self.resolution.setCurrentText("2048")
        self.resolution.setToolTip("Texels square (the atlas is square), any up to 8192; Atlas size: the Cards node's")
        form.addRow("Resolution", self.resolution)
        self.format = QtWidgets.QComboBox()
        self.format.setObjectName("cardMapsFormat")
        for label, value in FORMATS:
            self.format.addItem(label, value)
        self.format.currentIndexChanged.connect(self._format_changed)
        form.addRow("Format", self.format)
        self.bit_depth = QtWidgets.QComboBox()
        self.bit_depth.setObjectName("cardMapsBitDepth")
        self.bit_depth.setToolTip("Bits a channel: 8 or 16 (PNG, TIFF), 16 (half) or 32 (float) for EXR")
        form.addRow("Bit depth", self.bit_depth)
        self.padding = QtWidgets.QSpinBox()
        self.padding.setObjectName("cardMapsPadding")
        self.padding.setRange(-1, 256)
        self.padding.setSpecialValueText("Full")
        self.padding.setValue(-1)
        self.padding.setSuffix(" px")
        self.padding.setToolTip("Texels the colors spread past the strands so mip maps don't bleed in the empty "
                                "texels; Full: all the way, each card's colors meeting its neighbours' halfway")
        form.addRow("Padding", self.padding)
        self.supersample = QtWidgets.QComboBox()
        self.supersample.setObjectName("cardMapsSupersample")
        for label, value in SUPERSAMPLING:
            self.supersample.addItem(label, value)
        self.supersample.setToolTip("Anti-aliasing: baked 2 or 4 times bigger, then brought down (slower; the bake "
                                    "never bigger than 8192)")
        form.addRow("Supersampling", self.supersample)
        folder_row = QtWidgets.QHBoxLayout()
        self.folder = QtWidgets.QLineEdit()
        self.folder.setObjectName("cardMapsFolder")
        folder_row.addWidget(self.folder, 1)
        browse = QtWidgets.QPushButton("...")
        browse.setToolTip("Pick the folder")
        browse.clicked.connect(self.browse)
        folder_row.addWidget(browse)
        form.addRow("Folder", folder_row)
        self.name = QtWidgets.QLineEdit()
        self.name.setObjectName("cardMapsName")
        self.name.setToolTip("The files' {name}: hair_Depth.tga...")
        form.addRow("Name", self.name)
        outer.addLayout(form)

        self.export_button = QtWidgets.QPushButton("Export")
        self.export_button.setObjectName("cardMapsExport")
        self.export_button.setProperty("role", "primary")
        self.export_button.setMinimumHeight(theme.px(36))
        self.export_button.setToolTip("Bake the cards and write every file of the preset")
        self.export_button.clicked.connect(self.export)
        outer.addWidget(self.export_button)
        self.status = widgets.StatusLabel()
        self.status.setObjectName("cardMapsStatus")
        outer.addWidget(self.status)
        self.preview = QtWidgets.QWidget()
        self.preview_layout = QtWidgets.QHBoxLayout(self.preview)
        self.preview_layout.setContentsMargins(0, 0, 0, 0)
        self.preview_layout.setSpacing(theme.px(4))
        self.preview.setVisible(False)
        outer.addWidget(self.preview)

        # Advanced: the packing.
        advanced = QtWidgets.QWidget()
        adv = QtWidgets.QVBoxLayout(advanced)
        adv.setContentsMargins(0, 0, 0, 0)
        settings = QtWidgets.QFormLayout()
        self.normal = QtWidgets.QComboBox()
        self.normal.setObjectName("cardMapsNormal")
        self.normal.addItem("OpenGL (Y up: Unity, Blender, Maya)", "opengl")
        self.normal.addItem("DirectX (Y down: Unreal, 3ds Max)", "directx")
        self.normal.currentIndexChanged.connect(lambda *_: self._edited())
        settings.addRow("Normal map", self.normal)
        self.template = QtWidgets.QLineEdit("{name}_{map}")
        self.template.setObjectName("cardMapsTemplate")
        self.template.setToolTip("File names: {name}, {map} (the row's name) and {res}; the extension is added")
        settings.addRow("File names", self.template)
        adv.addLayout(settings)
        self.table = PackingTable(self.sources["channels"])
        self.table.changed.connect(self._edited)
        adv.addWidget(self.table)
        rows = QtWidgets.QHBoxLayout()
        add = QtWidgets.QPushButton("Add File")
        add.setObjectName("cardMapsAddRow")
        add.clicked.connect(lambda: self.table.add_row())
        rows.addWidget(add)
        remove = QtWidgets.QPushButton("Remove File")
        remove.setObjectName("cardMapsRemoveRow")
        remove.clicked.connect(self.table.remove_rows)
        rows.addWidget(remove)
        rows.addStretch(1)
        self.save_name = QtWidgets.QLineEdit()
        self.save_name.setObjectName("cardMapsSaveName")
        self.save_name.setPlaceholderText("Preset name")
        rows.addWidget(self.save_name)
        self.save_button = QtWidgets.QPushButton("Save as Preset")
        self.save_button.setObjectName("cardMapsSave")
        self.save_button.setToolTip("Save this packing as a preset of yours (Documents/GroomLab/card_presets: every "
                                    "host lists it)")
        self.save_button.clicked.connect(self.save_preset)
        rows.addWidget(self.save_button)
        adv.addLayout(rows)
        self.advanced = widgets.Section("Advanced", advanced, expanded=False)
        outer.addWidget(self.advanced)
        outer.addStretch(1)

        self.refresh_presets(gl.CARD_MAP_PRESET)
        self.set_groom(groom)

    # ---- State

    def set_groom(self, groom):
        self.groom = groom
        gl = _groomlab()
        self.groom_label.setText("Groom: %s" % groom if groom else "Select a groom, then open this again")
        if groom:
            if not self.folder.text():
                self.folder.setText(gl.asset_folder("exports"))
            self.name.setText(gl.asset_name(groom))
        self.export_button.setEnabled(bool(groom))

    def refresh_presets(self, select=None):
        select = select or self.preset.currentText()
        self.presets = _groomlab().card_map_presets()
        self._filling = True
        try:
            self.preset.clear()
            for p in self.presets:
                self.preset.addItem(p["name"])
                self.preset.setItemData(self.preset.count() - 1, p.get("description", ""), QtCore.Qt.ToolTipRole)
        finally:
            self._filling = False
        index = self.preset.findText(select)
        self.preset.setCurrentIndex(index if index >= 0 else 0)
        self._preset_picked()

    def _preset(self, name):
        return next((p for p in self.presets if p["name"] == name), None)

    def _preset_picked(self, *_):
        if self._filling:
            return
        p = self._preset(self.preset.currentText())
        self.delete_button.setEnabled(bool(p) and not p.get("builtin"))
        if not p:  # Custom: the table as it is
            self.description.setText("Your own packing (Advanced): save it as a preset to keep it")
            return
        self._filling = True
        try:
            self.description.setText(p.get("description") or "")
            self.table.set_files(p["files"])
            self.normal.setCurrentIndex(max(0, self.normal.findData(p.get("normal", "opengl"))))
            self.format.setCurrentIndex(max(0, self.format.findData(p.get("format", "tga"))))
            self._format_changed()
            index = self.bit_depth.findData(p.get("bitDepth", 8))
            if index >= 0:
                self.bit_depth.setCurrentIndex(index)
        finally:
            self._filling = False
        custom = self.preset.findText(CUSTOM)
        if custom >= 0 and self.preset.currentIndex() != custom:
            self.preset.removeItem(custom)

    def _edited(self):
        """The packing changed by hand: the preset is Custom now."""
        if self._filling:
            return
        if self.preset.currentText() != CUSTOM:
            self._filling = True
            try:
                if self.preset.findText(CUSTOM) < 0:
                    self.preset.addItem(CUSTOM)
                self.preset.setCurrentIndex(self.preset.findText(CUSTOM))
            finally:
                self._filling = False
            self._preset_picked()

    def _format_changed(self, *_):
        current = self.bit_depth.currentData()
        self.bit_depth.blockSignals(True)
        self.bit_depth.clear()
        depths = self.formats.get(self.format.currentData(), [8])
        for d in depths:
            label = {16: "16-bit half" if self.format.currentData() == "exr" else "16-bit",
                     32: "32-bit float"}.get(d, "%d-bit" % d)
            self.bit_depth.addItem(label, d)
        index = self.bit_depth.findData(current)
        self.bit_depth.setCurrentIndex(index if index >= 0 else 0)
        self.bit_depth.blockSignals(False)

    def current_preset(self):
        """The packing as a preset dict (what Export writes)."""
        return {"name": self.preset.currentText() or CUSTOM, "format": self.format.currentData(),
                "bitDepth": self.bit_depth.currentData(), "normal": self.normal.currentData(),
                "files": self.table.files()}

    def resolution_value(self):
        text = self.resolution.currentText().strip()
        if not text or not text[0].isdigit():
            return None  # Atlas size
        return max(16, min(8192, int("".join(ch for ch in text if ch.isdigit()) or 0)))

    # ---- Actions

    def browse(self):
        from maya import cmds
        picked = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the card maps",
                                  startingDirectory=self.folder.text() or _groomlab().asset_folder("exports"))
        if picked:
            self.folder.setText(picked[0])

    def export(self):
        """Writes the maps; returns the paths (None when it could not)."""
        gl = _groomlab()
        if not self.groom:
            self.status.setText("Select a groom first")
            return None
        preset = self.current_preset()
        check = groomlab_core.check_card_map_preset(preset)
        if check:
            self.status.setText("Can't export: %s" % check)
            return None
        try:
            with widgets.busy([self.export_button], self.status, "Baking the card maps..."):
                paths = gl.bake_card_maps(self.groom, preset=preset, resolution=self.resolution_value(),
                                          format=self.format.currentData(), bit_depth=self.bit_depth.currentData(),
                                          padding=self.padding.value(), supersample=self.supersample.currentData(),
                                          directory=self.folder.text().strip() or None,
                                          name=self.name.text().strip() or None,
                                          name_template=self.template.text().strip() or "{name}_{map}")
        except Exception as e:  # noqa: BLE001 (the panel says why, never a bare traceback)
            self.status.setText("Export failed: %s" % str(e).strip())
            return None
        folder = os.path.dirname(paths[0]) if paths else ""
        self.status.setText("Wrote %d maps into %s" % (len(paths), folder))
        self._show_preview(paths)
        return paths

    def _show_preview(self, paths):
        while self.preview_layout.count():
            item = self.preview_layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        size = theme.px(56)
        shown = 0
        for path in paths:
            image = QtGui.QImage(path)
            if image.isNull():
                continue  # a format Qt doesn't read (EXR)
            thumb = QtWidgets.QLabel()
            thumb.setPixmap(QtGui.QPixmap.fromImage(image.scaled(size, size, QtCore.Qt.KeepAspectRatio,
                                                                 QtCore.Qt.SmoothTransformation)))
            thumb.setToolTip(os.path.basename(path))
            self.preview_layout.addWidget(thumb)
            shown += 1
        self.preview_layout.addStretch(1)
        self.preview.setVisible(shown > 0)

    def save_preset(self):
        name = self.save_name.text().strip()
        if not name:
            self.status.setText("Type the preset's name beside Save as Preset")
            return None
        try:
            path = _groomlab().save_card_map_preset(name, self.current_preset())
        except (ValueError, RuntimeError) as e:
            self.status.setText("Can't save: %s" % str(e).strip())
            return None
        self.refresh_presets(name)
        self.status.setText("Saved the preset %s (%s)" % (name, path))
        return path

    def delete_preset(self):
        p = self._preset(self.preset.currentText())
        if not p or p.get("builtin"):
            return False
        done = _groomlab().delete_card_map_preset(p["name"])
        self.refresh_presets(_groomlab().CARD_MAP_PRESET)
        self.status.setText(("Deleted the preset %s" if done else "Couldn't delete %s") % p["name"])
        return done


def show(groom=None):
    """Open (or raise) the Export Card Maps window on `groom` (default: the active groom)."""
    global _window
    from maya.app.general.mayaMixin import MayaQWidgetBaseMixin

    class MayaCardMapsPanel(MayaQWidgetBaseMixin, CardMapsPanel):
        pass

    groom = groom or _groomlab().active_groom()
    if _window is not None and not widgets.alive(_window):
        _window = None
    if _window is None:
        _window = MayaCardMapsPanel(groom=groom)  # (Maya's mixin takes the first argument as the parent)
        _window.setWindowTitle("GroomLab Card Maps")
        _window.setWindowIcon(widgets.icon("logo"))
        _window.setWindowFlags(_window.windowFlags() | QtCore.Qt.Window)
        _window.resize(theme.px(620), theme.px(560))
        widgets.remember_geometry(_window, "cardMaps")
        widgets.at_exit(_close_at_exit)
    else:
        _window.set_groom(groom)
        _window.refresh_presets()
    _window.show()
    _window.raise_()
    return _window


def window():
    """The open Export Card Maps window, or None."""
    return _window if _window is not None and widgets.alive(_window) and _window.isVisible() else None


def _close_at_exit():
    global _window
    w, _window = _window, None
    if w is not None and widgets.alive(w):
        widgets.dispose(w)
