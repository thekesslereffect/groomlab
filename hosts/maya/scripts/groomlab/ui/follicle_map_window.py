# SPDX-License-Identifier: Apache-2.0
"""Bake Follicle Map (GroomLab > Utilities > Bake Follicle Map): a linear gradient round every
strand's root baked into the scalp's uvs for skin shaders. A plain window over Maya, never modal.

From the top: the groom, the Radius (how far each follicle's gradient reaches on the skin, in scene
units; it opens on the radius measured from the roots' spacing, Measure gives it back, and it may
reach well past the strands' width), the Resolution, the output folder (the scene's exports
folder) and name, and Bake; under it the map written. The gradient is 1 - distance / radius:
shape it in the shader (a clamp or a remap for a hard dot, a power for a softer one). The work
is groomlab.bake_follicle_map's.

    from groomlab.ui import follicle_map_window
    follicle_map_window.show(groom)
"""

import os

from groomlab.ui.qt import QtCore, QtGui, QtWidgets

from groomlab.ui import theme, widgets

_window = None

RESOLUTIONS = ("1024", "2048", "4096", "8192")


def _groomlab():
    import groomlab
    return groomlab


class FollicleMapPanel(QtWidgets.QWidget):
    """The Bake Follicle Map panel (see the module)."""

    def __init__(self, groom=None, parent=None):
        super(FollicleMapPanel, self).__init__(parent)
        self.setObjectName("groomLabFollicleMap")
        outer = QtWidgets.QVBoxLayout(self)
        pad = theme.px(14)
        outer.setContentsMargins(pad, pad, pad, pad)
        outer.setSpacing(theme.px(8))

        title = QtWidgets.QLabel("Bake Follicle Map")
        title.setProperty("role", "title")
        outer.addWidget(title)
        self.groom_label = QtWidgets.QLabel()
        self.groom_label.setProperty("role", "about")
        outer.addWidget(self.groom_label)
        about = QtWidgets.QLabel("A gradient round every root: white at the root, falling evenly to black at the "
                                 "radius (1 - distance / radius). Shape it in the skin shader: a clamp or a remap "
                                 "for a hard dot, a power for a softer one.")
        about.setWordWrap(True)
        about.setProperty("role", "about")
        outer.addWidget(about)

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        radius_row = QtWidgets.QHBoxLayout()
        self.radius = QtWidgets.QDoubleSpinBox()
        self.radius.setObjectName("follicleMapRadius")
        self.radius.setDecimals(4)
        self.radius.setRange(0.0001, 1000.0)
        self.radius.setSingleStep(0.01)
        self.radius.setKeyboardTracking(False)
        self.radius.setToolTip("How far each follicle's gradient reaches on the skin (scene units): any length, past "
                               "the strands' width for a wider shadow")
        radius_row.addWidget(self.radius, 1)
        self.measure_button = QtWidgets.QPushButton("Measure")
        self.measure_button.setObjectName("follicleMapMeasure")
        self.measure_button.setToolTip("The radius from the roots' spacing: 0.4 of the usual distance from a root to its "
                                       "nearest neighbour (the gradients just short of touching)")
        self.measure_button.clicked.connect(self.measure)
        radius_row.addWidget(self.measure_button)
        form.addRow("Radius", radius_row)
        self.resolution = QtWidgets.QComboBox()
        self.resolution.setObjectName("follicleMapResolution")
        self.resolution.setEditable(True)
        self.resolution.addItems(RESOLUTIONS)
        self.resolution.setCurrentText("2048")
        self.resolution.setToolTip("Texels across each tile (16 to 8192); a UDIM scalp bakes a map per tile")
        form.addRow("Resolution", self.resolution)
        folder_row = QtWidgets.QHBoxLayout()
        self.folder = QtWidgets.QLineEdit()
        self.folder.setObjectName("follicleMapFolder")
        folder_row.addWidget(self.folder, 1)
        browse = QtWidgets.QPushButton("...")
        browse.setToolTip("Pick the folder")
        browse.clicked.connect(self.browse)
        folder_row.addWidget(browse)
        form.addRow("Folder", folder_row)
        self.name = QtWidgets.QLineEdit()
        self.name.setObjectName("follicleMapName")
        self.name.setToolTip("The file's name: <name>_FollicleMap.png")
        form.addRow("Name", self.name)
        outer.addLayout(form)

        self.bake_button = QtWidgets.QPushButton("Bake")
        self.bake_button.setObjectName("follicleMapBake")
        self.bake_button.setProperty("role", "primary")
        self.bake_button.clicked.connect(self.bake)
        outer.addWidget(self.bake_button)
        self.status = QtWidgets.QLabel()
        self.status.setWordWrap(True)
        self.status.setProperty("role", "about")
        outer.addWidget(self.status)
        self.preview = QtWidgets.QLabel()
        self.preview.setAlignment(QtCore.Qt.AlignCenter)
        self.preview.setVisible(False)
        outer.addWidget(self.preview, 1)
        outer.addStretch(1)
        self.set_groom(groom)

    def set_groom(self, groom):
        gl = _groomlab()
        self.groom = groom
        self.groom_label.setText(groom or "Select a groom")
        if not self.folder.text():
            self.folder.setText(gl.asset_folder("exports"))
        if groom:
            self.name.setText(gl.asset_name(groom))
            self.measure()

    def measure(self):
        """The radius from the roots' spacing into the field; returns it (None without a groom)."""
        if not self.groom:
            return None
        try:
            radius = _groomlab().follicle_auto_radius(self.groom)
        except Exception as e:  # noqa: BLE001 (the panel says why)
            self.status.setText("Can't measure: %s" % str(e).strip())
            return None
        if radius > 0.0:
            self.radius.setValue(radius)
            self.status.setText("Radius measured from the roots' spacing: %.4g" % radius)
        return radius

    def resolution_value(self):
        text = "".join(ch for ch in self.resolution.currentText() if ch.isdigit())
        return max(16, min(8192, int(text or 2048)))

    def browse(self):
        from maya import cmds
        picked = cmds.fileDialog2(fileMode=3, dialogStyle=2, caption="Folder for the follicle map",
                                  startingDirectory=self.folder.text() or _groomlab().asset_folder("exports"))
        if picked:
            self.folder.setText(picked[0])

    def bake(self):
        """Bakes the map; returns groomlab.bake_follicle_map's result (None when it could not)."""
        if not self.groom:
            self.status.setText("Select a groom first")
            return None
        try:
            with widgets.busy([self.bake_button], self.status, "Baking the follicle map..."):
                baked = _groomlab().bake_follicle_map(self.groom, directory=self.folder.text().strip() or None,
                                                      name=self.name.text().strip() or None,
                                                      resolution=self.resolution_value(), radius=self.radius.value())
        except Exception as e:  # noqa: BLE001 (the panel says why, never a bare traceback)
            self.status.setText("Bake failed: %s" % str(e).strip())
            return None
        self.status.setText("Wrote %s (radius %.4g)" % (baked["pattern"], baked["radius"]))
        image = QtGui.QImage(baked["files"][0]) if baked["files"] else QtGui.QImage()
        if not image.isNull():
            size = theme.px(220)
            self.preview.setPixmap(QtGui.QPixmap.fromImage(image.scaled(size, size, QtCore.Qt.KeepAspectRatio,
                                                                        QtCore.Qt.SmoothTransformation)))
            self.preview.setToolTip(os.path.basename(baked["files"][0]))
        self.preview.setVisible(not image.isNull())
        return baked


