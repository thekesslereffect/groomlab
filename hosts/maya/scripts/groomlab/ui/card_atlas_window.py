# SPDX-License-Identifier: Apache-2.0
"""The Card Atlas window (GroomLab > Card Atlas...): hair card textures made block by block, as
FiberShop makes them, each block a groom of its own (groomlab.card_atlas). A plain window over
Maya, never modal.

Left, the atlas: its square with each block's rectangle (drag to move, the corner handle to
resize: placed by hand from then on; Auto Layout packs them all again) over the last preview bake
of the map picked above it (Bake Preview). Right: the atlas (pick one, New Atlas, its texture
size, padding, map preset and name), its blocks (Add, Duplicate, Remove; picking one selects its
groom, so the Graph Editor and the Groom Tools work on it), the block's card (width, length, the
root band's thickness) and the root band's Tilt, Turn and Roll (roots spread in depth: the texture
reads with depth), and the exports (the maps, the cards' shapes).

    from groomlab.ui import card_atlas_window
    card_atlas_window.show()
"""

import os

from groomlab.ui.qt import QtCore, QtGui, QtWidgets

from groomlab.ui import theme, widgets

_window = None

MAPS = ("Albedo", "Alpha", "Depth", "Normal", "AO", "Direction", "Root", "ID", "Seed", "Specular", "Translucency")
SIZES = (512, 1024, 2048, 4096, 8192)


def _atlas_api():
    from groomlab import card_atlas
    return card_atlas


def _short(node):
    return node.split("|")[-1] if node else ""


class BlockItem(QtWidgets.QGraphicsRectItem):
    """A block's rectangle on the atlas: drag it, or its bottom-right corner to resize it."""

    HANDLE = 10.0

    def __init__(self, canvas, groom, name, rect):
        super(BlockItem, self).__init__(rect)
        self.canvas, self.groom, self.name = canvas, groom, name
        self.setFlag(QtWidgets.QGraphicsItem.ItemClipsChildrenToShape, True)  # a long name cut at its edge
        self.setAcceptHoverEvents(True)
        self._drag = None
        t = theme.current()
        self.accent = QtGui.QColor(t.hex("accent"))
        self.setBrush(QtGui.QColor(self.accent.red(), self.accent.green(), self.accent.blue(), 28))
        self.label = QtWidgets.QGraphicsSimpleTextItem(name, self)
        self.label.setBrush(QtGui.QColor(t.token("text")))
        self.label.setFont(theme.font(theme.px(11), bold=True))
        self._place_label()
        self.set_picked(False)

    def set_picked(self, on):
        """The picked block's rectangle drawn bolder, filled a little more."""
        a = self.accent
        pen = QtGui.QPen(a, 2.5 if on else 1.2)
        pen.setCosmetic(True)
        self.setPen(pen)
        self.setBrush(QtGui.QColor(a.red(), a.green(), a.blue(), 56 if on else 24))

    def _place_label(self):
        r = self.rect()
        self.label.setPos(r.left() + 4, r.top() + 2)

    def _on_handle(self, pos):
        r = self.rect()
        return abs(pos.x() - r.right()) < self.HANDLE and abs(pos.y() - r.bottom()) < self.HANDLE

    def hoverMoveEvent(self, event):
        self.setCursor(QtCore.Qt.SizeFDiagCursor if self._on_handle(event.pos()) else QtCore.Qt.SizeAllCursor)
        super(BlockItem, self).hoverMoveEvent(event)

    def mousePressEvent(self, event):
        self.canvas.picked.emit(self.groom)
        self._drag = ("size" if self._on_handle(event.pos()) else "move", event.pos(), QtCore.QRectF(self.rect()))
        event.accept()

    def mouseMoveEvent(self, event):
        if not self._drag:
            return
        kind, start, rect = self._drag
        delta = event.pos() - start
        side = self.canvas.side
        if kind == "move":
            r = rect.translated(delta)
            r.moveLeft(min(max(r.left(), 0.0), side - r.width()))
            r.moveTop(min(max(r.top(), 0.0), side - r.height()))
        else:
            r = QtCore.QRectF(rect.topLeft(), QtCore.QSizeF(max(8.0, rect.width() + delta.x()), max(8.0, rect.height() + delta.y())))
            r.setRight(min(r.right(), side))
            r.setBottom(min(r.bottom(), side))
        self.setRect(r)
        self._place_label()

    def mouseReleaseEvent(self, event):
        if self._drag:
            self._drag = None
            self.canvas.moved.emit(self.groom, self.canvas.cell_of(self.rect()))
        event.accept()


