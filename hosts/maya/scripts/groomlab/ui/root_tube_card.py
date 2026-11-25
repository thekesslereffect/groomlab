# SPDX-License-Identifier: Apache-2.0
"""The Tube tool card's root tube blocks (Groom Tools, the Tube tool):

Tube roots (issue 61)   Draw root: the root's outline clicked on the scalp CV by CV, then (Draw
                        before extrude) its path drawn from the root's middle; Edit roots, which
                        turns into Finish edit while editing; Select root (pick a root by its
                        middle and drag it over the scalp), with Duplicate and Delete for the
                        selected root. A mode's button again leaves it (Draw and Select: as they
                        are; Edit: finished). Undo CV, Delete, Duplicate; the new tubes' Length,
                        Segments and Taper; Snap to neighbours. Subdivide (#85) cuts the selected
                        root into Voronoi cells (a cell cut again: a destructive cut the map
                        leaves be); the brush beside it paints the one density map
                        (tubeSubdivisionDensity) that makes them smaller and more numerous;
                        Multiplier makes the cuts denser overall; Apply cuts them again by both,
                        keeping the cells' grooming.
Tube hierarchy (#62)    + splits the selected tubes (or every tube of the base group) in two, a
                        level deeper; - undoes a split (the selected tubes', else the deepest
                        level); then a row of level buttons, 1 (the base) always there, shows and
                        picks a level to work on (the Tube tool and the brushes then edit that
                        level's tubes; the others hidden).

The viewport side is groomlab.ui.root_tube_tool; the work, groomlab.root_tubes.
"""

from groomlab.ui.qt import QtCore, QtWidgets

import groomlab
from groomlab import root_tubes
from groomlab.ui import theme, widgets


class LeadTrailLayout(QtWidgets.QLayout):
    """Two widgets on a row: `lead` at the left at its own width, `trail` at the right, the space
    between them free. When the lead grows into the trail (more level buttons than fit), the trail
    goes down to a row of its own, still on the right."""

    def __init__(self, lead, trail, parent=None):
        super(LeadTrailLayout, self).__init__(parent)
        self._items = []
        self._gap = theme.px(8)
        self.setContentsMargins(0, 0, 0, 0)
        self.addWidget(lead)
        self.addWidget(trail)

    def addItem(self, item):
        self._items.append(item)

    def count(self):
        return len(self._items)

    def itemAt(self, index):
        return self._items[index] if 0 <= index < len(self._items) else None

    def takeAt(self, index):
        return self._items.pop(index) if 0 <= index < len(self._items) else None

    def hasHeightForWidth(self):
        return True

    def heightForWidth(self, width):
        return self._arrange(QtCore.QRect(0, 0, width, 0), move=False)

    def setGeometry(self, rect):
        super(LeadTrailLayout, self).setGeometry(rect)
        self._arrange(rect, move=True)

    def sizeHint(self):
        return self.minimumSize()

    def minimumSize(self):
        size = QtCore.QSize()
        for item in self._items:
            size = size.expandedTo(item.minimumSize())
        return size

    def _arrange(self, rect, move):
        if len(self._items) < 2:
            return 0
        lead, trail = self._items[0].sizeHint(), self._items[1].sizeHint()
        one_row = lead.width() + self._gap + trail.width() <= rect.width()
        lead_w = min(lead.width(), rect.width())
        if move:
            self._items[0].setGeometry(QtCore.QRect(rect.x(), rect.y(), lead_w, lead.height()))
            y = rect.y() if one_row else rect.y() + lead.height() + theme.px(4)
            self._items[1].setGeometry(QtCore.QRect(rect.right() + 1 - trail.width(), y, trail.width(), trail.height()))
        if one_row:
            return max(lead.height(), trail.height())
        return lead.height() + theme.px(4) + trail.height()


