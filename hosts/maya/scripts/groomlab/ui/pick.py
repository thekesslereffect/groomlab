# SPDX-License-Identifier: Apache-2.0
"""Pick mode: the next mesh selected in the viewport or the Outliner goes into an input of a
groom (its scalp, its rest mesh, a collider), for the eyedroppers of the Attribute Editor and the
Graph Editor.

Selecting the mesh first and then pressing a button in the groom's Attribute Editor can't work:
selecting the mesh shows the mesh's Attribute Editor. So the button arms a pick instead, and a
one-shot SelectionChanged scriptJob waits for the next selection, made anywhere (the viewport,
the Outliner). The first mesh the input takes is applied; anything else (no mesh, the scalp as a
collider, nothing) cancels with a message. Either way the groom is selected again, so its
Attribute Editor comes back. Esc, or pressing the button again, cancels without touching the
selection. Nothing modal waits for the pick.
"""

from maya import cmds

_armed = None  # the pick waiting for a selection, or None


class Pick(object):
    """An armed pick: `key` names the input ("scalp", "collider", "collider:<name>"...)."""

    def __init__(self, groom, key, what, check, apply, on_end):
        self.groom, self.key, self.what = groom, key, what
        self.check, self.apply, self.on_end = check, apply, on_end
        self.job = None
        self.esc = None


def armed(groom=None, key=None):
    """The armed pick (for `groom`'s input `key` when given), or None."""
    pick = _armed
    if pick is None or (groom is not None and not _same(pick.groom, groom)) or (key is not None and pick.key != key):
        return None
    return pick


def meshes(selection=None):
    """The mesh shapes (long names, no intermediate shapes) in `selection` (the current one by default),
    in its order: a selected transform gives its meshes."""
    found = cmds.ls(selection if selection is not None else cmds.ls(selection=True, long=True) or [],
                    dag=True, type="mesh", noIntermediate=True, long=True) or []
    return list(dict.fromkeys(found))  # once each, in order


def groom_selected(groom):
    """Whether the groom (or its transform) is selected."""
    selected = cmds.ls(selection=True, dag=True, type="groomLabShape", long=True) or []
    return any(_same(g, groom) for g in selected)


def usable(check, selection=None):
    """The first selected mesh `check` takes (check(mesh) returns "" for it), or None."""
    return next((m for m in meshes(selection) if not check(m)), None)


def start(groom, key, what, check, apply, on_end=None, use_selection=True):
    """Pick a mesh for `groom`'s input `key`.

    `what` names what to click, for messages ("a collider mesh"); `check(mesh)` says why a mesh
    shape can't go there ("" when it can); `apply(mesh)` puts it there and returns a message
    (raising RuntimeError or ValueError when it can't). With `use_selection`, a mesh the input
    takes that is selected already along with the groom goes there now. Else the pick is armed
    until the next selection change: `on_end(applied)` is called when it ends, however it ends.
    Pressing the same input's button again (start with an armed key) cancels it.

    Returns "applied", "armed", "cancelled" or "failed"."""
    global _armed
    if armed(groom, key):
        cancel()
        return "cancelled"
    cancel()  # another input's pick
    if use_selection and groom_selected(groom):
        mesh = usable(check)
        if mesh:
            done = _apply(groom, apply, mesh)
            if done:
                cmds.select(groom)  # as a pick leaves it
                if on_end:
                    on_end(True)
            return "applied" if done else "failed"
    pick = Pick(groom, key, what, check, apply, on_end)
    _armed = pick
    try:
        pick.job = cmds.scriptJob(event=["SelectionChanged", lambda: _selection_changed_soon(pick)],
                                  runOnce=True, killWithScene=True)
    except RuntimeError:  # batch: no UI events (the tests call selection_changed)
        pick.job = None
    pick.esc = _watch_escape()
    _say("Click %s in the viewport or the Outliner (Esc cancels)" % what)
    return "armed"


def cancel(message=None):
    """Drop the armed pick, if any, leaving the selection alone. Returns whether there was one."""
    pick = _armed
    if pick is None:
        return False
    _end(pick, False)
    if message:
        _say(message)
    return True