class AtlasCanvas(QtWidgets.QGraphicsView):
    """The atlas square: the preview map under the blocks' rectangles."""

    picked = QtCore.Signal(str)
    moved = QtCore.Signal(str, tuple)

    def __init__(self, parent=None):
        super(AtlasCanvas, self).__init__(parent)
        self.setObjectName("cardAtlasCanvas")
        self.side = 512.0
        self.setScene(QtWidgets.QGraphicsScene(0, 0, self.side, self.side, self))
        self.setRenderHint(QtGui.QPainter.Antialiasing)
        self.setBackgroundBrush(QtGui.QColor(theme.current().token("sunken")))
        self.setFrameShape(QtWidgets.QFrame.NoFrame)
        self.setMinimumSize(theme.px(360), theme.px(360))
        self.image = None
        self.items = {}

    def resizeEvent(self, event):
        super(AtlasCanvas, self).resizeEvent(event)
        self.fitInView(self.scene().sceneRect(), QtCore.Qt.KeepAspectRatio)

    def drawBackground(self, painter, rect):
        super(AtlasCanvas, self).drawBackground(painter, rect)
        square = QtCore.QRectF(0, 0, self.side, self.side)
        painter.fillRect(square, QtGui.QColor("#000000"))
        if self.image is not None and not self.image.isNull():
            painter.drawImage(square, self.image)
        painter.setPen(QtGui.QPen(QtGui.QColor(theme.current().token("border")), 0))
        painter.drawRect(square)

    def cell_of(self, rect):
        """A rectangle of the canvas as an atlas cell (u0, v0 at the root: the bottom, u1, v1)."""
        s = self.side
        return (rect.left() / s, 1.0 - rect.bottom() / s, rect.right() / s, 1.0 - rect.top() / s)

    def rect_of(self, cell):
        s = self.side
        return QtCore.QRectF(cell[0] * s, (1.0 - cell[3]) * s, (cell[2] - cell[0]) * s, (cell[3] - cell[1]) * s)

    def set_blocks(self, blocks, selected=None):
        """`blocks`: [(groom, name, cell)]."""
        scene = self.scene()
        for item in self.items.values():
            scene.removeItem(item)
        self.items = {}
        for groom, name, cell in blocks:
            item = BlockItem(self, groom, name, self.rect_of(cell))
            scene.addItem(item)
            item.set_picked(groom == selected)
            self.items[groom] = item
        self.viewport().update()

    def set_image(self, path):
        self.image = QtGui.QImage(path) if path and os.path.isfile(path) else None
        self.viewport().update()


