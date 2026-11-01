# SPDX-License-Identifier: Apache-2.0
"""What GroomLab's Wrap-style windows share (Transfer Groom: two mesh views; Reference: a photo
and a mesh view): an embedded Maya view of one mesh with its own camera and numbered marker dots,
the clicks reaching the window through a Qt event filter, Ctrl+Z / Ctrl+Y for the window's own
undo stack, and keeping the window's bookkeeping out of Maya's undo queue.

    view = mesh_views.MeshView(holder_widget, "groomLabTransferUI", "groomLabTransferOld", color)
    view.ensure(); view.set_mesh(mesh); view.frame()
    view.set_markers(points, matched, hover)      # the plugin's groomLabMarkerSet draws them
    hit = view.pick(x, y)                         # port pixels: the mesh's point, or None

Picking (transfer.pick_in_view): the view's M3dView turns the port pixel into a ray
(viewToWorld), MFnMesh.closestIntersection gives the face and barycentrics. The filter sees the
mouse moving without a button too (hover), which a dragger context does not, and Maya's current
tool is never changed.
"""

import contextlib

from maya import cmds

from groomlab.ui.qt import QtCore, QtWidgets, shiboken, to_int

MARKER_SET_TYPE = "groomLabMarkerSet"  # the plugin's locator drawing one view's points (marker_set.h)
DOT_RADIUS = 6.0     # a marker dot's radius, logical pixels
PICK_PIXELS = 10.0   # how near (logical pixels) a click must be to a point to grab or delete it

_no_undo_depth = [0]  # nested no_undo blocks open now: only the outermost one turns the queue off and on


@contextlib.contextmanager
def no_undo():
    """A window's own bookkeeping (cameras, marker sets, isolation, what it stores) kept out of
    Maya's undo queue. Only the outermost block switches the queue, back to what it found, so a
    nested block or one a Qt event opens inside another never leaves the queue off."""
    was = _no_undo_depth[0] == 0 and cmds.undoInfo(query=True, state=True)
    _no_undo_depth[0] += 1
    if was:
        cmds.undoInfo(stateWithoutFlush=False)
    try:
        yield
    finally:
        _no_undo_depth[0] -= 1
        if was:
            cmds.undoInfo(stateWithoutFlush=True)


def undo_back_on(was_on, window_name):
    """A safety net when a window goes: the queue on again if it was on when the window opened
    and is off now with no no_undo block open (Maya saves the queue's state in its prefs on quit,
    so a queue left off would stay off in every later session)."""
    if was_on and _no_undo_depth[0] == 0 and not cmds.undoInfo(query=True, state=True):
        cmds.undoInfo(stateWithoutFlush=True)
        cmds.warning("GroomLab: Maya's undo queue was found off when the %s window closed; turned it back on"
                     % window_name)


def maya_name(qobject):
    from maya import OpenMayaUI as omui
    return omui.MQtUtil.fullName(int(shiboken.getCppPointer(qobject)[0]))


def control(name):
    from maya import OpenMayaUI as omui
    pointer = omui.MQtUtil.findControl(name)
    return shiboken.wrapInstance(int(pointer), QtWidgets.QWidget) if pointer else None


class ViewEvents(QtCore.QObject):
    """The mouse in one embedded view, handed to the window's _view_event(side, widget, event)."""

    def __init__(self, window, side):
        super(ViewEvents, self).__init__(window)
        self.window = window
        self.side = side

    def eventFilter(self, obj, event):
        try:
            return self.window._view_event(self.side, obj, event)
        except Exception as e:  # never let a pick error escape into Maya's event loop
            cmds.warning("GroomLab: marker pick: %s" % e)
            return False


class UndoKeys(QtCore.QObject):
    """Ctrl+Z / Ctrl+Y (and Ctrl+Shift+Z) anywhere in a window: its own undo and redo, taken
    before Maya's own hotkeys see them."""

    def __init__(self, window):
        super(UndoKeys, self).__init__(window)
        self.window = window

    @staticmethod
    def which(event):
        mods = to_int(event.modifiers()) & to_int(QtCore.Qt.ControlModifier | QtCore.Qt.ShiftModifier |
                                                   QtCore.Qt.AltModifier)
        ctrl, shift = to_int(QtCore.Qt.ControlModifier), to_int(QtCore.Qt.ShiftModifier)
        if event.key() == QtCore.Qt.Key_Z and mods == ctrl:
            return "undo"
        if (event.key() == QtCore.Qt.Key_Y and mods == ctrl) or (event.key() == QtCore.Qt.Key_Z and mods == ctrl | shift):
            return "redo"
        return None

    def eventFilter(self, obj, event):
        kind = event.type()
        if kind in (QtCore.QEvent.ShortcutOverride, QtCore.QEvent.KeyPress):
            which = self.which(event)
            if which:
                event.accept()
                if kind == QtCore.QEvent.KeyPress:
                    getattr(self.window, which)()
                return True
        return False

    def watch(self, widget):
        """The key filter on `widget` and everything in it."""
        for w in [widget] + widget.findChildren(QtWidgets.QWidget):
            w.installEventFilter(self)


