# SPDX-License-Identifier: Apache-2.0
"""The Groom Tools tabs besides the brushes (Qt: groomlab.ui.qt):

    Guides  (a section of the Tools tab) the groups guides live in, and edits on many at once
    Paint   maps: 0-1 values (gray, or color r g b a) painted on the guides, the scalp's vertices or a
            texture, that any param can read (and HairColor, as painted hair color)
    Sculpt  correctives: sculpt layers on top of the groom
    Reference  photos as cameras: look through them, line them up with the mesh (by points), lock them

Each tab works on its owner's groom (the Groom Tools' pinned or selected one)
and refreshes itself when the groom's guides or graph change.
"""

import os

from groomlab.ui import qt
from groomlab.ui.qt import QtCore, QtWidgets

import groomlab
from groomlab import graph as gm
from groomlab.ui import definitions, menu, theme, widgets


class _GroomTab(QtWidgets.QWidget):
    """A tab following one groom: refresh() when it or its guides and graph change."""

    WATCH = ("guideData", "graph")
    LIGHT = ()  # attributes whose change calls light_refresh() instead of refresh()

    def __init__(self, owner):
        super(_GroomTab, self).__init__()
        self.owner = owner
        self.groom = None
        self._callback = [None]  # the MMessage callback id, in a box the destroyed signal can reach
        self._pending = False
        # Qt deletes the widget under its wrapper (a closed dock, a workspace change): the
        # callback must go with it, or it keeps firing into a dead tab.
        self.destroyed.connect(lambda *_, box=self._callback: _remove_callback(box))

    def set_groom(self, groom):
        # The node, not just its name: a new scene's groom of the same name is another node,
        # and the attribute callback must move to it.
        from maya import cmds
        node = (cmds.ls(groom, uuid=True) or [None])[0] if groom else None
        if groom != self.groom or node != getattr(self, "_node", None):
            self.groom = groom
            self._node = node
            self._watch()
        self.refresh()

    def unwatch(self):
        """Stop following the groom (closing, or the owner closing)."""
        _remove_callback(self._callback)

    def closeEvent(self, event):
        self.unwatch()
        super(_GroomTab, self).closeEvent(event)

    def status(self, text):
        self.owner.status.setText(text)

    def run(self, fn, *args, **kwargs):
        try:
            result = fn(*args, **kwargs)
            self.status("")
            return result if result is not None else True
        except (RuntimeError, ValueError, gm.GraphError, OSError, IOError) as e:
            from groomlab.ui import messages
            self.status(messages.plain(e))
            return None

    def need(self):
        if not self.groom:
            self.status("Select a groom.")
        return bool(self.groom)

    def _watch(self):
        from maya.api import OpenMaya as om
        _remove_callback(self._callback)
        if not self.groom:
            return
        try:
            node = om.MSelectionList().add(self.groom).getDependNode(0)
        except RuntimeError:
            return
        box = self._callback

        def changed(message, plug, other, data):
            if not message & om.MNodeMessage.kAttributeSet:
                return
            name = plug.partialName(useLongNames=True)
            if name not in self.WATCH and name not in self.LIGHT:
                return
            if not widgets.alive(self):  # the tab is gone
                _remove_callback(box)
                return
            if name not in self.WATCH:
                if not getattr(self, "_light_pending", False):
                    self._light_pending = True
                    QtCore.QTimer.singleShot(0, self._light_deferred)
                return
            if not self._pending:
                self._pending = True
                QtCore.QTimer.singleShot(0, self._deferred)

        box[0] = om.MNodeMessage.addAttributeChangedCallback(node, changed)

    def _deferred(self):
        self._pending = False
        from maya import cmds
        try:
            self.objectName()
        except RuntimeError:
            return
        if self.groom and cmds.objExists(self.groom):
            self.refresh()

    def _light_deferred(self):
        self._light_pending = False
        from maya import cmds
        try:
            self.objectName()
        except RuntimeError:
            return
        if self.groom and cmds.objExists(self.groom):
            self.light_refresh()

    def refresh(self):
        pass

    def light_refresh(self):
        pass


# Deleting a texture map deletes its file too (the maps list's right-click menu turns it off).
DELETE_MAP_FILES_VAR = "groomLabDeleteMapFiles"


def delete_map_files():
    """Whether Delete deletes a texture map's file too (on unless turned off)."""
    from maya import cmds
    return not cmds.optionVar(exists=DELETE_MAP_FILES_VAR) or bool(cmds.optionVar(query=DELETE_MAP_FILES_VAR))


def set_delete_map_files(on):
    from maya import cmds
    cmds.optionVar(intValue=(DELETE_MAP_FILES_VAR, int(bool(on))))


def _remove_callback(box):
    """Drop the MMessage callback whose id is in `box` (a one-item list), if any."""
    if box[0] is None:
        return
    from maya.api import OpenMaya as om
    try:
        om.MMessage.removeCallback(box[0])
    except RuntimeError:
        pass
    box[0] = None


def _action_bar(chips, glyphs):
    """One row of a list's actions, the same in every tab: the main ones as labelled chips on the
    left, the rest as small glyphs on the right. Each is (text, tooltip, slot), the glyphs' text a
    widgets.GLYPHS name; a slot may be a QMenu (the button opens it)."""
    bar = QtWidgets.QHBoxLayout()
    bar.setSpacing(theme.px(4))
    made = []
    for group, role in ((chips, "chip"), (glyphs, None)):
        if role is None:
            bar.addStretch(1)
        for text, tip, slot in group:
            b = QtWidgets.QToolButton()
            b.setText(text)
            b.setToolTip(tip)
            if role:
                b.setProperty("role", role)
            else:
                b.setAutoRaise(True)
                b.setFixedSize(theme.px(28), theme.px(26))
                if text in widgets.GLYPHS:  # a drawn glyph (it follows the theme's text color)
                    b.setText("")
                    b.setIcon(widgets.glyph(text))
                    b.setIconSize(QtCore.QSize(theme.px(18), theme.px(18)))
                    b.glyph = text
                    b.setProperty("role", "glyph")
            if isinstance(slot, QtWidgets.QMenu):
                b.setMenu(slot)
                b.setPopupMode(QtWidgets.QToolButton.InstantPopup)
            else:
                b.clicked.connect(lambda _=False, s=slot: s())
            bar.addWidget(b)
            made.append(b)
    return bar, made


def _bar(entries, bind, owner):
    """_action_bar from a list's bar (engine/ui/lists.json): the entries with a glyph on the right, the
    rest as chips; a submenu entry's button opens its menu (filled from the definitions)."""
    chips, glyphs = [], []
    for item in definitions.resolve(entries, None):
        if item["kind"] == "submenu":
            slot = QtWidgets.QMenu(owner)
            widgets.fill_menu(slot, definitions.items_of(item["entry"]), bind, None)
            slot.setToolTipsVisible(True)
        else:
            slot = bind(item)
        (glyphs if item["glyph"] else chips).append((item["glyph"] or item["label"], item["tip"], slot))
    return _action_bar(chips, glyphs)


def _missing_map_files(groom, entries):
    """groomlab.missing_map_files from the maps already read (groomlab.maps)."""
    from groomlab import presets
    return dict((m["name"], m["texture"]) for m in entries
                if m["kind"] == "texture" and m["texture"]
                and not presets.file_exists(groomlab.resolve_path(m["texture"], groom)))


def _group_users(groom):
    """{group: [(node, param)]}: groomlab.group_users for every group, from one read of the graph."""
    users = {}
    for node in groomlab.get_graph(groom, expand_default=True).get("nodes", []):
        params = node.get("params", {})
        if node.get("type") != "Guides":
            continue
        group = params.get("group")
        if group:
            users.setdefault(group, []).append((node["name"], "group"))
        seen = set()
        for name in groomlab._group_list(params.get("exclude")):
            if name not in seen:  # named twice: one user, as group_users counts it
                seen.add(name)
                users.setdefault(name, []).append((node["name"], "exclude"))
    return users


def _about(text):
    label = QtWidgets.QLabel(text)
    label.setWordWrap(True)
    label.setProperty("role", "about")
    return label


# ---------------------------------------------------------------------------
# Guides


