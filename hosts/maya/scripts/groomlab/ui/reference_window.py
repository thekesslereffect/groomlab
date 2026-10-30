# SPDX-License-Identifier: Apache-2.0
"""The Photos window (Groom Tools > Photos > Align Photo..., GroomLab > Guides from Photos
(experimental)...): the reference photos lined up with the mesh. Wrap-style, as the Transfer Groom
window (and built from the same pieces, mesh_views.py): never modal, two views side by side, the
reference photo on the left and the groom's scalp on the right, and the settings in a panel beside
them. Nothing has to be done through a Maya camera; each photo's camera and image plane are made and
kept lined up (from the solve), so the viewport, renders and Arnold can look through them.

Two stages (the chips at the top left):
  - Align: landmarks only. The panel: the pairs and their errors, the camera (Focal Length, Keep
    focal length, Lock) and the mesh drawn over the photo.
  - Guides (experimental): Guides from Photos besides: flow arrows, mask touch-ups, Generate and
    the measured look.

Photos: a tab each along the top (Import Photo..., Remove). Each has its own landmarks, flow
arrows, mask touch-ups and solved camera (a groomlab.reference camera).

The photo view (2D): the wheel zooms at the cursor, the middle button (or Alt+left) pans, F or
Fit fits it. The tools (the chips above it):
  - Landmarks, Wrap-style (the core's ReferencePhotoEditor, reference_edit.h, the Transfer
    window's pairing rules): click a feature on the photo and the same feature on the mesh (either
    order); pair N is the Nth point on each. Drag a point to adjust it, Ctrl+click deletes it and
    its partner. With 4 pairs the camera solves live (the focal length kept; 6 or more solve it
    too, unless Keep focal length); each pair shows its reprojection error (a line to where the
    mesh point lands, the table's Error column), bad pairs in red. The mesh's outline or wireframe
    draws over the photo through the camera (Overlay, with its opacity).
  - Flow: drag along the hair from root to tip: an arrow (as the Reference Flow tool drew them);
    Ctrl+drag erases. Each arrow one undo step.
  - Hair / Backdrop: brush touch-ups of the hair mask (Ctrl swaps the kind); the refined mask
    shows as a tint (Mask tint), the orientation field as short lines (Flow field).
The mesh view: a Maya view of the scalp with its own camera (Alt navigates, Frame fits); clicks
place mesh landmarks (Landmarks tool), numbered dots drawn by the plugin's groomLabMarkerSet.

Ctrl+Z / Ctrl+Y in the window (or Undo / Redo) undo and redo the current photo's edits (the
window's own stack per photo; what it keeps on the camera and the groom is stored outside Maya's
undo queue on every edit). Generate and Apply are Maya undo steps; after Maya's undo or redo the
window follows (photos imported or removed, the guides, what is stored).

Scripts and tests drive it without the mouse:

    from groomlab.ui import reference_window
    w = reference_window.show(groom)            # the Align stage (stage="guides": Guides from Photos)
    cam = w.import_photo("C:/photos/front.jpg")
    w.add_landmark("photo", (412, 230)); w.add_landmark("mesh", (0, 9.5, 3.1))  # ... 4+ pairs
    w.set_focal(50.0)                           # by hand: Keep focal length turns on
    w.set_stage("guides"); w.add_flow_stroke([(400, 120), (410, 300)]); w.generate()
"""

import math
import os

from maya import cmds

from groomlab.ui.qt import QtCore, QtGui, QtWidgets, event_pos, to_int
from groomlab.ui import mesh_views, theme, widgets

_window = None

UI_GROUP = "groomLabReferenceUI"  # the mesh view's camera and markers (hidden, outside the undo queue)
SIDES = ("photo", "mesh")
COLORS = {"photo": (1.0, 0.85, 0.1), "mesh": (0.15, 0.9, 1.0)}  # yellow photo, cyan mesh
STAGES = ("align", "guides")
MODES = ("landmarks", "flow", "hair", "backdrop")
MODE_LABELS = {"landmarks": "Landmarks", "flow": "Flow", "hair": "Hair", "backdrop": "Backdrop"}
OVERLAYS = ("off", "outline", "wireframe")
PHOTO_FILTER = "Images (*.png *.jpg *.jpeg *.tif *.tiff *.tga *.bmp *.exr *.iff);;All Files (*.*)"
ARROW = QtGui.QColor(255, 158, 38)       # the flow arrows (the Reference Flow tool's orange)
ARROW_NEW = QtGui.QColor(255, 230, 102)  # the one being drawn
ERASE = QtGui.QColor(255, 77, 77)
BAD = QtGui.QColor(255, 70, 70)
HAIR_TINT = QtGui.QColor(80, 230, 120, 90)
FIELD = QtGui.QColor(150, 215, 255, 210)
INCLUDE = QtGui.QColor(80, 230, 120, 120)
EXCLUDE = QtGui.QColor(255, 80, 80, 120)


def _fpos(event):
    """A mouse event's position in the widget as a QPointF (sub-pixel where the event has it)."""
    position = getattr(event, "position", None)
    if position is not None:
        return QtCore.QPointF(position())
    local = getattr(event, "localPos", None)
    return QtCore.QPointF(local()) if local is not None else QtCore.QPointF(event_pos(event))


def _qcolor(rgb, alpha=255):
    return QtGui.QColor(int(rgb[0] * 255), int(rgb[1] * 255), int(rgb[2] * 255), alpha)


