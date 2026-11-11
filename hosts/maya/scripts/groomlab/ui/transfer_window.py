# SPDX-License-Identifier: Apache-2.0
"""GroomLab > Utilities > Transfer Groom to Scalp...: the groom onto another mesh, by a transfer
method (groomlab.transfer: closest, bounds, markers align/warp, uv). A Wrap-style window, never
modal: two views side by side, the groom's current scalp on the left and the new scalp (the "New
scalp" choice) on the right, each a Maya modelEditor of its own with its own camera (Alt
navigates as in any view; Frame fits the mesh), and the transfer's options below.

Marker points are clicked on the meshes in the two views (marker methods; with the others the
views show the meshes only). The rules (the core's TransferMarkerEditor, transfer_markers_edit.h):

  - each side keeps its own ordered list; pair N is the Nth point on the left and the Nth on the
    right, placed in any order. A matched point draws as a filled dot, one without a partner yet
    as a ring; each is numbered, faint where it lies behind the mesh (a groomLabMarkerSet per
    side, the plugin's locator, draws them in screen space above the mesh).
  - click on the mesh adds a point at the end of that side's list (a ray against the mesh);
    click-drag on a point slides it over the surface; Ctrl+click on a point deletes it and its
    partner at the same index (both lists renumber). Hovering a point lights it and its partner.
  - Symmetry: a click also adds the mirror (across x = the mesh's bounding-box center, snapped
    onto the mesh) as the next point on that side; deleting either deletes both and their partners.
  - Ctrl+Z / Ctrl+Y in the window undo and redo marker edits (the window's own stack, not Maya's).
  - complete pairs are stored on the groom on every edit (transferMarkers: the transfer and the
    .groom file read them); unmatched points live in the window while it is open.

Scripts and tests drive it without the mouse:

    from groomlab.ui import transfer_window
    w = transfer_window.show(groom, scalp="newHead")
    w.set_method("markersWarp"); w.add_point("old", (0, 1, 0)); w.add_point("new", (5, 1, 0)); w.transfer()

The views, their markers, the key filter and the undo-queue guard are mesh_views.py's, shared with
the Reference window (reference_window.py).

Picking (transfer.pick_in_view): the view's M3dView turns the port pixel into a ray
(viewToWorld), MFnMesh.closestIntersection gives the face and barycentrics. The clicks reach the
window through a Qt event filter on the two embedded views only (left button without Alt): Maya's
current tool is never changed, the main viewport keeps working, and the filter also sees the
mouse moving without a button (hover), which a dragger context does not.
"""

from maya import cmds

from groomlab.ui.qt import QtCore, QtWidgets, event_pos, to_int
from groomlab.ui import mesh_views, theme, widgets

_window = None

SIDES = ("old", "new")
UI_GROUP = "groomLabTransferUI"  # the views' cameras and markers (hidden, outside the undo queue)
COLORS = {"old": (1.0, 0.85, 0.1), "new": (0.15, 0.9, 1.0)}  # yellow left, cyan right


_no_undo = mesh_views.no_undo  # the window's bookkeeping kept out of Maya's undo queue


