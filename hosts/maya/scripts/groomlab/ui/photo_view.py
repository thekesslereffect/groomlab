# SPDX-License-Identifier: Apache-2.0
"""The 2D view through a locked reference photo: the viewport's navigation zooms and pans the
picture instead of the camera (reference.zoom_view_2d, pan_view_2d, reset_view_2d).

Looking through a reference that is locked (Groom Tools > Photos > Lock), in the viewport it
shows in:
- the wheel zooms in and out about the cursor,
- Alt+middle drag pans the picture (it follows the cursor),
- Alt+right drag zooms about where the drag began (right or down: in),
- F shows the whole view again.
The camera never moves (a locked one cannot), so the photo stays lined up with the mesh and the
brushes work as ever, picking through the zoomed view. Through anything else (an unlocked
reference, which these move to line it up by eye, or any other camera) the events go on to Maya
untouched. The app's viewport does the same (camera.h's 2D view)."""

from maya import cmds

from groomlab.ui.qt import QtCore, QtWidgets, shiboken

_filters = {}  # model panel -> its ViewFilter (kept alive while the panel's view lives)
WHEEL_STEP = 1.0 / 0.9  # the zoom a wheel notch gives (the app's dolly step, as 2D)


def _view(panel):
    from maya.api import OpenMayaUI as omui
    return omui.M3dView.getM3dViewFromModelPanel(panel)


class ViewFilter(QtCore.QObject):
    """Events of one model panel's view: the 2D gestures while it looks through a locked reference."""

    def __init__(self, panel, widget):
        super(ViewFilter, self).__init__(widget)
        self.panel = panel
        self.widget = widget
        self.drag = None  # ("pan" | "zoom", last position, where it began)
        self.told = set()  # the references the hint was said for (once each look)

    def _locked_reference(self):
        from groomlab import reference
        try:
            camera = reference.current_reference(self.panel)
        except Exception:
            return None
        return camera if camera and reference.is_locked(camera) else None

    def _port(self):
        view = _view(self.panel)
        return view.portWidth(), view.portHeight()

    def _from_middle(self, pos):
        """A widget point as pixels of the port from its middle (x right, y up)."""
        port = self._port()
        scale = port[0] / float(max(self.widget.width(), 1))
        return ((pos.x() - 0.5 * self.widget.width()) * scale, (0.5 * self.widget.height() - pos.y()) * scale), port

    def _hint(self, camera):
        if camera in self.told:
            return
        self.told.add(camera)
        from groomlab.ui import messages
        messages.say("Zooming into the photo in 2D: its camera stays. F shows all of it again.", warning=False, seconds=4)

    def eventFilter(self, obj, event):
        try:
            return self._event(event)
        except Exception:  # (never in the way of Maya's own navigation)
            self.drag = None
            return False

    def _event(self, event):
        kind = event.type()
        if kind == QtCore.QEvent.Wheel:
            camera = self._locked_reference()
            steps = event.angleDelta().y() / 120.0
            if not camera or not steps:
                return False
            from groomlab import reference
            at, port = self._from_middle(event.position() if hasattr(event, "position") else event.pos())
            reference.zoom_view_2d(camera, WHEEL_STEP ** steps, at=at, port=port)
            self._hint(camera)
            event.accept()
            return True
        if kind == QtCore.QEvent.MouseButtonPress:
            alt = bool(event.modifiers() & QtCore.Qt.AltModifier)
            button = event.button()
            if not alt or button not in (QtCore.Qt.MiddleButton, QtCore.Qt.RightButton):
                return False
            camera = self._locked_reference()
            if not camera:
                return False
            self.drag = ("pan" if button == QtCore.Qt.MiddleButton else "zoom", event.pos(), event.pos())
            self._hint(camera)
            event.accept()
            return True
        if kind == QtCore.QEvent.MouseMove and self.drag:
            camera = self._locked_reference()
            if not camera:
                self.drag = None
                return False
            from groomlab import reference
            mode, last, began = self.drag
            pos = event.pos()
            scale = self._port()[0] / float(max(self.widget.width(), 1))
            dx, dy = (pos.x() - last.x()) * scale, (pos.y() - last.y()) * scale
            if mode == "pan":
                reference.pan_view_2d(camera, dx, -dy, port=self._port())
            else:
                at, port = self._from_middle(began)
                # As the app's Alt+right: right or down zooms in (0.005 a pixel, logical).
                factor = 1.0 - ((pos.x() - last.x()) + (pos.y() - last.y())) * 0.005
                reference.zoom_view_2d(camera, 1.0 / max(factor, 0.05), at=at, port=port)
            self.drag = (mode, pos, began)
            event.accept()
            return True
        if kind == QtCore.QEvent.MouseButtonRelease and self.drag:
            self.drag = None
            event.accept()
            return True
        if kind in (QtCore.QEvent.ShortcutOverride, QtCore.QEvent.KeyPress):
            if event.key() != QtCore.Qt.Key_F or event.modifiers() & ~QtCore.Qt.KeypadModifier:
                return False
            camera = self._locked_reference()
            if not camera:
                return False
            event.accept()
            if kind == QtCore.QEvent.KeyPress:
                from groomlab import reference
                reference.reset_view_2d(camera)
            return True
        return False


def watch(panel):
    """The 2D gestures on `panel`'s view (once; again after Maya made its view anew)."""
    if cmds.about(batch=True):
        return None
    try:
        widget = shiboken.wrapInstance(int(_view(panel).widget()), QtWidgets.QWidget)
    except Exception:
        return None
    current = _filters.get(panel)
    if current is not None:
        try:
            if current.widget is widget or shiboken.getCppPointer(current.widget)[0] == shiboken.getCppPointer(widget)[0]:
                return current
        except Exception:
            pass
    view_filter = ViewFilter(panel, widget)
    widget.installEventFilter(view_filter)
    _filters[panel] = view_filter
    return view_filter