class GuidesTab(_GroomTab):
    # What the group list's bar, right-click menu and Edit Guides menu run (engine/ui/lists.json's
    # groups): method names, or functions of the tab.
    ACTIONS = {
        "group.new": lambda self: self.owner.new_group(),
        "group.rename": "rename_group",
        "group.color": lambda self: self.pick_color(self.group()),
        "group.defaultColor": lambda self: self.set_color(self.group(), None),
        "group.select": "select_group",
        "group.resetSegments": "reset_group_segments",
        "group.delete": "delete_group",
        "guides.children": "generate_children",
        "guides.syncPos": lambda self: self.menu_action(lambda: menu.symmetry_sync(False)),
        "guides.syncNeg": lambda self: self.menu_action(lambda: menu.symmetry_sync(True)),
        "guides.mirrorSelection": lambda self: self.menu_action(menu.mirror_selection),
        "guides.mirrorGroup": lambda self: self.menu_action(menu.mirror_group_dialog),
        "guides.reparent": lambda self: self.menu_action(menu.reparent_dialog),
        "group.promoteDemote": "promote_demote",
        "guides.fromCurves": lambda self: self.menu_action(menu.guides_from_selected_curves),
        "guides.clearSelection": lambda self: self.menu_action(menu.clear_selection),
    }

    def __init__(self, owner):
        super(GuidesTab, self).__init__(owner)
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(theme.px(6))
        lists = definitions.get("lists/groups")
        layout.addWidget(_about(lists["about"]))
        bind = definitions.binder(self.ACTIONS, self)
        bar, _ = _bar(lists["bar"], bind, self)
        layout.addLayout(bar)

        self.tree = QtWidgets.QTreeWidget()
        self.tree.setHeaderLabels(["Group", "", "Guides", "Level"])
        self.tree.header().setStretchLastSection(False)
        self.tree.header().setSectionResizeMode(0, QtWidgets.QHeaderView.Stretch)
        self.tree.setMinimumHeight(theme.px(120))
        self.tree.setEditTriggers(QtWidgets.QAbstractItemView.DoubleClicked | QtWidgets.QAbstractItemView.EditKeyPressed)
        self.tree.itemClicked.connect(lambda item, _: self.owner.pick_group(item.data(0, QtCore.Qt.UserRole)))
        self.tree.setToolTip("Click: brushes and new guides use this group. Double-click the name: rename. "
                             "Checkbox: show or hide. Swatch: its color. Right-click: more")
        self.tree.itemChanged.connect(self.item_changed)
        self.tree.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self.context_menu)
        layout.addWidget(self.tree, 1)

        # Edits on many guides: one menu.
        edits = QtWidgets.QPushButton(lists["edits"]["label"] + "  \u25be")
        edits.setToolTip(lists["edits"]["tip"])
        self.edits_menu = QtWidgets.QMenu(edits)
        widgets.fill_menu(self.edits_menu, lists["edits"]["items"], bind)
        self.edits_menu.setToolTipsVisible(True)
        edits.setMenu(self.edits_menu)
        row = QtWidgets.QHBoxLayout()
        row.addWidget(edits, 1)
        # Generate Children's count, inline (no prompt): children per guide for the selected
        # guides, else for the picked group.
        self.children_count = QtWidgets.QSpinBox()
        self.children_count.setRange(1, 500)
        self.children_count.setValue(menu.CHILDREN_COUNT)
        self.children_count.setPrefix("x ")
        self.children_count.setToolTip("Children per guide for Edit Guides > Children (the selected guides, else the "
                                       "picked group)")
        self.children_count.valueChanged.connect(lambda v: setattr(menu, "CHILDREN_COUNT", int(v)))
        row.addWidget(self.children_count)
        layout.addLayout(row)

        # Segments: the selected guides' (else the picked group's, else every guide's) point count,
        # resampled live while dragging (a preview from the stored guides) and committed as one
        # undo step on release; a whole group's new guides get it too.
        seg_row = QtWidgets.QHBoxLayout()
        self.segments_label = QtWidgets.QLabel("Segments")
        seg_row.addWidget(self.segments_label)
        self.segments = widgets.SoftSlider(12, lo=2, hi=64, integer=True, minimum=1, maximum=256,
                                           key="guides.segments")
        self.segments.setToolTip("Points along the guides, kept on their smooth curve: the selected guides (their own "
                                 "count from then on, kept when the group's changes; the group's count makes them "
                                 "follow it again), else the picked group's (its new guides get as many too), else "
                                 "every guide. Frozen guides keep theirs. Drag to see it live; release (or type a "
                                 "value) to apply it as one undo step. More segments let a guide curl tighter.")
        self.segments.dragged.connect(self.segments_dragged)
        self.segments.committed.connect(self.segments_committed)
        seg_row.addWidget(self.segments, 1)
        layout.addLayout(seg_row)
        self._preview_segments = None
        self._previewing = False

    LIGHT = ("guideSelection",)

    def light_refresh(self):
        self.sync_segments()

    def segments_group(self):
        """The group Segments works on: the palette's picked one, with no guides selected (None: the
        selected guides, else every guide)."""
        picked = getattr(self.owner, "current_group", None)
        group = picked() if picked else ""
        if not group or not self.groom or groomlab.selected_guides(self.groom):
            return None
        return group if any(g["name"] == group for g in groomlab.list_guide_groups(self.groom)) else None

    def sync_segments(self):
        """The slider shows the targets' segment count (the most common; the label says when they differ)."""
        if self._previewing:
            return
        counts = []
        group = None
        if self.groom:
            try:
                group = self.segments_group()
                counts = groomlab.guide_segments(self.groom, group=group)
            except RuntimeError:
                counts = []
        selected = bool(self.groom and groomlab.selected_guides(self.groom))
        scope = ("%d selected" % len(counts) if selected else
                 "%s: %d" % (group, len(counts)) if group else "all %d" % len(counts))
        if counts:
            common = max(set(counts), key=counts.count)
            mixed = min(counts) != max(counts)
            self.segments.setValue(common)
            self.segments_label.setText("Segments (%s%s)" % (scope, ", mixed" if mixed else ""))
            self.segments_label.setToolTip("%d to %d segments" % (min(counts), max(counts)) if mixed else "")
        else:
            self.segments_label.setText("Segments")
        self.segments.setEnabled(bool(counts))

    def segments_dragged(self, value):
        """Live: the guides resampled to `value` as a preview, at most one per 30 ms."""
        if not self.groom:
            return
        self._preview_segments = int(round(value))
        if not getattr(self, "_preview_queued", False):
            self._preview_queued = True
            QtCore.QTimer.singleShot(30, self._apply_preview)

    def _apply_preview(self):
        self._preview_queued = False
        if self._preview_segments is None or not self.groom:
            return
        try:
            groomlab.preview_resample_guides(self.groom, self._preview_segments, group=self.segments_group())
            self._previewing = True
        except Exception as e:  # a timer's slot: nothing above it would report it
            self.status(str(e).strip())

    def end_preview(self):
        self._preview_segments = None
        if self._previewing and self.groom:
            try:
                groomlab.end_guide_preview(self.groom)
            except RuntimeError:
                pass
        self._previewing = False

    def segments_committed(self, value):
        """Apply: the targets resampled to `value`, one undo step (selected guides already at it: none).
        A group (or every guide) commits anyway: its new guides take the count on."""
        segments = int(round(value))
        if not self.groom:
            return
        self._preview_segments = None
        group = self.segments_group()
        selected = bool(groomlab.selected_guides(self.groom))
        counts = groomlab.guide_segments(self.groom, group=group)
        if counts and (not selected or any(c != segments for c in counts)):
            count = self.run(groomlab.resample_guides, self.groom, segments, group=group)
            if count is not None:
                scope = " (new guides of %s too)" % group if group else "" if selected else " (new guides too)"
                self.status("Resampled %d guides to %d segments%s." % (count, segments, scope))
        self.end_preview()
        self.sync_segments()

    def set_groom(self, groom):
        if groom != self.groom:
            self.end_preview()
        super(GuidesTab, self).set_groom(groom)

    def closeEvent(self, event):
        self.end_preview()
        super(GuidesTab, self).closeEvent(event)

    def group(self):
        item = self.tree.currentItem()
        return item.data(0, QtCore.Qt.UserRole) if item else None

    def refresh(self):
        # The palette's group stays highlighted (a new group is picked there before the list is
        # filled again); with none picked there ("all groups"), the row that was.
        picked = getattr(self.owner, "current_group", None)
        current = (picked() if picked else "") or self.group()
        self.tree.clear()
        if not self.groom:
            self.sync_segments()
            return
        items = {}
        hidden = set(groomlab.hidden_guide_groups(self.groom))
        colors = groomlab.guide_group_colors(self.groom)
        self.tree.blockSignals(True)
        groups = groomlab.list_guide_groups(self.groom)
        named = _group_users(self.groom)
        pending = list(groups)
        while pending:  # parents before children
            left = []
            for g in pending:
                parent = items.get(g["parent"]) if g["parent"] else self.tree.invisibleRootItem()
                if parent is None:
                    left.append(g)
                    continue
                users = ", ".join("%s (%s)" % u for u in named.get(g["name"], []))
                item = QtWidgets.QTreeWidgetItem(parent, [g["name"], "", str(g["count"]), str(g["depth"])])
                item.setData(0, QtCore.Qt.UserRole, g["name"])
                item.setFlags(item.flags() | QtCore.Qt.ItemIsUserCheckable | QtCore.Qt.ItemIsEditable)
                item.setCheckState(0, QtCore.Qt.Unchecked if g["name"] in hidden else QtCore.Qt.Checked)
                item.setToolTip(0, "%s: checked shows it in the viewport; double-click to rename%s%s"
                                % (g["name"], "\nNamed in the graph by " + users if users else "",
                                   "\nNew guides: %d segments" % g["segments"] if g["segments"] else ""))
                items[g["name"]] = item
                self.tree.setItemWidget(item, 1, self._swatch(g["name"], colors.get(g["name"]), g["color"]))
            if len(left) == len(pending):
                break
            pending = left
        self.tree.blockSignals(False)
        self.tree.expandAll()
        for column in (1, 2, 3):
            self.tree.resizeColumnToContents(column)
        if current in items:
            self.tree.setCurrentItem(items[current])
        elif items:
            self.tree.setCurrentItem(self.tree.topLevelItem(0))
        self.sync_segments()
        # The palette's group choice follows the groups (this runs when the guides change).
        fill = getattr(self.owner, "fill_levels", None)
        if fill is not None and getattr(self.owner, "groom", lambda: None)() == self.groom:
            fill(self.groom, select=self.owner.current_group(), groups=groups)

    def _swatch(self, name, color, drawn):
        """A small button in the color the group draws in: its own `color` filled, else its automatic
        one (`drawn`) inside a dashed outline. Click to pick one."""
        b = QtWidgets.QToolButton()
        b.setFixedSize(theme.px(18), theme.px(18))
        b.setToolTip("%s's color in the viewport: click to pick%s" % (
            name, ", right-click to clear" if color else " (automatic: its own hue from the groom's)"))
        if color:
            b.setStyleSheet("QToolButton { background: %s; border-radius: %dpx; border: none; }" % (color, theme.px(4)))
        else:
            b.setStyleSheet("QToolButton { background: %s; border: %dpx dashed %s; border-radius: %dpx; }"
                            % (drawn, max(1, theme.px(1)), theme.current().color("ui.textDim"), theme.px(4)))
        b.clicked.connect(lambda _=False, n=name: self.pick_color(n))
        b.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        b.customContextMenuRequested.connect(lambda _pos, n=name: self.set_color(n, None))
        return b

    def pick_color(self, name):
        if not self.need():
            return
        from groomlab.ui.qt import QtGui
        current = groomlab.guide_group_colors(self.groom).get(name, "#3d7eff")
        chosen = QtWidgets.QColorDialog.getColor(QtGui.QColor(current), self.window(), "Color of " + name)
        if chosen.isValid():
            self.set_color(name, chosen.name())

    def set_color(self, name, color):
        if self.run(groomlab.set_guide_group_color, self.groom, name, color):
            QtCore.QTimer.singleShot(0, self.refresh)  # not from inside the swatch's own click

    def item_changed(self, item, column):
        """The checkbox (show or hide) or the name (rename) of a group row changed."""
        if column != 0 or not self.groom:
            return
        name = item.data(0, QtCore.Qt.UserRole)
        text = item.text(0).strip()
        if text and text != name:  # renamed in place
            if self.run(groomlab.rename_guide_group, self.groom, name, text):
                item.setData(0, QtCore.Qt.UserRole, text)
                self.owner.fill_levels(self.groom, select=text)
            QtCore.QTimer.singleShot(0, self.refresh)
            return
        self.run(groomlab.show_guide_group, self.groom, name, item.checkState(0) == QtCore.Qt.Checked)

    def context_menu(self, pos):
        item = self.tree.itemAt(pos)
        menu_ = QtWidgets.QMenu(self)
        state, actions = set(), dict(self.ACTIONS)
        if item is not None:
            name = item.data(0, QtCore.Qt.UserRole)
            self.tree.setCurrentItem(item)
            state.add("group")
            if name in groomlab.guide_group_colors(self.groom):
                state.add("group.color")
            actions["group.rename"] = definitions.bound(lambda: self.tree.editItem(item, 0))  # in place
        widgets.fill_menu(menu_, definitions.get("lists/groups/context"), definitions.binder(actions, self), state)
        qt.exec_(menu_, self.tree.viewport().mapToGlobal(pos))

    def _need_group(self):
        if not self.need():
            return False
        if not self.group():
            self.status("The groom has no guide groups yet: New Group..., or Place or Draw guides.")
        return bool(self.group())

    def select_group(self):
        if self._need_group():
            from maya import cmds
            cmds.select(cmds.listRelatives(self.groom, parent=True))
            self.run(groomlab.select_guides, self.groom, group=self.group())
            self.status("Selected %d guides of %s." % (len(groomlab.selected_guides(self.groom)), self.group()))

    def reset_group_segments(self):
        if self._need_group():
            segments = self.run(groomlab.reset_group_segments, self.groom, self.group())
            if segments:
                self.status("Every guide of %s at %d segments (frozen ones kept)." % (self.group(), segments))
            self.sync_segments()

    def rename_group(self):
        if not self._need_group():
            return
        old = self.group()
        name, ok = QtWidgets.QInputDialog.getText(self, "Rename Group", "New name:", text=old)
        if ok and name and name != old and self.run(groomlab.rename_guide_group, self.groom, old, name):
            self.refresh()
            found = self.tree.findItems(name, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive)
            if found:
                self.tree.setCurrentItem(found[0])
            self.owner.fill_levels(self.groom, select=name)

    def delete_group(self):
        if not self._need_group():
            return
        name = self.group()
        text = "Delete the group %s with its child groups and guides?" % name
        users = groomlab.group_users(self.groom, name)
        if users:
            text += "\n\nThe graph names it: %s." % ", ".join("%s (%s)" % u for u in users)
        if QtWidgets.QMessageBox.question(self, "Delete Group", text) == QtWidgets.QMessageBox.Yes:
            self.run(groomlab.remove_guide_group, self.groom, name)
            self.refresh()

    def promote_demote(self):
        box = QtWidgets.QMessageBox(self)
        box.setWindowTitle("Promote / Demote Group")
        box.setText("Move a guide group one level up (promote), or down under a sibling group (demote)?")
        promote = box.addButton("Promote...", QtWidgets.QMessageBox.AcceptRole)
        demote = box.addButton("Demote...", QtWidgets.QMessageBox.AcceptRole)
        box.addButton(QtWidgets.QMessageBox.Cancel)
        qt.exec_(box)
        if box.clickedButton() is promote:
            self.menu_action(menu.promote_group_dialog)
        elif box.clickedButton() is demote:
            self.menu_action(menu.demote_group_dialog)

    def menu_action(self, fn):
        self.run(fn)
        if self.groom:
            self.refresh()

    def generate_children(self):
        """Edit Guides > Children: the inline count of children for the selected guides, else for
        the picked group (the last group when none is picked)."""
        group = self.owner.current_group() if hasattr(self.owner, "current_group") else ""
        self.menu_action(lambda: menu.generate_children_dialog(count=self.children_count.value(), group=group or None,
                                                               groom=self.groom))