class RootTubeCard(QtCore.QObject):
    """Owns the two blocks (root_block, hierarchy_block); `owner` is the Groom Tools palette."""

    def __init__(self, owner):
        super(RootTubeCard, self).__init__(owner)
        self.owner = owner
        from groomlab import units
        from groomlab.ui import root_tube_tool
        self.tool = root_tube_tool.TOOL
        self.tool.listeners.append(self._said)
        self.length_factor = units.factor("length")

        # -- Root tubes --------------------------------------------------------------------
        self.root_block = widgets.Block("Tube roots", name="rootTubes")
        self.root_block.setToolTip("Tubes drawn root first: click the root's outline on the scalp, one "
                                   "CV at a time, and close it on its first CV. Each tube is its own region.")
        self.draw_root = widgets.chip("Draw root", "Click the root's outline on the scalp, one CV at a time (drag "
                                      "to move the CV); click the first CV to close it. Near a neighbouring root a "
                                      "CV snaps onto it, sealing the tubes", False, "rootTubeDraw")
        self.draw_root.clicked.connect(self.toggle_draw)
        self.edit_root = widgets.chip("Edit roots", "Show the root tubes' outlines (the selected guides', else all) "
                                      "with their CVs, the guides hidden; drag CVs on the scalp (Ctrl+click an edge: "
                                      "a new CV there), then Finish edit",
                                      False, "rootTubeEdit")
        self.edit_root.clicked.connect(self.toggle_edit)
        self.select_root = widgets.chip("Select root", "Show every root's middle: click one to select its root, drag "
                                        "it to move the root over the scalp (with Snap, its CVs sealed onto a "
                                        "neighbour stay put). Again: leave", False, "rootTubeSelect")
        self.select_root.clicked.connect(self.toggle_select)
        self.root_block.add(widgets.chip_row((self.draw_root, self.edit_root, self.select_root)))
        self.undo_cv = widgets.chip("Undo CV", "Take back the last CV (or the drawn path); in Edit roots, the last "
                                    "CV move", None, "rootTubeUndoCv")
        self.undo_cv.clicked.connect(lambda: self._run(self.tool.undo_cv))
        self.delete_root = widgets.chip("Delete", "Delete the selected root (Select root): its tube and the tube "
                                        "hierarchy under it", None, "rootTubeDelete")
        self.delete_root.clicked.connect(self.delete_clicked)
        self.duplicate_root = widgets.chip("Duplicate", "Copy the selected root (Select root): the copy follows the "
                                           "cursor over the scalp, a click sets it down there with its tube and "
                                           "hierarchy. Again: stop", False, "rootTubeDuplicate")
        self.duplicate_root.clicked.connect(self.duplicate_clicked)
        self.root_block.add(widgets.chip_row((self.undo_cv, self.delete_root, self.duplicate_root)))
        # Subdividing roots (#85): Subdivide | paint the density map (the brush) | Multiplier | Apply.
        # One density map for every cut (tubeSubdivisionDensity), made by the first Subdivide.
        self.subdivide_root = widgets.chip("Subdivide", "Cut the selected root (Select root) into Voronoi cells, each "
                                           "a root tube of its own, a slice of the old tube, by the density map "
                                           "(tubeSubdivisionDensity, made at 0.5 the first time) and the multiplier; "
                                           "the tube hierarchy under it is made again in each. A cell cut again is a "
                                           "destructive cut: the map and Apply leave it be", None, "rootTubeSubdivide")
        self.subdivide_root.clicked.connect(self.subdivide_clicked)
        self.paint_density = widgets.chip("", "Paint subdivision density: the density map on the scalp (Subdivide makes "
                                          "it): white, more and smaller roots; black, fewer and larger. Again, Q or "
                                          "another tool stops painting; Apply cuts by it", False, "rootTubePaintDensity")
        self.paint_density.setIcon(widgets.tool_icon("paint"))
        # (Its icon large in the button: little padding round it.)
        self.paint_density.setStyleSheet("QToolButton { padding: %dpx; }" % theme.px(1))
        self.paint_density.setIconSize(QtCore.QSize(theme.px(22), theme.px(22)))
        self.paint_density.clicked.connect(self.paint_density_clicked)
        self.multiplier = widgets.SoftSlider(1.0, 0.25, 4.0, minimum=0.05, maximum=20.0,
                                             key="palette.rootSubdivisionMultiplier", label="Multiplier")
        self.multiplier.setToolTip("Subdivision multiplier: how dense the cuts are overall, with the painted density: 1 "
                                   "as painted, 2 about twice the roots (each smaller), 0.5 half. Apply cuts again at it")
        self.apply_density = widgets.chip("Apply", "Cut the subdivided roots again all over, by the density map and the "
                                          "multiplier as they are now (the cells' grooming and tube hierarchy kept)",
                                          None, "rootTubeApplyDensity")
        self.apply_density.clicked.connect(self.apply_clicked)
        row = QtWidgets.QWidget()
        layout = QtWidgets.QHBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(4))
        for w, stretch in ((self.subdivide_root, 2), (self.paint_density, 0), (self.multiplier, 3),
                           (self.apply_density, 2)):
            if stretch:
                # (One row however narrow the card: these give first.)
                w.setSizePolicy(QtWidgets.QSizePolicy.Ignored, w.sizePolicy().verticalPolicy())
                w.setMinimumWidth(theme.px(40))
            else:
                # (The brush: just its icon.)
                w.setSizePolicy(QtWidgets.QSizePolicy.Fixed, w.sizePolicy().verticalPolicy())
                w.setFixedWidth(theme.px(34))
            layout.addWidget(w, stretch)
        self.root_block.add(row)
        self._painting = None  # painting a density map: (the roots it is for, the map)
        self.snap = widgets.switch("Snap to neighbours", "A CV near a neighbouring root's CV or edge snaps onto it, "
                                   "so the tubes meet with no gap", True, "rootTubeSnap")
        self.snap.toggled.connect(lambda on: self._set(snap=on))
        self.root_block.add(self.snap.row)
        self.extrude_draw = widgets.switch("Draw guide before extrude", "Once the outline closes, drag from its middle "
                                           "out to draw the tube's guide (as the Draw tool draws one): smoothed, its "
                                           "curls kept, its points as many as its length and turns need. Off: the "
                                           "tube goes straight off the scalp, Length long, in Segments",
                                           True, "rootTubeDrawFirst")
        self.extrude_draw.toggled.connect(self.draw_toggled)
        self.root_block.add(self.extrude_draw.row)
        self.density = widgets.SoftSlider(1.0, 0.25, 4.0, minimum=0.05, maximum=20.0, key="palette.rootTubeDensity",
                                          label="Point density")
        self.density.setToolTip("How many points a drawn guide gets: 1, about one per root width along it and "
                                "enough that no span turns more than 15 degrees; 2, twice as many")
        self.density.committed.connect(lambda v: self._set(density=v))
        key = "palette.rootTubeLength@" + units.unit("length")[0]
        self.length = widgets.SoftSlider(1.0 * self.length_factor, minimum=0.001 * self.length_factor, key=key,
                                         label="Length", suffix=units.label("length"))
        self.length.setToolTip("A tube's length when it goes straight off the scalp (Draw guide before extrude off)")
        self.length.committed.connect(lambda v: self._set(length=v / self.length_factor))
        self.segments = widgets.SoftSlider(12, 2, 32, integer=True, minimum=2, maximum=256,
                                           key="palette.rootTubeSegments", label="Segments")
        self.segments.setToolTip("A straight tube guide's segments (Draw guide before extrude off)")
        self.segments.committed.connect(lambda v: self._set(segments=int(v)))
        self.taper = widgets.SoftSlider(0.5, 0.0, 1.0, minimum=0.0, maximum=4.0, key="palette.rootTubeTaper",
                                        label="Tip width")
        self.taper.setToolTip("The tube's width at its tip, as a share of its root's")
        self.taper.committed.connect(lambda v: self._set(taper=v))
        for field in (self.density, self.length, self.segments, self.taper):
            self.root_block.add(field)
        self.draw_toggled(self.extrude_draw.isChecked(), remember=False)

        # -- Tube hierarchy ----------------------------------------------------------------
        self.add_level = widgets.chip("+", "Split each selected tube in two (with none selected: every tube of "
                                      "the base group), sharing its root shape: a level deeper. Again: 2 to 4...",
                                      None, "tubeLevelAdd")
        self.add_level.clicked.connect(self.add_clicked)
        self.remove_level = widgets.chip("\u2212", "Undo a split: with tubes selected, theirs only (a split tube's "
                                         "deepest split tubes, or a split tube back into its parent); with none, the "
                                         "deepest level everywhere", None, "tubeLevelRemove")
        self.remove_level.clicked.connect(self.remove_clicked)
        self.hierarchy_block = widgets.Block("Tube hierarchy", name="tubeHierarchy")
        self.hierarchy_block.setToolTip("Tube levels: each level splits the tubes above it in two. Shaping "
                                        "a tube carries the smaller tubes inside it; pick a level to edit its tubes")
        for b in (self.add_level, self.remove_level):
            b.setFixedWidth(theme.px(28))
            font = b.font()
            font.setBold(True)
            b.setFont(font)
        self.base_label = QtWidgets.QLabel()
        self.base_label.setProperty("role", "about")
        self.hierarchy_block.add(self.base_label)
        self.level_group = QtWidgets.QButtonGroup(self)
        self.level_group.setExclusive(True)
        self.level_group.buttonClicked.connect(self.level_clicked)
        # One row: the level buttons on the left (fill_levels fills them), + and - on the right;
        # levels reaching + and - push them down to a row of their own.
        levels = QtWidgets.QWidget()
        self.level_layout = QtWidgets.QHBoxLayout(levels)
        self.level_layout.setContentsMargins(0, 0, 0, 0)
        self.level_layout.setSpacing(theme.px(4))
        edits = QtWidgets.QWidget()
        edit_layout = QtWidgets.QHBoxLayout(edits)
        edit_layout.setContentsMargins(0, 0, 0, 0)
        edit_layout.setSpacing(theme.px(4))
        edit_layout.addWidget(self.add_level)
        edit_layout.addWidget(self.remove_level)
        self.level_row = QtWidgets.QWidget()
        LeadTrailLayout(levels, edits, self.level_row)
        self.hierarchy_block.add(self.level_row)
        self.lock_view = widgets.switch("Lock view", "Keep the tubes of the level shown now in view while you pick "
                                        "other levels to work on, whose guides show (step down to level 1 and comb "
                                        "its guides while level 5's tubes stay in view and follow). Only the tubes' "
                                        "view: the other visibility controls work as they do", False, "tubeLockView")
        self.lock_view.toggled.connect(self.lock_toggled)
        self.hierarchy_block.add(self.lock_view.row)
        self.mixed_view = widgets.switch("Mixed view", "Working on a level, also show the tubes (not the guides) of the "
                                         "levels above where no level below continues them (a level 1 tube not split "
                                         "to level 2 shows while you work on level 2), and their roots in Select root "
                                         "and Edit roots", True, "tubeMixedView")
        self.mixed_view.toggled.connect(self.mixed_toggled)
        self.hierarchy_block.add(self.mixed_view.row)
        self._locked = 0  # Lock view: the level kept in view (0: none), of hierarchy _locked_base
        self._locked_base = None
        self._current = 1  # the level picked
        self._levels = None  # (base, [groups]) shown
        self._base = None

        self.blocks = (self.root_block, self.hierarchy_block)
        self.show(False)

    # -- Showing ---------------------------------------------------------------------------

    def show(self, on):
        for b in self.blocks:
            b.setVisible(on)
        if on:
            self.refresh(self.owner.groom())

    def visible(self):
        return self.root_block.isVisible()

    def refresh(self, groom, full=True):
        """The buttons as the tool stands, and the hierarchy's levels. `full` False (the palette's
        frequent tick): the levels and the density map read again only for another groom or after
        an action here (each read parses the guides and lays the scalp: a hitch every tick); `full`:
        also when the guides or maps changed since they were read."""
        self.tool.poll()  # (after an undo the tool reads the groom again: its outlines follow)
        mode = self._mode()
        editing = mode == "edit"
        selecting = mode in ("select", "duplicate")
        picked = selecting and self.tool.selected is not None
        self.draw_root.setChecked(mode in ("place", "draw"))
        self.edit_root.setChecked(editing)
        self.edit_root.setText("Finish edit" if editing else "Edit roots")
        self.select_root.setChecked(selecting)
        self.undo_cv.setEnabled(mode in ("place", "draw", "edit"))
        self.delete_root.setEnabled(picked)
        self.duplicate_root.setEnabled(picked)
        self.duplicate_root.setChecked(mode == "duplicate")
        self.subdivide_root.setEnabled(picked)
        # (The levels and the density map read only when the guides or maps changed, or for another
        # groom or level picked: each read parses the guides and lays the scalp, a hitch per tick.)
        from groomlab.ui import groom_watch
        stamp = groom_watch.stamp(groom, ("guideData", "scalpMaps"))
        key = (stamp, self.owner.current_group())
        heavy = (groom != getattr(self, "_read_groom", "?") or self._levels is None
                 or (full and (stamp is None or key != getattr(self, "_read_key", None))))
        self._read_groom = groom
        if heavy:
            self._read_key = key
        if heavy:
            self.fill_density_maps(groom)
        else:
            self.paint_density.setEnabled(getattr(self, "_has_map", False) or self._painting is not None)
        painting = self._painting is not None
        self.paint_density.setChecked(painting)
        if heavy:
            self.fill_levels(groom)

    def fill_levels(self, groom):
        info = {"base": None, "levels": []}
        if groom:
            try:
                info = root_tubes.tube_levels(groom, picked=self.owner.current_group() or self._base)
            except (RuntimeError, ValueError):
                pass
        base, levels = info["base"], info["levels"]
        self._base = base
        picked = self.owner.current_group()
        current = levels.index(picked) + 1 if picked in levels else 1
        self._current = current
        if groom and base:
            try:  # (Mixed view follows the level shown)
                root_tubes.set_mixed_view(groom, base, current if self.mixed_view.isChecked() else 0)
            except RuntimeError:
                pass
        if self._locked and (base != self._locked_base or self._locked > len(levels)):
            self._unlock()  # (another hierarchy, or its locked level gone)
        state = (base, tuple(levels), current)
        if state == self._levels:
            return
        self._levels = state
        for b in list(self.level_group.buttons()):
            self.level_group.removeButton(b)
            b.deleteLater()
        while self.level_layout.count():
            item = self.level_layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        self.base_label.setText("Base: %s" % base if base else "No guides yet.")
        for k in range(max(len(levels), 1)):
            b = widgets.chip(str(k + 1), "Level %d%s: show its tubes and edit them" %
                             (k + 1, " (the base)" if k == 0 else ""), k + 1 == current, "tubeLevel%d" % (k + 1))
            b.setFixedWidth(theme.px(28))
            self.level_group.addButton(b, k + 1)
            self.level_layout.addWidget(b)
        self.level_row.layout().invalidate()
        self.add_level.setEnabled(bool(base) and len(levels) < root_tubes.MAX_LEVELS)
        self.remove_level.setEnabled(len(levels) > 1)

    # -- Root tubes ------------------------------------------------------------------------

    def _said(self, text):
        if text:
            self.owner.status.setText(text)
        try:
            self.refresh(self.owner.groom())
        except RuntimeError:
            pass

    def _set(self, **settings):
        self.tool.update_settings(**settings)

    def _settings(self):
        return {"draw": self.extrude_draw.isChecked(), "length": self.length.value() / self.length_factor,
                "segments": int(self.segments.value()), "taper": self.taper.value(),
                "snap": self.snap.isChecked(), "group": self._new_group(), "density": self.density.value()}

    def draw_toggled(self, on, remember=True):
        """Draw guide before extrude: the drawn guide's Point density; off, the straight tube's
        Length and Segments."""
        self.density.setVisible(bool(on))
        self.length.setVisible(not on)
        self.segments.setVisible(not on)
        if remember:
            self._set(draw=bool(on))

    def _new_group(self):
        """New root tubes go into the picked group when it is a top group, else rootTubes."""
        picked = self.owner.current_group()
        if picked:
            for g in groomlab.list_guide_groups(self.owner.groom()):
                if g["name"] == picked and not g["parent"]:
                    return picked
        return None

    def _run(self, fn, *args, **kwargs):
        try:
            return fn(*args, **kwargs)
        except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
            from groomlab.ui import messages
            self.owner.status.setText(messages.error("Tube roots", e))
            return None
        finally:
            self.refresh(self.owner.groom())

    def _mode(self):
        """The root tool's mode, or None when it isn't the current tool."""
        return self.tool.mode if self.tool.active() else None

    def _enter(self, mode, button):
        """The root tool in `mode` (its button again: left)."""
        groom = self.owner.groom()
        if not groom:
            self.owner.status.setText("Select a groom first.")
            button.setChecked(False)
            return
        from groomlab.ui import root_tube_tool
        self._run(root_tube_tool.start, groom, mode, **self._settings())

    def toggle_draw(self, _on=None):
        if self._mode() in ("place", "draw"):  # again: the outline being drawn dropped, the mode left
            return self.back_to_brush()
        self.to_base_level()
        self._enter("place", self.draw_root)

    def to_base_level(self):
        """A deeper level of the tube hierarchy picked: level 1 shown and picked instead (a new
        root tube's guide is a level 1 guide, drawn there)."""
        groom = self.owner.groom()
        if not groom:
            return
        try:
            info = root_tubes.tube_levels(groom, picked=self.owner.current_group() or self._base)
        except (RuntimeError, ValueError):
            return
        levels = info.get("levels") or []
        if info.get("base") and len(levels) > 1 and self.owner.current_group() in levels[1:]:
            try:
                self._after_levels(groom, info["base"], 1)
                self.owner.status.setText("Level 1 shown: new roots go there.")
            except (RuntimeError, ValueError) as e:
                self.owner.status.setText(str(e).strip())

    def toggle_edit(self, _on=None):
        # Finish edit (the button said so, even should Maya have changed tools meanwhile): the
        # edits given to the roots, the mode left; never edit mode begun again.
        if self._mode() == "edit" or self.edit_root.text() == "Finish edit":
            self._run(self.tool.finish_edit)
            if self.tool.mode != "edit":  # (finished; on an error the edits stay, still editing)
                self.back_to_brush()
            return
        self._enter("edit", self.edit_root)

    def toggle_select(self, _on=None):
        if self._mode() in ("select", "duplicate"):  # again: left
            return self.back_to_brush()
        self._enter("select", self.select_root)

    def duplicate_clicked(self, _on=None):
        if self._mode() == "duplicate":  # again: no copy
            self._run(self.tool.end_duplicate)
            return
        self._run(self.tool.begin_duplicate)

    def delete_clicked(self):
        self._run(self.tool.delete_selected)

    # -- Subdividing roots (#85) -----------------------------------------------------------

    def _said_cut(self, info, verb):
        if not info:
            return
        made, kept = len(info["cells"]), info["kept"]
        if not made and not info["removed"]:
            self.owner.status.setText("Density unchanged: nothing cut again.")
            return
        self.owner.status.setText("%s: %d root%s made%s." % (verb, made, "" if made == 1 else "s",
                                                              ", %d kept" % kept if kept else ""))

    def density_map(self, groom):
        """The density map's name when the groom has it (the one map, else an older numbered one,
        its painting read as it is: the core's subdivision_density_map_in, every host's), else None."""
        import groomlab_core
        from groomlab import painting
        try:
            return groomlab_core.subdivision_density_map_in(list(painting.list_maps(groom))) or None
        except RuntimeError:
            return None

    def fill_density_maps(self, groom):
        """Paint and Apply usable once Subdivide has made the map (Paint too while painting, to finish)."""
        has = bool(groom) and self.density_map(groom) is not None
        self._has_map = has
        self.apply_density.setEnabled(has)
        self.paint_density.setEnabled(has or self._painting is not None)

    def subdivide_clicked(self):
        groom = self.owner.groom()
        roots = self.tool.picked if self._mode() in ("select", "duplicate") else []
        if not groom or not roots:
            self.owner.status.setText("Select a root, then Subdivide.")
            return
        self.tool.leave()
        info = self._run(self._subdivide, groom, roots)
        self._said_cut(info, "Subdivided")
        if info and info.get("cells"):
            # (The cells are in the level below the root cut: that level shown, so they are seen.)
            try:
                self._levels = None
                self.fill_levels(groom)
                info_levels = root_tubes.tube_levels(groom, [int(info["cells"][0])])
                level = int(info.get("cellLevel") or self._current + 1)  # (the core's: the level the cells are in)
                self._after_levels(groom, info_levels["base"], level)
            except (RuntimeError, ValueError, KeyError, IndexError):
                pass
        self._enter("select", self.select_root)
        self._redraw()

    def _subdivide(self, groom, roots):
        """By the density map (made at 0.5 the first time: paint it later, then Apply), the multiplier."""
        name = self.density_map(groom) or root_tubes.ensure_density_map(groom, root_tubes.DENSITY_MAP)
        return root_tubes.subdivide(groom, roots, name, multiplier=self.multiplier.value())

    def apply_clicked(self):
        """The subdivided roots cut again all over by the map, at the multiplier (changed or not)."""
        groom = self.owner.groom()
        name = self.density_map(groom) if groom else None
        if not name:
            self.owner.status.setText("No density map yet: Subdivide first.")
            return
        self.tool.leave()
        info = self._run(root_tubes.apply_density, groom, [], name, self.multiplier.value(), True)
        self._said_cut(info, "Applied")
        self._redraw()

    @staticmethod
    def _redraw():
        """The viewports drawn again now (a cut shows without moving the camera)."""
        from maya import cmds
        try:
            cmds.refresh(force=True)
        except RuntimeError:
            pass

    def paint_density_clicked(self, _on=None):
        groom = self.owner.groom()
        if not groom:
            self.owner.status.setText("Select a groom first.")
            self.paint_density.setChecked(False)
            return
        if self._painting is not None:  # Finish painting: cut by the map
            self.finish_painting()
            return
        # The one map (Subdivide makes it); Apply cuts by it after.
        name, roots = self.density_map(groom), []
        if not name:
            self.owner.status.setText("No density map yet: Subdivide first.")
            self.paint_density.setChecked(False)
            return
        paint = self.owner.paint_tab
        try:
            root_tubes.ensure_density_map(groom, name)
            self.tool.leave()
            # The Paint tab on the density map, painting toward white (as picking it there would).
            paint.refresh(force=True)
            paint.select(name)
            paint.value.setValue(1.0)
            self._painting = (roots, name)
            if self._paint_ended not in paint.painting_ended:
                paint.painting_ended.append(self._paint_ended)
            self.owner.activate("paint")
        except Exception as e:  # noqa: BLE001 (a button never ends in a bare traceback)
            self._painting = None
            from groomlab.ui import messages
            self.owner.status.setText(messages.error("Paint subdivision density", e))
            self.refresh(groom)
            return
        self.owner.status.setText("Painting %s: white for more roots, then Apply." % name)
        self.refresh(groom)

    def _paint_ended(self):
        """Painting stopped some other way (Q, another tool or brush): finished as the button
        finishes it, once the tool change that ended it is through."""
        if self._painting is None:
            return
        from maya import cmds
        cmds.evalDeferred(self.finish_painting)

    def finish_painting(self):
        """Painting done (the button again, Q, another tool): the Tools tab and the Tube tool again.
        Nothing is cut: Apply cuts by the map (a map painted for roots no cut holds yet: those)."""
        if self._painting is None:
            return
        (_roots, name), self._painting = self._painting, None
        self.owner.show_tab("tools")
        self.back_to_brush()
        self.owner.status.setText("Painted %s: Apply cuts its roots by it." % name)

    def back_to_brush(self):
        """The Tube brush again (the root tool put away)."""
        self.tool.leave()
        self.owner.activate("shape")
        self.refresh(self.owner.groom())

    # -- Tube hierarchy ----------------------------------------------------------------------

    def _after_levels(self, groom, base, level):
        """The level `level` shown and picked (the brushes and new guides work there)."""
        self.owner.fill_levels(groom)
        group = root_tubes.show_tube_level(groom, base, level, self._locked)
        root_tubes.set_mixed_view(groom, base, level if self.mixed_view.isChecked() else 0)
        self.owner.pick_group(group)
        self._levels = None
        self.fill_levels(groom)
        try:
            self.owner.guides_tab.refresh()
        except Exception:  # noqa: BLE001 (the list catches up on its own)
            pass

    def lock_toggled(self, on):
        """Lock view: the level picked now stays in view (whatever level is picked after); off, only
        the picked level shows again."""
        groom = self.owner.groom()
        self._locked = self._current if on and self._base else 0
        self._locked_base = self._base
        if not groom or not self._base:
            return
        try:
            root_tubes.show_tube_level(groom, self._base, self._current, self._locked)
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            return
        self.owner.status.setText("Level %d's tubes kept in view." % self._locked if self._locked
                                  else "Lock view off.")

    def mixed_toggled(self, on):
        """Mixed view: the levels above shown where no level below continues them, on the level picked."""
        groom = self.owner.groom()
        if not groom:
            return
        try:
            root_tubes.set_mixed_view(groom, self._base, self._current if on and self._base else 0)
        except RuntimeError as e:
            self.owner.status.setText(str(e).strip())
            return
        self.owner.status.setText("Mixed view: level %d with the levels above." % self._current if on
                                  else "Mixed view off.")

    def _unlock(self):
        self._locked = 0
        try:
            groom = self.owner.groom()
            if groom:
                root_tubes.set_tube_view(groom, "")
        except RuntimeError:
            pass
        self.lock_view.blockSignals(True)
        self.lock_view.setChecked(False)
        self.lock_view.blockSignals(False)

    def add_clicked(self):
        groom = self.owner.groom()
        if not groom:
            self.owner.status.setText("Select a groom first.")
            return
        selected = groomlab.selected_guides(groom)
        info = self._run(root_tubes.add_tube_level, groom, selected or None, self.owner.current_group() or self._base)
        if not info:
            return
        self._after_levels(groom, info["base"], len(info["levels"]))
        self.owner.status.setText("Split %d tube%s: %s has %d levels." %
                                  (info["split"], "" if info["split"] == 1 else "s", info["base"], len(info["levels"])))

    def remove_clicked(self):
        groom = self.owner.groom()
        if not groom or not self._base:
            return
        selected = groomlab.selected_guides(groom)
        info = self._run(root_tubes.remove_tube_level, groom, self._base, selected or None)
        if info:
            self._after_levels(groom, info["base"], len(info["levels"]))
            if info.get("removed") is not None:
                self.owner.status.setText("Removed %d tube%s: %s has %d levels." %
                                          (info["removed"], "" if info["removed"] == 1 else "s", info["base"],
                                           len(info["levels"])))

    def level_clicked(self, button):
        groom = self.owner.groom()
        if not groom or not self._base:
            return
        level = self.level_group.id(button)
        try:
            self._after_levels(groom, self._base, level)
        except (RuntimeError, ValueError) as e:
            self.owner.status.setText(str(e).strip())
            return
        self.owner.status.setText("Level %d shown." % level)