def show(groom=None):
    """Open (or raise) the Bake Follicle Map window on `groom` (default: the active groom)."""
    global _window
    from maya.app.general.mayaMixin import MayaQWidgetBaseMixin

    class MayaFollicleMapPanel(MayaQWidgetBaseMixin, FollicleMapPanel):
        pass

    groom = groom or _groomlab().active_groom()
    if _window is not None and not widgets.alive(_window):
        _window = None
    if _window is None:
        _window = MayaFollicleMapPanel(groom=groom)  # (Maya's mixin takes the first argument as the parent)
        _window.setWindowTitle("GroomLab Follicle Map")
        _window.setWindowIcon(widgets.icon("logo"))
        _window.setWindowFlags(_window.windowFlags() | QtCore.Qt.Window)
        _window.resize(theme.px(460), theme.px(480))
        widgets.remember_geometry(_window, "follicleMap")
        widgets.at_exit(_close_at_exit)
    else:
        _window.set_groom(groom)
    _window.show()
    _window.raise_()
    return _window


def window():
    """The open Bake Follicle Map window, or None."""
    return _window if _window is not None and widgets.alive(_window) and _window.isVisible() else None


def _close_at_exit():
    global _window
    w, _window = _window, None
    if w is not None and widgets.alive(w):
        widgets.dispose(w)