# ---------------------------------------------------------------------------
# Paint


KIND_LABELS = {"guides": "guides", "vertices": "vertices", "texture": "texture"}
# The map kinds and values (engine/ui/choices.json's mapKind, mapValues): what each is, said in the
# New Map dialog, the list's tooltips and Convert to.
KIND_TIPS = dict((c["value"], c["tip"]) for c in definitions.choices("mapKind"))
VALUES_TIPS = dict((c["value"], c["tip"]) for c in definitions.choices("mapValues"))
CHANNELS = "rgba"
MISSING_ROLE = QtCore.Qt.UserRole + 3  # a texture map's list row: its image's expected path when it is missing
IMAGE_FILTER = ("Images (*.png *.jpg *.jpeg *.tga *.bmp *.psd *.gif *.hdr *.pic *.pnm *.pgm *.ppm);;"
                "PNG (*.png);;All files (*.*)")


def reveal_file(path):
    """Show a file in the system's file browser (Explorer: its folder with the file selected); a
    file that is not there shows its folder, if that is. Returns what was shown, or ""."""
    import subprocess
    import sys
    from groomlab.ui.qt import QtGui
    path = os.path.normpath(path)
    if os.path.isfile(path) and sys.platform == "win32":
        subprocess.Popen(["explorer", "/select,", path])
        return path
    folder = path if os.path.isdir(path) else os.path.dirname(path)
    if not os.path.isdir(folder):
        return ""
    QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(folder))
    return folder