def _selection_changed_soon(pick):
    # After the selection change is over: applying reselects, and the scalp may ask how to move.
    cmds.evalDeferred(lambda: selection_changed(pick))


def selection_changed(pick=None):
    """The selection changed while `pick` (the armed one by default) waited: apply the first mesh
    its input takes, or cancel; then select the groom again. Returns what happened, as start does."""
    pick = pick or _armed
    if pick is None or pick is not _armed:
        return "cancelled"  # cancelled meanwhile
    _end(pick, False, call=False)
    if not cmds.objExists(pick.groom):
        return "cancelled"
    selected = cmds.ls(selection=True, long=True) or []
    found = meshes(selected)
    mesh = next((m for m in found if not pick.check(m)), None)
    if mesh:
        done = _apply(pick.groom, pick.apply, mesh)
    else:
        done = False
        if found:
            why = pick.check(found[0])
        elif selected:
            why = "%s is not a mesh" % _short(selected[0])
        else:
            why = "nothing picked"
        _say("%s: pick cancelled" % why, warning=True)
    cmds.select(pick.groom)  # its Attribute Editor again
    if pick.on_end:
        pick.on_end(done)
    return "applied" if done else ("failed" if mesh else "cancelled")


def _apply(groom, apply, mesh):
    try:
        message = apply(mesh)
    except (RuntimeError, ValueError) as e:
        _say(str(e), warning=True)
        return False
    if message:
        _say(message)
    return True


def _end(pick, applied, call=True):
    global _armed
    if _armed is pick:
        _armed = None
    if pick.job is not None and cmds.scriptJob(exists=pick.job):
        cmds.scriptJob(kill=pick.job, force=True)
    pick.job = None
    if pick.esc is not None:
        try:
            pick.esc[0].removeEventFilter(pick.esc[1])
        except RuntimeError:  # Qt went first (Maya quitting)
            pass
        pick.esc = None
    if call and pick.on_end:
        pick.on_end(applied)


def _watch_escape():
    """Esc anywhere in Maya cancels the armed pick: an event filter on the application, or None
    (no Qt application: batch)."""
    try:
        from groomlab.ui.qt import QtCore, QtWidgets
    except ImportError:
        return None
    app = QtWidgets.QApplication.instance()
    if app is None:
        return None

    class EscapeFilter(QtCore.QObject):
        def eventFilter(self, obj, event):
            if (event.type() in (QtCore.QEvent.KeyPress, QtCore.QEvent.ShortcutOverride)
                    and event.key() == QtCore.Qt.Key_Escape and _armed is not None):
                QtCore.QTimer.singleShot(0, lambda: cancel("Pick cancelled"))
            return False

    watcher = EscapeFilter(app)
    app.installEventFilter(watcher)
    return app, watcher


# ---------------------------------------------------------------------------
# The groom's mesh inputs: what each takes, and putting a mesh there

WHAT = {"scalp": "the new scalp mesh", "rest": "the scalp's rest mesh", "collider": "a collider mesh",
        "extraScalp": "another scalp mesh"}