class MeshView(object):
    """One embedded Maya view of a mesh in a holder widget (made once the window is in Maya's UI):
    a paneLayout and a modelEditor with a hidden camera of its own under `group`, showing only
    meshes and locators, isolated to its mesh; a groomLabMarkerSet drawn only through its camera
    holds its numbered points."""

    def __init__(self, holder, group, prefix, color):
        self.holder = holder
        self.group = group
        self.prefix = prefix
        self.color = color
        self.camera = None
        self.editor = None
        self.pane = None
        self.marker_set = None
        self.mesh = None
        self.filter = None

    # ---- Making and going ----------------------------------------------------------------------

    def ensure(self, wireframe=False):
        """The pane, editor, camera and marker set (once)."""
        if self.editor:
            return False
        selection = cmds.ls(selection=True, long=True) or []  # kept: a new camera selects itself
        with no_undo():
            self._make(wireframe)
            selection = [n for n in selection if cmds.objExists(n)]
            if selection:
                cmds.select(selection, replace=True)
            else:
                cmds.select(clear=True)
        return True

    def _make(self, wireframe):
        if not cmds.objExists(self.group):
            cmds.group(empty=True, name=self.group)
            cmds.setAttr(self.group + ".hiddenInOutliner", True)
        cam = cmds.camera(name=self.prefix + "Camera")[0]
        cam = cmds.parent(cam, self.group)[0]
        cam = cmds.ls(cam, long=True)[0]
        cmds.setAttr(cam + ".visibility", False)
        cmds.xform(cam, rotation=(-8, 25, 0), worldSpace=True)
        self.camera = cam
        parent = maya_name(self.holder.layout())
        cmds.setParent(parent)
        self.pane = cmds.paneLayout(configuration="single", parent=parent)
        self.editor = cmds.modelEditor(parent=self.pane, camera=cam, displayAppearance="smoothShaded", grid=False,
                                       headsUpDisplay=False, manipulators=False, selectionHiliteDisplay=False,
                                       wireframeOnShaded=wireframe)
        holder = cmds.createNode("transform", name=self.prefix + "Markers", parent=self.group, skipSelect=True)
        marker_set = cmds.createNode(MARKER_SET_TYPE, name=self.prefix + "MarkersShape", parent=holder,
                                     skipSelect=True)
        self.marker_set = cmds.ls(marker_set, long=True)[0]
        cmds.setAttr(self.marker_set + ".color", *self.color)
        cmds.setAttr(self.marker_set + ".camera", cam, type="string")
        widget = control(self.pane)
        if widget is not None:
            self.holder.layout().addWidget(widget)
        # Meshes and the markers alone (a plugin's draw override, the groom's strands, does not
        # follow isolate select), then isolated to this view's mesh.
        cmds.modelEditor(self.editor, edit=True, allObjects=False)
        cmds.modelEditor(self.editor, edit=True, polymeshes=True, locators=True)
        cmds.isolateSelect(self.editor, state=1)

    def watch(self, window, side):
        """The window's _view_event gets this view's mouse (hover too)."""
        widget = self.view_widget()
        if widget is not None:
            widget.setMouseTracking(True)
            self.filter = ViewEvents(window, side)
            widget.installEventFilter(self.filter)

    def teardown(self, nodes=True):
        """The view goes (and with `nodes`, its isolation set; the group is the window's)."""
        if self.filter is not None:
            w = self.view_widget() if self.editor else None
            if w is not None:
                w.removeEventFilter(self.filter)
            self.filter = None
        with no_undo():
            self.marker_set = None
            if self.editor and cmds.modelEditor(self.editor, exists=True):
                try:
                    view_set = cmds.isolateSelect(self.editor, query=True, viewObjects=True)
                    cmds.isolateSelect(self.editor, state=0)
                    if nodes and view_set and cmds.objExists(view_set):
                        cmds.delete(view_set)
                except RuntimeError:
                    pass
                cmds.deleteUI(self.editor, editor=True)
            if self.pane and cmds.paneLayout(self.pane, exists=True):
                cmds.deleteUI(self.pane, layout=True)
            self.editor = self.pane = self.camera = None

    @staticmethod
    def hand_back_active_view():
        """The active view handed back to a main viewport (before a window's views go)."""
        for panel in cmds.getPanel(visiblePanels=True) or []:
            if cmds.getPanel(typeOf=panel) == "modelPanel":
                try:
                    cmds.modelEditor(panel, edit=True, activeView=True)
                except RuntimeError:
                    continue
                break

    # ---- The view --------------------------------------------------------------------------------

    def view(self):
        """The M3dView (maya.api.OpenMayaUI)."""
        from maya.api import OpenMayaUI as omui
        return omui.M3dView.getM3dViewFromModelEditor(self.editor)

    def view_widget(self):
        try:
            return shiboken.wrapInstance(int(self.view().widget()), QtWidgets.QWidget)
        except Exception:
            return None

    def pixel_scale(self):
        """Port pixels per logical pixel."""
        widget = self.view_widget()
        return self.view().portWidth() / float(max(widget.width(), 1)) if widget is not None else 1.0

    def frame(self, mesh=None):
        """The camera fits the mesh (keeping its direction)."""
        import math
        from maya.api import OpenMaya as om
        from groomlab import transfer
        mesh = mesh or self.mesh
        if not mesh or not self.camera or not cmds.objExists(mesh):
            return
        center, diagonal = transfer.mesh_bounds(mesh)
        radius = diagonal * 0.5
        shape = cmds.listRelatives(self.camera, shapes=True, fullPath=True)[0]
        fov = math.radians(min(cmds.camera(shape, query=True, horizontalFieldOfView=True),
                               cmds.camera(shape, query=True, verticalFieldOfView=True)))
        distance = radius / math.sin(fov * 0.5) * 1.05
        m = cmds.xform(self.camera, query=True, matrix=True, worldSpace=True)
        back = om.MVector(m[8], m[9], m[10]).normal()
        with no_undo():
            cmds.xform(self.camera, translation=[center[i] + back[i] * distance for i in range(3)], worldSpace=True)
            cmds.setAttr(shape + ".centerOfInterest", distance)
            cmds.setAttr(shape + ".nearClipPlane", max(distance * 1e-3, 1e-3))
            cmds.setAttr(shape + ".farClipPlane", distance * 100.0)

    def set_wireframe(self, on):
        if self.editor:
            cmds.modelEditor(self.editor, edit=True, wireframeOnShaded=bool(on))

    def isolate(self, objects):
        """Only `objects` (long names) show."""
        if not self.editor:
            return
        objects = cmds.ls(objects, long=True) or []
        current = self.isolated()
        with no_undo():
            for o in current:
                if o not in objects:
                    cmds.isolateSelect(self.editor, removeDagObject=o)
            for o in objects:
                if o not in current:
                    cmds.isolateSelect(self.editor, addDagObject=o)

    def show_plugin_shapes(self, on):
        """Plugin shapes (a groom's strands and guides) in this view too, or not (they draw whatever
        the isolation: every groom in the scene)."""
        if not self.editor or not cmds.modelEditor(self.editor, exists=True):
            return
        if cmds.modelEditor(self.editor, query=True, pluginShapes=True) != bool(on):
            cmds.modelEditor(self.editor, edit=True, pluginShapes=bool(on))

    def isolated(self):
        """What the view shows (long names)."""
        view_set = cmds.isolateSelect(self.editor, query=True, viewObjects=True)
        return cmds.ls(cmds.sets(view_set, query=True) or [], long=True) if view_set else []

    def show(self, markers=True):
        """The view shows its mesh, and (`markers`) its marker set."""
        shown = [self.mesh] if self.mesh and cmds.objExists(self.mesh) else []
        if markers and self.marker_set and cmds.objExists(self.marker_set):
            shown.append(cmds.listRelatives(self.marker_set, parent=True, fullPath=True)[0])
        self.isolate(shown)

    def schedule_refresh(self):
        try:
            view = self.view()
        except Exception:
            return
        try:
            view.scheduleRefresh()
        except Exception:
            view.refresh(False, False)

    # ---- Points ------------------------------------------------------------------------------------

    def port(self, widget, pos):
        """A widget position as the view's port pixel (origin bottom left)."""
        view = self.view()
        w, h = max(widget.width(), 1), max(widget.height(), 1)
        return pos.x() * view.portWidth() / float(w), view.portHeight() - pos.y() * view.portHeight() / float(h)

    def pick(self, x, y):
        """The point of the mesh under port pixel (x, y): transfer.pick_mesh's dict, or None."""
        from groomlab import transfer
        if not self.mesh or not self.editor:
            return None
        return transfer.pick_in_view(self.view(), self.mesh, x, y)

    def point_near(self, points, x, y, pixels=None):
        """The index of the world point (x, y, z each) projected nearest port pixel (x, y), within
        `pixels` (PICK_PIXELS) logical pixels, or None."""
        from maya.api import OpenMaya as om
        view = self.view()
        limit = (PICK_PIXELS if pixels is None else pixels) * self.pixel_scale()
        best, best_d = None, limit * limit
        for i, p in enumerate(points):
            px, py, inside = view.worldToView(om.MPoint(p[0], p[1], p[2]))
            d = (px - x) ** 2 + (py - y) ** 2
            if inside and d <= best_d:
                best, best_d = i, d
        return best

    def _plug(self, attr):
        from maya.api import OpenMaya as om
        return om.MSelectionList().add("%s.%s" % (self.marker_set, attr)).getPlug(0)

    def set_markers(self, points, matched, hover=-1, bad=None):
        """The marker set given the points ((x, y, z) each), their surface normals on the mesh (the
        behind test), matched flags, the hover and the dot size: set in one go through the API
        (outside the undo queue: the windows keep their own stacks). `bad`: unused here (the
        photo view marks bad pairs)."""
        from maya.api import OpenMaya as om
        from groomlab import transfer
        if not self.marker_set or not cmds.objExists(self.marker_set):
            return
        normals = []
        fn = transfer._mesh_fn(self.mesh) if points and self.mesh and cmds.objExists(self.mesh) else None
        for p in points:
            normal = om.MVector()
            if fn is not None:
                try:
                    normal = fn.getClosestNormal(om.MPoint(p[0], p[1], p[2]), om.MSpace.kWorld)[0]
                except RuntimeError:
                    pass
            normals.append(normal)
        self._plug("points").setMObject(
            om.MFnVectorArrayData().create(om.MVectorArray([om.MVector(p[0], p[1], p[2]) for p in points])))
        self._plug("normals").setMObject(om.MFnVectorArrayData().create(om.MVectorArray(normals)))
        self._plug("matched").setMObject(om.MFnIntArrayData().create(om.MIntArray([1 if m else 0 for m in matched])))
        self._plug("hover").setInt(int(hover))
        scale = self.pixel_scale() if self.editor else 1.0
        self._plug("size").setFloat(DOT_RADIUS * max(scale, 1.0))
        self.schedule_refresh()

    def set_hover(self, hover):
        if self.marker_set and cmds.objExists(self.marker_set):
            self._plug("hover").setInt(int(hover))
            self.schedule_refresh()

    def marker_state(self):
        """What the marker set draws (tests): {"points": [[x, y, z], ...], "matched": [0/1], "hover"}."""
        node = self.marker_set
        points = cmds.getAttr(node + ".points") or []
        return {"points": [list(p) for p in points], "matched": list(cmds.getAttr(node + ".matched") or []),
                "hover": cmds.getAttr(node + ".hover")}

    # ---- Pictures ----------------------------------------------------------------------------------

    def paint_into(self, painter, window, temp_base):
        """The view's color buffer (Viewport 2.0, which a widget grab may miss) painted over where
        it is in `window`'s grab."""
        import os
        from maya.api import OpenMaya as om
        from groomlab.ui.qt import QtGui
        widget = self.view_widget()
        if widget is None:
            return
        view = self.view()
        view.refresh(False, True)
        image = om.MImage()
        view.readColorBuffer(image, True)
        temp = temp_base + ".png"
        image.writeToFile(temp, "png")
        buffer = QtGui.QImage(temp)
        os.remove(temp)
        if not buffer.isNull():
            corner = widget.mapTo(window, QtCore.QPoint(0, 0))
            painter.drawImage(QtCore.QRect(corner, widget.size()), buffer)