def _map_icon(channels):
    """A small swatch for the map list: a gray ramp for a gray map, red green blue for a color map."""
    from groomlab.ui.qt import QtGui
    size = theme.px(12)
    pixmap = QtGui.QPixmap(size, size)
    pixmap.fill(QtCore.Qt.transparent)
    painter = QtGui.QPainter(pixmap)
    painter.setPen(QtCore.Qt.NoPen)
    if channels == 4:
        third = max(1, size // 3)
        for i, color in enumerate(("#e0464b", "#3fbf5f", "#3d7eff")):
            painter.setBrush(QtGui.QColor(color))
            painter.drawRect(i * third, 0, third if i < 2 else size - 2 * third, size)
    else:
        ramp = QtGui.QLinearGradient(0, 0, size, 0)
        ramp.setColorAt(0.0, QtGui.QColor("#101010"))
        ramp.setColorAt(1.0, QtGui.QColor("#f0f0f0"))
        painter.setBrush(QtGui.QBrush(ramp))
        painter.drawRect(0, 0, size, size)
    painter.end()
    return QtGui.QIcon(pixmap)


FOR_TIP = definitions.get("lists/maps/forTip")


class NewMapDialog(QtWidgets.QDialog):
    """What the map is for (nothing yet, or a setting: Density, Width... or any mappable param),
    name, kind (guides, vertices, texture), gray or color values and, for a texture, its
    resolution. `targets`: groomlab.setting_targets' (settings, others)."""

    def __init__(self, parent, title="New Map", name="mask", kind="guides", color=False, targets=None):
        super(NewMapDialog, self).__init__(parent)
        self.setWindowTitle(title)
        form = QtWidgets.QFormLayout(self)
        settings, others = targets or ([], [])
        self.target = QtWidgets.QComboBox()
        self.target.setObjectName("newMapFor")
        self.target.addItem("Nothing yet (a blank map)", None)
        model = self.target.model()
        for s in settings:
            self.target.addItem("Setting: %s" % s["label"], "%s.%s" % (s["node"], s["param"]) if s["node"] else None)
            row = self.target.count() - 1
            self.target.setItemData(row, s["hint"], QtCore.Qt.ToolTipRole)
            if not s["node"]:
                model.item(row).setEnabled(False)
        if others:
            self.target.addItem("Other setting...", "other")
        self.target.setToolTip(FOR_TIP)
        form.addRow("For", self.target)
        self.other = QtWidgets.QComboBox()
        self.other.setObjectName("newMapOther")
        for o in others:
            self.other.addItem(o["label"], "%s.%s" % (o["node"], o["param"]))
            self.other.setItemData(self.other.count() - 1, o["hint"], QtCore.Qt.ToolTipRole)
        self.other_label = QtWidgets.QLabel("Setting")
        form.addRow(self.other_label, self.other)
        self.name = QtWidgets.QLineEdit(name)
        self.name.setToolTip("Letters, digits and _ (not starting with a digit)")
        form.addRow("Name", self.name)
        self.kind = QtWidgets.QComboBox()
        self.values_box = QtWidgets.QComboBox()
        self.values_box.setObjectName("newMapValues")
        self.resolution = QtWidgets.QComboBox()
        for box, name in ((self.kind, "mapKind"), (self.values_box, "mapValues"), (self.resolution, "mapResolution")):
            for c in definitions.choices(name):
                box.addItem(c["label"], c["value"])
                if c.get("tip"):
                    box.setItemData(box.count() - 1, c["tip"], QtCore.Qt.ToolTipRole)
            form.addRow(definitions.get("choices/" + name)["label"], box)
        self.kind.setCurrentIndex(self.kind.findData(kind))
        self.values_box.setCurrentIndex(self.values_box.findData("color" if color else "gray"))
        self.resolution.setCurrentIndex(self.resolution.findData(definitions.choice_default("mapResolution")))
        self.about = QtWidgets.QLabel()
        self.about.setWordWrap(True)
        self.about.setProperty("role", "about")
        form.addRow(self.about)
        self.kind.currentIndexChanged.connect(self._kind_changed)
        self.values_box.currentIndexChanged.connect(self._kind_changed)
        self.target.currentIndexChanged.connect(self._target_changed)
        self.other.currentIndexChanged.connect(self._kind_changed)
        self._target_changed()
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def set_target(self, node, param):
        """Pick the setting node.param (a common one, else under Other). False when it is not offered."""
        key = "%s.%s" % (node, param)  # items hold "node.param" (a string: Qt keeps it as it is)
        for i in range(self.target.count()):
            if self.target.itemData(i) == key:
                self.target.setCurrentIndex(i)
                return True
        index = self.other.findData(key)
        other = self.target.findData("other")
        if index < 0 or other < 0:
            return False
        self.target.setCurrentIndex(other)
        self.other.setCurrentIndex(index)
        return True

    def chosen_target(self):
        """(node, param) the map is for, or None (a blank map)."""
        data = self.target.currentData()
        if data == "other":
            data = self.other.currentData()
        if not data:
            return None
        node, param = data.split(".", 1)
        return node, param

    def _target_changed(self, *_):
        target = self.target.currentData()
        self.other.setVisible(target == "other")
        self.other_label.setVisible(target == "other")
        for_setting = target is not None
        # A setting's map is named for it (<node>_<param>), gray, on the scalp's vertices unless picked otherwise.
        self.name.setEnabled(not for_setting)
        if for_setting:
            self.values_box.setCurrentIndex(0)
            if self.kind.currentData() == "guides":
                self.kind.setCurrentIndex(1)
        self._kind_changed()

    def _kind_changed(self, *_):
        kind = self.kind.currentData()
        for_setting = self.chosen_target() is not None
        # Guide layers are gray: color lives on the scalp (vertices or a texture).
        self.values_box.setEnabled(kind != "guides" and not for_setting)
        if kind == "guides" and self.values_box.currentData() == "color":
            self.values_box.setCurrentIndex(0)
        text = KIND_TIPS[kind] + "\n\n" + VALUES_TIPS[self.values_box.currentData()]
        if for_setting:
            node, param = self.chosen_target()
            text = "For %s.%s: named %s_%s. %s\n\n%s" % (node, param, node, param, FOR_TIP, KIND_TIPS[kind])
        if kind == "texture" and not groomlab.scene_folder():
            text += "\n\nThe scene is untitled: save it first, so the image lands beside it."
        self.about.setText(text)
        self.resolution.setEnabled(kind == "texture")

    def values(self):
        """(name, kind, resolution, color, target): color True for a color map; target (node, param)
        for a setting's map, else None."""
        return (self.name.text().strip(), self.kind.currentData(), int(self.resolution.currentData()),
                self.values_box.currentData() == "color" and self.kind.currentData() != "guides",
                self.chosen_target())


class PaintTab(_GroomTab):
    """Maps: named 0-1 values painted on the scalp, of three kinds (a layer on the guides, a
    value per scalp vertex, a texture at the scalp's UVs) that convert into each other, read by
    any mappable param through a Map node."""

    # What the map list's bar and right-click menu run (engine/ui/lists.json's maps).
    ACTIONS = {
        "map.new": lambda self: self.new_map(),
        "map.import": lambda self: self.import_map(),
        "map.rename": "rename_map",
        "map.convert": "convert_map",
        "map.makeGray": lambda self: self.set_channels(1),
        "map.makeColor": lambda self: self.set_channels(4),
        "map.relink": lambda self: self.relink_image(),
        "map.reloadImage": "reload_image",
        "map.showFile": "show_in_explorer",
        "map.delete": "delete_map",
        "map.deleteFiles": definitions.bound(lambda on: set_delete_map_files(on)),
    }

    WATCH = ("guideData", "graph", "scalpMaps", "scalpMapDisplay", "scalpMapChannel", "strandColorMode",
             "strandColorSource")
    def __init__(self, owner):
        super(PaintTab, self).__init__(owner)
        self.paint_color = (1.0, 1.0, 1.0, 1.0)  # what paint puts on a color map (r g b a, 0-1)
        self.paint_channels = CHANNELS          # the channels it changes
        self.paint_mode = "replace"
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(theme.px(6))
        from groomlab.ui import palette  # the painting brushes (the catalog's Paint ones), beside the maps they paint
        layout.addWidget(owner.tool_grid(dict(palette.SECTIONS).get("Paint", [])))
        bar, made = _bar(definitions.get("lists/maps/bar"), definitions.binder(self.ACTIONS, self), self)
        made[3].setObjectName("paintConvert")
        self.convert_menu = made[3].menu()
        layout.addLayout(bar)
        self.list = QtWidgets.QListWidget()
        self.list.setMinimumHeight(theme.px(110))
        self.list.setToolTip("A map is a 0-1 value painted on the scalp that any param can read (in the Graph "
                             "Editor, right-click a param > Paint). Double-click: rename; right-click: more")
        self.list.currentItemChanged.connect(lambda item, _: self.map_changed(item.data(QtCore.Qt.UserRole) if item else ""))
        self.list.itemDoubleClicked.connect(lambda _: self.rename_map())
        self.list.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.list.customContextMenuRequested.connect(
            lambda pos: self.list_menu(self.list.viewport().mapToGlobal(pos)))
        layout.addWidget(self.list, 1)
        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        self.mode_group = QtWidgets.QButtonGroup(self)
        self.mode_group.setExclusive(True)
        mode_row = QtWidgets.QHBoxLayout()
        mode_row.setContentsMargins(0, 0, 0, 0)
        mode_row.setSpacing(theme.px(3))
        self.mode_buttons = {}
        for choice in definitions.choices("paintMode"):
            button = QtWidgets.QToolButton()
            button.setText(choice["label"])
            button.setToolTip(choice.get("tip", ""))
            button.setCheckable(True)
            button.setProperty("role", "chip")
            button.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
            button.clicked.connect(lambda _checked=False, value=choice["value"]: self.paint_mode_changed(value))
            self.mode_group.addButton(button)
            self.mode_buttons[choice["value"]] = button
            mode_row.addWidget(button)
        self.mode_buttons["replace"].setChecked(True)
        mode_widget = QtWidgets.QWidget()
        mode_widget.setToolTip(definitions.get("choices/paintMode")["tip"])
        mode_widget.setLayout(mode_row)
        form.addRow("Mode", mode_widget)
        self.value = widgets.SoftSlider(1.0, 0.0, 1.0, minimum=0.0, maximum=1.0, key=None)
        self.value.setToolTip("What the Paint brush paints toward, and what Fill sets")
        self.value.committed.connect(lambda v: self.owner.push(value=v))
        fill = QtWidgets.QPushButton("Fill")
        fill.setToolTip("Set the map to the value: on the selected guides when some are (a guide map), else "
                        "everywhere")
        fill.clicked.connect(lambda: self.fill())
        row = QtWidgets.QHBoxLayout()
        row.setSpacing(theme.px(6))
        row.addWidget(self.value, 1)
        row.addWidget(fill)
        self.value_row = QtWidgets.QWidget()
        self.value_row.setLayout(row)
        row.setContentsMargins(0, 0, 0, 0)
        self.value_label = QtWidgets.QLabel("Value")
        form.addRow(self.value_label, self.value_row)
        # A color map: the color painted, the channels it goes into, and Fill with it.
        color_row = QtWidgets.QHBoxLayout()
        color_row.setContentsMargins(0, 0, 0, 0)
        color_row.setSpacing(theme.px(4))
        self.swatch = QtWidgets.QToolButton()
        self.swatch.setObjectName("paintColorSwatch")
        self.swatch.setFixedSize(theme.px(40), theme.px(24))
        self.swatch.setToolTip("The color the Paint brush paints on a color map (Ctrl: its inverse): click to pick")
        self.swatch.clicked.connect(lambda _=False: self.pick_paint_color())
        color_row.addWidget(self.swatch)
        self.channel_buttons = {}
        for letter in CHANNELS:
            b = QtWidgets.QToolButton()
            b.setText(letter.upper())
            b.setObjectName("paintChannel_" + letter)
            b.setCheckable(True)
            b.setChecked(True)
            b.setProperty("role", "chip")
            b.setToolTip(definitions.choice("paintChannel", letter)["tip"])
            b.toggled.connect(lambda _on, toggled=letter: self.channels_changed(toggled))
            color_row.addWidget(b)
            self.channel_buttons[letter] = b
        color_row.addStretch(1)
        fill_color = QtWidgets.QPushButton("Fill")
        fill_color.setObjectName("paintFillColor")
        fill_color.setToolTip("Fill the map with the color, in the channels ticked")
        fill_color.clicked.connect(lambda: self.fill())
        color_row.addWidget(fill_color)
        self.color_row = QtWidgets.QWidget()
        self.color_row.setLayout(color_row)
        self.color_label = QtWidgets.QLabel("Color")
        form.addRow(self.color_label, self.color_row)
        layout.addLayout(form)
        self._show_swatch()
        views = QtWidgets.QHBoxLayout()
        self.view_guides = QtWidgets.QCheckBox("Guides")
        self.view_guides.setToolTip("Color the guides by the map")
        self.view_guides.toggled.connect(self.toggle_view_guides)
        self.view_strands = QtWidgets.QCheckBox("Strands")
        self.view_strands.setToolTip("Color the strands by the map")
        self.view_strands.toggled.connect(self.toggle_view_strands)
        self.view_scalp = QtWidgets.QCheckBox("Scalp")
        self.view_scalp.setObjectName("paintShowOnScalp")
        self.view_scalp.setToolTip("Show the map on the scalp (black to white), whatever its kind")
        self.view_scalp.toggled.connect(self.toggle_view_scalp)
        show = QtWidgets.QLabel("Show on")
        show.setProperty("role", "about")
        views.addWidget(show)
        views.addWidget(self.view_guides)
        views.addWidget(self.view_strands)
        views.addWidget(self.view_scalp)
        self.view_channel = QtWidgets.QComboBox()
        self.view_channel.setObjectName("paintScalpChannel")
        for c in definitions.choices("scalpChannel"):
            self.view_channel.addItem(c["label"], c["value"])
        self.view_channel.setToolTip(definitions.get("choices/scalpChannel")["tip"])
        self.view_channel.currentIndexChanged.connect(lambda _i: self.view_channel_changed())
        views.addWidget(self.view_channel)
        views.addStretch(1)
        layout.addLayout(views)
        self.users = QtWidgets.QLabel()
        self.users.setWordWrap(True)
        self.users.setProperty("role", "hint")
        layout.addWidget(self.users)

    def menu_state(self):
        """The conditions the list menu's entries need (engine/ui/lists.json's maps): a map picked, on
        the scalp, in color, a texture."""
        if self.list.currentItem() is None:
            return set()
        state = {"map"}
        if self.current_kind() in ("vertices", "texture"):
            state.add("map.scalp")
            if self.current_channels() == 4:
                state.add("map.color")
        if self.current_kind() == "texture":
            state.add("map.texture")
        return state

    def list_menu(self, global_pos):
        menu_ = QtWidgets.QMenu(self)
        widgets.fill_menu(menu_, definitions.get("lists/maps/context"), definitions.binder(self.ACTIONS, self),
                          self.menu_state(), checked=lambda item: delete_map_files())
        menu_.setToolTipsVisible(True)
        qt.exec_(menu_, global_pos)

    def image_actions(self):
        """The list menu's entries for the picked map's image: (label, tip, function)."""
        bind = definitions.binder(self.ACTIONS, self)
        return [(item["label"], item["tip"], bind(item))
                for item in definitions.resolve(definitions.get("lists/maps/image"), self.menu_state())]

    def current_missing(self):
        """The picked texture map's expected image path when the file is missing, else ""."""
        self._fresh()
        item = self.list.currentItem()
        return (item.data(MISSING_ROLE) or "") if item else ""

    # -- The list --------------------------------------------------------------

    def _fresh(self):
        """The list as the groom has it, when a refresh was put off while the tab was hidden."""
        if getattr(self, "_stale", False):
            self.refresh(force=True)

    def current(self):
        self._fresh()
        item = self.list.currentItem()
        return item.data(QtCore.Qt.UserRole) if item else ""

    def current_kind(self):
        self._fresh()
        item = self.list.currentItem()
        return item.data(QtCore.Qt.UserRole + 1) if item else ""

    def current_channels(self):
        """1 for a gray map (or none), 4 for a color map."""
        self._fresh()
        item = self.list.currentItem()
        return (item.data(QtCore.Qt.UserRole + 2) or 1) if item else 1

    def select(self, name):
        for i in range(self.list.count()):
            if self.list.item(i).data(QtCore.Qt.UserRole) == name:
                self.list.setCurrentItem(self.list.item(i))
                return True
        return False

    def showEvent(self, event):
        super(PaintTab, self).showEvent(event)
        if getattr(self, "_stale", False):  # changed while hidden
            self.refresh()

    def refresh(self, force=False):
        if not force and not self.isVisible() and self.owner.tool != "paint":
            self._stale = True  # read when the tab shows, or when its map is asked for
            return
        self._stale = False
        current = self.current()
        self.list.blockSignals(True)
        self.list.clear()
        entries, missing = [], {}
        if self.groom:
            try:
                entries = groomlab.maps(self.groom)
                missing = _missing_map_files(self.groom, entries)
            except RuntimeError:
                entries, missing = [], {}
        for m in entries:
            color = m.get("channels") == 4
            lost = m["name"] in missing
            item = QtWidgets.QListWidgetItem("%s%s   (%s%s)" % ("\u26a0 " if lost else "", m["name"],
                                                               KIND_LABELS.get(m["kind"], m["kind"]),
                                                               ", color" if color else ""))
            item.setIcon(_map_icon(4 if color else 1))
            item.setData(QtCore.Qt.UserRole, m["name"])
            item.setData(QtCore.Qt.UserRole + 1, m["kind"])
            item.setData(QtCore.Qt.UserRole + 2, 4 if color else 1)
            tip = KIND_TIPS.get(m["kind"], "") + "\n" + VALUES_TIPS["color" if color else "gray"] + \
                ("\n" + m["texture"] if m.get("texture") else "")
            if lost:
                expected = groomlab.map_texture(self.groom, m["name"])
                item.setData(MISSING_ROLE, expected)
                item.setForeground(theme.current().qcolor(theme.current().get("ui.warning", "yellow")))
                tip = ("The image is missing: %s\nRight-click > Relink Image... to point the map at it (or GroomLab > "
                       "Utilities > Find Missing Files...)." % expected)
            item.setToolTip(tip)
            self.list.addItem(item)
        if not (current and self.select(current)) and self.list.count():
            self.list.setCurrentItem(self.list.item(0))
        self.list.blockSignals(False)
        self.map_changed(self.current(), push=False)
        self.push_paint_flags()

    def push_paint_flags(self):
        """While painting, the tool follows this tab's groom and map: after the active groom
        changes (or its maps do) the tool gets the map's kind and this groom's file, never
        keeping the previous groom's."""
        name = self.current()
        if self.groom and name and self.owner.tool == "paint":
            self.owner.push(attribute=name, **self.context_flags())

    def paint_flags(self):
        """The Paint brush's flags for the picked map (brush_tool's names): where it lives (and a
        texture's file), and the color and channels a color map is painted with."""
        kind = self.current_kind() or "guides"
        texture = ""
        if kind == "texture" and self.groom:
            texture = next((m["texture"] for m in groomlab.maps(self.groom) if m["name"] == self.current()), "")
        return {"paint_kind": kind, "paint_texture": texture, "paint_color": self.paint_color,
                "paint_channels": self.paint_channels}

    def context_flags(self):
        """paint_flags as the brush context's flags."""
        flags = self.paint_flags()
        return {"paintKind": flags["paint_kind"], "paintTexture": flags["paint_texture"],
                "paintColor": tuple(flags["paint_color"]), "paintChannels": flags["paint_channels"],
                "paintMode": self.paint_mode}

    def paint_mode_changed(self, mode):
        self.paint_mode = mode or "replace"
        if self.owner.tool == "paint":
            self.owner.push(paintMode=self.paint_mode)

    # -- Color maps --------------------------------------------------------------

    def _show_swatch(self):
        r, g, b, a = [int(round(c * 255)) for c in self.paint_color]
        self.swatch.setStyleSheet("QToolButton { background: rgb(%d, %d, %d); border-radius: %dpx; border: %dpx solid %s; }"
                                  % (r, g, b, theme.px(4), max(1, theme.px(1)), theme.current().color("ui.textDim")))
        self.swatch.setToolTip("Paint color r %.2f g %.2f b %.2f a %.2f: click to pick (Ctrl paints its inverse)"
                               % self.paint_color)

    def pick_paint_color(self):
        """The swatch's click: Qt's color dialog (never opened by scripts; they call set_paint_color)."""
        from groomlab.ui.qt import QtGui
        start = QtGui.QColor.fromRgbF(*self.paint_color)
        chosen = QtWidgets.QColorDialog.getColor(start, self.window(), "Paint Color",
                                                 QtWidgets.QColorDialog.ShowAlphaChannel)
        if chosen.isValid():
            self.set_paint_color((chosen.redF(), chosen.greenF(), chosen.blueF(), chosen.alphaF()))

    def set_paint_color(self, color):
        """The color painted on a color map (r, g, b[, a]), sent to the Paint brush."""
        values = [min(max(float(c), 0.0), 1.0) for c in color]
        if len(values) == 3:
            values.append(1.0)
        self.paint_color = tuple(values)
        self._show_swatch()
        if self.owner.tool == "paint":
            self.owner.push(paintColor=self.paint_color)

    def set_paint_channels(self, letters):
        """The channels painting, smoothing and filling change ("rgba" or a subset)."""
        letters = "".join(c for c in CHANNELS if c in (letters or ""))
        if not letters:
            raise ValueError("pick at least one channel")
        for letter, b in self.channel_buttons.items():
            b.blockSignals(True)
            b.setChecked(letter in letters)
            b.blockSignals(False)
        self.channels_changed()

    def channels_changed(self, toggled=None):
        """A channel toggle changed (`toggled`, its letter): the channels sent to the tool."""
        letters = "".join(c for c in CHANNELS if self.channel_buttons[c].isChecked())
        if not letters:  # at least one: the last one stays on
            self.channel_buttons[toggled or "r"].setChecked(True)
            return
        self.paint_channels = letters
        if self.owner.tool == "paint":
            self.owner.push(paintChannels=letters)

    def set_channels(self, channels):
        """Make the picked map gray (1) or color (4)."""
        if not self._need_map():
            return
        name = self.current()
        if self.run(groomlab.set_map_channels, self.groom, name, channels):
            self.refresh()
            self.select(name)

    def view_channel_changed(self):
        if self.groom:
            from maya import cmds
            cmds.setAttr(self.groom + ".scalpMapChannel", self.view_channel.currentData() or "rgb", type="string")

    def _sync_color_controls(self):
        """A gray map shows the value; a color map the swatch and channels (the value too: Fill with it)."""
        color = self.current_channels() == 4
        for widget in (self.color_label, self.color_row, self.view_channel):
            widget.setVisible(color)
        for widget in (self.value_label, self.value_row):
            widget.setVisible(not color)
        if self.groom:
            from maya import cmds
            try:
                shown = cmds.getAttr(self.groom + ".scalpMapChannel") or "rgb"
            except (RuntimeError, ValueError):
                shown = "rgb"
            self.view_channel.blockSignals(True)
            self.view_channel.setCurrentIndex(max(0, self.view_channel.findData(shown)))
            self.view_channel.blockSignals(False)

    # What painting turns on to show the map: the strands colored by it, and the scalp.
    PAINT_VIEWS = (("strandColorMode", None), ("strandColorSource", "string"), ("strandColorMin", None),
                   ("strandColorMax", None), ("scalpMapDisplay", "string"))

    def _views(self, groom):
        from maya import cmds
        return {attr: cmds.getAttr(groom + "." + attr) for attr, _ in self.PAINT_VIEWS}

    def show_while_painting(self, name):
        """While the Paint brush is on a map, the strands show it (and the scalp, a scalp map's);
        end_painting() puts back what they showed before painting started."""
        if not self.groom or not name:
            return
        painted = self.__dict__.setdefault("_painted_views", {})
        if self.groom not in painted:
            painted[self.groom] = [self._views(self.groom), None]
        groomlab.view_map(self.groom, name)
        if self.current_kind() in ("vertices", "texture"):
            groomlab.show_map_on_scalp(self.groom, name)
        painted[self.groom][1] = self._views(self.groom)  # what painting shows now
        self._sync_views()

    def end_painting(self):
        """Painting stopped: every groom it showed a map on shows what it did before, except a
        view the artist changed since (that stays as they set it)."""
        from maya import cmds
        painted, self._painted_views = self.__dict__.get("_painted_views", {}), {}
        for groom, (before, shown) in painted.items():
            try:
                now = self._views(groom)
                for attr, kind in self.PAINT_VIEWS:
                    if now[attr] == shown[attr] and now[attr] != before[attr]:
                        value = before[attr] if before[attr] is not None else ""
                        if kind:
                            cmds.setAttr(groom + "." + attr, value, type=kind)
                        else:
                            cmds.setAttr(groom + "." + attr, value)
            except (RuntimeError, ValueError):
                continue  # the groom is gone (a new scene, deleted)
        if self.groom:
            try:
                self._sync_views()
            except (RuntimeError, ValueError):
                pass

    def map_changed(self, name, push=True):
        from maya import cmds
        if not self.groom:
            self.users.setText("")
            return
        self._sync_color_controls()
        self.owner.sync_alpha()  # a scalp map takes an alpha, a guide map not
        if push and name and self.owner.tool == "paint":
            self.owner.push(attribute=name, **self.context_flags())
            self.show_while_painting(name)
        self._sync_views()
        users = groomlab.map_users(self.groom, name) if name else []
        self.users.setText("Read by: " + ", ".join("%s.%s" % u for u in users) if users else
                           ("Not read by the graph yet: + New Map > For a setting makes a map a setting reads; or in "
                            "the Graph Editor, right-click a param > Paint, or add a Map node." if name else
                            "No maps yet: + New Map (For: a setting such as Density, or blank), or in the Graph "
                            "Editor right-click a param > Paint."))

    def _sync_views(self):
        from maya import cmds
        name = self.current()
        mode = cmds.getAttr(self.groom + ".strandColorMode")
        source = cmds.getAttr(self.groom + ".strandColorSource")
        for box, on in ((self.view_strands, bool(name) and mode == 2 and source == name),
                        (self.view_guides, bool(name) and cmds.getAttr(self.groom + ".guideColorMode") == 1 and
                         cmds.getAttr(self.groom + ".guideColorLayer") == name),
                        (self.view_scalp, bool(name) and cmds.getAttr(self.groom + ".scalpMapDisplay") == name)):
            box.blockSignals(True)
            box.setChecked(on)
            box.blockSignals(False)

    def _need_map(self):
        if not self.need():
            return False
        if not self.current():
            self.status("Make a map first (New...).")
        return bool(self.current())

    # -- Actions -----------------------------------------------------------------

    def new_map(self, kind="guides", color=False):
        if not self.need():
            return None
        try:
            targets = groomlab.setting_targets(self.groom)
        except (RuntimeError, ValueError, KeyError):
            targets = None
        dialog = NewMapDialog(self, kind=kind, color=color, targets=targets)
        if qt.exec_(dialog) != QtWidgets.QDialog.Accepted:
            return None
        name, kind, resolution, color, target = dialog.values()
        if kind == "texture" and not menu.ensure_scene_saved("texture maps"):
            return None
        if target:
            return self.map_for_setting(target[0], target[1], kind=kind, resolution=resolution)
        return self.make_map(name, kind, resolution=resolution, color=color)

    def map_for_setting(self, node, param, kind="vertices", resolution=1024):
        """+ New Map > for a setting: the map node.param reads, or a white one made (`kind`) and
        linked to it (groomlab.paint_param, the Graph Editor's Paint), picked, and the Paint brush
        on it toward 1 (Ctrl: toward 0; on Density, no hair), all one undo step. Returns the
        map's name, or None (the status line says why)."""
        if not self.need():
            return None
        from groomlab.base import undo_chunk
        with undo_chunk("map for %s.%s" % (node, param)):
            before = groomlab.param_map(self.groom, node, param)
            name = self.run(groomlab.paint_param, self.groom, node, param, kind=kind, resolution=resolution, paint=False)
            if not name:
                return None
            self.refresh(force=True)
            self.select(name)
            channel = before[1] if before else "luminance"
            if channel in CHANNELS:  # one channel of a color map feeds it: paint that one, white
                self.set_paint_channels(channel)
                self.set_paint_color((1.0, 1.0, 1.0, 1.0))
            self.value.setValue(1.0)
            from maya import cmds
            if self.owner.tool == "paint" and cmds.currentCtx() == groomlab.BRUSH_CONTEXT:
                self.owner.push(value=1.0)  # painting already (the Paint tile again would stop it)
                self.map_changed(name)
            else:
                self.owner.activate("paint")
        self.status("%s %s, which %s.%s reads: Ctrl paints it toward 0%s, strokes back toward the value" %
                    ("Painting" if before else "Made", name, node, param,
                     " (no hair there)" if param == "density" else ""))
        return name

    def make_map(self, name, kind, resolution=1024, color=False):
        """Make a map (a color map: black, alpha 1) and pick it; New Map's work after the dialog."""
        if name and self.run(groomlab.add_map, self.groom, name, 0.0, kind=kind, resolution=resolution,
                             color=(0.0, 0.0, 0.0, 1.0) if color else None):
            self.refresh()
            self.select(name)
            return name
        return None

    def import_map(self, path=None):
        """A new texture map from an image (groomlab.import_map): `path`, else picked in a file
        dialog (only a click gets there; scripts pass the path). Returns the map's name."""
        if not self.need():
            return None
        if path is None:
            folder = groomlab.scene_folder() or groomlab.project_root()
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Import Image as Map", folder, IMAGE_FILTER)
            if not path:
                return None
        if not path.lower().endswith(".png") and not menu.ensure_scene_saved("texture maps"):
            return None
        name = self.run(groomlab.import_map, self.groom, path)
        if not name:
            return None
        self.refresh()
        self.select(name)
        if self.owner.tool == "paint":
            self.map_changed(name)
        self.status("%s reads %s" % (name, groomlab.map_texture(self.groom, name)))
        return name

    def relink_image(self, path=None, copy=None):
        """Point the picked map at another image (groomlab.set_map_texture): `path`, else picked in
        a file dialog (only a click gets there; scripts pass the path). Returns the stored path."""
        if not self._need_map():
            return None
        name = self.current()
        if path is None:
            start = self.current_missing() or (groomlab.map_texture(self.groom, name) if self.current_kind() == "texture"
                                               else "")
            folder = os.path.dirname(start) if start else ""
            if not folder or not os.path.isdir(folder):
                folder = groomlab.scene_folder() or groomlab.project_root()
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Relink Image: %s" % name, folder, IMAGE_FILTER)
            if not path:
                return None
            if not path.lower().endswith(".png") and not menu.ensure_scene_saved("texture maps"):
                return None
        stored = self.run(groomlab.set_map_texture, self.groom, name, path, copy=copy)
        if not stored:
            return None
        self.refresh()
        self.select(name)
        if self.owner.tool == "paint":
            self.map_changed(name)
        self.status("%s reads %s" % (name, stored))
        return stored

    def reload_image(self):
        """Read the picked texture map's image again (edited outside Maya)."""
        if not self._need_map():
            return None
        name = self.current()
        files = self.run(groomlab.reload_map, self.groom, name)
        if files:
            self.refresh()
            self.status("%s: image reloaded" % name if not self.current_missing() else
                        "%s: the image is missing (%s)" % (name, self.current_missing()))
        return files

    def show_in_explorer(self):
        """Show the picked texture map's image in the file browser (its folder when the file is
        missing). Returns what was shown, or ""."""
        if not self._need_map() or self.current_kind() != "texture":
            return ""
        path = groomlab.map_texture(self.groom, self.current())
        shown = reveal_file(path) if path else ""
        if not shown:
            self.status("Neither the image nor its folder is there: %s" % path)
        return shown

    def rename_map(self):
        if not self._need_map():
            return
        old = self.current()
        name, ok = QtWidgets.QInputDialog.getText(self, "Rename Map", "New name:", text=old)
        if ok and name and name != old and self.run(groomlab.rename_map, self.groom, old, name):
            self.refresh()
            self.select(name)

    def delete_map(self):
        if not self._need_map():
            return
        name = self.current()
        users = groomlab.map_users(self.groom, name)
        if users and QtWidgets.QMessageBox.question(
                self, "Delete Map", "Delete %s? %s go%s back to %s own value%s (undo brings the map back)."
                % (name, ", ".join("%s.%s" % u for u in users), "" if len(users) > 1 else "es",
                   "their" if len(users) > 1 else "its", "s" if len(users) > 1 else "")) != QtWidgets.QMessageBox.Yes:
            return
        self.run(groomlab.remove_map, self.groom, name, delete_file=delete_map_files())
        self.refresh()

    def convert_map(self, kind):
        if not self._need_map():
            return
        name = self.current()
        if kind == self.current_kind():
            self.status("%s already lives on %s." % (name, {"guides": "the guides", "vertices": "the scalp's vertices", "texture": "a texture"}[kind]))
            return
        resolution = 1024
        if kind == "texture":
            if not menu.ensure_scene_saved("texture maps"):
                return
            sizes = ["512", "1024", "2048", "4096"]
            picked, ok = QtWidgets.QInputDialog.getItem(self, "Convert to Texture", "Resolution:", sizes, 1, False)
            if not ok:
                return
            resolution = int(picked)
        if self.run(groomlab.convert_map, self.groom, name, kind, resolution=resolution):
            self.refresh()
            self.select(name)
            if self.owner.tool == "paint":
                self.map_changed(name)

    def fill(self, selected_only=None):
        """Set the map to the value: on the selected guides when there are some and the map lives on
        the guides (`selected_only` None), else everywhere."""
        if not self._need_map():
            return
        if self.current_channels() == 4:  # a color map: the color, in the channels ticked
            if self.run(groomlab.fill_map, self.groom, self.current(), color=self.paint_color,
                        channels=self.paint_channels, mode=self.paint_mode):
                self.status("Filled %s's %s with the color." % (self.current(), self.paint_channels))
            return
        if selected_only is None:
            selected_only = self.current_kind() == "guides" and bool(groomlab.selected_guides(self.groom))
        if self.run(groomlab.fill_map, self.groom, self.current(), self.value.value(), selected_only=selected_only,
                    mode=self.paint_mode):
            self.status("Filled %s with %g%s." % (self.current(), self.value.value(),
                                                  " on the selected guides" if selected_only else ""))

    def unfreeze_all(self):
        if self.need():
            self.run(groomlab.unfreeze_all, self.groom)

    def toggle_view_guides(self, on):
        if self._need_map():
            groomlab.color_guides_by_map(self.groom, self.current() if on else None)

    def toggle_view_strands(self, on):
        if self._need_map():
            groomlab.view_map(self.groom, self.current() if on else None)

    def toggle_view_scalp(self, on):
        if self._need_map():
            groomlab.show_map_on_scalp(self.groom, self.current() if on else None)


# ---------------------------------------------------------------------------
# Correctives (the Correctives tab)

BASE = "<base groom>"  # the base groom's row (never a layer name: those are identifiers)
NODE = "node:"  # a Sculpt node's row: NODE + its name (a layer name has no colon)


class CorrectivesBox(_GroomTab):
    """The groom as a stack, as Yeti's layers: the base groom (its guides) and
    correctives on top. The picked row is what brushes edit. A corrective's
    weight is keyable (its name in the Channel Box): unkeyed it applies on every
    frame; a shot fix is keyed 0 -> 1 -> 0 around its frame. The graph's Sculpt
    nodes show above the base groom ("Node: sculpt1"): picked, brushes shape that
    node's guides (groomlab.sculpts)."""

    WATCH = ("correctiveData", "shotLayer", "shotSculpt", "sculptData", "sculptNode", "graph")
    # What the stack's bar and right-click menus run (engine/ui/lists.json's correctives); the menus
    # act on the row clicked (picked first).
    ACTIONS = {
        "corrective.new": "new_layer",
        "corrective.fixFrame": "fix_frame",
        "corrective.up": lambda self: self.move(1),
        "corrective.down": lambda self: self.move(-1),
        "corrective.solo": "solo",
        "corrective.delete": "delete",
        "corrective.goTo": lambda self: (self.run(self.shots.go_to_layer, self.groom, self.current()), self._changed()),
        "corrective.keyWeight": "key_weight",
        "corrective.clearKeys": lambda self: (self.run(self.shots.clear_keys, self.groom, self.current()),
                                              self._changed()),
        "corrective.rename": lambda self: self.tree.editItem(self.tree.currentItem(), 0),
        "sculpt.clearEdits": "clear_node_edits",
    }

    def __init__(self, owner):
        super(CorrectivesBox, self).__init__(owner)
        from groomlab import sculpts, shots
        self.shots = shots
        self.sculpts = sculpts
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(theme.px(6))
        lists = definitions.get("lists/correctives")
        layout.addWidget(_about(lists["about"]))
        bar, _ = _bar(lists["bar"], definitions.binder(self.ACTIONS, self), self)
        layout.addLayout(bar)

        self.tree = QtWidgets.QTreeWidget()
        self.tree.setHeaderLabels(["Layer", "Weight", "Keys"])
        self.tree.setRootIsDecorated(False)
        self.tree.header().setStretchLastSection(False)
        self.tree.header().setSectionResizeMode(0, QtWidgets.QHeaderView.Stretch)
        self.tree.setMinimumHeight(theme.px(110))
        self.tree.setEditTriggers(QtWidgets.QAbstractItemView.DoubleClicked | QtWidgets.QAbstractItemView.EditKeyPressed)
        self.tree.setToolTip("Click: what brushes edit. Checkbox: show or hide. Double-click the name: rename. "
                             "Weight: now, on this frame. Right-click: more")
        self.tree.currentItemChanged.connect(lambda item, _: self.picked(item))
        self.tree.itemChanged.connect(self.item_changed)
        self.tree.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self.context_menu)
        layout.addWidget(self.tree, 1)

        row = QtWidgets.QHBoxLayout()
        row.addWidget(QtWidgets.QLabel("Weight"))
        self.weight = widgets.SoftSlider(1.0, 0.0, 1.0, minimum=0.0, key=None)
        self.weight.setToolTip("The corrective's weight on this frame (above 1 exaggerates). A keyed weight is "
                               "keyed here as you change it")
        self.weight.committed.connect(self.set_weight)
        self.weight.dragged.connect(self.set_weight)
        row.addWidget(self.weight, 1)
        self.key = QtWidgets.QToolButton()
        self.key.setText("◆")
        self.key.setToolTip(definitions.action("corrective.keyWeight")["tip"])
        self.key.setFixedSize(theme.px(28), theme.px(26))
        self.key.clicked.connect(self.key_weight)
        row.addWidget(self.key)
        layout.addLayout(row)
        self._state = None

    # -- helpers

    def current(self):
        """The picked corrective's name ("" for the base groom, a Sculpt node or nothing)."""
        item = self.tree.currentItem()
        name = item.data(0, QtCore.Qt.UserRole) if item else ""
        return "" if name == BASE or (name or "").startswith(NODE) else (name or "")

    def _picked_layer(self):
        if not self.need():
            return None
        name = self.current()
        if not name:
            self.status("Pick a corrective.")
        return name or None

    def _changed(self):
        self._state = None
        self._layers = None
        self.refresh()

    # -- actions

    def new_layer(self):
        if self.need():
            self.run(self.shots.add_layer, self.groom, all_frames=True)
            self._changed()

    def fix_frame(self):
        if self.need():
            self.run(self.shots.add_layer, self.groom, fade=6.0)
            self._changed()

    def move(self, step):
        name = self._picked_layer()
        if name:
            self.run(self.shots.move_layer, self.groom, name, step)
            self._changed()

    def solo(self):
        name = self._picked_layer()
        if name:
            self.run(self.shots.solo_layer, self.groom, name)
            self._changed()

    def delete(self):
        name = self._picked_layer()
        if name:
            self.run(self.shots.remove_layer, self.groom, name)
            self._changed()

    def set_weight(self, value):
        name = self.current()
        if self.groom and name:
            self.run(self.shots.set_layer, self.groom, name, weight=value)
            self._changed()

    def key_weight(self):
        name = self._picked_layer()
        if name:
            self.run(self.shots.key_weight, self.groom, name, value=self.weight.value())
            self._changed()

    def clear_node_edits(self, node=None):
        """A Sculpt node's row: drop the strokes kept in it (the picked row's node by default)."""
        if node is None:
            item = self.tree.currentItem()
            name = item.data(0, QtCore.Qt.UserRole) if item else ""
            node = name[len(NODE):] if name.startswith(NODE) else None
        if node:
            self.run(self.sculpts.clear_edits, self.groom, node)
            self._changed()

    def picked(self, item):
        if not (self.groom and item):
            return
        name = item.data(0, QtCore.Qt.UserRole)
        if name.startswith(NODE):
            node = name[len(NODE):]
            if node != self.sculpts.target(self.groom):
                self.run(self.sculpts.set_target, self.groom, node)
            QtCore.QTimer.singleShot(0, self._changed)
            return
        target = None if name == BASE else name
        if target != self.shots.target(self.groom) or self.sculpts.target(self.groom):
            self.run(self.shots.set_target, self.groom, target)
        QtCore.QTimer.singleShot(0, self._changed)  # not while the tree is changing its current item

    def item_changed(self, item, column):
        name = item.data(0, QtCore.Qt.UserRole)
        if not self.groom or name == BASE or name.startswith(NODE) or column != 0:
            return
        layer = next((l for l in self.shots.layers(self.groom) if l["name"] == name), None)
        if layer is None:
            return
        on = item.checkState(0) == QtCore.Qt.Checked
        text = item.text(0).strip()
        if on != layer["enabled"]:
            self.run(self.shots.set_layer, self.groom, name, enabled=on)
        if text and text != name:
            self.run(self.shots.set_layer, self.groom, name, rename=text)
        QtCore.QTimer.singleShot(0, self._changed)  # not inside the item's own signal

    def context_menu(self, pos):
        item = self.tree.itemAt(pos)
        name = item.data(0, QtCore.Qt.UserRole) if item else None
        if not name or name == BASE:
            return
        m = QtWidgets.QMenu(self)
        if name.startswith(NODE):
            node = name[len(NODE):]
            actions = dict(self.ACTIONS, **{"sculpt.clearEdits": definitions.bound(lambda: self.clear_node_edits(node))})
            widgets.fill_menu(m, definitions.get("lists/correctives/nodeContext"), definitions.binder(actions, self))
            qt.exec_(m, self.tree.viewport().mapToGlobal(pos))
            return
        self.tree.setCurrentItem(item)
        widgets.fill_menu(m, definitions.get("lists/correctives/context"), definitions.binder(self.ACTIONS, self))
        qt.exec_(m, self.tree.viewport().mapToGlobal(pos))

    # -- refresh

    def tick(self, full=False):
        """The palette's poll: the weights on this frame (`full`: the whole stack), only while the box
        shows; shown again, it reads the whole stack."""
        if not self.isVisible():
            self._hidden = True
            return
        if getattr(self, "_hidden", False):
            self._hidden = False
            full = True
        self.refresh(weights_only=not full)

    def refresh(self, weights_only=False):
        """Rebuild the stack. With `weights_only` (the palette's tick) the layers read last time are
        kept and only their weights on this frame are read again: cheap enough for every tick."""
        from maya import cmds
        if not self.groom or not cmds.objExists(self.groom):
            self.tree.clear()
            self._state = None
            self._layers = None
            return
        cached = getattr(self, "_layers", None)
        if weights_only and cached is not None and cached[0] == self.groom:
            layers, plugs = cached[1], cached[2]
            for l in layers:
                plug = plugs.get(l["name"])
                if plug is None:
                    continue
                try:
                    l["weight"] = cmds.getAttr(plug)  # on this frame
                except (RuntimeError, ValueError):  # gone meanwhile: the next full refresh drops it
                    pass
        else:
            layers = self.shots.layers(self.groom)
            self._layers = (self.groom, layers, self.shots.weight_plugs(self.groom))
        frame = cmds.currentTime(query=True)
        target = self.shots.target(self.groom)
        sculpting = self.sculpts.target(self.groom)
        # The Sculpt nodes and their edit counts: read again on a full refresh only (the edits are
        # JSON the size of the strokes), the tick keeping the last read.
        if not weights_only or getattr(self, "_nodes", None) is None or self._nodes[0] != self.groom:
            self._nodes = (self.groom, [(n, self.sculpts.edited_guides(self.groom, n))
                                        for n in self.sculpts.sculpt_nodes(self.groom)])
        nodes = self._nodes[1]
        state = (self.groom, frame, target, sculpting, nodes, [sorted((k, str(v)) for k, v in l.items()) for l in layers])
        if state == self._state:
            return
        self._state = state
        self.tree.blockSignals(True)
        self.tree.clear()
        picked = None
        for l in reversed(layers):  # top of the stack first
            now = l["weight"] if l["enabled"] else 0.0
            keys = "◆ %d" % len(l["keys"]) if l["keyed"] else "all frames"
            item = QtWidgets.QTreeWidgetItem(self.tree, [l["name"], "%d%%" % round(100 * now), keys])
            item.setData(0, QtCore.Qt.UserRole, l["name"])
            item.setFlags(item.flags() | QtCore.Qt.ItemIsUserCheckable | QtCore.Qt.ItemIsEditable)
            item.setCheckState(0, QtCore.Qt.Checked if l["enabled"] else QtCore.Qt.Unchecked)
            item.setToolTip(0, "%s: %d guides corrected%s" % (
                l["name"], l["guides"], "; strongest on frame %g" % l["frame"] if l["frame"] is not None else ""))
            if l["name"] == target:
                picked = item
        # The Sculpt nodes: their guides as the graph makes them, brushed in the node.
        for node, edited in nodes:
            item = QtWidgets.QTreeWidgetItem(self.tree, ["Node: " + node, "", "%d edited" % edited if edited else ""])
            item.setData(0, QtCore.Qt.UserRole, NODE + node)
            item.setToolTip(0, "Brushes shape %s's guides (the graph's, made again when what feeds it changes; the "
                               "strokes follow them). Right-click: Clear Edits" % node)
            if node == sculpting:
                picked = item
        # The base groom at the bottom: the stack's foundation.
        base = QtWidgets.QTreeWidgetItem(self.tree, ["Base groom", "", ""])
        base.setData(0, QtCore.Qt.UserRole, BASE)
        base.setToolTip(0, "Brushes edit the guides themselves")
        font = base.font(0)
        font.setBold(True)
        base.setFont(0, font)
        self.tree.setCurrentItem(picked or base)
        self.tree.blockSignals(False)
        for column in (1, 2):
            self.tree.resizeColumnToContents(column)
        layer = next((l for l in layers if l["name"] == target), None)
        self.weight.setEnabled(layer is not None)
        self.key.setEnabled(layer is not None)
        if layer is not None and not self.weight.slider.isSliderDown():
            self.weight.setValue(layer["weight"])