def problem(groom, kind, mesh, name=None):
    """Why the mesh (a shape or its transform) can't be the groom's input `kind` ("" when it can).

    scalp: any mesh but the scalp it has. rest: a mesh with the scalp's vertices, not the scalp
    itself. collider (`name`: the collider whose mesh it would be; None: a new one): any mesh but
    the scalp and the groom's other colliders. extraScalp (`name`: the other scalp whose mesh it
    would be; None: a new one): any mesh but the scalp and the groom's other scalps."""
    import groomlab
    shapes = meshes([mesh]) if cmds.objExists(mesh) else []
    if not shapes:
        return "%s is not a mesh" % _short(mesh)
    mesh = shapes[0]  # a transform: its mesh
    scalp = groomlab.scalp_mesh(groom)
    if kind == "scalp":
        return "%s is the scalp already" % _short(mesh) if _same(mesh, scalp) else ""
    if kind == "rest":
        if not scalp:
            return "the groom has no scalp yet"
        if _same(mesh, scalp):
            return "%s is the scalp: pick an undeformed copy of it" % _short(mesh)
        have, want = cmds.polyEvaluate(mesh, vertex=True), cmds.polyEvaluate(scalp, vertex=True)
        if have != want:
            return "%s has %d vertices, the scalp %d: a rest mesh has the scalp's" % (_short(mesh), have, want)
        return ""
    if kind == "collider":
        if _same(mesh, scalp):
            return "%s is the scalp, not a collider" % _short(mesh)
        taken = next((c for c in groomlab.colliders(groom) if _same(c["mesh"], mesh)), None)
        if taken and taken["name"] != name:
            return "%s is a collider already (%s)" % (_short(mesh), taken["name"])
        return ""
    if kind == "extraScalp":
        if _same(mesh, scalp):
            return "%s is the groom's scalp already" % _short(mesh)
        taken = next((s for s in groomlab.scalps(groom) if _same(s["mesh"], mesh)), None)
        if taken and taken["name"] != name:
            return "%s is a scalp of the groom already (%s)" % (_short(mesh), taken["name"])
        return ""
    raise ValueError("no mesh input %r" % kind)


def set_input(groom, kind, mesh, name=None, ask=True):
    """Make the mesh the groom's input `kind` (problem's kinds): the scalp (groomlab.set_scalp;
    with `ask`, a groom with guides asks how they move, as Change Scalp does), the rest mesh
    (set_rest_mesh), a new collider (add_collider: a ColliderMesh and a Collide node), or with
    `name` the collider `name`'s mesh (its connection swapped, the graph as it is). Returns a
    message; raises ValueError when it can't go there."""
    import groomlab
    why = problem(groom, kind, mesh, name)
    if why:
        raise ValueError(why)
    if kind == "scalp":
        return _set_scalp(groom, mesh, ask)
    if kind == "rest":
        groomlab.set_rest_mesh(groom, mesh)
        return "rest mesh: %s" % _short(mesh)
    if kind == "extraScalp":
        if name is None:
            return "scalp: %s (a Scalp, Scatter and Interpolate grow on it)" % groomlab.add_scalp(groom, mesh)
        groomlab.add_scalp(groom, mesh, name=name, wire=False)
        return "scalp %s: %s" % (name, _short(mesh))
    if name is None:
        return "collider: %s" % groomlab.add_collider(groom, mesh)
    groomlab.add_collider(groom, mesh, name=name, wire=False)
    return "collider %s: %s" % (name, _short(mesh))


def _set_scalp(groom, mesh, ask):
    import groomlab
    current = groomlab.scalp_mesh(groom)
    fit = False
    if ask and current and groomlab.list_guide_groups(groom):
        answer = cmds.confirmDialog(
            title="Move the Groom", button=["Closest Point", "Fit and Move", "Cancel"],
            message="Move the guides and parts onto %s?\n\nFit first moves and scales the old scalp onto the new "
                    "one (for a scalp elsewhere or of another size)." % _short(mesh),
            defaultButton="Closest Point", cancelButton="Cancel", dismissString="Cancel")
        if answer == "Cancel":
            return "scalp unchanged"
        fit = answer.startswith("Fit")
    missing = groomlab.set_scalp(groom, mesh, fit=fit)
    message = "scalp: %s" % _short(mesh)
    if missing:
        message += " (not carried onto it: %s)" % ", ".join(missing)
    return message


def input_key(kind, name=None):
    """The pick key of an input: "collider:<name>" for an existing collider's mesh."""
    return kind if name is None else "%s:%s" % (kind, name)


def pick_input(groom, kind, name=None, on_end=None, ask=True, use_selection=True):
    """The eyedropper of the groom's input `kind` (set_input's): a mesh it takes that is
    selected already goes there now; else the next one picked in the viewport or the Outliner
    (start). Returns what start returns."""
    what = WHAT[kind] if name is None else "the mesh of %s %s" % ("scalp" if kind == "extraScalp" else "collider", name)
    return start(groom, input_key(kind, name), what, lambda mesh: problem(groom, kind, mesh, name),
                 lambda mesh: set_input(groom, kind, mesh, name, ask=ask), on_end=on_end, use_selection=use_selection)


