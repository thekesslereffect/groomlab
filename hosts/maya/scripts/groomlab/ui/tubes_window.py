# SPDX-License-Identifier: Apache-2.0
"""GroomLab > Utilities > Tubes to Curves...: the selected modeled hair tubes (open quad tubes) as
NURBS curves, a curve per edge run along each tube and one through its middle. A plain window over
Maya, never modal (Qt: groomlab.ui.qt); Convert works on what is selected when it is clicked. The
work is groomlab.utils.tubes_to_curves'.

    from groomlab.ui import tubes_window
    w = tubes_window.show()
    w.centers.setChecked(False); w.convert()
"""

from groomlab.ui.qt import QtCore, QtWidgets

from groomlab.ui import theme, widgets

_window = None


def _groom():
    import groomlab
    return groomlab.active_groom()


class TubesWindow(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super(TubesWindow, self).__init__(parent)
        self.setWindowTitle("Tubes to Curves")
        self.setWindowIcon(widgets.icon("logo"))
        self.setObjectName("groomLabTubesWindow")
        outer = QtWidgets.QVBoxLayout(self)
        pad = theme.px(14)
        outer.setContentsMargins(pad, pad, pad, pad)
        outer.setSpacing(theme.px(8))

        title = QtWidgets.QLabel("Tubes to Curves")
        title.setProperty("role", "title")
        outer.addWidget(title)
        about = QtWidgets.QLabel("Select your modeled hair tubes and click Convert. The edge loops along "
                                 "each tube become curves. Added to a groom, they become its guides; "
                                 "otherwise they stay in the scene as Maya curves, grouped in the Outliner.")
        about.setWordWrap(True)
        about.setProperty("role", "about")
        outer.addWidget(about)

        def check(text, on, tip, name):
            box = QtWidgets.QCheckBox(text)
            box.setChecked(on)
            box.setToolTip(tip)
            box.setObjectName(name)
            return box

        self.shapes = check(
            "Create shape curves", True,
            "Make one curve along each lengthwise edge of each tube", "tubesShapes")
        self.centers = check(
            "Create center curve per tube", True,
            "Make one curve through the middle of each tube", "tubesCenters")
        self.centers_group = check(
            "Add to its own group: tubesCenter", True,
            "Put the center curves in a group of their own (tubesCenter in a groom). Off: each center curve "
            "goes with its tube's shape curves", "tubesCentersGroup")
        # Where the curves go: guides of the selected groom, or Maya curves in the scene (one or the other).
        self.to_groom = QtWidgets.QRadioButton("Add to the selected groom")
        self.to_groom.setToolTip("Turn the curves into guides of the selected groom, each kind in its own "
                                 "guide group. With no groom selected, they become Maya curves")
        self.to_groom.setObjectName("tubesToGroom")
        self.to_scene = QtWidgets.QRadioButton("Convert to Maya curves")
        self.to_scene.setToolTip("Make Maya curves in the scene, grouped in the Outliner")
        self.to_scene.setObjectName("tubesToScene")
        self.destination = QtWidgets.QButtonGroup(self)
        self.destination.addButton(self.to_groom)
        self.destination.addButton(self.to_scene)
        self.to_groom.setChecked(True)
        self.existing_groups = check(
            "Add to existing guide groups", False,
            "Add to the tubesShape and tubesCenter guide groups if the groom has them. Off: new groups each "
            "time (tubesShape2, tubesShape3...)", "tubesExistingGroups")
        self.region_map = check(
            "Keep each tube's hair to its own guides", True,
            "Paint a guide map (tubes) with a value per tube and set Interpolate's Region Map to it: each strand "
            "then blends only the guides of the tube rooted nearest it, never a neighbouring tube's",
            "tubesRegionMap")
        self.ignore_bevels = check(
            "Count bevels as one curve", True,
            "Bevels count as one curve however many edges they have: edges that sit much closer "
            "together than the ones beside them make one curve", "tubesIgnoreBevels")
        self.smooth = check(
            "Use the smoothed surface", True,
            "Curves follow the tube as smooth mesh preview (the 3 key) draws it: a point per vertex, "
            "where it lies on the smooth surface. Off: through the vertices as modeled", "tubesSmooth")
        self.volume = check(
            "Add center volume", False,
            "Fill each tube with this many curves: the center curve and others arranged in rings "
            "round it, staying inside the tube from root to tip. Off: one center curve", "tubesVolume")
        self.volume_count = QtWidgets.QSpinBox()
        self.volume_count.setRange(2, 200)
        self.volume_count.setValue(5)
        self.volume_count.setToolTip("How many curves fill each tube, the center curve included")
        self.volume_count.setObjectName("tubesVolumeCount")
        destination_row = QtWidgets.QHBoxLayout()
        destination_row.setSpacing(theme.px(16))
        destination_row.addWidget(self.to_groom)
        destination_row.addWidget(self.to_scene)
        destination_row.addStretch(1)
        outer.addLayout(destination_row)
        outer.addWidget(self.existing_groups)
        outer.addWidget(self.region_map)

        def column(heading, name):
            card = QtWidgets.QFrame()
            card.setProperty("role", "card")
            card.setObjectName(name)
            box = QtWidgets.QVBoxLayout(card)
            inner = theme.px(10)
            box.setContentsMargins(inner, inner, inner, inner)
            box.setSpacing(theme.px(6))
            label = QtWidgets.QLabel(heading)
            label.setProperty("role", "subheading")
            box.addWidget(label)
            return card, box
        columns = QtWidgets.QHBoxLayout()
        columns.setSpacing(theme.px(8))
        shape_card, shape_box = column("Tube Shape", "tubesShapeColumn")
        shape_box.addWidget(self.shapes)
        self.shape_group_hint = QtWidgets.QLabel("Goes in its own group: tubesShape")
        self.shape_group_hint.setProperty("role", "hint")
        self.shape_group_hint.setWordWrap(True)
        shape_box.addWidget(self.shape_group_hint)
        shape_box.addStretch(1)
        center_card, center_box = column("Tube Center", "tubesCenterColumn")
        center_box.addWidget(self.centers)
        center_box.addWidget(self.centers_group)
        volume_row = QtWidgets.QHBoxLayout()
        volume_row.setSpacing(theme.px(8))
        volume_row.addWidget(self.volume)
        volume_row.addWidget(self.volume_count)
        volume_row.addStretch(1)
        center_box.addLayout(volume_row)
        center_box.addStretch(1)
        columns.addWidget(shape_card, 1)
        columns.addWidget(center_card, 1)
        outer.addLayout(columns)
        outer.addWidget(self.ignore_bevels)
        outer.addWidget(self.smooth)

        self.convert_button = QtWidgets.QPushButton("Convert")
        self.convert_button.setProperty("role", "primary")
        self.convert_button.setObjectName("tubesConvert")
        self.convert_button.clicked.connect(self.convert)
        close = QtWidgets.QPushButton("Close")
        close.clicked.connect(self.close)
        # The result, in a frame two lines tall beside the buttons; a longer one scrolls (its scroll bar only then).
        result = QtWidgets.QFrame()
        result.setProperty("role", "card")
        result.setObjectName("tubesResult")
        result_box = QtWidgets.QVBoxLayout(result)
        inner = theme.px(8)
        result_box.setContentsMargins(inner, theme.px(6), theme.px(4), theme.px(6))
        self.status = QtWidgets.QLabel("")
        self.status.setWordWrap(True)
        self.status.setProperty("role", "about")
        self.status.setObjectName("tubesStatus")
        self.status.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)
        self.status.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self.status.setStyleSheet("background: transparent;")
        scroll = QtWidgets.QScrollArea()
        scroll.setObjectName("tubesStatusScroll")
        scroll.setWidget(self.status)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAsNeeded)
        scroll.setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }")
        scroll.setFixedHeight(self.status.fontMetrics().lineSpacing() * 2 + theme.px(4))
        result_box.addWidget(scroll)
        bottom = QtWidgets.QHBoxLayout()
        bottom.setSpacing(theme.px(8))
        bottom.addWidget(result, 1)
        bottom.addWidget(self.convert_button, 0, QtCore.Qt.AlignVCenter)
        bottom.addWidget(close, 0, QtCore.Qt.AlignVCenter)
        outer.addLayout(bottom)

        self.shapes.toggled.connect(self._sync)
        self.centers.toggled.connect(self._sync)
        self.volume.toggled.connect(self._sync)
        self.to_groom.toggled.connect(self._sync)
        self._jobs = []  # scriptJobs: the scene changed (grooms made, renamed, deleted, selected)
        self._refresh_queued = False
        theme.style(self)
        self.refresh()
        self._fit_height()

    def _sync(self, *_):
        """Existing groups only mean something going into a groom; each column's group only with its curves."""
        groom = self.to_groom.isChecked()
        self.existing_groups.setEnabled(groom)
        self.region_map.setEnabled(groom)
        self.shape_group_hint.setEnabled(self.shapes.isChecked())
        self.centers_group.setEnabled(self.centers.isChecked())
        self.volume.setEnabled(self.centers.isChecked())
        self.volume_count.setEnabled(self.centers.isChecked() and self.volume.isChecked())
        self.convert_button.setEnabled(self.shapes.isChecked() or self.centers.isChecked())

    def refresh(self):
        """The groom the curves would go to, named on its checkbox."""
        groom = _groom()
        self.to_groom.setText("Add to the selected groom (%s)" % groom.split("|")[-1] if groom else
                              "Add to the selected groom")
        self._sync()

    def options(self):
        """The choices as tubes_to_curves takes them (the groom: None when not adding to one)."""
        groom = _groom() if self.to_groom.isChecked() else None
        return {"groom": groom, "shapes": self.shapes.isChecked(), "centers": self.centers.isChecked(),
                "centers_group": self.centers_group.isChecked(),
                "existing_groups": self.existing_groups.isChecked(),
                "region_map": self.region_map.isChecked(),
                "ignore_bevels": self.ignore_bevels.isChecked(),
                "smooth": self.smooth.isChecked(),
                "center_count": self.volume_count.value() if self.volume.isChecked() else 1}

    def event(self, e):
        if e.type() == QtCore.QEvent.WindowActivate:  # the groom named as the selection is now
            self.refresh()
        return super(TubesWindow, self).event(e)

    # -- Keeping the groom's name current while open ---------------------------

    _EVENTS = ("SelectionChanged", "DagObjectCreated", "NameChanged", "Undo", "Redo",
               "NewSceneOpened", "SceneOpened")

    def _fit_height(self):
        """Exactly as tall as the contents at the window's width (an older saved size never leaves
        empty space or squeezes them); the width stays the artist's."""
        layout = self.layout()
        if layout.hasHeightForWidth():
            height = layout.totalHeightForWidth(max(self.width(), layout.totalMinimumSize().width()))
        else:
            height = layout.totalSizeHint().height()
        if height > 0 and (self.minimumHeight() != height or self.maximumHeight() != height):
            self.setFixedHeight(height)

    def resizeEvent(self, e):
        super(TubesWindow, self).resizeEvent(e)
        if e.oldSize().width() != e.size().width():  # (wider or narrower: the text wraps anew)
            self._fit_height()

    def showEvent(self, e):
        super(TubesWindow, self).showEvent(e)
        self._fit_height()
        self.watch()

    def hideEvent(self, e):
        self.unwatch()
        super(TubesWindow, self).hideEvent(e)

    def closeEvent(self, e):
        self.unwatch()
        super(TubesWindow, self).closeEvent(e)

    def watch(self):
        """Follow the scene while shown: a groom made, picked, renamed or deleted renames the
        'Add to the selected groom' checkbox at once."""
        import groomlab
        from maya import cmds
        groomlab.on_current_groom(self._current_groom_changed)
        if not self._jobs:
            self._jobs = [cmds.scriptJob(event=[name, self.queue_refresh]) for name in self._EVENTS]
        self.refresh()

    def unwatch(self):
        import groomlab
        from maya import cmds
        groomlab.remove_current_groom_listener(self._current_groom_changed)
        jobs, self._jobs = self._jobs, []
        for job in jobs:
            if cmds.scriptJob(exists=job):
                try:
                    cmds.scriptJob(kill=job, force=True)
                except RuntimeError:  # killed from its own callback: once it is done
                    cmds.evalDeferred(lambda j=job: cmds.scriptJob(exists=j) and cmds.scriptJob(kill=j, force=True))

    def _current_groom_changed(self, _groom):
        self.queue_refresh()

    def queue_refresh(self):
        """One refresh after a burst of scene events (a groom made creates several nodes)."""
        if not widgets.alive(self):  # deleted without closing
            self.unwatch()
            return
        if self._refresh_queued:
            return
        self._refresh_queued = True

        def run():
            self._refresh_queued = False
            if widgets.alive(self) and self.isVisible():
                self.refresh()
        QtCore.QTimer.singleShot(0, run)

    def clear_message(self):
        self.status.setText("")

    def convert(self):
        """Convert the selected tubes with the choices; the result said here and over the viewport."""
        from maya import cmds
        from groomlab import utils
        from groomlab.ui import menu
        self.refresh()
        options = self.options()
        meshes = utils.selected_tube_meshes(options["groom"])
        if not meshes:
            self.status.setText("Select the hair tubes to convert.")
            cmds.warning("GroomLab: select the hair tubes to convert")
            return None
        try:
            result = utils.tubes_to_curves(meshes, **options)
        except ValueError as e:
            self.status.setText(str(e))
            cmds.warning("GroomLab: %s" % e)
            return None
        text = menu.tubes_report(result)
        self.status.setText(text)
        menu._say(text)
        if result["group"]:
            cmds.select(result["group"])
        return result


def show():
    """Open (or raise) the Tubes to Curves window."""
    global _window
    from maya.app.general.mayaMixin import MayaQWidgetBaseMixin

    class MayaTubesWindow(MayaQWidgetBaseMixin, TubesWindow):
        pass

    if _window is not None and not widgets.alive(_window):
        _window = None
    if _window is None:
        _window = MayaTubesWindow()
        _window.setWindowFlags(_window.windowFlags() | QtCore.Qt.Window)
        widgets.remember_geometry(_window, "tubesToCurves")
        widgets.at_exit(_close_at_exit)
    else:
        _window.refresh()
    _window.clear_message()
    _window.show()
    _window.raise_()
    return _window


def window():
    """The open Tubes to Curves window, or None."""
    return _window if _window is not None and widgets.alive(_window) and _window.isVisible() else None


def _close_at_exit():
    global _window
    w, _window = _window, None
    if w is not None and widgets.alive(w):
        w.unwatch()
        widgets.dispose(w)