# ---------------------------------------------------------------------------
# Reference (photos as cameras)


PHOTO_FILTER = "Images (*.png *.jpg *.jpeg *.tif *.tiff *.tga *.bmp *.exr *.iff);;All Files (*.*)"


class ReferenceTab(_GroomTab):
    """Grooming from photos: each photo a camera (groomlab.reference). Line one up with the mesh
    (Align Photo...: the Photos window, the photo and the mesh side by side, or by eye looking
    through it), lock it, and set how the photo shows. The list follows the viewport: looking
    through a reference elsewhere picks it here."""

    WATCH = ()
    # What the photo list's and the point pairs' bars and menus run (engine/ui/lists.json's photos).
    ACTIONS = {
        "photo.import": lambda self: self.import_photo(),
        "photo.lookThrough": "look_through",
        "photo.rename": "rename",
        "photo.selectCamera": "select_camera",
        "photo.remove": "remove",
        "photo.addView": "cameras_menu",  # a menu: the cameras with an image plane, as it opens
        "photo.align": lambda self: self.open_window("align"),
        "photos.guides": lambda self: self.open_window("guides"),
    }

    def __init__(self, owner):
        super(ReferenceTab, self).__init__(owner)
        from groomlab import reference
        self.ref = reference
        self._thumbs = {}   # (path, mtime) -> QIcon
        self._rows = None   # what the list shows
        self._shown = None  # what the controls and pairs show
        # No scroll area of its own: the Groom Tools' column scrolls (a second one inside it clipped
        # the sections below and scrolled apart from the rest).
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(theme.px(6))
        lists = definitions.get("lists/photos")
        bind = definitions.binder(self.ACTIONS, self)
        layout.addWidget(_about(lists["about"]))
        bar, made = _bar(lists["bar"], bind, self)
        made[0].setObjectName("referenceImport")
        layout.addLayout(bar)

        self.list = QtWidgets.QListWidget()
        self.list.setObjectName("referenceList")
        self.list.setIconSize(QtCore.QSize(theme.px(48), theme.px(36)))
        self.list.setMinimumHeight(theme.px(96))
        self.list.setToolTip("The reference photos. Double-click: look through it; right-click: more")
        self.list.currentItemChanged.connect(lambda *_: self.picked())
        self.list.itemDoubleClicked.connect(lambda _: self.look_through())
        self.list.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.list.customContextMenuRequested.connect(
            lambda pos: self.list_menu(self.list.viewport().mapToGlobal(pos)))
        layout.addWidget(self.list, 1)

        # How it shows: looking through, locked, the photo's visibility, opacity and side.
        row = QtWidgets.QHBoxLayout()
        row.setSpacing(theme.px(6))
        self.look = QtWidgets.QPushButton("Look Through")
        self.look.setObjectName("referenceLook")
        self.look.setCheckable(True)
        self.look.setToolTip("Show the photo's camera in the viewport (again: back to the camera it showed before)")
        self.look.clicked.connect(lambda on: self.look_through() if on else self.look_back())
        row.addWidget(self.look, 1)
        self.lock = QtWidgets.QToolButton()
        self.lock.setText("Lock")
        self.lock.setObjectName("referenceLock")
        self.lock.setCheckable(True)
        self.lock.setProperty("role", "chip")
        self.lock.setToolTip("Lock the camera where it is: its position, rotation, focal length and the photo on it "
                             "(tumbling through it does nothing)")
        self.lock.toggled.connect(self.set_locked)
        row.addWidget(self.lock)
        layout.addLayout(row)
        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setContentsMargins(0, 0, 0, 0)
        self.opacity = widgets.SoftSlider(reference.DEFAULT_OPACITY, 0.0, 1.0, minimum=0.0, maximum=1.0, key=None)
        self.opacity.setObjectName("referenceOpacity")
        self.opacity.setToolTip("How opaque the photo is: see the mesh through it")
        self.opacity.committed.connect(self.set_opacity)
        self.opacity.dragged.connect(self.set_opacity)
        form.addRow("Opacity", self.opacity)
        self.focal = widgets.SoftSlider(reference.DEFAULT_FOCAL, reference.FOCAL_RANGE[0], reference.FOCAL_RANGE[1],
                                        minimum=2.5, maximum=3500.0, key=None)
        self.focal.setObjectName("referenceFocal")
        self.focal.setToolTip("The photo camera's focal length (mm): Align Photo works it out from the point pairs; set "
                              "it by hand when you know the lens (Keep focal length turns on, so the alignment keeps "
                              "it). A locked camera keeps its own")
        self.focal.committed.connect(self.set_focal)
        self.focal.dragged.connect(self.set_focal)
        self.focal.menu_hook = lambda menu_: menu_.addAction(
            "Reset Focal Length (%g mm)" % reference.DEFAULT_FOCAL, self.reset_focal)
        form.addRow("Focal Length", self.focal)
        sides = QtWidgets.QHBoxLayout()
        sides.setSpacing(theme.px(4))
        self.side_buttons = {}
        for key, label, tip in [(c["value"], c["label"], c["tip"]) for c in definitions.choices("photoDepth")]:
            b = QtWidgets.QToolButton()
            b.setText(label)
            b.setObjectName("referenceDepth_" + key)
            b.setCheckable(True)
            b.setAutoExclusive(True)
            b.setProperty("role", "chip")
            b.setToolTip(tip)
            b.clicked.connect(lambda _=False, k=key: self.set_depth(k))
            sides.addWidget(b)
            self.side_buttons[key] = b
        sides.addStretch(1)
        self.visible = QtWidgets.QToolButton()  # the eye, as the groom's in the header
        self.visible.setObjectName("referenceVisible")
        self.visible.setCheckable(True)
        self.visible.setAutoRaise(True)
        self.visible.setProperty("role", "glyph")
        self.visible.setIconSize(QtCore.QSize(theme.px(20), theme.px(20)))
        self.visible.setToolTip("Show or hide the photo (the camera stays)")
        self.visible.toggled.connect(self.set_visible)
        self._eye(True)
        sides.addWidget(self.visible)
        form.addRow(definitions.get("choices/photoDepth")["label"], sides)
        layout.addLayout(form)

        # Align Photo...: the Photos window (the photo and the mesh side by side), and where it stands.
        item = definitions.resolve(lists["alignBar"], None)[0]
        align = QtWidgets.QPushButton(item["label"])  # a full-width button, as Look Through
        align.setObjectName("referenceAlign")
        align.setToolTip(item["tip"])
        align.clicked.connect(lambda _=False, s=bind(item): s())
        layout.addWidget(align)
        self.align_about = QtWidgets.QLabel()
        self.align_about.setObjectName("referenceAlignAbout")
        self.align_about.setWordWrap(True)
        self.align_about.setProperty("role", "hint")
        layout.addWidget(self.align_about)
        self._controls = (self.look, self.lock, self.opacity, self.focal, self.visible, align) + \
            tuple(self.side_buttons.values())
        self._build_generate(layout)
        self.refresh()

    # -- Guides from Photos (experimental) -------------------------------------------

    def _build_generate(self, layout):
        """The Guides from Photos section: the Reference window does the work (the photo and the
        mesh side by side: landmarks, flow arrows, the mask, Generate, the measured look); here, a
        way in and where things stand."""
        heading = QtWidgets.QLabel("GUIDES FROM PHOTOS (EXPERIMENTAL)")
        heading.setProperty("role", "subheading")
        heading.setObjectName("referenceGuidesHeading")
        layout.addWidget(heading)
        self.gen_hint = QtWidgets.QLabel()
        self.gen_hint.setObjectName("referenceGuidesHint")
        self.gen_hint.setWordWrap(True)
        self.gen_hint.setProperty("role", "hint")
        layout.addWidget(self.gen_hint)
        self.cameras_menu = QtWidgets.QMenu(self)
        self.cameras_menu.aboutToShow.connect(lambda m=self.cameras_menu: self._fill_cameras_menu(m))
        bar, made = _bar(definitions.get("lists/photos/guidesBar"), definitions.binder(self.ACTIONS, self), self)
        made[0].setObjectName("referenceOpenWindow")
        made[1].setObjectName("referenceAddView")
        layout.addLayout(bar)

    def open_window(self, stage="align"):
        """The Photos window on this groom (on the picked photo): its Align stage (line the photo up)
        or its Guides stage (Guides from Photos)."""
        from groomlab.ui import reference_window
        if not self.need():
            return None
        window = reference_window.show(self.groom, stage=stage)
        if window is not None and self.current():
            window.select_photo(self.current())
        return window

    def _fill_cameras_menu(self, menu_):
        menu_.clear()
        try:
            cameras = self.ref.image_plane_cameras()
        except (RuntimeError, ValueError):
            cameras = []
        if not cameras:
            action = menu_.addAction("(no other camera has an image plane)")
            action.setEnabled(False)
        for camera in cameras:
            menu_.addAction(camera.split("|")[-1], lambda c=camera: self.add_view(c))

    def add_view(self, camera):
        """A camera with a photo on its image plane, as a reference."""
        made = self.run(self.ref.adopt_camera, camera)
        if made:
            self.refresh()
            self.select(made)
            self.status("%s is a reference now: line it up in Guides from Photos." % made.split("|")[-1])
        return made

    def show_generate(self):
        """Where Guides from Photos stands: the photos, their landmarks and arrows, the next step."""
        from groomlab import from_reference
        rows = []
        try:
            for name in self.ref.list_references():
                pairs = len(self.ref.complete_pairs(name))
                strokes = len(from_reference.reference_hints(self.groom, name)) if self.groom else 0
                rows.append((name, pairs, strokes))
        except (RuntimeError, ValueError):
            pass
        if not rows:
            step = "Open Guides from Photos, import a photo of the groom and line it up with a few landmarks."
        else:
            step = "; ".join("%s: %d landmark pair%s, %d flow arrow%s" % (
                name.split("|")[-1], pairs, "" if pairs == 1 else "s", strokes, "" if strokes == 1 else "s")
                for name, pairs, strokes in rows) + ". Generate in Guides from Photos."
        if self.groom and from_reference.measured_look(self.groom):
            step += " A measured look is ready to apply there."
        self.gen_hint.setText(step)

    # -- The list ------------------------------------------------------------------

    def current(self):
        """The picked reference camera, or None."""
        item = self.list.currentItem()
        name = item.data(QtCore.Qt.UserRole) if item else None
        return name if name and self._exists(name) else None

    @staticmethod
    def _exists(name):
        from maya import cmds
        return bool(name) and cmds.objExists(name)

    def select(self, camera):
        for i in range(self.list.count()):
            if self.list.item(i).data(QtCore.Qt.UserRole) == camera:
                self.list.setCurrentRow(i)
                return True
        return False

    def _thumbnail(self, path):
        from groomlab.ui.qt import QtGui
        try:
            key = (path, os.path.getmtime(path))
        except OSError:
            return QtGui.QIcon()
        if key not in self._thumbs:
            size = self.list.iconSize()
            image = QtGui.QImage(path)
            pixmap = QtGui.QPixmap.fromImage(image.scaled(size, QtCore.Qt.KeepAspectRatio,
                                                          QtCore.Qt.SmoothTransformation)) if not image.isNull() else None
            self._thumbs[key] = QtGui.QIcon(pixmap) if pixmap is not None else QtGui.QIcon()
        return self._thumbs[key]

    def refresh(self):
        """The list of references (rebuilt when they change), then the picked one's controls."""
        try:
            names = self.ref.list_references()
            rows = []
            for name in names:
                info = self.ref.reference_info(name)
                rows.append((name, info["image"], info["width"], info["height"], info["locked"]))
        except (RuntimeError, ValueError):
            rows = []
        if rows != self._rows:
            self._rows = rows
            current = self.current()
            self.list.blockSignals(True)
            self.list.clear()
            for name, path, width, height, locked in rows:
                item = QtWidgets.QListWidgetItem("%s\n%d x %d%s" % (name.split("|")[-1], width, height,
                                                                    "  ·  locked" if locked else ""))
                item.setData(QtCore.Qt.UserRole, name)
                item.setIcon(self._thumbnail(path))
                item.setToolTip(path)
                self.list.addItem(item)
            if not (current and self.select(current)) and self.list.count():
                self.list.setCurrentRow(0)
            self.list.blockSignals(False)
            self._shown = None
        self.show_controls()
        self.show_generate()

    def picked(self):
        """Another reference picked: its controls. While the viewport looks through a reference, it
        looks through the picked one instead (Back to Perspective still returns to the camera before)."""
        camera = self.current()
        try:
            viewing = self.ref.current_reference()
        except (RuntimeError, ValueError):
            viewing = None
        if camera and viewing and not self.ref._same_node(viewing, camera):
            self.run(self.ref.look_through, camera)
        self._shown = None
        self.show_controls()

    def show_controls(self):
        camera = self.current()
        for widget in self._controls:
            widget.setEnabled(camera is not None)
        if not camera:
            self.align_about.setText("Import a photo to start.")
            self._shown = None
            return
        info = self.ref.reference_info(camera)
        state = (camera, info["locked"], info["visible"], round(info["opacity"], 4), info["depth"])
        self.focal.setEnabled(not info["locked"])  # (a locked camera keeps its focal length)
        if not self.focal.slider.isSliderDown() and abs(self.focal.value() - info["focal"]) > 1e-4:
            self.focal.setValue(info["focal"])
        if state != (self._shown or (None,))[0:5]:
            for widget, value in ((self.lock, info["locked"]), (self.visible, info["visible"])):
                widget.blockSignals(True)
                widget.setChecked(value)
                widget.blockSignals(False)
            self._eye(info["visible"])
            if not self.opacity.slider.isSliderDown():
                self.opacity.setValue(info["opacity"])
            self.side_buttons[info["depth"]].setChecked(True)
        self.show_alignment(info)
        self._shown = state
        self.follow_view(camera)

    def show_alignment(self, info):
        """Where the picked photo's alignment stands (its pairs are edited in the Photos window)."""
        complete = sum(1 for p in info["pairs"] if p.get("image") and p.get("mesh"))
        need = self.ref.MIN_PAIRS_FIXED if info["keepFocal"] else self.ref.MIN_PAIRS
        if info["rms"] >= 0 and complete >= self.ref.MIN_PAIRS_FIXED:
            text = "Lined up by %d pairs: %.2f px off on average, %.1f mm." % (complete, info["rms"], info["focal"])
        elif complete:
            text = "%d of %d pairs%s: Align Photo to add more." % (
                complete, need, " (focal length kept)" if info["keepFocal"] else ", or 4 keeping the focal length")
        else:
            text = "Not lined up yet: Align Photo, then click a feature on the photo and the same one on the mesh."
        self.align_about.setText(text)

    def list_menu(self, global_pos):
        menu_ = QtWidgets.QMenu(self)
        widgets.fill_menu(menu_, definitions.get("lists/photos/context"), definitions.binder(self.ACTIONS, self),
                          {"photo"} if self.current() else ())
        qt.exec_(menu_, global_pos)

    # -- Following the viewport ------------------------------------------------------

    def poll(self, full=False):
        """The palette's tick: the list and the controls follow the scene and the viewport's camera,
        rebuilt only when something changed (the photos, the camera looked through, the tool) or with
        `full` (every few seconds, for edits made elsewhere: a photo's landmarks, its opacity)."""
        from maya import cmds
        try:
            names = tuple(self.ref.list_references())
            viewing = self.ref.current_reference()
        except (RuntimeError, ValueError):
            names, viewing = (), None
        state = (names, viewing, cmds.currentCtx(), self.current())
        if full or state != getattr(self, "_poll_state", None):
            self._poll_state = state
            self.refresh()

    def follow_view(self, camera=None):
        """The viewport looks through a reference: pick it here; Look Through shows whether it is on."""
        try:
            viewing = self.ref.current_reference()
        except (RuntimeError, ValueError):
            viewing = None
        if viewing and viewing != self.current() and self.select(viewing):
            return  # picked(): the controls follow
        camera = camera or self.current()
        on = bool(viewing) and viewing == camera
        if self.look.isChecked() != on:
            self.look.blockSignals(True)
            self.look.setChecked(on)
            self.look.blockSignals(False)
        self.look.setText("Back to Perspective" if self.look.isChecked() else "Look Through")

    # -- Actions ----------------------------------------------------------------------

    def _need(self):
        camera = self.current()
        if not camera:
            self.status("Import a photo first.")
        return camera

    def import_photo(self, path=None):
        """A photo as a reference camera; without `path`, a file dialog asks for one (a click)."""
        if path is None:
            from maya import cmds
            start = cmds.workspace(query=True, rootDirectory=True)
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Import Reference Photo", start, PHOTO_FILTER)
            if not path:
                return None
        if not menu.ensure_scene_saved("reference photos"):
            return None
        camera = self.run(self.ref.import_reference, path, groom=self.groom)
        if camera:
            self.refresh()
            self.select(camera)
            self.status("Imported %s: Align Photo... lines it up with the mesh." % camera.split("|")[-1])
        return camera

    def rename(self):
        camera = self._need()
        if not camera:
            return
        name, ok = QtWidgets.QInputDialog.getText(self, "Rename Reference", "New name:", text=camera.split("|")[-1])
        if ok and name:
            new = self.run(self.ref.rename_reference, camera, name)
            if new:
                self.refresh()
                self.select(new)

    def remove(self):
        """Remove the picked reference (undoable; its photo file stays)."""
        camera = self._need()
        if camera and self.run(self.ref.remove_reference, camera):
            self.status("Removed %s (Undo brings it back; its photo stays on disk)." % camera.split("|")[-1])
            self.refresh()

    def select_camera(self):
        camera = self._need()
        if camera:
            from maya import cmds
            cmds.select(camera)

    def look_through(self):
        camera = self._need()
        if camera and self.run(self.ref.look_through, camera):
            self.status("")
        self.follow_view()

    def look_back(self):
        self.run(self.ref.look_back)
        self.follow_view()

    def set_locked(self, on):
        camera = self._need()
        if camera:
            self.run(self.ref.lock_reference, camera, on)
            self.refresh()

    def _eye(self, on):
        self.visible.glyph = "eye" if on else "eyeOff"
        self.visible.setIcon(widgets.glyph(self.visible.glyph))

    def set_visible(self, on):
        self._eye(on)
        camera = self._need()
        if camera:
            self.run(self.ref.set_reference_visible, camera, on)

    def set_opacity(self, value):
        camera = self._need()
        if camera:
            self.run(self.ref.set_reference_opacity, camera, value)

    def set_focal(self, value):
        """The focal length set by hand (Keep focal length turns on: the alignment keeps it)."""
        camera = self._need()
        if camera:
            self.run(self.ref.set_reference_focal, camera, value)

    def reset_focal(self):
        """The focal length back to the default lens and the slider to its starting range (a solve gone
        wrong); Keep focal length turns on, as with any focal length set by hand."""
        camera = self._need()
        if camera:
            self.focal.setRange(*self.ref.FOCAL_RANGE)
            self.focal.setValue(self.ref.DEFAULT_FOCAL)
            self.set_focal(self.ref.DEFAULT_FOCAL)

    def set_depth(self, where):
        camera = self._need()
        if camera:
            self.run(self.ref.set_reference_depth, camera, where)