class CardAtlasPanel(QtWidgets.QWidget):
    """The Card Atlas window (see the module)."""

    def __init__(self, parent=None):
        super(CardAtlasPanel, self).__init__(parent)
        self.setObjectName("groomLabCardAtlas")
        self.atlas = None
        self.groom = None
        self.preview_paths = {}
        self._filling = False
        outer = QtWidgets.QHBoxLayout(self)
        pad = theme.px(10)
        outer.setContentsMargins(pad, pad, pad, pad)
        outer.setSpacing(theme.px(10))

        # ---- The atlas (left): what it shows and its two actions over the square.
        left = QtWidgets.QVBoxLayout()
        left.setSpacing(theme.px(6))
        bar = QtWidgets.QHBoxLayout()
        bar.setSpacing(theme.px(4))
        self.map = QtWidgets.QComboBox()
        self.map.setObjectName("cardAtlasMap")
        self.map.addItems(MAPS)
        self.map.setToolTip("The map the atlas shows (from the last Bake Preview)")
        self.map.currentIndexChanged.connect(lambda *_: self._show_map())
        show_row, _ = widgets.field_row("Show", self.map, label_width=44)
        show_row.setMaximumWidth(theme.px(240))
        bar.addWidget(show_row)
        bar.addStretch(1)
        self.layout_button = widgets.chip("Auto Layout", "Every block packed again at one texel density (blocks placed by "
                                          "hand too)", name="cardAtlasLayout")
        self.layout_button.clicked.connect(lambda *_: self.auto_layout())
        bar.addWidget(self.layout_button)
        self.bake_button = widgets.chip("Bake Preview", "Every block's hair baked small into its rectangle (the maps the "
                                        "exports write)", name="cardAtlasBake")
        self.bake_button.clicked.connect(lambda *_: self.bake_preview())
        bar.addWidget(self.bake_button)
        left.addLayout(bar)
        self.canvas = AtlasCanvas()
        self.canvas.picked.connect(self.pick_block)
        self.canvas.moved.connect(self.move_block)
        left.addWidget(self.canvas, 1)
        hint = widgets.small_label("Drag a rectangle to move it, its corner to resize it: placed by hand from then on.",
                                   role="hint", wrap=True)
        left.addWidget(hint)
        outer.addLayout(left, 3)

        # ---- The side column (right): the atlas, its blocks, the picked block, the exports.
        scroll = QtWidgets.QScrollArea()
        scroll.setObjectName("cardAtlasSide")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setMinimumWidth(theme.px(300))
        scroll.setMaximumWidth(theme.px(380))
        inner = QtWidgets.QWidget()
        scroll.setWidget(inner)
        side = QtWidgets.QVBoxLayout(inner)
        side.setContentsMargins(0, 0, theme.px(4), 0)
        side.setSpacing(theme.px(10))

        head = QtWidgets.QHBoxLayout()
        title = QtWidgets.QLabel("Card Atlas")
        title.setProperty("role", "title")
        head.addWidget(title, 1)
        new_atlas = widgets.chip("New Atlas", "A new, empty atlas (a group in the scene): Add its blocks", name="cardAtlasNew")
        new_atlas.clicked.connect(lambda *_: self.new_atlas())
        head.addWidget(new_atlas)
        side.addLayout(head)

        # The atlas.
        atlas_block = widgets.Block("Atlas")
        self.atlas_box = QtWidgets.QComboBox()
        self.atlas_box.setObjectName("cardAtlasPick")
        self.atlas_box.setToolTip("The atlas the window shows (a group in the scene)")
        self.atlas_box.currentIndexChanged.connect(lambda *_: self._atlas_picked())
        atlas_block.labeled("Atlas", self.atlas_box)
        self.size = QtWidgets.QComboBox()
        self.size.setObjectName("cardAtlasSize")
        for size in SIZES:
            self.size.addItem("%d x %d" % (size, size), size)
        self.size.setToolTip("The texture's size, texels square")
        self.size.currentIndexChanged.connect(lambda *_: self._settings_changed())
        atlas_block.labeled("Size", self.size)
        self.padding = widgets.SoftSlider(8, 0, 32, integer=True, minimum=0, maximum=256, key=None, label="Padding",
                                          suffix="px")
        self.padding.setObjectName("cardAtlasPadding")
        self.padding.setToolTip("Texels between the blocks")
        self.padding.committed.connect(lambda *_: self._settings_changed())
        atlas_block.add(self.padding)
        self.preset = QtWidgets.QComboBox()
        self.preset.setObjectName("cardAtlasPreset")
        self.preset.setToolTip("How Export Maps packs the maps (Export > Card Maps' presets)")
        self.preset.currentIndexChanged.connect(lambda *_: self._settings_changed())
        atlas_block.labeled("Preset", self.preset)
        self.name = QtWidgets.QLineEdit()
        self.name.setObjectName("cardAtlasName")
        self.name.setToolTip("The maps' file name ({name} of the preset's naming)")
        self.name.editingFinished.connect(self._settings_changed)
        atlas_block.labeled("Name", self.name)
        self.root_at_top = widgets.switch("Roots at Top", "Each block's root at the top of its rectangle and its tip at the "
                                          "bottom (as hand-made card atlases lay hair out); off: at the bottom",
                                          checked=True, name="cardAtlasRootAtTop")
        self.root_at_top.toggled.connect(lambda *_: self._settings_changed())
        atlas_block.add(self.root_at_top.row)
        side.addWidget(atlas_block)

        # Its blocks: the list, its actions on the title row, the picked one's box under it.
        blocks_block = widgets.Block("Blocks", actions=(
            widgets.glyph_button("plus", "Add a block beside the others: a root band, the card's graph and guides", self.add_block),
            widgets.glyph_button("duplicate", "Duplicate this block (its card and graph)", self.duplicate_block),
            widgets.glyph_button("delete", "Remove this block (its groom and root band)", self.remove_block)))
        for b, name in zip((blocks_block.head.itemAt(i).widget() for i in range(1, 4)),
                           ("cardAtlasAdd", "cardAtlasDuplicate", "cardAtlasRemove")):
            b.setObjectName(name)
        self.list = QtWidgets.QListWidget()
        self.list.setObjectName("cardAtlasBlocks")
        self.list.setProperty("role", "rows")
        self.list.currentRowChanged.connect(lambda row: self.pick_block(self.list.item(row).data(QtCore.Qt.UserRole)) if row >= 0 else None)
        blocks_block.add(self.list)
        self.empty = widgets.EmptyState("layers", "No blocks yet", "Add one: a groom on a root band, baked into its own "
                                        "rectangle of the atlas.")
        blocks_block.add(self.empty)

        edit = widgets.glyph_button("graphLine", "Edit in Graph Editor: the block's groom picked (the Groom Tools and "
                                    "brushes work on it) and the Graph Editor opened on it", self.edit_block)
        edit.setObjectName("cardAtlasEdit")
        self.item = widgets.ItemBox(actions=(edit,))
        self.item.setObjectName("cardAtlasBlockBox")
        self.fields = {}
        for key, label, value, lo, hi, minimum, maximum, decimals, suffix, tip in (
                ("width", "Width", 2.0, 0.1, 10.0, 0.01, 1e4, 3, "", "Across the card at its root (scene units)"),
                ("length", "Length", 15.0, 1.0, 40.0, 0.1, 1e5, 3, "", "The card's length, root to tip"),
                ("thickness", "Root Height", 0.5, 0.05, 2.0, 0.001, 100.0, 3, "",
                 "The root band's height down the top of the card: where the roots spread"),
                ("tilt", "Root Tilt", 0.0, -90.0, 90.0, -90.0, 90.0, 1, u"\u00b0",
                 "The root band tilted back (its lower edge away from the viewer): the roots spread in depth, as a "
                 "scalp seen at an angle (90: lying flat). The guides move with it, not turn"),
                ("turn", "Root Turn", 0.0, -90.0, 90.0, -90.0, 90.0, 1, u"\u00b0", "The root band turned: one side of the card nearer"),
                ("roll", "Root Roll", 0.0, -90.0, 90.0, -90.0, 90.0, 1, u"\u00b0",
                 "The root band rolled in the card's plane: the hair leaving at a slant")):
            slider = widgets.SoftSlider(value, lo, hi, minimum=minimum, maximum=maximum, decimals=decimals, key=None,
                                        label=label, suffix=suffix)
            slider.setObjectName("cardAtlas_" + key)
            slider.setToolTip(tip)
            slider.committed.connect(lambda *_: self._block_changed())
            self.item.add(slider)
            self.fields[key] = slider
        size_row = QtWidgets.QHBoxLayout()
        size_row.setContentsMargins(theme.px(2), 0, 0, 0)
        self.card_size = widgets.small_label("", role="hint")
        self.card_size.setObjectName("cardAtlasCardSize")
        self.card_size.setToolTip("The block's card as it bakes and exports: as wide and as long as its hair reaches (combed "
                                  "or noised past its Width, longer or shorter than its Length), fitted before every bake, "
                                  "export and edit; Fit refreshes it after grooming")
        size_row.addWidget(self.card_size, 1)
        fit_now = widgets.chip("Fit", "Fit every block's card to its hair now (after combing or a graph change)",
                               name="cardAtlasFitNow")
        fit_now.clicked.connect(lambda *_: self.fit_cards())
        size_row.addWidget(fit_now)
        self.item.add(size_row)
        blocks_block.add(self.item)
        side.addWidget(blocks_block)

        # The exports.
        export_block = widgets.Block("Export")
        self.folder = QtWidgets.QLineEdit()
        self.folder.setObjectName("cardAtlasFolder")
        self.folder.setPlaceholderText("the scene's exports folder")
        self.folder.setToolTip("Where the maps and the card shapes go")
        browse = widgets.chip("...", "Pick the folder")
        browse.clicked.connect(lambda *_: self.browse())
        export_block.labeled("Folder", self.folder, browse)
        self.segments = widgets.SoftSlider(8, 1, 32, integer=True, minimum=1, maximum=64, key=None, label="Segments")
        self.segments.setObjectName("cardAtlasSegments")
        self.segments.setToolTip("The card shapes' quads along")
        export_block.add(self.segments)
        self.bend = widgets.SoftSlider(0.0, -90.0, 90.0, minimum=-180.0, maximum=180.0, decimals=1, key=None, label="Bend",
                                       suffix=u"\u00b0")
        self.bend.setObjectName("cardAtlasBend")
        self.bend.setToolTip("The card shapes bent over their length (degrees, away from the head)")
        export_block.add(self.bend)
        export_row = QtWidgets.QHBoxLayout()
        export_row.setSpacing(theme.px(6))
        shapes = QtWidgets.QPushButton("Export Card Shapes")
        shapes.setObjectName("cardAtlasExportShapes")
        shapes.setToolTip("The blocks' cards as UV-ready meshes (FBX), one per block, to place by hand")
        shapes.clicked.connect(lambda *_: self.export_shapes())
        export_row.addWidget(shapes, 1)
        maps = QtWidgets.QPushButton("Export Maps")
        maps.setObjectName("cardAtlasExportMaps")
        maps.setProperty("role", "primary")
        maps.setToolTip("Every block baked into one texture set, written as the preset packs it")
        maps.clicked.connect(lambda *_: self.export_maps())
        export_row.addWidget(maps, 1)
        export_block.add(export_row)
        to_scene = QtWidgets.QPushButton("Cards to Scene")
        to_scene.setObjectName("cardAtlasCardsToScene")
        to_scene.setToolTip("The blocks' cards made in the scene beside the blocks (UV-ready meshes, as Export Card Shapes "
                            "writes them), textured with the preview maps (baked first when there are none)")
        to_scene.clicked.connect(lambda *_: self.cards_to_scene())
        export_block.add(to_scene)
        side.addWidget(export_block)
        self.status = widgets.StatusLabel()
        self.status.setObjectName("cardAtlasStatus")
        side.addWidget(self.status)
        side.addStretch(1)
        outer.addWidget(scroll, 2)
        theme.style(self)
        self.refresh()

    # ---- Filling

    def refresh(self, atlas=None):
        """The atlases of the scene, the picked one (`atlas`, else the one shown) and its blocks."""
        api = _atlas_api()
        found = api.atlases()
        keep = atlas or self.atlas
        self._filling = True
        try:
            self.atlas_box.clear()
            for a in found:
                self.atlas_box.addItem(_short(a), a)
            if keep in found:
                self.atlas_box.setCurrentIndex(found.index(keep))
            if self.preset.count() == 0:
                import groomlab
                for p in groomlab.card_map_presets():
                    self.preset.addItem(p["name"])
        finally:
            self._filling = False
        self.atlas = self.atlas_box.currentData() if found else None
        self._fill_atlas()

    def _fill_atlas(self):
        api = _atlas_api()
        self._filling = True
        try:
            self.list.clear()
            blocks = []
            if self.atlas:
                s = api.settings(self.atlas)
                index = self.size.findData(int(s["size"]))
                self.size.setCurrentIndex(index if index >= 0 else 2)
                self.padding.setValue(int(s["padding"]))
                i = self.preset.findText(s["preset"])
                if i >= 0:
                    self.preset.setCurrentIndex(i)
                self.name.setText(s["name"])
                self.root_at_top.setChecked(bool(s.get("rootAtTop", True)))
                for groom in api.blocks(self.atlas):
                    b = api.block(groom)
                    item = QtWidgets.QListWidgetItem(b["name"])
                    item.setData(QtCore.Qt.UserRole, groom)
                    self.list.addItem(item)
                    blocks.append((groom, b["name"], b["cell"]))
                    if groom == self.groom:
                        self.list.setCurrentItem(item)
            if self.groom not in [g for g, _, _ in blocks]:
                self.groom = blocks[0][0] if blocks else None
                if blocks:
                    self.list.setCurrentRow(0)
            self.canvas.set_blocks(blocks, self.groom)
            from groomlab.ui.tool_tabs import fit_rows
            fit_rows(self.list, minimum=1, maximum=8)
            self._fill_block()
        finally:
            self._filling = False

    def _fill_block(self):
        api = _atlas_api()
        self.item.setVisible(bool(self.groom))
        self.list.setVisible(self.list.count() > 0)
        self.empty.setVisible(self.list.count() == 0)
        if not self.groom:
            self.card_size.setText("")
            self.item.set_title("")
            return
        b = api.block(self.groom)
        self.item.set_title(b["name"])
        w, h = api.card_size(self.groom)
        self.card_size.setText("Card %.2f x %.2f" % (w, h))
        rotation = b.get("rootRotation") or [0.0, 0.0, 0.0]
        values = {"width": b["width"], "length": b["length"], "thickness": b["thickness"],
                  "tilt": rotation[0], "turn": rotation[1], "roll": rotation[2]}
        for key, spin in self.fields.items():
            spin.blockSignals(True)
            spin.setValue(values[key])
            spin.blockSignals(False)

    def _show_map(self):
        self.canvas.set_image(self.preview_paths.get(self.map.currentText()))

    # ---- Actions

    def _atlas_picked(self):
        if self._filling:
            return
        self.atlas = self.atlas_box.currentData()
        self.groom = None
        self.preview_paths = {}
        self.canvas.set_image(None)
        self._fill_atlas()

    def new_atlas(self):
        atlas = _atlas_api().create_atlas("hairCards")
        self.groom = None
        self.preview_paths = {}
        self.canvas.set_image(None)
        self.refresh(atlas)
        self.status.setText("New atlas %s: Add a block" % _short(atlas))
        return atlas

    def _require_atlas(self):
        if not self.atlas:
            self.new_atlas()
        return self.atlas

    def add_block(self):
        atlas = self._require_atlas()
        name = "block%d" % (len(_atlas_api().blocks(atlas)) + 1)
        self.groom = _atlas_api().add_block(atlas, name)
        self.refresh()
        self.status.setText("Added %s" % name)
        return self.groom

    def duplicate_block(self):
        if not self.groom:
            return None
        self.groom = _atlas_api().duplicate_block(self.groom)
        self.refresh()
        return self.groom

    def remove_block(self):
        if not self.groom:
            return
        _atlas_api().remove_block(self.groom)
        self.groom = None
        self.refresh()

    def pick_block(self, groom):
        if not groom or self._filling:
            return
        self.groom = groom
        from maya import cmds
        try:
            import groomlab
            groomlab.set_current_groom(groom)
            cmds.select(cmds.listRelatives(groom, parent=True, fullPath=True)[0], replace=True)
        except Exception:  # noqa: BLE001 - picking never fails the window
            pass
        self._filling = True
        try:
            for row in range(self.list.count()):
                if self.list.item(row).data(QtCore.Qt.UserRole) == groom:
                    self.list.setCurrentRow(row)
            for g, item in self.canvas.items.items():
                item.setSelected(g == groom)
        finally:
            self._filling = False
        self._fill_block()

    def move_block(self, groom, cell):
        _atlas_api().set_block(groom, cell=cell)
        self.groom = groom
        self._fill_atlas()

    def _block_changed(self):
        if self._filling or not self.groom:
            return
        v = dict((k, s.value()) for k, s in self.fields.items())
        _atlas_api().set_block(self.groom, width=v["width"], length=v["length"], thickness=v["thickness"],
                               root_rotation=(v["tilt"], v["turn"], v["roll"]))
        self._fill_atlas()

    def _settings_changed(self):
        if self._filling or not self.atlas:
            return
        _atlas_api().set_settings(self.atlas, size=self.size.currentData(), padding=int(round(self.padding.value())),
                                  preset=self.preset.currentText(), name=self.name.text().strip() or None,
                                  root_at_top=self.root_at_top.isChecked())
        self._fill_atlas()

    def fit_cards(self):
        """Every block's card fitted to its hair now."""
        if not self.atlas:
            return False
        changed = _atlas_api().fit(self.atlas)
        self._fill_atlas()
        self.status.setText("Fitted the cards to the hair" if changed else "The cards already fit the hair")
        return changed

    def auto_layout(self):
        if self.atlas:
            _atlas_api().fit(self.atlas)
            _atlas_api().update_layout(self.atlas, pack=True)
            self._fill_atlas()

    def edit_block(self):
        if not self.groom:
            return
        self.pick_block(self.groom)
        from groomlab.ui import graph_editor
        graph_editor.show(self.groom)

    def bake_preview(self):
        if not self.atlas:
            return None
        try:
            self.preview_paths = _atlas_api().preview(self.atlas)
        except (RuntimeError, ValueError) as e:
            self.status.setText("Can't bake: %s" % str(e).strip())
            return None
        self._show_map()
        self.status.setText("Baked the preview")
        return self.preview_paths

    def browse(self):
        from maya import cmds
        picked = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the card atlas")
        if picked:
            self.folder.setText(picked[0])

    def _folder(self):
        folder = self.folder.text().strip()
        if not folder:
            import groomlab
            folder = groomlab.asset_folder("exports")
            self.folder.setText(folder)
        return folder

    def export_maps(self):
        if not self.atlas:
            return None
        try:
            paths = _atlas_api().export_maps(self.atlas, self._folder())
        except (RuntimeError, ValueError) as e:
            self.status.setText("Can't export: %s" % str(e).strip())
            return None
        self.status.setText("Wrote %d maps to %s" % (len(paths), self._folder()))
        return paths

    def cards_to_scene(self):
        """The cards made in the scene, textured with the preview maps (baked when there are none)."""
        if not self.atlas:
            return None
        if not self.preview_paths and not self.bake_preview():
            return None
        try:
            group = _atlas_api().cards_to_scene(self.atlas, segments=int(round(self.segments.value())),
                                                bend=self.bend.value(), maps=self.preview_paths)
        except (RuntimeError, ValueError) as e:
            self.status.setText("Can't make the cards: %s" % str(e).strip())
            return None
        self.status.setText("Made the cards in the scene: %s" % _short(group))
        return group

    def export_shapes(self):
        if not self.atlas:
            return None
        path = os.path.join(self._folder(), "%s_cards.fbx" % (self.name.text().strip() or _short(self.atlas)))
        try:
            _atlas_api().export_shapes(self.atlas, path, segments=int(round(self.segments.value())), bend=self.bend.value())
        except (RuntimeError, ValueError) as e:
            self.status.setText("Can't export: %s" % str(e).strip())
            return None
        self.status.setText("Wrote the card shapes to %s" % path)
        return path


def show():
    """Open (or raise) the Card Atlas window."""
    global _window
    from maya.app.general.mayaMixin import MayaQWidgetBaseMixin

    class MayaCardAtlasPanel(MayaQWidgetBaseMixin, CardAtlasPanel):
        pass

    if _window is not None and not widgets.alive(_window):
        _window = None
    if _window is None:
        _window = MayaCardAtlasPanel()
        _window.setWindowTitle("GroomLab Card Atlas")
        _window.setWindowIcon(widgets.icon("logo"))
        _window.setWindowFlags(_window.windowFlags() | QtCore.Qt.Window)
        _window.resize(theme.px(980), theme.px(640))
        widgets.remember_geometry(_window, "cardAtlas")
        widgets.at_exit(_close_at_exit)
    else:
        _window.refresh()
    _window.show()
    _window.raise_()
    return _window


def window():
    """The open Card Atlas window, or None."""
    return _window if _window is not None and widgets.alive(_window) and _window.isVisible() else None


def _close_at_exit():
    global _window
    w, _window = _window, None
    if w is not None and widgets.alive(w):
        widgets.dispose(w)