def selected_position(groom=None, selection=None):
    """Where the selection is, for a param holding a world position (Transform's
    pivot): the mean of the selected vertices (or CVs, lattice points: any components), else the
    first selected locator's or transform's rotate pivot, in world space and centimeters, as groom
    data is (the scalp is read in world space), whatever the UI's units. `groom` (and its transform)
    is passed over, so it may stay selected. `selection`: names, the current selection by default.
    Returns ((x, y, z), what: the label of what it came from); raises ValueError when nothing
    selected has a position."""
    import maya.api.OpenMaya as om
    names = cmds.ls(selection=True, long=True) if selection is None else cmds.ls(selection, long=True)
    groom_nodes = set()
    if groom:
        groom_nodes = set(cmds.ls(groom, long=True) or []) | set(cmds.listRelatives(groom, parent=True, fullPath=True) or [])
    points, transform, label_of = [], None, ""
    for name in names or []:
        if name.split(".")[0] in groom_nodes:
            continue
        sel = om.MSelectionList()
        try:
            sel.add(name)
            dag, comp = sel.getComponent(0)
        except (RuntimeError, TypeError):
            continue  # not a DAG node
        if not comp.isNull():
            it = om.MItGeometry(dag, comp)
            while not it.isDone():
                points.append(it.position(om.MSpace.kWorld))
                it.next()
            label_of = label_of or name.split("|")[-1]
        elif transform is None and not points:
            if not dag.hasFn(om.MFn.kTransform):
                dag.pop()  # a shape (a locator's): its transform
            if dag.hasFn(om.MFn.kTransform) and dag.length() > 0:
                transform = om.MFnTransform(dag).rotatePivot(om.MSpace.kWorld)
                label_of = dag.partialPathName()
    if points:
        p = om.MPoint(sum(q.x for q in points) / len(points), sum(q.y for q in points) / len(points),
                      sum(q.z for q in points) / len(points))
        if len(points) > 1:
            label_of = "%d points" % len(points)
    elif transform is not None:
        p = om.MPoint(transform)
    else:
        raise ValueError("select a vertex, a locator or a transform first")
    return (p.x, p.y, p.z), label_of


def pick_position(groom, node, param, selection=None):
    """Sets the groom's node `node`'s position param `param` from what is selected
    (selected_position), as the Graph Editor sets a param (a keyed one: its attribute). Returns a
    message saying where it put it; raises ValueError when nothing selected has a position."""
    import groomlab
    position, what = selected_position(groom, selection)
    groomlab.set_param_value(groom, node, param, [float(c) for c in position])
    return "%s.%s set to (%.4g, %.4g, %.4g) from %s" % ((node, param) + tuple(position) + (what,))


def scene_meshes():
    """Every mesh shape in the scene (long names, no intermediate shapes), by their menu labels."""
    shapes = cmds.ls(type="mesh", noIntermediate=True, long=True) or []
    return sorted(shapes, key=lambda s: label(s).lower())


def label(mesh):
    """A mesh shape as a menu shows it: its transform's shortest unique name."""
    parent = cmds.listRelatives(mesh, parent=True, fullPath=True) or [mesh]
    return (cmds.ls(parent[0]) or [parent[0]])[0]


def _say(text, warning=False):
    if warning:
        cmds.warning("GroomLab: %s" % text)
    try:
        cmds.inViewMessage(amg="GroomLab: " + text, pos="topCenter", fade=True)
    except RuntimeError:  # batch: no viewport
        pass


def _short(node):
    return node.split("|")[-1] if node else ""


def _same(a, b):
    """Whether two names are the same node, however named."""
    if not a or not b:
        return False
    if a == b:
        return True
    try:
        return cmds.ls(a, long=True) == cmds.ls(b, long=True)
    except RuntimeError:
        return False