class PhotoView(QtWidgets.QWidget):
    """The photo, 2D: pan and zoom, the overlays, the landmarks, arrows and mask strokes; the clicks
    call the window's edits (as the scripts do)."""

    def __init__(self, window):
        super(PhotoView, self).__init__(window)
        self.window = window
        self.setObjectName("referencePhotoView")
        self.setMouseTracking(True)
        self.setFocusPolicy(QtCore.Qt.ClickFocus)
        self.setMinimumSize(theme.px(220), theme.px(180))
        self.pixmap = None
        self.path = None
        self.size_px = (0, 0)  # the photo's size (its camera's, when the image cannot be read)
        self.scale = 1.0
        self.offset = QtCore.QPointF(0, 0)
        self.user_moved = False
        self.drag = None   # ["pan"|"point"|"stroke"|"erase"|"mask", ...]
        self.last = QtCore.QPointF()
        self.hover_pos = None

    # ---- The photo and the view's transform ----------------------------------------------------

    def set_photo(self, path, size):
        if path != self.path:
            image = QtGui.QImage(path) if path and os.path.isfile(path) else QtGui.QImage()
            self.pixmap = QtGui.QPixmap.fromImage(image) if not image.isNull() else None
            self.path = path
            self.user_moved = False
        self.size_px = (int(size[0]), int(size[1]))
        if not self.user_moved:
            self.fit()
        self.update()

    def fit(self):
        w, h = self.size_px
        if w <= 0 or h <= 0:
            return
        self.scale = min(self.width() / float(w), self.height() / float(h)) * 0.96
        self.offset = QtCore.QPointF((self.width() - w * self.scale) * 0.5, (self.height() - h * self.scale) * 0.5)
        self.user_moved = False
        self.update()

    def to_photo(self, pos):
        return ((pos.x() - self.offset.x()) / self.scale, (pos.y() - self.offset.y()) / self.scale)

    def to_widget(self, u, v):
        return QtCore.QPointF(self.offset.x() + u * self.scale, self.offset.y() + v * self.scale)

    def transform(self):
        return QtGui.QTransform(self.scale, 0, 0, self.scale, self.offset.x(), self.offset.y())

    def zoom_at(self, pos, factor):
        u, v = self.to_photo(pos)
        self.scale = max(0.02, min(self.scale * factor, 64.0))
        self.offset = QtCore.QPointF(pos.x() - u * self.scale, pos.y() - v * self.scale)
        self.user_moved = True
        self.update()

    def resizeEvent(self, event):
        super(PhotoView, self).resizeEvent(event)
        if not self.user_moved:
            self.fit()

    # ---- Drawing --------------------------------------------------------------------------------

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        try:
            self._paint(painter)
        finally:
            painter.end()

    def _paint(self, painter):
        w = self.window
        t = theme.current()
        painter.fillRect(self.rect(), t.qcolor("ui.background"))
        W, H = self.size_px
        if W <= 0:
            painter.setPen(t.qcolor("ui.textDim"))
            painter.drawText(self.rect(), QtCore.Qt.AlignCenter | QtCore.Qt.TextWordWrap,
                             "Import a photo (Import Photo... above)")
            return
        painter.setRenderHint(QtGui.QPainter.Antialiasing, True)
        painter.setRenderHint(QtGui.QPainter.SmoothPixmapTransform, True)
        painter.save()
        painter.setTransform(self.transform())
        if self.pixmap is not None:
            painter.drawPixmap(QtCore.QRectF(0, 0, W, H), self.pixmap, QtCore.QRectF(self.pixmap.rect()))
        else:
            painter.fillRect(QtCore.QRectF(0, 0, W, H), t.qcolor("ui.raised"))
        preview = w.preview() if (w.mask_tint.isChecked() or w.field_lines.isChecked()) else None
        if preview:
            x0, y0, cw, ch = preview["crop"]
            if w.mask_tint.isChecked() and preview.get("tint") is not None:
                painter.drawImage(QtCore.QRectF(x0, y0, cw, ch), preview["tint"])
            if w.field_lines.isChecked() and preview.get("qlines"):
                pen = QtGui.QPen(FIELD, 1.2)
                pen.setCosmetic(True)
                painter.setPen(pen)
                painter.drawLines(preview["qlines"])
        # Mask touch-ups: the brush lines, as wide as they paint.
        editor = w.editor()
        for s in (editor.mask_strokes if editor else []):
            pen = QtGui.QPen(INCLUDE if s["include"] else EXCLUDE, 2.0 * s["radius"])
            pen.setCapStyle(QtCore.Qt.RoundCap)
            pen.setJoinStyle(QtCore.Qt.RoundJoin)
            painter.setPen(pen)
            points = [QtCore.QPointF(*p) for p in s["points"]]
            painter.drawPolyline(QtGui.QPolygonF(points)) if len(points) > 1 else painter.drawPoint(points[0])
        # The mesh through the camera.
        lines = w.overlay_lines()
        if lines:
            pen = QtGui.QPen(QtGui.QColor(255, 255, 255, int(255 * w.overlay_opacity())), 1.0)
            pen.setCosmetic(True)
            painter.setPen(pen)
            painter.drawLines(lines)
        # Flow arrows.
        head = W / 40.0
        for arrow in w.arrows():
            self._arrow(painter, arrow, ARROW)
        if self.drag and self.drag[0] in ("stroke", "erase") and len(self.drag[1]) >= 2:
            import groomlab_core as gl
            self._arrow(painter, gl.reference_arrow(self.drag[1], head if self.drag[0] == "stroke" else 0.0),
                        ARROW_NEW if self.drag[0] == "stroke" else ERASE)
        if self.drag and self.drag[0] == "mask" and self.drag[1]:
            pen = QtGui.QPen(INCLUDE if self.drag[2] else EXCLUDE, 2.0 * self.drag[3])
            pen.setCapStyle(QtCore.Qt.RoundCap)
            painter.setPen(pen)
            points = [QtCore.QPointF(*p) for p in self.drag[1]]
            painter.drawPolyline(QtGui.QPolygonF(points)) if len(points) > 1 else painter.drawPoint(points[0])
        painter.restore()
        self._paint_landmarks(painter)
        if w.mode() in ("hair", "backdrop") and self.hover_pos is not None and not self.drag:
            painter.setPen(QtGui.QPen(QtGui.QColor(255, 255, 255, 180), 1.0))
            painter.setBrush(QtCore.Qt.NoBrush)
            r = w.brush_size()
            painter.drawEllipse(self.hover_pos, r, r)

    def _arrow(self, painter, arrow, color):
        pen = QtGui.QPen(color, 2.0)
        pen.setCosmetic(True)
        pen.setCapStyle(QtCore.Qt.RoundCap)
        painter.setPen(pen)
        painter.setBrush(QtCore.Qt.NoBrush)
        painter.drawPolyline(QtGui.QPolygonF([QtCore.QPointF(*p) for p in arrow["line"]]))
        if arrow.get("head"):
            painter.drawPolyline(QtGui.QPolygonF([QtCore.QPointF(*p) for p in arrow["head"]]))

    def _paint_landmarks(self, painter):
        """The photo's landmarks as the Transfer window's dots: filled matched, a ring pending,
        numbered, a white ring hovered; bad pairs red; with a solve, a line to where each pair's
        mesh point lands through the camera."""
        w = self.window
        editor = w.editor()
        if editor is None:
            return
        solve = w.solve_result() or {}
        bad = solve.get("bad") or []
        projected = solve.get("projected") or []
        color = _qcolor(COLORS["photo"])
        mesh_color = _qcolor(COLORS["mesh"])
        font = painter.font()
        font.setBold(True)
        font.setPixelSize(theme.px(13))
        painter.setFont(font)
        r = theme.px(5.5)
        for i, p in enumerate(editor.points("photo")):
            at = self.to_widget(p.point.x, p.point.y)
            if solve.get("ok") and i < len(projected) and projected[i][0] >= 0:
                lands = self.to_widget(*projected[i])
                painter.setPen(QtGui.QPen(BAD if (i < len(bad) and bad[i]) else mesh_color, 1.5))
                painter.drawLine(at, lands)
                painter.setBrush(QtCore.Qt.NoBrush)
                painter.drawEllipse(lands, theme.px(2.5), theme.px(2.5))
            matched = editor.matched("photo", i)
            lit = w.hover_index("photo") == i
            is_bad = i < len(bad) and bad[i]
            dot = BAD if is_bad else color
            if lit:
                painter.setPen(QtGui.QPen(QtGui.QColor(255, 255, 255), 2.0))
                painter.setBrush(QtCore.Qt.NoBrush)
                painter.drawEllipse(at, r + theme.px(4.5), r + theme.px(4.5))
            if matched:
                painter.setPen(QtGui.QPen(QtGui.QColor(0, 0, 0, 200), 1.5))
                painter.setBrush(dot)
            else:
                painter.setPen(QtGui.QPen(dot, 2.5))
                painter.setBrush(QtCore.Qt.NoBrush)
            painter.drawEllipse(at, r, r)
            path = QtGui.QPainterPath()
            path.addText(at + QtCore.QPointF(r + 3.0, -r - 2.0), font, str(i + 1))
            painter.setPen(QtGui.QPen(QtGui.QColor(0, 0, 0, 200), 3.0))
            painter.setBrush(QtCore.Qt.NoBrush)
            painter.drawPath(path)
            painter.setPen(QtCore.Qt.NoPen)
            painter.setBrush(QtGui.QColor(255, 255, 255) if lit else dot)
            painter.drawPath(path)

    # ---- The mouse ------------------------------------------------------------------------------

    def mousePressEvent(self, event):
        pos = _fpos(event)
        self.last = pos
        mods = to_int(event.modifiers())
        alt = bool(mods & to_int(QtCore.Qt.AltModifier))
        ctrl = bool(mods & to_int(QtCore.Qt.ControlModifier))
        button = event.button()
        if button == QtCore.Qt.MiddleButton or (button == QtCore.Qt.LeftButton and alt):
            self.drag = ["pan"]
            return
        if button != QtCore.Qt.LeftButton or self.size_px[0] <= 0:
            return
        self.window.press_photo(self, self.to_photo(pos), ctrl)

    def mouseMoveEvent(self, event):
        pos = _fpos(event)
        delta = pos - self.last
        self.last = pos
        self.hover_pos = pos
        if self.drag and self.drag[0] == "pan":
            self.offset += delta
            self.user_moved = True
            self.update()
            return
        buttons = to_int(event.buttons())
        if self.drag and buttons & to_int(QtCore.Qt.LeftButton):
            self.window.drag_photo(self, self.to_photo(pos))
            return
        if not buttons:
            self.window.hover_photo(self, self.to_photo(pos))
        if self.window.mode() in ("hair", "backdrop"):
            self.update()

    def mouseReleaseEvent(self, event):
        drag, self.drag = self.drag, None
        if drag and drag[0] != "pan":
            self.window.release_photo(self, drag)
        self.update()

    def wheelEvent(self, event):
        steps = event.angleDelta().y() / 120.0
        if steps:
            self.zoom_at(_fpos(event), 1.15 ** steps)

    def leaveEvent(self, event):
        self.hover_pos = None
        self.window.set_hover(None)
        self.update()

    def keyPressEvent(self, event):
        if event.key() == QtCore.Qt.Key_F and not to_int(event.modifiers()):
            self.fit()
            return
        super(PhotoView, self).keyPressEvent(event)


