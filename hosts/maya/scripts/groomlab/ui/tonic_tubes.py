# SPDX-License-Identifier: Apache-2.0
"""The Tube tool card's Tonic-style blocks (Groom Tools, the Tube tool):

Tube roots (issue 61)   Draw root: the root's outline clicked on the scalp CV by CV, then (Draw
                        before extrude) its path drawn from the root's middle; Edit roots, which
                        turns into Finish edit while editing; Select root (pick a root by its
                        middle and drag it over the scalp), with Duplicate and Delete for the
                        selected root. A mode's button again leaves it (Draw and Select: as they
                        are; Edit: finished). Undo CV, Update regions; the new tubes' Length,
                        Segments and Taper; Snap to neighbours.
Tube hierarchy (#62)    Add splits the selected tubes (or every tube of the base group) in two, a
                        level deeper; a row of level buttons, 1 (the base) always there, shows and
                        picks a level to work on (the Tube tool and the brushes then edit that
                        level's tubes; the others hidden). Remove drops the deepest level.

The viewport side is groomlab.ui.root_tube_tool; the work, groomlab.root_tubes.
"""

from groomlab.ui.qt import QtCore, QtWidgets

import groomlab
from groomlab import root_tubes
from groomlab.ui import theme, widgets


class TonicTubes(QtCore.QObject):
    """Owns the two blocks (root_block, hierarchy_block); `owner` is the Groom Tools palette."""

    def __init__(self, owner):
        super(TonicTubes, self).__init__(owner)
        self.owner = owner
        from groomlab import units
        from groomlab.ui import root_tube_tool
        self.tool = root_tube_tool.TOOL
        self.tool.listeners.append(self._said)
        self.length_factor = units.factor("length")

        # -- Root tubes --------------------------------------------------------------------
        self.root_block = widgets.Block("Tube roots", name="rootTubes")
        self.root_block.setToolTip("Tonic-style tubes drawn root first: click the root's outline on the scalp, one "
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
        self.regions = widgets.chip("Update regions", "Fit every root tube whose tube drifted off its root back onto it "
                                    "(tubes from an earlier GroomLab too), then paint every root tube's area in its own "
                                    "color on the %s map, which Interpolate reads as its regions" % root_tubes.REGION_MAP,
                                    None,
                                    "rootTubeRegions")
        self.regions.clicked.connect(self.update_regions)
        self.root_block.add(widgets.chip_row((self.undo_cv, self.delete_root, self.duplicate_root, self.regions)))
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
        self.add_level = widgets.chip("Add", "Split each selected tube in two (with none selected: every tube of "
                                      "the base group), sharing its root shape: a level deeper. Again: 2 to 4...",
                                      None, "tubeLevelAdd")
        self.add_level.clicked.connect(self.add_clicked)
        self.remove_level = widgets.chip("Remove", "Remove the deepest level (its tubes)", None, "tubeLevelRemove")
        self.remove_level.clicked.connect(self.remove_clicked)
        self.hierarchy_block = widgets.Block("Tube hierarchy", name="tubeHierarchy")
        self.hierarchy_block.setToolTip("Tonic-style tube levels: each level splits the tubes above it in two. Shaping "
                                        "a tube carries the smaller tubes inside it; pick a level to edit its tubes")
        self.hierarchy_block.add(widgets.chip_row((self.add_level, self.remove_level)))
        self.base_label = QtWidgets.QLabel()
        self.base_label.setProperty("role", "about")
        self.hierarchy_block.add(self.base_label)
        self.level_group = QtWidgets.QButtonGroup(self)
        self.level_group.setExclusive(True)
        self.level_group.buttonClicked.connect(self.level_clicked)
        self.level_row = QtWidgets.QWidget()
        self.level_layout = QtWidgets.QHBoxLayout(self.level_row)
        self.level_layout.setContentsMargins(0, 0, 0, 0)
        self.level_layout.setSpacing(theme.px(4))
        self.hierarchy_block.add(self.level_row)
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

    def refresh(self, groom):
        """The buttons as the tool stands, and the hierarchy's levels."""
        from maya import cmds
        ours = cmds.currentCtx() == "groomLabRootTubeCtx"
        mode = self.tool.mode if ours else None
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
        self.level_layout.addStretch(1)
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
        self.tool.settings.update(settings)
        if "snap" in settings and self.tool.groom:
            self.tool.neighbours = root_tubes.snap_targets(self.tool.groom) if settings["snap"] else []
            self.tool.redraw()

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
        from maya import cmds
        return self.tool.mode if cmds.currentCtx() == "groomLabRootTubeCtx" else None

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
        self._enter("place", self.draw_root)

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

    def back_to_brush(self):
        """The Tube brush again (the root tool put away)."""
        self.tool.leave()
        self.owner.activate("shape")
        self.refresh(self.owner.groom())

    def update_regions(self):
        groom = self.owner.groom()
        if not groom:
            return
        out = self._run(root_tubes.refresh_root_tubes, groom)
        if out:
            note = (" (an Interpolate reads another region map: set its regionMap to %s)" % root_tubes.REGION_MAP
                    if out["other"] else "")
            n = len(out["refitted"])
            fixed = " %d tube%s fitted to %s root again." % (n, "" if n == 1 else "s", "its" if n == 1 else "their") if n else ""
            self.owner.status.setText("%d root tube%s painted on %s%s.%s" %
                                      (out["tubes"], "" if out["tubes"] == 1 else "s", root_tubes.REGION_MAP, note, fixed))

    # -- Tube hierarchy ----------------------------------------------------------------------

    def _after_levels(self, groom, base, level):
        """The level `level` shown and picked (the brushes and new guides work there)."""
        self.owner.fill_levels(groom)
        group = root_tubes.show_tube_level(groom, base, level)
        self.owner.pick_group(group)
        self._levels = None
        self.fill_levels(groom)
        try:
            self.owner.guides_tab.refresh()
        except Exception:  # noqa: BLE001 (the list catches up on its own)
            pass

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
        self.owner.status.setText("Split %d tube%s: %s has %d levels; level %d shown." %
                                  (info["split"], "" if info["split"] == 1 else "s", info["base"], len(info["levels"]),
                                   len(info["levels"])))

    def remove_clicked(self):
        groom = self.owner.groom()
        if not groom or not self._base:
            return
        info = self._run(root_tubes.remove_tube_level, groom, self._base)
        if info:
            self._after_levels(groom, info["base"], len(info["levels"]))

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
        self.owner.status.setText("Level %d: its tubes shown; the Tube tool and brushes edit them." % level)