class TransferWindow(QtWidgets.QWidget):
    def __init__(self, parent=None, groom=None, scalp=None):
        super(TransferWindow, self).__init__(parent)
        from groomlab import transfer
        import groomlab_core
        self.transfer_module = transfer
        self.groom = groom
        self.editor = groomlab_core.TransferMarkerEditor()
        self.views = {}        # side: its mesh_views.MeshView (made with the window's views)
        self.meshes = {}       # side: the mesh transform the view shows
        self._hover = None     # (side, index)
        self._drag = None      # [side, index, first move]
        self._owned_press = False
        self._scene_job = None
        self._stored_revision = None
        self._undo_was_on = cmds.undoInfo(query=True, state=True)  # _undo_back_on's when it closes
        self.setWindowTitle("Transfer Groom to Scalp")
        self.setWindowIcon(widgets.icon("logo"))
        self.setObjectName("groomLabTransferWindow")
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose, True)
        outer = QtWidgets.QVBoxLayout(self)
        pad = theme.px(10)
        outer.setContentsMargins(pad, pad, pad, pad)
        outer.setSpacing(theme.px(6))

        top = QtWidgets.QHBoxLayout()
        self.groom_label = QtWidgets.QLabel("Groom: " + groom.split("|")[-1])
        top.addWidget(self.groom_label)
        top.addStretch(1)
        top.addWidget(QtWidgets.QLabel("New scalp"))
        self.scalp_box = QtWidgets.QComboBox()
        self.scalp_box.setObjectName("transferScalp")
        self.scalp_box.setToolTip("The mesh the groom moves onto (shown in the right view)")
        self.scalp_box.setMinimumWidth(theme.px(180))
        self.scalp_box.currentIndexChanged.connect(lambda *_: self._new_scalp_changed())
        top.addWidget(self.scalp_box)
        outer.addLayout(top)

        # The two views: a title, the Maya view (made once the window is in Maya's UI), Frame.
        views = QtWidgets.QHBoxLayout()
        views.setSpacing(theme.px(6))
        self.view_titles, self.view_holders, self.frame_buttons = {}, {}, {}
        for side in SIDES:
            column = QtWidgets.QVBoxLayout()
            head = QtWidgets.QHBoxLayout()
            title = QtWidgets.QLabel()
            title.setStyleSheet("color: rgb(%d, %d, %d); font-weight: bold;" % tuple(int(c * 255) for c in COLORS[side]))
            head.addWidget(title, 1)
            frame = QtWidgets.QPushButton("Frame")
            frame.setToolTip("Fit the mesh in this view")
            frame.clicked.connect(lambda checked=False, s=side: self.frame(s))
            head.addWidget(frame)
            column.addLayout(head)
            holder = QtWidgets.QWidget()
            holder.setObjectName("groomLabTransfer%sView" % side.capitalize())
            holder.setMinimumSize(theme.px(220), theme.px(180))  # side by side on a small screen
            holder_layout = QtWidgets.QVBoxLayout(holder)
            holder_layout.setObjectName("groomLabTransfer%sViewLayout" % side.capitalize())
            holder_layout.setContentsMargins(0, 0, 0, 0)
            column.addWidget(holder, 1)
            views.addLayout(column, 1)
            self.view_titles[side], self.view_holders[side], self.frame_buttons[side] = title, holder, frame
            self.views[side] = mesh_views.MeshView(holder, UI_GROUP, "groomLabTransfer%s" % side.capitalize(),
                                                   COLORS[side])
        outer.addLayout(views, 1)

        self.hint = QtWidgets.QLabel("Click a point on each mesh: pair N is the Nth point on the left and the Nth on the "
                                     "right, in any order. Drag a point to slide it, Ctrl+click to delete it and its "
                                     "partner, Alt to move the camera, Ctrl+Z / Ctrl+Y to undo and redo.")
        self.hint.setWordWrap(True)
        self.hint.setProperty("role", "about")
        outer.addWidget(self.hint)

        options = QtWidgets.QHBoxLayout()
        options.addWidget(QtWidgets.QLabel("Method"))
        self.method_box = QtWidgets.QComboBox()
        self.method_box.setObjectName("transferMethod")
        for m in transfer.METHODS:
            self.method_box.addItem(transfer.METHOD_LABELS[m], m)
        self.method_box.setToolTip(
            "Closest point: where the new scalp is nearest (the same place and size).\n"
            "Fit bounding boxes: the old scalp's bounds moved and scaled onto the new one's first.\n"
            "Markers, align: 3+ marker pairs give the best move, turn and scale (a head elsewhere, turned).\n"
            "Markers, warp: 4+ pairs spread over the head bend space onto the new one (other proportions).\n"
            "Matching UVs: both meshes laid out alike in UV; each root at its UV.")
        self.method_box.currentIndexChanged.connect(self._method_changed)
        options.addWidget(self.method_box)
        self.symmetry = QtWidgets.QCheckBox("Symmetry")
        self.symmetry.setToolTip("A click also adds its mirror across the mesh's middle (x), snapped onto the mesh, as "
                                 "the next point on that side; deleting either deletes both")
        options.addWidget(self.symmetry)
        self.wireframe = QtWidgets.QCheckBox("Wireframe")
        self.wireframe.setToolTip("Wireframe on shaded in both views")
        self.wireframe.toggled.connect(self._wireframe_changed)
        options.addWidget(self.wireframe)
        options.addStretch(1)
        self.undo_button = QtWidgets.QPushButton("Undo")
        self.undo_button.setToolTip("Undo the last marker edit (Ctrl+Z)")
        self.undo_button.clicked.connect(lambda: self.undo())
        options.addWidget(self.undo_button)
        self.redo_button = QtWidgets.QPushButton("Redo")
        self.redo_button.setToolTip("Redo (Ctrl+Y)")
        self.redo_button.clicked.connect(lambda: self.redo())
        options.addWidget(self.redo_button)
        self.clear_button = QtWidgets.QPushButton("Clear")
        self.clear_button.setToolTip("Delete every marker point (undoable)")
        self.clear_button.clicked.connect(lambda: self.clear_points())
        options.addWidget(self.clear_button)
        outer.addLayout(options)

        checks = QtWidgets.QHBoxLayout()
        self.keep_lengths = QtWidgets.QCheckBox("Keep guide lengths")
        self.keep_lengths.setToolTip("Guides turn with the new head but keep their lengths (off: they scale with it)")
        checks.addWidget(self.keep_lengths)
        self.fit_size = QtWidgets.QCheckBox("Fit groom to the new scalp's size")
        self.fit_size.setToolTip("The new scalp taken as the old one resized: the groom's scale grows with its area "
                                 "(lengths, widths and clumps follow, the strand count stays, guides scale about their "
                                 "roots). Off: the groom keeps the size shown")
        checks.addWidget(self.fit_size)
        self.maps = QtWidgets.QCheckBox("Project painted maps")
        self.maps.setChecked(True)
        self.maps.setToolTip("Vertex and texture maps carried onto the new scalp by the same method (a texture map "
                             "gets a new image under this groom's masks folder; the old file is left as it is)")
        checks.addWidget(self.maps)
        self.color_sets = QtWidgets.QCheckBox("Copy mesh color sets")
        self.color_sets.setToolTip("Color sets of the old mesh the graph reads by name, copied onto the new mesh "
                                   "(its history baked; a mesh with deformers is left alone)")
        checks.addWidget(self.color_sets)
        checks.addStretch(1)
        outer.addLayout(checks)

        bottom = QtWidgets.QHBoxLayout()
        self.status = QtWidgets.QLabel()
        self.status.setObjectName("transferStatus")
        bottom.addWidget(self.status, 1)
        self.transfer_button = QtWidgets.QPushButton("Transfer")
        self.transfer_button.setProperty("role", "primary")
        self.transfer_button.clicked.connect(lambda: self.transfer())
        bottom.addWidget(self.transfer_button)
        close = QtWidgets.QPushButton("Close")
        close.clicked.connect(self.close)
        bottom.addWidget(close)
        outer.addLayout(bottom)
        self.message = widgets.StatusLabel()
        self.message.setObjectName("transferMessage")
        outer.addWidget(self.message)
        theme.style(self)
        widgets.fit_to_screen(self, theme.px(980), theme.px(640))  # within a small screen

        self._keys = mesh_views.UndoKeys(self)
        self._keys.watch(self)

        self.editor.reset(self._stored_pairs())
        self.refresh_scalps(scalp)
        self._method_changed()

    # ---- Choices ---------------------------------------------------------------------------

    def refresh_scalps(self, scalp=None):
        """The meshes the groom can move onto (every mesh but its scalp); `scalp` picked."""
        import groomlab
        current = groomlab.scalp_mesh(self.groom)
        current = cmds.listRelatives(current, parent=True, fullPath=True)[0] if current else None
        meshes = [cmds.listRelatives(m, parent=True, fullPath=True)[0]
                  for m in cmds.ls(type="mesh", noIntermediate=True, long=True) or []]
        meshes = [m for m in dict.fromkeys(meshes) if m != current]
        picked = cmds.ls(scalp, long=True) if scalp else []
        if not picked:
            picked = [m for m in groomlab.transforms(groomlab.selected_meshes()) if m in meshes]  # (faces too)
        if not picked and self.scalp() in meshes:
            picked = [self.scalp()]
        self.scalp_box.blockSignals(True)
        self.scalp_box.clear()
        for m in meshes:
            self.scalp_box.addItem(m.split("|")[-1], m)
        if picked and picked[0] in meshes:
            self.scalp_box.setCurrentIndex(meshes.index(picked[0]))
        self.scalp_box.blockSignals(False)
        self.meshes["old"] = current
        self._new_scalp_changed(reload_left=True)

    def scalp(self):
        return self.scalp_box.currentData()

    def set_new_scalp(self, mesh):
        """Picks the new scalp (the right view reloads; the left one is kept)."""
        long_name = (cmds.ls(mesh, long=True) or [None])[0]
        index = self.scalp_box.findData(long_name)
        if index < 0:
            raise ValueError("%s is not a mesh the groom can move onto" % mesh)
        self.scalp_box.setCurrentIndex(index)

    def method(self):
        return self.method_box.currentData()

    def set_method(self, method):
        index = self.method_box.findData(method)
        if index < 0:
            raise ValueError("transfer method %r" % method)
        self.method_box.setCurrentIndex(index)

    def marker_editing(self):
        """Whether clicks in the views place markers (the marker methods)."""
        return self.method() in self.transfer_module.MARKER_METHODS

    def _method_changed(self, *_):
        editing = self.marker_editing()
        self.keep_lengths.setEnabled(self.method() != "closest")
        for w in (self.symmetry, self.undo_button, self.redo_button, self.clear_button, self.hint):
            w.setEnabled(editing)
        self._redraw()

    def _new_scalp_changed(self, reload_left=False):
        self.meshes["new"] = self.scalp()
        for side in SIDES:
            mesh = self.meshes.get(side)
            name = mesh.split("|")[-1] if mesh else "(none)"
            self.view_titles[side].setText(("Groom's scalp: " if side == "old" else "New scalp: ") + name)
        if self.editors:
            for side in (SIDES if reload_left else ("new",)):
                self.frame(side)
        self._redraw()

    # ---- The views -------------------------------------------------------------------------

    @property
    def editors(self):
        """side: the modelEditor of each view made."""
        return {side: v.editor for side, v in self.views.items() if v.editor}

    @property
    def panes(self):
        return {side: v.pane for side, v in self.views.items() if v.pane}

    @property
    def cameras(self):
        """side: each view's camera transform."""
        return {side: v.camera for side, v in self.views.items() if v.camera}

    @property
    def marker_sets(self):
        """side: each view's groomLabMarkerSet shape (long name)."""
        return {side: v.marker_set for side, v in self.views.items() if v.marker_set}

    def ensure_views(self):
        """Makes the two Maya views (once the window is part of Maya's UI): a paneLayout and a
        modelEditor in each holder, each with a hidden camera of its own, isolated to its mesh."""
        if self.editors:
            return
        for side in SIDES:
            self.views[side].mesh = self.meshes.get(side)
            self.views[side].ensure(wireframe=self.wireframe.isChecked())
        QtWidgets.QApplication.processEvents()
        for side in SIDES:
            self.views[side].watch(self, side)
            self._keys.watch(self.view_holders[side])
            self.frame(side)
        from maya.api import OpenMaya as om
        self._scene_job = [om.MSceneMessage.addCallback(when, self._scene_cleared)
                           for when in (om.MSceneMessage.kBeforeNew, om.MSceneMessage.kBeforeOpen)]
        # Undo and redo can put the groom back on another scalp (a transfer undone): the views follow.
        self._scene_job += [om.MEventMessage.addEventCallback(event, self._undone) for event in ("Undo", "Redo")]
        self._redraw()

    def view(self, side):
        """The M3dView (maya.api.OpenMayaUI) of a side's view."""
        return self.views[side].view()

    def view_widget(self, side):
        return self.views[side].view_widget()

    def frame(self, side):
        """The side's camera fits its mesh (keeping its direction)."""
        self.views[side].mesh = self.meshes.get(side)
        self.views[side].frame()

    def _wireframe_changed(self, on):
        for v in self.views.values():
            v.set_wireframe(on)

    def _isolate(self, side, objects):
        """Only `objects` (long names) show in the side's view."""
        self.views[side].isolate(objects)

    def isolated(self, side):
        """What the side's view shows (long names)."""
        return self.views[side].isolated()

    # ---- Marker points ---------------------------------------------------------------------

    def _side(self, side):
        side = {"left": "old", "right": "new"}.get(side, side)
        if side not in SIDES:
            raise ValueError("marker side %r: old/left or new/right" % (side,))
        return side

    def _marker_point(self, side, world_point):
        import groomlab_core
        mesh = self.meshes.get(side)
        point = self.transfer_module.closest_on_mesh(mesh, world_point) if mesh else list(world_point)
        return groomlab_core.MarkerPoint(point=point)

    def add_point(self, side, world_point):
        """A point (snapped onto the side's mesh) at the end of the side's list; with Symmetry its
        mirror after it. Returns the new point's index."""
        side = self._side(side)
        p = self._marker_point(side, world_point)
        if self.symmetry.isChecked() and self.meshes.get(side):
            import groomlab_core
            center, diagonal = self.transfer_module.mesh_bounds(self.meshes[side])
            mirror = self._marker_point(side, list(groomlab_core.mirror_x(p.point, center[0])))
            index = self.editor.add_mirrored(side, p, mirror, diagonal * 1e-3)
        else:
            index = self.editor.add(side, p)
        self._edited()
        return index

    def delete_point(self, side, index):
        """Deletes a point, its mirror (when added together) and their partners at the same indices.
        Returns the indices deleted."""
        gone = self.editor.remove(self._side(side), int(index))
        self._hover = None
        self._edited()
        return list(gone)

    def move_point(self, side, index, world_point, new_step=True):
        """A point slid to `world_point` (snapped onto the mesh); one undo step (a drag's later
        moves join its first)."""
        side = self._side(side)
        self.editor.move(side, int(index), self._marker_point(side, world_point), new_step)
        self._edited(store=new_step)

    def clear_points(self):
        self.editor.clear()
        self._edited()

    def set_markers(self, markers):
        """Replaces both lists with pairs (any form transfer.markers_json takes): one undo step."""
        import groomlab_core
        text = self.transfer_module.markers_json(markers) if markers else "[]"
        self.editor.set_pairs(groomlab_core.transfer_markers_from_json(text))
        self._edited()

    def undo(self):
        if self.editor.undo():
            self._hover = None
            self._edited()
            return True
        return False

    def redo(self):
        if self.editor.redo():
            self._hover = None
            self._edited()
            return True
        return False

    def points(self, side):
        """A side's points, [x, y, z] each, in order."""
        return [[p.point.x, p.point.y, p.point.z] for p in self.editor.points(self._side(side))]

    def pairs(self):
        """The complete pairs: [{"old": {"point": [...]}, "new": {...}}, ...]."""
        return [{"old": {"point": [m.old_point.x, m.old_point.y, m.old_point.z]},
                 "new": {"point": [m.new_point.x, m.new_point.y, m.new_point.z]}} for m in self.editor.pairs()]

    def pending(self):
        """The unmatched points: [(side, index), ...] (the longer list's tail)."""
        return [(side, i) for side in SIDES for i in range(self.editor.count(side)) if not self.editor.matched(side, i)]

    def _stored_pairs(self):
        import groomlab_core
        try:
            text = cmds.getAttr(self.transfer_module._shape(self.groom) + ".transferMarkers") or ""
            return groomlab_core.transfer_markers_from_json(text)
        except Exception:
            return []

    def _edited(self, store=True):
        if store:
            self._store()
        self._redraw()

    def _store(self):
        """The complete pairs on the groom (snapped onto both meshes: their faces too), outside
        Maya's undo queue (the window's own stack undoes them)."""
        pairs = self.pairs()
        markers = pairs
        if pairs and self.scalp():
            try:
                markers = self.transfer_module.snap_markers(self.groom, self.scalp(), pairs)
            except Exception:
                markers = pairs
        with _no_undo():
            self.transfer_module.set_transfer_markers(self.groom, markers)
        self._stored_revision = self.editor.revision

    # ---- Drawing ---------------------------------------------------------------------------

    def _redraw(self):
        self._draw_markers()
        self._update_status()

    def _draw_markers(self):
        """Each side's points on its marker set (the plugin draws them: filled dots matched, rings
        pending, numbered, faint behind the mesh, a white ring hovered), shown in its own view."""
        if not self.editors:
            return
        editing = self.marker_editing() and self.isVisible()
        for side in SIDES:
            self.views[side].mesh = self.meshes.get(side)
            self._update_marker_set(side)
            self.views[side].show(markers=editing)

    def _update_marker_set(self, side):
        """The side's marker set given the model's points, matched flags and the hover (outside the
        undo queue: the window's own stack undoes marker edits)."""
        v = self.views[side]
        if not v.marker_set or not cmds.objExists(v.marker_set):
            return
        editing = self.marker_editing() and self.isVisible()
        points = self.points(side) if editing else []
        v.set_markers(points, [self.editor.matched(side, i) for i in range(len(points))],
                      self._hover_index(side) if editing else -1)

    def _hover_index(self, side):
        """The point the side's set rings: the hovered one, or its partner (a matched point's)."""
        if self._hover is None:
            return -1
        hovered, i = self._hover
        if hovered == side or self.editor.matched(hovered, i):
            return i if i < self.editor.count(side) else -1
        return -1

    def _schedule_refresh(self, side):
        self.views[side].schedule_refresh()

    def marker_state(self, side):
        """What the side's marker set draws (tests): {"points": [[x, y, z], ...], "matched": [0/1],
        "hover": index or -1}."""
        return self.views[self._side(side)].marker_state()

    def requirement(self):
        """Why Transfer cannot run yet, or ""."""
        import groomlab_core
        if not self.scalp():
            return "No other mesh to move the groom onto"
        return groomlab_core.marker_requirement(self.method(), self.editor.pair_count)

    def _update_status(self):
        why = self.requirement()
        text = self.editor.summary() if self.marker_editing() else "Markers are used by the marker methods"
        if why:
            text += "  —  " + why
        self.status.setText(text)
        self.transfer_button.setEnabled(not why)
        self.transfer_button.setToolTip(why or "Move the groom onto the new scalp")
        self.undo_button.setEnabled(self.marker_editing() and self.editor.can_undo)
        self.redo_button.setEnabled(self.marker_editing() and self.editor.can_redo)

    # ---- The mouse in the views --------------------------------------------------------------

    def _port(self, side, widget, pos):
        """A widget position as the side's M3dView port pixel (origin bottom left)."""
        return self.views[side].port(widget, pos)

    def pick(self, side, x, y):
        """The point of the side's mesh under port pixel (x, y): transfer.pick_mesh's dict, or None."""
        side = self._side(side)
        if side not in self.editors:
            return None
        self.views[side].mesh = self.meshes.get(side)
        return self.views[side].pick(x, y)

    def point_near(self, side, x, y, pixels=None):
        """The index of the side's point projected nearest port pixel (x, y), within `pixels` (the
        view's PICK_PIXELS), or None."""
        side = self._side(side)
        return self.views[side].point_near(self.points(side), x, y, pixels)

    def _set_hover(self, hover):
        """Lights a point and its partner: (side, index), or None."""
        if hover is not None:
            hover = (self._side(hover[0]), int(hover[1]))
        if hover != self._hover:
            self._hover = hover
            if self.editors:
                for side in SIDES:
                    self.views[side].set_hover(self._hover_index(side) if self.marker_editing() else -1)

    def _view_event(self, side, widget, event):
        kind = event.type()
        if kind == QtCore.QEvent.Leave:
            self._set_hover(None)
            return False
        if kind not in (QtCore.QEvent.MouseButtonPress, QtCore.QEvent.MouseMove, QtCore.QEvent.MouseButtonRelease,
                        QtCore.QEvent.MouseButtonDblClick):
            return False
        if not self.marker_editing() or side not in self.editors:
            return False
        mods = to_int(event.modifiers())
        alt = bool(mods & to_int(QtCore.Qt.AltModifier))
        ctrl = bool(mods & to_int(QtCore.Qt.ControlModifier))
        left = event.button() == QtCore.Qt.LeftButton
        x, y = self._port(side, widget, event_pos(event))
        if kind in (QtCore.QEvent.MouseButtonPress, QtCore.QEvent.MouseButtonDblClick):
            if not left or alt:
                return False
            self._owned_press = True
            near = self.point_near(side, x, y)
            if ctrl:
                if near is not None:
                    self.delete_point(side, near)
                return True
            if near is not None:
                self._drag = [side, near, True]
                return True
            hit = self.pick(side, x, y)
            if hit:
                index = self.add_point(side, hit["point"])
                self._drag = [side, index, False]  # sliding the new point joins its add
            return True
        if kind == QtCore.QEvent.MouseMove:
            buttons = to_int(event.buttons())
            if self._drag and buttons & to_int(QtCore.Qt.LeftButton) and self._drag[0] == side:
                hit = self.pick(side, x, y)
                if hit:
                    _, index, first = self._drag
                    self.editor.move(side, index, self._marker_point(side, hit["point"]), first)
                    self._drag[2] = False
                    self._update_marker_set(side)
                return True
            if not buttons:
                near = self.point_near(side, x, y)
                self._set_hover(None if near is None else (side, near))
            return self._owned_press
        # Release
        if self._owned_press and left:
            self._owned_press = False
            if self._drag:
                self._drag = None
                self._edited()
            return True
        return False

    # ---- Transfer --------------------------------------------------------------------------

    def transfer(self):
        """Transfers with the window's choices. Returns utils.transfer_to_scalp's result, or None
        with why in the message line."""
        from groomlab import utils
        why = self.requirement()
        if why:
            self.message.setText(why)
            return None
        method = self.method()
        markers = self.pairs() if method in self.transfer_module.MARKER_METHODS else None
        # One undo step (transfer_groom's chunk): nothing undoable happens here outside it; the
        # window's redraws after it (marker sets, isolation, cameras) stay out of the undo queue.
        try:
            with widgets.busy([self.transfer_button], self.message,
                              "Transferring %s onto %s..." % (self.groom.split("|")[-1], self.scalp().split("|")[-1])):
                result = utils.transfer_to_scalp(self.groom, self.scalp(), maps=self.maps.isChecked(),
                                                 color_sets=self.color_sets.isChecked(), method=method, markers=markers,
                                                 keep_lengths=self.keep_lengths.isChecked(),
                                                 fit_size=self.fit_size.isChecked())
        except (ValueError, RuntimeError) as e:
            self.message.setText(str(e).strip())
            self._redraw()
            return None
        self.message.setText(result["message"])
        if result["missing"]:
            cmds.warning("GroomLab: not carried onto the new scalp: %s" % ", ".join(result["missing"]))
        # The groom is on the new scalp now (the left view shows it); the markers stored with it
        # are kept (Undo brings the old scalp back with them).
        self.editor.reset(self._stored_pairs())
        self.refresh_scalps()
        return result

    # ---- Lifetime --------------------------------------------------------------------------

    def showEvent(self, event):
        super(TransferWindow, self).showEvent(event)
        if not self.editors:
            QtCore.QTimer.singleShot(0, self._show_views)
        else:
            self._redraw()

    def _show_views(self):
        if widgets.alive(self) and self.isVisible():
            self.ensure_views()

    def _undone(self, *_):
        # After Maya's undo or redo has finished (deferred): the views follow the groom's scalp.
        QtCore.QTimer.singleShot(0, self.follow_groom)

    def follow_groom(self):
        """The views made to match the groom again after an undo or redo moved it to another scalp:
        its scalp on the left, the mesh it came off picked on the right (a transfer undone shows the
        old scalp and the new one again, its markers as stored). Nothing when it hasn't moved."""
        if not widgets.alive(self) or not cmds.objExists(self.groom):
            return
        import groomlab
        current = groomlab.scalp_mesh(self.groom)
        current = cmds.listRelatives(current, parent=True, fullPath=True)[0] if current else None
        shown = self.meshes.get("old")
        if current == shown:
            return
        came_off = shown if shown and cmds.objExists(shown) else None
        self.editor.reset(self._stored_pairs())
        self.refresh_scalps(scalp=came_off)
        self.message.setText("The groom is on %s again" % (current or "no scalp").split("|")[-1])

    def _scene_cleared(self, *_):
        # Before a new scene (File New, Open): its nodes go with it (the cameras, the markers); the
        # views go first, then the window.
        try:
            self.teardown(nodes=False, callbacks=False)  # (removed later: not from inside one)
        except Exception as e:
            cmds.warning("GroomLab: closing the Transfer Groom window: %s" % e)
        QtCore.QTimer.singleShot(0, self.close)

    def teardown(self, nodes=True, callbacks=True):
        """Deletes the views and (`nodes`) their cameras and the markers (closing does it)."""
        if self._scene_job and callbacks:
            from maya.api import OpenMaya as om
            for callback in self._scene_job:
                om.MMessage.removeCallback(callback)
            self._scene_job = None
        if self.editors:
            mesh_views.MeshView.hand_back_active_view()
        for v in self.views.values():
            v.teardown(nodes=nodes)
        if nodes and cmds.objExists(UI_GROUP):
            with _no_undo():
                cmds.delete(UI_GROUP)

    def closeEvent(self, event):
        global _window
        self.teardown()
        mesh_views.undo_back_on(self._undo_was_on, "Transfer Groom")
        if _window is self:
            _window = None
        super(TransferWindow, self).closeEvent(event)

    def grab_image(self, path):
        """The window as it is drawn (its views too) saved to `path` (a PNG)."""
        import os
        from groomlab.ui.qt import QtGui
        cmds.refresh(force=True)
        QtWidgets.QApplication.processEvents()
        picture = self.grab()
        # The views draw through Viewport 2.0, which a widget grab may miss: their own color
        # buffers painted over where they are.
        painter = QtGui.QPainter(picture)
        try:
            for side in self.editors:
                self.views[side].paint_into(painter, self, "%s.%s" % (os.path.splitext(path)[0], side))
        finally:
            painter.end()
        return picture.save(path)


def show(groom, scalp=None):
    """Open (or raise) the Transfer Groom window for `groom`; `scalp`: the new scalp to pick."""
    global _window
    from maya.app.general.mayaMixin import MayaQWidgetBaseMixin

    class MayaTransferWindow(MayaQWidgetBaseMixin, TransferWindow):
        pass

    if _window is not None and (not widgets.alive(_window) or _window.groom != groom):
        if widgets.alive(_window):
            _window.close()
        _window = None
    if _window is None:
        _window = MayaTransferWindow(groom=groom, scalp=scalp)  # the first positional arg is the Qt parent
        _window.setWindowFlags(_window.windowFlags() | QtCore.Qt.Window)
        widgets.remember_geometry(_window, "transfer")
    elif scalp:
        _window.refresh_scalps(scalp)
    _window.show()
    _window.raise_()
    _window.ensure_views()
    return _window


def window():
    """The open Transfer Groom window, or None."""
    return _window if _window is not None and widgets.alive(_window) and _window.isVisible() else None