class ReferenceWindow(QtWidgets.QWidget):
    def __init__(self, parent=None, groom=None):
        super(ReferenceWindow, self).__init__(parent)
        import groomlab_core
        from groomlab import from_reference, reference
        self.gl = groomlab_core
        self.ref = reference
        self.fr = from_reference
        self.groom = groom
        self.editors = {}     # camera: its ReferencePhotoEditor
        self.stored = {}      # camera: what the window last stored for it (photo_state's JSON)
        self.solves = {}      # camera: the last solve (solve_reference_landmarks' dict)
        self.unused = set()   # cameras left out of Generate
        self.camera = None    # the current photo's camera
        self.mesh = None      # the scalp transform the mesh view shows
        self._scalp = None    # (shape, groomlab_core.ScalpMesh) for the overlay and the preview
        self._overlay = None  # (key, [QLineF])
        self._arrows = None   # (revision, [arrow dicts])
        self._preview = None  # (key, preview dict)
        self._hover = None    # (side, index)
        self._drag = None     # mesh view drag: [index, first move]
        self._owned_press = False
        self._scene_job = None
        self._last_rows = None
        self.last_result = {}
        self._undo_was_on = cmds.undoInfo(query=True, state=True)
        self.setWindowTitle("Photos")
        self.setWindowIcon(widgets.icon("logo"))
        self.setObjectName("groomLabReferenceWindow")
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose, True)
        outer = QtWidgets.QVBoxLayout(self)
        pad = theme.px(10)
        outer.setContentsMargins(pad, pad, pad, pad)
        outer.setSpacing(theme.px(6))

        # The stage (Align, Guides); the photos: a tab each; Import Photo..., Remove.
        top = QtWidgets.QHBoxLayout()
        self.stage_buttons = {}
        stages = QtWidgets.QButtonGroup(self)
        stages.setExclusive(True)
        for stage, label, tip in (
                ("align", "Align", "Line the photos up with the mesh: landmark pairs, the camera"),
                ("guides", "Guides (experimental)", "Guides from Photos: flow arrows and mask touch-ups on the "
                                                    "lined-up photos, Generate and the measured look")):
            b = QtWidgets.QToolButton()
            b.setText(label)
            b.setObjectName("referenceStage_" + stage)
            b.setCheckable(True)
            b.setProperty("role", "chip")
            b.setToolTip(tip)
            b.clicked.connect(lambda _=False, s=stage: self.set_stage(s))
            stages.addButton(b)
            top.addWidget(b)
            self.stage_buttons[stage] = b
        self.stage_buttons["align"].setChecked(True)
        top.addSpacing(theme.px(12))
        self.tabs = QtWidgets.QTabBar()
        self.tabs.setObjectName("referencePhotoTabs")
        self.tabs.setExpanding(False)
        self.tabs.setDrawBase(False)
        self.tabs.setUsesScrollButtons(True)
        self.tabs.setToolTip("The reference photos: each has its own landmarks, flow arrows, mask and camera")
        self.tabs.currentChanged.connect(lambda *_: self._tab_changed())
        top.addWidget(self.tabs, 1)
        self.import_button = QtWidgets.QPushButton("Import Photo...")
        self.import_button.setObjectName("referenceImportPhoto")
        self.import_button.setToolTip("A photo of the groom (copied into the scene's groomlab/<scene>/references folder; a "
                                      "camera with the photo on an image plane is made for it)")
        self.import_button.clicked.connect(lambda: self.import_photo())
        top.addWidget(self.import_button)
        self.remove_button = QtWidgets.QPushButton("Remove")
        self.remove_button.setObjectName("referenceRemovePhoto")
        self.remove_button.setToolTip("Remove this photo's camera (its file stays on disk; Maya's undo brings it back)")
        self.remove_button.clicked.connect(lambda: self.remove_photo())
        top.addWidget(self.remove_button)
        outer.addLayout(top)

        body = QtWidgets.QHBoxLayout()
        body.setSpacing(theme.px(6))
        # Left: the photo, its tools above it.
        photo_column = QtWidgets.QVBoxLayout()
        tools = QtWidgets.QHBoxLayout()
        tools.setSpacing(theme.px(4))
        self.mode_buttons = {}
        group = QtWidgets.QButtonGroup(self)
        group.setExclusive(True)
        tips = {"landmarks": "Click a feature on the photo, then the same feature on the mesh (either order). Drag a "
                             "point to adjust it; Ctrl+click deletes it and its partner.",
                "flow": "Drag along the hair from root to tip: a flow arrow the guides follow (optional). Ctrl+drag "
                        "erases.",
                "hair": "Paint where the photo shows hair the mask missed (Ctrl: backdrop)",
                "backdrop": "Paint where the mask took backdrop for hair (Ctrl: hair)"}
        for mode in MODES:
            b = QtWidgets.QToolButton()
            b.setText(MODE_LABELS[mode])
            b.setObjectName("referenceMode_" + mode)
            b.setCheckable(True)
            b.setProperty("role", "chip")
            b.setToolTip(tips[mode])
            b.clicked.connect(lambda _=False, m=mode: self.set_mode(m))
            group.addButton(b)
            tools.addWidget(b)
            self.mode_buttons[mode] = b
        self.mode_buttons["landmarks"].setChecked(True)
        tools.addStretch(1)
        fit = QtWidgets.QToolButton()
        fit.setText("Fit")
        fit.setObjectName("referenceFitPhoto")
        fit.setProperty("role", "chip")
        fit.setToolTip("Fit the photo in its view (F). The wheel zooms, the middle button or Alt+drag pans.")
        fit.clicked.connect(lambda: self.photo_view.fit())
        tools.addWidget(fit)
        photo_column.addLayout(tools)
        self.photo_view = PhotoView(self)
        photo_column.addWidget(self.photo_view, 1)
        body.addLayout(photo_column, 1)

        # Middle: the mesh, a Maya view of its own.
        mesh_column = QtWidgets.QVBoxLayout()
        head = QtWidgets.QHBoxLayout()
        self.mesh_title = QtWidgets.QLabel()
        self.mesh_title.setStyleSheet("color: rgb(%d, %d, %d); font-weight: bold;" % tuple(int(c * 255) for c in COLORS["mesh"]))
        head.addWidget(self.mesh_title, 1)
        frame = QtWidgets.QPushButton("Frame")
        frame.setToolTip("Fit the scalp in this view (Alt navigates as in any view)")
        frame.clicked.connect(lambda: self.mesh_view.frame())
        head.addWidget(frame)
        mesh_column.addLayout(head)
        holder = QtWidgets.QWidget()
        holder.setObjectName("groomLabReferenceMeshView")
        holder.setMinimumSize(theme.px(220), theme.px(180))
        holder_layout = QtWidgets.QVBoxLayout(holder)
        holder_layout.setObjectName("groomLabReferenceMeshViewLayout")
        holder_layout.setContentsMargins(0, 0, 0, 0)
        mesh_column.addWidget(holder, 1)
        body.addLayout(mesh_column, 1)
        self.mesh_view = mesh_views.MeshView(holder, UI_GROUP, "groomLabReferenceMesh", COLORS["mesh"])

        # Right: the settings, in a scroll area (small screens).
        scroll = QtWidgets.QScrollArea()
        scroll.setObjectName("referenceSidePanel")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setMinimumWidth(theme.px(250))
        scroll.setMaximumWidth(theme.px(340))
        inner = QtWidgets.QWidget()
        scroll.setWidget(inner)
        self._build_panel(QtWidgets.QVBoxLayout(inner))
        body.addWidget(scroll)
        outer.addLayout(body, 1)

        self.hint = QtWidgets.QLabel()
        self.hint.setObjectName("referenceHint")
        self.hint.setWordWrap(True)
        self.hint.setProperty("role", "about")
        outer.addWidget(self.hint)
        bottom = QtWidgets.QHBoxLayout()
        self.status = QtWidgets.QLabel()
        self.status.setObjectName("referenceStatus")
        self.status.setWordWrap(True)
        bottom.addWidget(self.status, 1)
        self.undo_button = QtWidgets.QPushButton("Undo")
        self.undo_button.setObjectName("referenceUndo")
        self.undo_button.clicked.connect(lambda: self.undo())
        bottom.addWidget(self.undo_button)
        self.redo_button = QtWidgets.QPushButton("Redo")
        self.redo_button.setObjectName("referenceRedo")
        self.redo_button.clicked.connect(lambda: self.redo())
        bottom.addWidget(self.redo_button)
        bottom.addSpacing(theme.px(12))
        # The one button rule: secondary actions, the primary one on the right, Close last.
        bottom.addWidget(self.look_button)
        bottom.addWidget(self.generate_button)
        close = QtWidgets.QPushButton("Close")
        close.clicked.connect(self.close)
        bottom.addWidget(close)
        outer.addLayout(bottom)
        self.message = widgets.StatusLabel()
        self.message.setObjectName("referenceMessage")
        outer.addWidget(self.message)
        theme.style(self)
        widgets.fit_to_screen(self, theme.px(1240), theme.px(720))

        self._keys = mesh_views.UndoKeys(self)
        self._keys.watch(self)
        self.set_stage("align")
        self._refresh_mesh()
        self.refresh_photos()

    # ---- The side panel -------------------------------------------------------------------------

    def _heading(self, layout, text):
        label = QtWidgets.QLabel(text)
        label.setProperty("role", "subheading")
        layout.addWidget(label)

    def _build_panel(self, layout):
        layout.setContentsMargins(0, 0, theme.px(4), 0)
        layout.setSpacing(theme.px(6))
        self._heading(layout, "LANDMARKS")
        self.solve_label = QtWidgets.QLabel()
        self.solve_label.setObjectName("referenceSolve")
        self.solve_label.setWordWrap(True)
        self.solve_label.setProperty("role", "hint")
        layout.addWidget(self.solve_label)
        self.pairs_tree = QtWidgets.QTreeWidget()
        self.pairs_tree.setObjectName("referenceLandmarks")
        self.pairs_tree.setHeaderLabels(["#", "Photo", "Mesh", "Error"])
        self.pairs_tree.setRootIsDecorated(False)
        header = self.pairs_tree.header()
        header.setStretchLastSection(False)
        for column in (0, 1, 3):
            header.setSectionResizeMode(column, QtWidgets.QHeaderView.ResizeToContents)
        header.setSectionResizeMode(2, QtWidgets.QHeaderView.Stretch)
        self.pairs_tree.setMinimumHeight(theme.px(90))
        self.pairs_tree.setMaximumHeight(theme.px(170))
        self.pairs_tree.setToolTip("Landmark pairs: a point on the photo and the same point on the mesh, and how far (photo "
                                   "pixels) the solved camera puts the mesh point from it. Red: a pair to fix first.")
        layout.addWidget(self.pairs_tree)
        row = QtWidgets.QHBoxLayout()
        row.addStretch(1)
        clear = QtWidgets.QToolButton()
        clear.setText("Clear")
        clear.setProperty("role", "chip")
        clear.setToolTip("Delete every landmark of this photo (undoable)")
        clear.clicked.connect(lambda: self.clear_landmarks())
        row.addWidget(clear)
        layout.addLayout(row)
        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        self.overlay_box = QtWidgets.QComboBox()
        self.overlay_box.setObjectName("referenceOverlay")
        for key, label in (("outline", "Outline"), ("wireframe", "Wireframe"), ("off", "Off")):
            self.overlay_box.addItem(label, key)
        self.overlay_box.setToolTip("The mesh drawn over the photo through the camera: its outline or its wireframe")
        self.overlay_box.currentIndexChanged.connect(lambda *_: self._overlay_changed())
        form.addRow("Mesh over photo", self.overlay_box)
        self.opacity = widgets.SoftSlider(70, 0, 100, integer=True, minimum=0, maximum=100, key=None,
                                          label="Opacity", suffix="%")
        self.opacity.setObjectName("referenceOverlayOpacity")
        self.opacity.setToolTip("How opaque the mesh over the photo is")
        self.opacity.dragged.connect(lambda *_: self.photo_view.update())
        self.opacity.committed.connect(lambda *_: self.photo_view.update())
        form.addRow(self.opacity)  # the name inside the field
        layout.addLayout(form)

        self._heading(layout, "CAMERA")
        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        self.focal = widgets.SoftSlider(self.ref.DEFAULT_FOCAL, self.ref.FOCAL_RANGE[0], self.ref.FOCAL_RANGE[1],
                                        minimum=2.5, maximum=3500.0, key=None, label="Focal Length", suffix="mm")
        self.focal.setObjectName("referenceFocal")
        self.focal.setToolTip("The photo camera's focal length (mm): the landmarks solve it (6 or more pairs); set it "
                              "by hand when you know the lens (Keep focal length turns on, the camera follows)")
        self.focal.dragged.connect(lambda v: self.set_focal(v, final=False))
        self.focal.committed.connect(lambda v: self.set_focal(v))
        self.focal.menu_hook = lambda menu_: menu_.addAction(
            "Reset Focal Length (%g mm)" % self.ref.DEFAULT_FOCAL, self.reset_focal)
        form.addRow(self.focal)
        layout.addLayout(form)
        row = QtWidgets.QHBoxLayout()
        self.keep_focal = QtWidgets.QCheckBox("Keep focal length")
        self.keep_focal.setObjectName("referenceKeepFocal")
        self.keep_focal.setToolTip("Solve the position and rotation only, keeping the focal length (4 pairs are "
                                   "enough; else 6, and the focal length is solved too)")
        self.keep_focal.toggled.connect(self.set_keep_focal)
        row.addWidget(self.keep_focal)
        row.addStretch(1)
        self.lock = QtWidgets.QToolButton()
        self.lock.setText("Lock")
        self.lock.setObjectName("referenceLock")
        self.lock.setCheckable(True)
        self.lock.setProperty("role", "chip")
        self.lock.setToolTip("Lock the camera where it is (its position, rotation and focal length): the landmarks "
                             "no longer move it, nor does tumbling through it in a view")
        self.lock.toggled.connect(self.set_locked)
        row.addWidget(self.lock)
        layout.addLayout(row)

        # Guides from Photos (the Guides stage only).
        self.guides_panel = QtWidgets.QWidget()
        self.guides_panel.setObjectName("referenceGuidesPanel")
        outer = layout
        layout = QtWidgets.QVBoxLayout(self.guides_panel)
        layout.setContentsMargins(0, theme.px(6), 0, 0)
        layout.setSpacing(theme.px(6))
        outer.addWidget(self.guides_panel)

        self._heading(layout, "FLOW AND MASK")
        self.flow_label = QtWidgets.QLabel()
        self.flow_label.setObjectName("referenceFlowLabel")
        self.flow_label.setWordWrap(True)
        self.flow_label.setProperty("role", "hint")
        layout.addWidget(self.flow_label)
        row = QtWidgets.QHBoxLayout()
        clear = QtWidgets.QToolButton()
        clear.setText("Clear Arrows")
        clear.setObjectName("referenceClearArrows")
        clear.setProperty("role", "chip")
        clear.setToolTip("Forget this photo's flow arrows (undoable)")
        clear.clicked.connect(lambda: self.clear_flow())
        row.addWidget(clear)
        clear = QtWidgets.QToolButton()
        clear.setText("Clear Touch-ups")
        clear.setObjectName("referenceClearMask")
        clear.setProperty("role", "chip")
        clear.setToolTip("Forget this photo's mask touch-ups (undoable)")
        clear.clicked.connect(lambda: self.clear_mask())
        row.addWidget(clear)
        row.addStretch(1)
        layout.addLayout(row)
        row = QtWidgets.QHBoxLayout()
        self.mask_tint = QtWidgets.QCheckBox("Mask tint")
        self.mask_tint.setObjectName("referenceMaskTint")
        self.mask_tint.setToolTip("Tint the hair mask the guides are grown in (automatic and refined, or the mask image, "
                                  "with the touch-ups)")
        self.mask_tint.toggled.connect(lambda _: self._preview_wanted())
        row.addWidget(self.mask_tint)
        self.field_lines = QtWidgets.QCheckBox("Flow field")
        self.field_lines.setObjectName("referenceFieldLines")
        self.field_lines.setToolTip("The strand directions read from the photo (with the arrows), as short lines")
        self.field_lines.toggled.connect(lambda _: self._preview_wanted())
        row.addWidget(self.field_lines)
        row.addStretch(1)
        layout.addLayout(row)
        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        self.brush = widgets.SoftSlider(14, 2, 120, integer=True, minimum=2, maximum=120, key=None,
                                        label="Brush", suffix="px")
        self.brush.setObjectName("referenceBrushSize")
        self.brush.setToolTip("The mask brush's radius on screen")
        form.addRow(self.brush)
        masks = QtWidgets.QHBoxLayout()
        self.mask_label = QtWidgets.QLabel()
        self.mask_label.setObjectName("referenceMaskLabel")
        masks.addWidget(self.mask_label, 1)
        mask_menu = QtWidgets.QMenu(self)
        mask_menu.addAction("Auto (a plain backdrop)", lambda: self.set_mask_image(None))
        mask_menu.addAction("Pick Image...", lambda: self.pick_mask_image())
        pick = QtWidgets.QToolButton()
        pick.setText("Mask")
        pick.setObjectName("referenceMaskImage")
        pick.setProperty("role", "chip")
        pick.setMenu(mask_menu)
        pick.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        pick.setToolTip("Where the hair is: automatic on a plain backdrop, or an image (white where the hair is)")
        masks.addWidget(pick)
        form.addRow("Mask", masks)
        layout.addLayout(form)

        self._heading(layout, "GENERATE")
        self.use_photo = QtWidgets.QCheckBox("Use this photo")
        self.use_photo.setObjectName("referenceUsePhoto")
        self.use_photo.setChecked(True)
        self.use_photo.setToolTip("Grow guides from this photo too")
        self.use_photo.toggled.connect(self._use_changed)
        layout.addWidget(self.use_photo)
        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        self.gen_symmetry = QtWidgets.QComboBox()
        self.gen_symmetry.setObjectName("referenceSymmetry")
        for label, key in (("Auto", "auto"), ("None", "none"), ("World X", "x"), ("Topology", "topology")):
            self.gen_symmetry.addItem(label, key)
        self.gen_symmetry.setToolTip("Auto: the scalp's own symmetry when it has one. With symmetry, one three-quarter "
                                     "photo covers both sides and guides come in mirrored pairs.")
        form.addRow("Symmetry", self.gen_symmetry)
        self.gen_count = QtWidgets.QSpinBox()
        self.gen_count.setObjectName("referenceGuideCount")
        self.gen_count.setRange(10, 20000)
        self.gen_count.setValue(300)
        self.gen_count.setToolTip("Roots spread over the whole scalp; those the photos show no hair over get none")
        form.addRow("Guide count", self.gen_count)
        self.gen_length = QtWidgets.QDoubleSpinBox()
        self.gen_length.setObjectName("referenceMaxLength")
        self.gen_length.setRange(0.0, 100000.0)
        self.gen_length.setDecimals(2)
        self.gen_length.setSpecialValueText("Auto")
        self.gen_length.setToolTip("The longest hair, in scene units (Auto: as far as the photos show it reaching). "
                                   "Guides stop earlier where the photos show the hair ending.")
        self.gen_length.valueChanged.connect(lambda *_: self._invalidate_preview())
        form.addRow("Max length", self.gen_length)
        layout.addLayout(form)
        # Generate and Measured Look sit in the window's bottom row (always in view, beside Close).
        self.generate_button = QtWidgets.QPushButton("Generate")
        self.generate_button.setObjectName("referenceGenerate")
        self.generate_button.setProperty("role", "primary")
        self.generate_button.setToolTip("Grow guides from the photos into the fromReference group (again: replaced). One "
                                        "undo step.")
        self.generate_button.clicked.connect(lambda: self.generate())
        self.look_button = QtWidgets.QPushButton("Measured Look")
        self.look_button.setObjectName("referenceApplyLook")
        self.look_button.setToolTip("The color, clumps, curls, flyaways, frizz, color variation and thinning measured on "
                                    "the photos, shown against the graph's before they are set (in the panel above "
                                    "Generate)")
        self.look_button.clicked.connect(lambda: self.preview_look())
        # Apply Measured Look's preview (in place, no dialog): each node's values, now and measured;
        # the checked nodes are set (added where the graph has none) in one undo step.
        self.look_panel = QtWidgets.QWidget()
        self.look_panel.setObjectName("referenceLookPanel")
        panel = QtWidgets.QVBoxLayout(self.look_panel)
        panel.setContentsMargins(0, 0, 0, 0)
        self.look_tree = QtWidgets.QTreeWidget()
        self.look_tree.setObjectName("referenceLookPreview")
        self.look_tree.setColumnCount(3)
        self.look_tree.setHeaderLabels(["Node / param", "Now", "Measured"])
        self.look_tree.setMinimumHeight(theme.px(160))
        panel.addWidget(self.look_tree)
        buttons = QtWidgets.QHBoxLayout()
        apply_ = QtWidgets.QPushButton("Apply")
        apply_.setObjectName("referenceLookApply")
        apply_.setProperty("role", "primary")
        apply_.setToolTip("Set the checked nodes' values (a node the graph lacks is added in the usual order). One undo "
                          "step.")
        apply_.clicked.connect(lambda: self.apply_look())
        cancel = QtWidgets.QPushButton("Cancel")
        cancel.setObjectName("referenceLookCancel")
        cancel.clicked.connect(lambda: self.look_panel.setVisible(False))
        buttons.addStretch(1)
        buttons.addWidget(apply_)
        buttons.addWidget(cancel)
        panel.addLayout(buttons)
        self.look_panel.setVisible(False)
        layout.addWidget(self.look_panel)
        outer.addStretch(1)

    # ---- Photos -----------------------------------------------------------------------------------

    def photos(self):
        """The reference cameras, in the tabs' order."""
        try:
            return self.ref.list_references()
        except (RuntimeError, ValueError):
            return []

    def refresh_photos(self, select=None):
        """The tabs from the scene's reference cameras (each one's stored state read the first time;
        one gone loses its editor). `select`: the camera to show."""
        cameras = self.photos()
        for cam in list(self.editors):
            if cam not in cameras:
                self.editors.pop(cam, None)
                self.stored.pop(cam, None)
                self.solves.pop(cam, None)
        for cam in cameras:
            if cam not in self.editors:
                self._load(cam)
        current = select or self.camera
        if current not in cameras:
            current = cameras[0] if cameras else None
        self.tabs.blockSignals(True)
        while self.tabs.count():
            self.tabs.removeTab(0)
        for cam in cameras:
            index = self.tabs.addTab(cam.split("|")[-1])
            self.tabs.setTabData(index, cam)
        if current:
            self.tabs.setCurrentIndex(cameras.index(current))
        self.tabs.blockSignals(False)
        self.select_photo(current)

    def _load(self, camera):
        editor = self.gl.ReferencePhotoEditor()
        text = self.fr.photo_state(self.groom, camera) if self.groom else "{}"
        editor.reset_json(text)
        self.editors[camera] = editor
        self.stored[camera] = text

    def _tab_changed(self):
        index = self.tabs.currentIndex()
        self.select_photo(self.tabs.tabData(index) if index >= 0 else None)

    def select_photo(self, camera):
        """Shows a photo (its camera's name, or None)."""
        if camera:
            long_name = self.ref._camera(camera)
            camera = next((c for c in self.photos() if self.ref._camera(c) == long_name), long_name)
            if camera not in self.editors:
                self._load(camera)
            index = next((i for i in range(self.tabs.count()) if self.tabs.tabData(i) == camera), -1)
            if index >= 0 and index != self.tabs.currentIndex():
                self.tabs.blockSignals(True)
                self.tabs.setCurrentIndex(index)
                self.tabs.blockSignals(False)
        self.camera = camera
        self._hover = None
        self._overlay = None
        self._arrows = None
        if camera:
            self.photo_view.set_photo(self.ref.image_path(camera), self.photo_size())
            self.use_photo.blockSignals(True)
            self.use_photo.setChecked(camera not in self.unused)
            self.use_photo.blockSignals(False)
            if camera not in self.solves:
                self._solve(apply=False)
        else:
            self.photo_view.set_photo(None, (0, 0))
        self.remove_button.setEnabled(bool(camera))
        self._redraw()
        if self.mask_tint.isChecked() or self.field_lines.isChecked():
            self._preview_wanted()

    def current_photo(self):
        return self.camera

    def photo_size(self, camera=None):
        camera = camera or self.camera
        if not camera:
            return (0, 0)
        return (cmds.getAttr(camera + ".glRefWidth"), cmds.getAttr(camera + ".glRefHeight"))

    def editor(self):
        """The current photo's ReferencePhotoEditor, or None."""
        return self.editors.get(self.camera) if self.camera else None

    def import_photo(self, path=None):
        """A photo as a new reference camera, shown; without `path`, a file dialog asks for one (a
        click only). Returns the camera (one Maya undo step), or None."""
        from groomlab.ui import menu
        if path is None:
            start = cmds.workspace(query=True, rootDirectory=True)
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Import Reference Photo", start, PHOTO_FILTER)
            if not path:
                return None
        menu.ensure_scene_saved("reference photos")
        try:
            camera = self.ref.import_reference(path, groom=self.groom)
        except (RuntimeError, ValueError) as e:
            self.message.setText(str(e).strip())
            return None
        self.refresh_photos(select=camera)
        self.message.setText("%s imported: click a feature on the photo, then the same feature on the mesh (4 pairs "
                             "line the camera up)." % camera.split("|")[-1])
        return self.camera

    def remove_photo(self, camera=None):
        """Removes a photo's camera (the current one by default; one Maya undo step)."""
        camera = camera or self.camera
        if not camera:
            return False
        try:
            self.ref.remove_reference(camera)
        except (RuntimeError, ValueError) as e:
            self.message.setText(str(e).strip())
            return False
        self.refresh_photos()
        self.message.setText("Removed %s (Maya's undo brings it back)." % camera.split("|")[-1])
        return True

    def _use_changed(self, on):
        if self.camera:
            (self.unused.discard if on else self.unused.add)(self.camera)
        self._redraw()

    # ---- The scalp and the mesh view ----------------------------------------------------------------

    def _refresh_mesh(self):
        import groomlab
        shape = groomlab.scalp_mesh(self.groom) if self.groom and cmds.objExists(self.groom) else None
        mesh = cmds.listRelatives(shape, parent=True, fullPath=True)[0] if shape else None
        changed = mesh != self.mesh
        self.mesh = mesh
        self.mesh_view.mesh = mesh
        self.mesh_title.setText("Mesh: " + (mesh.split("|")[-1] if mesh else "(the groom has no scalp)"))
        if changed:
            self._scalp = None
            self._overlay = None
            if self.mesh_view.editor:
                self.mesh_view.frame()

    def scalp_core(self):
        """The scalp as a groomlab_core ScalpMesh (its polygons kept: the wireframe's edges), cached."""
        import groomlab
        from maya.api import OpenMaya as om
        shape = groomlab.scalp_mesh(self.groom) if self.groom else None
        if not shape:
            return None
        if self._scalp is None or self._scalp[0] != shape:
            scalp = self.fr._mesh(shape)
            fn = om.MFnMesh(om.MSelectionList().add(shape).getDagPath(0))
            counts, _ = fn.getTriangles()
            try:
                scalp.set_polygons(list(counts))
            except Exception:
                pass
            self._scalp = (shape, scalp)
        return self._scalp[1]

    def ensure_views(self):
        """The mesh view (once the window is part of Maya's UI)."""
        if not self.mesh_view.ensure():
            return
        QtWidgets.QApplication.processEvents()
        self.mesh_view.watch(self, "mesh")
        self._keys.watch(self.mesh_view.holder)
        self.mesh_view.frame()
        from maya.api import OpenMaya as om
        self._scene_job = [om.MSceneMessage.addCallback(when, self._scene_cleared)
                           for when in (om.MSceneMessage.kBeforeNew, om.MSceneMessage.kBeforeOpen)]
        self._scene_job += [om.MEventMessage.addEventCallback(event, self._undone) for event in ("Undo", "Redo")]
        self._redraw()

    def view(self):
        """The mesh view's M3dView."""
        return self.mesh_view.view()

    def pick(self, x, y):
        """The scalp's point under the mesh view's port pixel (x, y) (transfer.pick_mesh's dict), or None."""
        return self.mesh_view.pick(x, y) if self.mesh_view.editor else None

    # ---- Modes and overlays -----------------------------------------------------------------------

    def stage(self):
        return next((s for s, b in self.stage_buttons.items() if b.isChecked()), "align")

    def set_stage(self, stage):
        """Align (landmarks only) or Guides (Guides from Photos: the flow, mask and Generate too)."""
        if stage not in STAGES:
            raise ValueError("stage %r: one of %s" % (stage, ", ".join(STAGES)))
        self.stage_buttons[stage].setChecked(True)
        guides = stage == "guides"
        for button in self.mode_buttons.values():
            button.setVisible(guides)
        for widget in (self.guides_panel, self.generate_button, self.look_button):
            widget.setVisible(guides)
        if not guides:
            self.mode_buttons["landmarks"].setChecked(True)
            self.look_panel.setVisible(False)
        self._hover = None
        self._redraw()

    def mode(self):
        return next((m for m, b in self.mode_buttons.items() if b.isChecked()), "landmarks")

    def set_mode(self, mode):
        """The photo view's tool; one of Guides' (the flow, the mask) moves to the Guides stage."""
        if mode not in MODES:
            raise ValueError("mode %r: one of %s" % (mode, ", ".join(MODES)))
        if mode != "landmarks" and self.stage() != "guides":
            self.set_stage("guides")
        self.mode_buttons[mode].setChecked(True)
        self._hover = None
        self._redraw()

    def set_focal(self, focal, final=True):
        """The current photo's focal length set by hand (mm): Keep focal length turns on and the camera
        lines up again keeping it (outside the undo queue, as the solves). A locked camera keeps its own."""
        if not self._need_photo():
            return False
        if self.ref.is_locked(self.camera):
            self.message.setText("%s is locked: unlock it to change its focal length" % self.camera.split("|")[-1])
            return False
        with mesh_views.no_undo():
            self.ref.set_reference_focal(self.camera, focal)
        self._show_camera()
        self._solve(apply=True, find_bad=final)
        if final:
            self._store()
        self._redraw()
        return True

    def reset_focal(self):
        """The focal length back to the default lens and the slider to its starting range (a solve gone
        wrong); Keep focal length turns on, as with any focal length set by hand."""
        self.focal.setRange(*self.ref.FOCAL_RANGE)
        return self.set_focal(self.ref.DEFAULT_FOCAL)

    def set_keep_focal(self, on):
        """Keep focal length (stored on the camera): the solve keeps it, else 6 or more pairs solve it."""
        if not self._need_photo():
            return
        with mesh_views.no_undo():
            self.ref.set_keep_focal(self.camera, on)
        self._edited()

    def set_locked(self, on):
        """Lock the current photo's camera (one Maya undo step): the landmarks no longer move it."""
        if not self._need_photo():
            return
        self.ref.lock_reference(self.camera, on)
        self._show_camera()
        if not on:
            self._edited(store=False)

    def _show_camera(self):
        """The camera controls from the current photo's camera."""
        camera = self.camera if self.camera and cmds.objExists(self.camera) else None
        locked = bool(camera) and self.ref.is_locked(camera)
        for widget, value in ((self.keep_focal, bool(camera) and self.ref.keep_focal(camera)), (self.lock, locked)):
            if widget.isChecked() != value:
                widget.blockSignals(True)
                widget.setChecked(value)
                widget.blockSignals(False)
        for widget in (self.keep_focal, self.lock, self.focal):
            widget.setEnabled(bool(camera))
        self.focal.setEnabled(bool(camera) and not locked)
        if camera and not self.focal.busy():
            focal = cmds.getAttr(self.ref._shape(camera) + ".focalLength")
            if abs(self.focal.value() - focal) > 1e-4:
                self.focal.setValue(focal)

    def brush_size(self):
        """The mask brush's radius in logical pixels on screen."""
        return float(self.brush.value())

    def overlay(self):
        return self.overlay_box.currentData()

    def set_overlay(self, kind=None, opacity=None, mask=None, field=None):
        """The overlays over the photo: the mesh ("outline", "wireframe", "off"), its opacity (0-1),
        the mask tint and the flow field lines."""
        if kind is not None:
            index = self.overlay_box.findData(kind)
            if index < 0:
                raise ValueError("overlay %r: one of %s" % (kind, ", ".join(OVERLAYS)))
            self.overlay_box.setCurrentIndex(index)
        if opacity is not None:
            self.opacity.setValue(int(round(100 * opacity)))
        if mask is not None:
            self.mask_tint.setChecked(bool(mask))
        if field is not None:
            self.field_lines.setChecked(bool(field))

    def overlay_opacity(self):
        return self.opacity.value() / 100.0

    def _overlay_changed(self):
        self._overlay = None
        self.photo_view.update()

    def _camera_for_overlay(self):
        solve = self.solves.get(self.camera) or {}
        if solve.get("ok"):
            return {k: solve[k] for k in ("rotation", "position", "focal_px", "width", "height")}
        return self.ref.camera_dict(self.camera)

    def overlay_lines(self):
        """The mesh over the current photo as QLineF (photo pixels), cached until the camera moves."""
        if not self.camera or self.overlay() == "off":
            return []
        scalp = self.scalp_core()
        if scalp is None:
            return []
        camera = self._camera_for_overlay()
        key = (self.camera, self.overlay(), repr(camera.get("matrix") or (camera.get("position"), camera.get("rotation"))),
               camera.get("focal_px") or camera.get("focal_mm"))
        if self._overlay is None or self._overlay[0] != key:
            segments = self.gl.reference_mesh_overlay(scalp, camera, self.overlay() == "outline")
            self._overlay = (key, [QtCore.QLineF(*s) for s in segments])
        return self._overlay[1]

    def arrows(self):
        """The current photo's flow arrows as drawn (core's reference_arrow), cached per revision."""
        editor = self.editor()
        if editor is None:
            return []
        if self._arrows is None or self._arrows[0] != (self.camera, editor.revision):
            head = self.photo_size()[0] / 40.0
            self._arrows = ((self.camera, editor.revision),
                            [self.gl.reference_arrow(s["points"], head if s["directed"] else 0.0) for s in editor.strokes])
        return self._arrows[1]

    # ---- The preview: the mask and the field ------------------------------------------------------

    def _preview_key(self):
        editor = self.editor()
        camera = self.ref.camera_dict(self.camera)
        return (self.camera, editor.revision if editor else 0, self.fr.view_mask(self.groom, self.camera),
                tuple(camera["matrix"]), camera["focal_mm"], self.gen_length.value())

    def _invalidate_preview(self):
        if self.mask_tint.isChecked() or self.field_lines.isChecked():
            self._preview_wanted()

    def _preview_wanted(self):
        """The preview again soon (the masks and fields take a moment), the view meanwhile as it is."""
        if not hasattr(self, "_preview_timer"):
            self._preview_timer = QtCore.QTimer(self)
            self._preview_timer.setSingleShot(True)
            self._preview_timer.timeout.connect(lambda: self.compute_preview())
        self._preview_timer.start(120)
        self.photo_view.update()

    def compute_preview(self):
        """The current photo's mask (as guides_from_reference works on it, the touch-ups in) and
        orientation field, for the tint and the field lines. Returns the preview dict, or None."""
        if not self.camera or not (self.mask_tint.isChecked() or self.field_lines.isChecked()):
            return None
        scalp = self.scalp_core()
        editor = self.editor()
        if scalp is None or editor is None:
            return None
        key = self._preview_key()
        if self._preview is not None and self._preview[0] == key:
            return self._preview[1]
        try:
            view = self.fr._camera_view(self.groom, self.camera, mask=self.fr.view_mask(self.groom, self.camera),
                                        hints=editor.strokes, masks=editor.mask_strokes)
            length = self.gen_length.value()
            preview = self.gl.preview_reference_view(scalp, view, max_length=length if length > 0 else 0.0)
        except (RuntimeError, ValueError) as e:
            self.message.setText("Mask preview: %s" % str(e).strip())
            return None
        alpha = QtGui.QImage(preview["mask"], preview["width"], preview["height"], preview["width"],
                             QtGui.QImage.Format_Alpha8).copy()
        tint = QtGui.QImage(preview["width"], preview["height"], QtGui.QImage.Format_ARGB32_Premultiplied)
        tint.fill(HAIR_TINT)
        painter = QtGui.QPainter(tint)
        painter.setCompositionMode(QtGui.QPainter.CompositionMode_DestinationIn)
        painter.drawImage(0, 0, alpha)
        painter.end()
        preview["tint"] = tint
        preview["qlines"] = [QtCore.QLineF(*l) for l in preview["lines"]]
        preview["hair_share"] = sum(bytearray(preview["mask"])) / (255.0 * max(1, len(preview["mask"])))
        self._preview = (key, preview)
        self.photo_view.update()
        return preview

    def preview(self):
        """The preview computed for the current state, or None (asked for when it is out of date)."""
        if self._preview is None or not self.camera:
            if self.camera:
                self._preview_wanted_later()
            return None
        if self._preview[0] != self._preview_key():
            self._preview_wanted_later()
            return self._preview[1] if self._preview[0][0] == self.camera else None
        return self._preview[1]

    def _preview_wanted_later(self):
        timer = getattr(self, "_preview_timer", None)
        if timer is None or not timer.isActive():
            QtCore.QTimer.singleShot(0, self._preview_wanted)

    # ---- Landmarks ----------------------------------------------------------------------------------

    def _side(self, side):
        side = {"left": "photo", "right": "mesh", "image": "photo"}.get(side, side)
        if side not in SIDES:
            raise ValueError("landmark side %r: photo/left or mesh/right" % (side,))
        return side

    def _point(self, side, point):
        if side == "mesh":
            from groomlab import transfer
            return transfer.closest_on_mesh(self.mesh, point) if self.mesh else list(point)
        return (float(point[0]), float(point[1]))

    def _need_photo(self):
        if self.editor() is None:
            raise ValueError("Import a photo first")
        return self.editor()

    def add_landmark(self, side, point):
        """A landmark at the end of a side's list: (u, v) photo pixels, or (x, y, z) (snapped onto the
        scalp). Returns its index. One undo step of the window's."""
        side = self._side(side)
        index = self._need_photo().add_landmark(side, self._point(side, point))
        self._edited()
        return index

    def delete_landmark(self, side, index):
        """Deletes a landmark and its partner; returns the indices deleted."""
        gone = self._need_photo().remove_landmark(self._side(side), int(index))
        self._hover = None
        self._edited()
        return list(gone)

    def move_landmark(self, side, index, point, new_step=True):
        """A landmark moved (a mesh point snapped onto the scalp); one undo step (a drag's later moves
        join its first). The camera solves again."""
        side = self._side(side)
        self._need_photo().move_landmark(side, int(index), self._point(side, point), new_step)
        self._edited(store=new_step)

    def clear_landmarks(self):
        if self.editor() is not None:
            self.editor().clear_landmarks()
            self._edited()

    def landmarks(self, side):
        """A side's landmarks in order: [u, v] on the photo, [x, y, z] on the mesh."""
        editor = self.editor()
        if editor is None:
            return []
        side = self._side(side)
        return [[p.point.x, p.point.y] if side == "photo" else [p.point.x, p.point.y, p.point.z]
                for p in editor.points(side)]

    def pairs(self):
        """The complete pairs: [{"image": [u, v], "mesh": [x, y, z]}, ...]."""
        photo, mesh = self.landmarks("photo"), self.landmarks("mesh")
        return [{"image": photo[i], "mesh": mesh[i]} for i in range(min(len(photo), len(mesh)))]

    def solve_result(self):
        """The current photo's last solve (solve_reference_landmarks' dict), or None."""
        return self.solves.get(self.camera) if self.camera else None

    def photo_point_near(self, u, v, pixels):
        """The photo landmark within `pixels` (photo pixels) of (u, v), or None."""
        editor = self.editor()
        best, best_d = None, pixels * pixels
        for i, p in enumerate(editor.points("photo") if editor else []):
            d = (p.point.x - u) ** 2 + (p.point.y - v) ** 2
            if d <= best_d:
                best, best_d = i, d
        return best

    # ---- Flow arrows and mask touch-ups ------------------------------------------------------------

    def add_flow_stroke(self, points, directed=True):
        """A flow arrow (photo pixels, root first): one undo step. Returns its index."""
        index = self._need_photo().add_stroke([tuple(p) for p in points], directed)
        self._edited(solve=False)
        return index

    def erase_flow(self, path, radius=None):
        """Erases the arrows within `radius` photo pixels (a 50th of the photo's width) of `path`."""
        radius = radius if radius is not None else self.photo_size()[0] / 50.0
        gone = self._need_photo().erase_strokes([tuple(p) for p in path], radius)
        if gone:
            self._edited(solve=False)
        return gone

    def clear_flow(self):
        if self.editor() is not None:
            self.editor().clear_strokes()
            self._edited(solve=False)

    def add_mask_stroke(self, points, radius=None, include=True):
        """A mask touch-up (photo pixels; `radius` photo pixels, a 60th of the photo's width by
        default): hair (`include`) or backdrop. One undo step."""
        radius = radius if radius is not None else self.photo_size()[0] / 60.0
        index = self._need_photo().add_mask_stroke([tuple(p) for p in points], float(radius), bool(include))
        self._edited(solve=False)
        return index

    def clear_mask(self):
        if self.editor() is not None:
            self.editor().clear_mask_strokes()
            self._edited(solve=False)

    def set_mask_image(self, path):
        """The current photo's mask image (white where the hair is), or None: automatic. One Maya undo step."""
        if not self.camera:
            return False
        try:
            self.fr.set_view_mask(self.groom, self.camera, path)
        except (RuntimeError, ValueError) as e:
            self.message.setText(str(e).strip())
            return False
        self.stored[self.camera] = self.fr.photo_state(self.groom, self.camera)
        self._invalidate_preview()
        self._redraw()
        return True

    def pick_mask_image(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Hair Mask (white where the hair is)", "", PHOTO_FILTER)
        if path:
            self.set_mask_image(path)

    # ---- Undo ---------------------------------------------------------------------------------------

    def undo(self):
        """The current photo's last edit undone (the window's own stack)."""
        editor = self.editor()
        if editor is not None and editor.undo():
            self._hover = None
            self._edited()
            return True
        return False

    def redo(self):
        editor = self.editor()
        if editor is not None and editor.redo():
            self._hover = None
            self._edited()
            return True
        return False

    # ---- After an edit: store, solve, redraw ---------------------------------------------------------

    def _edited(self, store=True, solve=True):
        if solve:
            self._solve(apply=store)
        if store:
            self._store()
        self._redraw()
        if self.mask_tint.isChecked() or self.field_lines.isChecked():
            self._preview_wanted()

    def _solve(self, apply=True, find_bad=True):
        """The camera solved from the current photo's landmarks (core: solve_reference_landmarks);
        with `apply`, the Maya camera takes it (outside the undo queue: it follows the landmarks)."""
        editor = self.editor()
        if editor is None:
            return None
        width, height = self.photo_size()
        try:
            result = self.gl.solve_reference_landmarks(editor, width, height, self.ref.focal_px(self.camera),
                                                       self.ref.keep_focal(self.camera), self.ref.camera_dict(self.camera),
                                                       find_bad)
        except (RuntimeError, ValueError) as e:
            result = {"ok": False, "why": str(e), "errors": [], "bad": [], "projected": [], "pairs": editor.pair_count}
        self.solves[self.camera] = result
        self._overlay = None
        if apply and result.get("ok"):
            with mesh_views.no_undo():
                if not self.ref.apply_solved_camera(self.camera, result):
                    self.message.setText("%s is locked: unlock it (Lock) for the landmarks to move it"
                                         % self.camera.split("|")[-1])
        return result

    def _store(self):
        """The current photo's state on the camera and the groom, outside Maya's undo queue."""
        editor = self.editor()
        if editor is None or not self.groom:
            return
        text = editor.state_json()
        result = self.solves.get(self.camera) or {}
        with mesh_views.no_undo():
            self.fr.store_photo_state(self.groom, self.camera, text, result.get("errors") if result.get("ok") else None)
        self.stored[self.camera] = self.fr.photo_state(self.groom, self.camera)

    def _redraw(self):
        self._draw_mesh_markers()
        self._update_panel()
        self.photo_view.update()

    def hover_index(self, side):
        """The point the side lights: the hovered one, or its partner (a matched point's)."""
        editor = self.editor()
        if self._hover is None or editor is None:
            return -1
        hovered, i = self._hover
        if hovered == side or editor.matched(hovered, i):
            return i if i < editor.count(side) else -1
        return -1

    def set_hover(self, hover):
        """Lights a landmark and its partner: (side, index), or None."""
        if hover is not None:
            hover = (self._side(hover[0]), int(hover[1]))
        if hover != self._hover:
            self._hover = hover
            if self.mesh_view.editor:
                self.mesh_view.set_hover(self.hover_index("mesh") if self.mode() == "landmarks" else -1)
            self.photo_view.update()

    def _draw_mesh_markers(self):
        if not self.mesh_view.editor:
            return
        editor = self.editor()
        editing = editor is not None and self.isVisible()
        points = self.landmarks("mesh") if editing else []
        self.mesh_view.set_markers(points, [editor.matched("mesh", i) for i in range(len(points))] if editing else [],
                                   self.hover_index("mesh") if editing else -1)
        self.mesh_view.show(markers=editing)

    def marker_state(self):
        """What the mesh view's marker set draws (tests)."""
        return self.mesh_view.marker_state()

    def requirement(self):
        """Why Generate cannot run yet, or ""."""
        if not self.groom or not cmds.objExists(self.groom):
            return "No groom"
        if not self.mesh:
            return "The groom has no scalp"
        if not self.used_photos():
            return "Import a photo (and check Use this photo)"
        return ""

    def used_photos(self):
        return [c for c in self.photos() if c not in self.unused]

    def _update_panel(self):
        editor = self.editor()
        solve = self.solve_result() or {}
        self._show_camera()
        # The landmark table.
        rows = []
        if editor is not None:
            photo, mesh = self.landmarks("photo"), self.landmarks("mesh")
            errors, bad = solve.get("errors") or [], solve.get("bad") or []
            for i in range(max(len(photo), len(mesh))):
                rows.append((str(i + 1), "%.0f, %.0f" % tuple(photo[i]) if i < len(photo) else "-",
                             "%.1f, %.1f, %.1f" % tuple(mesh[i]) if i < len(mesh) else "-",
                             "%.1f px" % errors[i] if solve.get("ok") and i < len(errors) else "",
                             bool(solve.get("ok") and i < len(bad) and bad[i])))
        if rows != self._last_rows:
            self._last_rows = rows
            self.pairs_tree.clear()
            for row in rows:
                item = QtWidgets.QTreeWidgetItem(list(row[:4]))
                if row[4]:
                    for c in range(4):
                        item.setForeground(c, QtGui.QBrush(BAD))
                self.pairs_tree.addTopLevelItem(item)
        if editor is None:
            self.solve_label.setText("No photo yet: Import Photo... above.")
        elif solve.get("ok"):
            bad_count = sum(1 for b in solve.get("bad") or [] if b)
            focal_mm = solve["focal_px"] * cmds.getAttr(self.ref._shape(self.camera) + ".horizontalFilmAperture") * 25.4 / \
                max(1.0, solve["width"])
            text = "%s; camera lined up: %.2f px error, %.0f mm%s." % (
                editor.summary(), solve["rms"], focal_mm, " (solved)" if solve.get("focal_solved") else " (kept)")
            if bad_count:
                text += " %d pair%s in red to fix first." % (bad_count, "" if bad_count == 1 else "s")
            self.solve_label.setText(text)
        else:
            self.solve_label.setText("%s. %s" % (editor.summary(), solve.get("why") or ""))
        strokes = len(editor.strokes) if editor else 0
        touchups = len(editor.mask_strokes) if editor else 0
        self.flow_label.setText("%d flow arrow%s, %d mask touch-up%s." % (strokes, "" if strokes == 1 else "s", touchups,
                                                                        "" if touchups == 1 else "s"))
        mask = self.fr.view_mask(self.groom, self.camera) if (self.camera and self.groom) else None
        self.mask_label.setText(os.path.basename(mask) if mask else "Auto (a plain backdrop)")
        why = self.requirement()
        self.generate_button.setEnabled(not why)
        self.generate_button.setToolTip(why or "Grow guides from the photos into the fromReference group (again: "
                                               "replaced). One undo step.")
        has_look = bool(self.groom and cmds.objExists(self.groom) and self.fr.measured_look(self.groom))
        self.look_button.setEnabled(has_look)
        self.undo_button.setEnabled(bool(editor and editor.can_undo))
        self.undo_button.setToolTip(("Undo " + editor.undo_text + " (Ctrl+Z)") if editor and editor.can_undo else
                                    "Undo this photo's last edit (Ctrl+Z)")
        self.redo_button.setEnabled(bool(editor and editor.can_redo))
        self.redo_button.setToolTip(("Redo " + editor.redo_text + " (Ctrl+Y)") if editor and editor.can_redo else
                                    "Redo (Ctrl+Y)")
        hints = {"landmarks": "Landmarks: click a feature on the photo, then the same feature on the mesh (either order); "
                              "drag to adjust, Ctrl+click deletes a pair. 4 pairs line the camera up.",
                 "flow": "Flow: drag along the hair from root to tip; Ctrl+drag erases.",
                 "hair": "Hair: paint where the mask missed hair (Ctrl: backdrop). Mask tint shows the mask.",
                 "backdrop": "Backdrop: paint where the mask took backdrop for hair (Ctrl: hair)."}
        self.hint.setText(hints[self.mode()] + "  Wheel zooms, middle drag pans, Ctrl+Z / Ctrl+Y undo and redo.")
        self.status.setText(("%d photo%s" % (len(self.photos()), "" if len(self.photos()) == 1 else "s")) +
                            ("  —  " + why if why and self.stage() == "guides" else ""))

    # ---- The mouse in the photo view -------------------------------------------------------------------

    def press_photo(self, view, uv, ctrl):
        """A left press in the photo view at photo pixel `uv` (the view's drag set from it)."""
        editor = self.editor()
        if editor is None:
            return
        u, v = uv
        mode = self.mode()
        if mode == "landmarks":
            near = self.photo_point_near(u, v, mesh_views.PICK_PIXELS / view.scale)
            if ctrl:
                if near is not None:
                    self.delete_landmark("photo", near)
                view.drag = ["owned"]
                return
            if near is not None:
                view.drag = ["point", near, True]
                return
            index = self.add_landmark("photo", (u, v))
            view.drag = ["point", index, False]  # sliding the new point joins its add
        elif mode == "flow":
            view.drag = ["erase" if ctrl else "stroke", [(u, v)]]
        else:
            include = (mode == "hair") != ctrl
            view.drag = ["mask", [(u, v)], include, self.brush_size() / view.scale]
        view.update()

    def drag_photo(self, view, uv):
        drag = view.drag
        if not drag:
            return
        u, v = uv
        if drag[0] == "point":
            self.editor().move_landmark("photo", drag[1], (u, v), drag[2])
            drag[2] = False
            self._solve(apply=False, find_bad=False)  # live: the bad pairs and the Maya camera on release
            self._update_panel()
        elif drag[0] in ("stroke", "erase", "mask"):
            last = drag[1][-1]
            spacing = self.photo_size()[0] / 300.0 if drag[0] != "mask" else max(0.5, drag[3] * 0.3)
            if math.hypot(u - last[0], v - last[1]) >= spacing:
                drag[1].append((u, v))
        view.update()

    def release_photo(self, view, drag):
        kind = drag[0]
        if kind == "point":
            self._edited()
        elif kind == "stroke":
            points = drag[1]
            length = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(points, points[1:]))
            if len(points) >= 2 and length >= self.photo_size()[0] / 100.0:
                self.add_flow_stroke(points)
        elif kind == "erase":
            self.erase_flow(drag[1], max(self.photo_size()[0] / 200.0, mesh_views.PICK_PIXELS / view.scale))
        elif kind == "mask":
            self.add_mask_stroke(drag[1], drag[3], drag[2])

    def hover_photo(self, view, uv):
        if self.mode() != "landmarks":
            return
        near = self.photo_point_near(uv[0], uv[1], mesh_views.PICK_PIXELS / view.scale)
        if near is not None:
            self.set_hover(("photo", near))
        elif self._hover and self._hover[0] == "photo":
            self.set_hover(None)

    # ---- The mouse in the mesh view ----------------------------------------------------------------------

    def _view_event(self, side, widget, event):
        kind = event.type()
        if kind == QtCore.QEvent.Leave:
            self.set_hover(None)
            return False
        if kind not in (QtCore.QEvent.MouseButtonPress, QtCore.QEvent.MouseMove, QtCore.QEvent.MouseButtonRelease,
                        QtCore.QEvent.MouseButtonDblClick):
            return False
        if self.mode() != "landmarks" or self.editor() is None or not self.mesh_view.editor:
            return False
        mods = to_int(event.modifiers())
        alt = bool(mods & to_int(QtCore.Qt.AltModifier))
        ctrl = bool(mods & to_int(QtCore.Qt.ControlModifier))
        left = event.button() == QtCore.Qt.LeftButton
        x, y = self.mesh_view.port(widget, event_pos(event))
        if kind in (QtCore.QEvent.MouseButtonPress, QtCore.QEvent.MouseButtonDblClick):
            if not left or alt:
                return False
            self._owned_press = True
            near = self.mesh_view.point_near(self.landmarks("mesh"), x, y)
            if ctrl:
                if near is not None:
                    self.delete_landmark("mesh", near)
                return True
            if near is not None:
                self._drag = [near, True]
                return True
            hit = self.pick(x, y)
            if hit:
                index = self.add_landmark("mesh", hit["point"])
                self._drag = [index, False]
            return True
        if kind == QtCore.QEvent.MouseMove:
            buttons = to_int(event.buttons())
            if self._drag and buttons & to_int(QtCore.Qt.LeftButton):
                hit = self.pick(x, y)
                if hit:
                    index, first = self._drag
                    self.editor().move_landmark("mesh", index, hit["point"], first)
                    self._drag[1] = False
                    self._solve(apply=False, find_bad=False)
                    self._draw_mesh_markers()
                    self.photo_view.update()
                return True
            if not buttons:
                near = self.mesh_view.point_near(self.landmarks("mesh"), x, y)
                if near is not None:
                    self.set_hover(("mesh", near))
                elif self._hover and self._hover[0] == "mesh":
                    self.set_hover(None)
            return self._owned_press
        if self._owned_press and left:
            self._owned_press = False
            if self._drag:
                self._drag = None
                self._edited()
            return True
        return False

    # ---- Generate and the measured look --------------------------------------------------------------

    def generate(self):
        """Grows the guides from the used photos (from_reference.generate: one Maya undo step).
        Returns its result, or None with why in the message line."""
        why = self.requirement()
        if why:
            self.message.setText(why)
            return None
        length = self.gen_length.value()
        views = self.used_photos()
        try:
            with widgets.busy([self.generate_button, self.look_button], self.message,
                              "Generating %d guides from %d photo(s)..." % (self.gen_count.value(), len(views))):
                result = self.fr.generate(self.groom, views, guide_count=self.gen_count.value(),
                                          symmetry=self.gen_symmetry.currentData(),
                                          max_length=length if length > 0 else None)
        except (RuntimeError, ValueError) as e:
            self.message.setText(str(e).strip())
            return None
        self.last_result = result
        self.message.setText("%d guides in fromReference (symmetry: %s; %d roots had no hair over them, %d were not "
                             "seen). Brush them, or Generate again after drawing flow arrows." % (
                                 len(result["guide_ids"]), result["symmetry"], result["roots_bald"],
                                 result["roots_unseen"]))
        self._redraw()
        return result

    def preview_look(self):
        """Apply Measured Look, step one: the measured values against the graph's, in place."""
        try:
            with widgets.busy([self.generate_button, self.look_button], self.message,
                              "Measuring the look on the photos..."):
                rows = self.fr.measured_look_preview(self.groom) if self.groom else []
        except (RuntimeError, ValueError) as e:
            self.message.setText(str(e).strip())
            rows = []
        self.look_tree.clear()
        items = {}

        def text(v):
            if v is None:
                return ""
            if isinstance(v, float):
                return "%.3g" % v
            if isinstance(v, (list, tuple)):
                return ", ".join(text(x) for x in v)
            if isinstance(v, dict):
                return "(map)"
            return str(v)
        for row in rows:
            item = items.get(row["type"])
            if item is None:
                item = QtWidgets.QTreeWidgetItem([row["node"] or "%s (added)" % row["type"], "", ""])
                item.setData(0, QtCore.Qt.UserRole, row["type"])
                item.setFlags(item.flags() | QtCore.Qt.ItemIsUserCheckable)
                item.setCheckState(0, QtCore.Qt.Checked)
                self.look_tree.addTopLevelItem(item)
                items[row["type"]] = item
            item.addChild(QtWidgets.QTreeWidgetItem([row["param"], text(row["current"]), text(row["value"])]))
        self.look_tree.expandAll()
        self.look_tree.resizeColumnToContents(0)
        self.look_panel.setVisible(bool(rows))
        if not rows:
            self.message.setText("Nothing measured yet: Generate first.")
        else:
            self.message.setText("Measured %d node(s): check the ones to set, then Apply." % len(items))
        return rows

    def look_types(self):
        """The node types checked in the preview."""
        return [self.look_tree.topLevelItem(i).data(0, QtCore.Qt.UserRole) for i in range(self.look_tree.topLevelItemCount())
                if self.look_tree.topLevelItem(i).checkState(0) == QtCore.Qt.Checked]

    def apply_look(self):
        """Apply Measured Look, step two: the checked nodes set (one Maya undo step)."""
        try:
            with widgets.busy([self.generate_button, self.look_button], self.message, "Setting the measured look..."):
                done = self.fr.apply_measured_look(self.groom, types=self.look_types())
        except (RuntimeError, ValueError) as e:
            self.message.setText(str(e).strip())
            return None
        self.look_panel.setVisible(False)
        self.message.setText("Measured look set on %s (one undo step)." % ", ".join(sorted(done.values())))
        return done

    # ---- Lifetime ------------------------------------------------------------------------------------

    def showEvent(self, event):
        super(ReferenceWindow, self).showEvent(event)
        if not self.mesh_view.editor:
            QtCore.QTimer.singleShot(0, self._show_views)
        else:
            self._redraw()

    def _show_views(self):
        if widgets.alive(self) and self.isVisible():
            self.ensure_views()

    def _undone(self, *_):
        # After Maya's undo or redo has finished (deferred): the window follows the scene.
        QtCore.QTimer.singleShot(0, self.follow_scene)

    def follow_scene(self):
        """The window made to match the scene again after Maya's undo or redo: photos imported or
        removed, the scalp, what is stored for each photo (an editor whose stored state changed is
        read again, its own history dropped), the guides and the measured look."""
        if not widgets.alive(self):
            return
        if not self.groom or not cmds.objExists(self.groom):
            self.message.setText("The groom is gone")
            return
        self._refresh_mesh()
        for cam in list(self.editors):
            if not cmds.objExists(cam):
                continue
            try:
                text = self.fr.photo_state(self.groom, cam)
            except (RuntimeError, ValueError):
                continue
            if text != self.stored.get(cam):
                self.editors[cam].reset_json(text)
                self.stored[cam] = text
                self.solves.pop(cam, None)
        self.refresh_photos()

    def _scene_cleared(self, *_):
        try:
            self.teardown(nodes=False, callbacks=False)
        except Exception as e:
            cmds.warning("GroomLab: closing the Photos window: %s" % e)
        QtCore.QTimer.singleShot(0, self.close)

    def teardown(self, nodes=True, callbacks=True):
        """Deletes the mesh view and (`nodes`) its camera and markers (closing does it)."""
        if self._scene_job and callbacks:
            from maya.api import OpenMaya as om
            for callback in self._scene_job:
                om.MMessage.removeCallback(callback)
            self._scene_job = None
        if self.mesh_view.editor:
            mesh_views.MeshView.hand_back_active_view()
        self.mesh_view.teardown(nodes=nodes)
        if nodes and cmds.objExists(UI_GROUP):
            with mesh_views.no_undo():
                cmds.delete(UI_GROUP)

    def closeEvent(self, event):
        global _window
        self.teardown()
        mesh_views.undo_back_on(self._undo_was_on, "Reference")
        if _window is self:
            _window = None
        super(ReferenceWindow, self).closeEvent(event)

    def grab_image(self, path):
        """The window as it is drawn (the mesh view too) saved to `path` (a PNG)."""
        cmds.refresh(force=True)
        QtWidgets.QApplication.processEvents()
        picture = self.grab()
        painter = QtGui.QPainter(picture)
        try:
            if self.mesh_view.editor:
                self.mesh_view.paint_into(painter, self, os.path.splitext(path)[0] + ".mesh")
        finally:
            painter.end()
        return picture.save(path)


def show(groom=None, stage=None):
    """Open (or raise) the Photos window for `groom` (the active groom by default), at `stage`
    ("align" or "guides"; None: as it is, Align when it opens)."""
    global _window
    from maya.app.general.mayaMixin import MayaQWidgetBaseMixin
    import groomlab
    groom = groom or groomlab.active_groom()
    if not groom:
        cmds.warning("GroomLab: select a groom")
        return None

    class MayaReferenceWindow(MayaQWidgetBaseMixin, ReferenceWindow):
        pass

    if _window is not None and (not widgets.alive(_window) or _window.groom != groom):
        if widgets.alive(_window):
            _window.close()
        _window = None
    if _window is None:
        _window = MayaReferenceWindow(groom=groom)
        _window.setWindowFlags(_window.windowFlags() | QtCore.Qt.Window)
        widgets.remember_geometry(_window, "reference")
    if stage:
        _window.set_stage(stage)
    _window.show()
    _window.raise_()
    _window.ensure_views()
    return _window


def window():
    """The open Photos window, or None."""
    return _window if _window is not None and widgets.alive(_window) and _window.isVisible() else None
