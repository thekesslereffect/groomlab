# SPDX-License-Identifier: Apache-2.0
"""The groom node: creating one, its scalp and rest mesh, its inputs, the active groom, locking, the
GPU look preview and its errors."""

import os

from maya import cmds

from groomlab.base import GROOM_TYPE, ensure_plugin, no_undo, undo_chunk, _original_shape, _scalp_shape, _shape_of
from groomlab.collision import colliders
from groomlab.extra_scalps import scalps
from groomlab.guides import add_guides, list_guide_groups


def create_groom(scalp=None, guides=None, name="groomLab1", fit=False):
    """Create a groom shape, optionally bound to a scalp mesh and guide curves.

    With `fit` (and a scalp), the groom stores the default graph with its defaults that measure
    something fitted to the scalp's size, as presets and nodes added in the Graph Editor are (the
    defaults are authored on a 20 cm head): a 2 m creature or a 2 cm test sphere gets the same
    hair, in proportion. Without, the graph is left empty (the default graph as authored). The
    menu's Create Groom fits.

    Returns the groom shape node name.
    """
    ensure_plugin()

    transform = cmds.createNode("transform", name=name)
    # Named from the transform's own name: createNode puts it in the current namespace already
    # (charA:hair gets charA:hairShape, not charA:charA:hairShape).
    leaf = transform.split("|")[-1].split(":")[-1]
    shape = cmds.createNode(GROOM_TYPE, name=leaf + "Shape", parent=transform)
    cmds.connectAttr("time1.outTime", shape + ".time")
    # The groom is grown in world space (from the scalp's world mesh): moving or
    # parenting its transform would move the hair off the scalp.
    cmds.setAttr(transform + ".inheritsTransform", False)
    for attr in ("translate", "rotate", "scale", "shear"):
        cmds.setAttr(transform + "." + attr, lock=True)

    if scalp:
        cmds.connectAttr(_shape_of(scalp, "mesh") + ".worldMesh[0]", shape + ".inMesh")
        if _original_shape(_shape_of(scalp, "mesh")):
            set_rest_mesh(shape)  # a deformed scalp: keep the same strands on every frame
        if fit:
            import groomlab_core
            from groomlab import presets
            size = presets.scalp_size(shape, scaled=False)  # just made: at 1x
            if size and abs(size / groomlab_core.DEFAULTS_SCALP_SIZE - 1.0) > 1e-6:
                cmds.setAttr(shape + ".graph", groomlab_core.default_graph_json_for(size), type="string")
    if guides:
        add_guides(shape, guides)
    return shape


# The sample meshes shipped with GroomLab (CC BY 4.0, Nick Rutlinh), in meshes/: centimetres, y up,
# facing +z. Each: its file, its largest extent in cm (it comes in that size in any scene unit),
# the name it gets, and the part the groom grows on (None: the only one). A .ma is a rig: its
# skinned meshes posed by its controls (the extent is its meshes').
SAMPLES = {
    "nika": ("nika_head.obj", 34.61, "nika_head", None),  # a head and shoulders
    "manny": ("manny_head.obj", 34.21, "manny_head", None),  # a head
    "wolf": ("wolf.obj", 109.7, "wolf", "wolf_body"),     # a sitting wolf: body, eyes, nails, mouth
    "buddy": ("buddy_rig.ma", 96.65, "buddy_rig", "buddy_body"),  # a posable character (buddy.obj: its body alone)
}
MESHES_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "meshes"))


def add_sample(sample="nika"):
    """Add a sample mesh (SAMPLES: "nika", a head and shoulders; "manny", a head; "wolf", a
    sitting wolf; "buddy", a posable character rig) to the scene at the origin, sized for the
    scene's units and turned for a z-up scene, and select the part to groom: Create Groom from
    Selection grows hair on it. A sample of several parts comes in under a group of its name; a
    rig as it was built (buddy_rig: its controls pose it, a groom follows its skinned body). One
    undo step (a rig none: delete it). Returns the selected part's transform."""
    if sample not in SAMPLES:
        raise ValueError("no sample %r (%s)" % (sample, ", ".join(sorted(SAMPLES))))
    filename, extent_cm, name, scalp = SAMPLES[sample]
    path = os.path.join(MESHES_DIR, filename)
    if not os.path.isfile(path):
        raise RuntimeError("the %s sample is missing from the GroomLab install (%s)" % (sample, path))
    if filename.endswith(".ma"):
        return _add_rig_sample(sample, path, extent_cm, scalp)
    if not cmds.pluginInfo("objExport", query=True, loaded=True):
        cmds.loadPlugin("objExport", quiet=True)
    with undo_chunk("add sample %s" % sample):
        before = set(cmds.ls(type="transform", long=True))
        cmds.file(path, i=True, type="OBJ", options="mo=1", ignoreVersion=True, mergeNamespacesOnClash=False,
                  namespace="groomlab_sample")
        made = [t for t in cmds.ls(type="transform", long=True) if t not in before and cmds.listRelatives(t, shapes=True)]
        if not made:
            raise RuntimeError("the %s sample did not import" % sample)
        made = [cmds.rename(t, t.split("|")[-1].split(":")[-1]) for t in made]
        cmds.namespace(removeNamespace="groomlab_sample", mergeNamespaceWithRoot=True)
        if len(made) == 1:
            top = made[0] = cmds.rename(made[0], name)
        else:
            top = cmds.group(made, name=name, world=True)
            made = cmds.listRelatives(top, children=True, type="transform", fullPath=False)
        factor = _sample_scale(top, extent_cm)
        if factor != 1.0:
            cmds.setAttr(top + ".scale", factor, factor, factor)
        if cmds.upAxis(query=True, axis=True) == "z":
            cmds.setAttr(top + ".rotateX", 90)
        cmds.makeIdentity(top, apply=True, translate=True, rotate=True, scale=True)
        for part in made:
            cmds.delete(part, constructionHistory=True)
        target = next((m for m in made if m.split("|")[-1] == scalp), made[0]) if scalp else made[0]
        cmds.select(target, replace=True)
    return cmds.ls(target, long=False)[0]


def _sample_scale(nodes, extent_cm):
    """The scale that gives `nodes` (their world bounding box) the largest extent `extent_cm` in
    the scene's own units, whatever unit the import read the numbers in: 1.0 when it already has."""
    bbox = cmds.exactWorldBoundingBox(nodes)
    extent = max(bbox[3] - bbox[0], bbox[4] - bbox[1], bbox[5] - bbox[2])
    unit = cmds.currentUnit(query=True, linear=True)
    want = float(cmds.convertUnit(extent_cm, fromUnit="cm", toUnit=unit).rstrip("abcdefghijklmnopqrstuvwxyz"))
    return want / extent if extent > 0 and abs(extent / want - 1.0) > 1e-3 else 1.0


def _bind_where_placed(top, meshes):
    """The rig's skins bound again where its top group (just sized or turned, at the bind pose)
    put them: each influence's bindPreMatrix its joint's inverse world matrix now, each original
    (Orig) shape's points carried by the top group's matrix. The skinned meshes stay where they
    are; their original shapes, which a groom's rest mesh is (create_groom), now match them (else
    a z-up scene's rest pose would lie on its back, a scaled rig's be its file size)."""
    from maya.api import OpenMaya as om

    def sources(node, attr):  # {logical index: the full path of the DAG node plugged in}
        plug = om.MFnDependencyNode(om.MSelectionList().add(node).getDependNode(0)).findPlug(attr, False)
        out = {}
        for k in range(plug.numElements()):
            element = plug.elementByPhysicalIndex(k)
            source = element.source()
            if not source.isNull and source.node().hasFn(om.MFn.kDagNode):
                out[element.logicalIndex()] = om.MDagPath.getAPathTo(source.node()).fullPathName()
        return out

    placed = om.MMatrix(cmds.getAttr(top + ".worldMatrix[0]"))
    for mesh in meshes:
        orig = _original_shape(mesh)
        for skin in cmds.ls(cmds.listHistory(mesh, pruneDagObjects=True) or [], type="skinCluster"):
            fixed = sources(skin, "bindPreMatrix")  # (a rig whose joints drive them: left as they are)
            for i, joint in sources(skin, "matrix").items():
                if i not in fixed:
                    cmds.setAttr("%s.bindPreMatrix[%d]" % (skin, i), cmds.getAttr(joint + ".worldInverseMatrix[0]"),
                                 type="matrix")
            if orig:
                geom = om.MMatrix(cmds.getAttr(skin + ".geomMatrix"))
                carry = geom * placed * geom.inverse()
                fn = om.MFnMesh(om.MSelectionList().add(orig).getDagPath(0))
                fn.setPoints([p * carry for p in fn.getPoints(om.MSpace.kObject)], om.MSpace.kObject)
    joints = cmds.listRelatives(top, allDescendents=True, type="joint", fullPath=True) or []
    for pose in set(cmds.ls(cmds.listConnections(joints, type="dagPose") or [])):
        members = list(sources(pose, "members").values())
        if members:
            cmds.dagPose(members, reset=True, name=pose)  # Go to Bind Pose: this placement


def _add_rig_sample(sample, path, extent_cm, scalp):
    """A rig sample (Maya ASCII: a top group holding the joints, the controls and geometry_GRP,
    whose skinned meshes don't inherit their parents' transforms, so the joints alone move them):
    imported without a namespace (a name taken in the scene gets a number), sized and turned at
    its top group (nothing frozen: the rig stays posable and the joints carry the meshes), the
    part to groom selected. Returns it.

    Maya can't undo a file import, so none of it is recorded (an undo would leave the rig in the
    sample's namespace): delete buddy_rig to take it out."""
    with no_undo():
        new = cmds.file(path, i=True, type="mayaAscii", ignoreVersion=True, mergeNamespacesOnClash=False,
                        namespace="groomlab_sample", returnNewNodes=True, preserveReferences=False) or []
        tops = cmds.ls(new, assemblies=True, long=True) or []
        parts = cmds.listRelatives(cmds.ls(new, type="mesh", noIntermediate=True, long=True) or [], parent=True,
                                   fullPath=True) or []
        if len(tops) != 1 or not parts:
            raise RuntimeError("the %s sample did not import" % sample)
        target = next((p for p in parts if p.split("|")[-1].split(":")[-1] == scalp), parts[0])
        # Kept by UUID through the namespace's merge, which renames what clashes.
        top_id, target_id = cmds.ls(tops[0], uuid=True)[0], cmds.ls(target, uuid=True)[0]
        cmds.namespace(removeNamespace="groomlab_sample", mergeNamespaceWithRoot=True)
        top, target = cmds.ls(top_id, long=True)[0], cmds.ls(target_id, long=True)[0]
        meshes = cmds.ls(cmds.listRelatives(top, allDescendents=True, type="mesh", fullPath=True), noIntermediate=True)
        factor = _sample_scale(meshes, extent_cm)
        if factor != 1.0:
            cmds.setAttr(top + ".scale", factor, factor, factor)
        if cmds.upAxis(query=True, axis=True) == "z":
            cmds.setAttr(top + ".rotateX", 90)
        if factor != 1.0 or cmds.upAxis(query=True, axis=True) == "z":
            _bind_where_placed(top, meshes)
        cmds.select(target, replace=True)
    return cmds.ls(target, long=False)[0]


def set_follow_scalp(groom, on=True):
    """Follow the scalp: grow the groom on its rest mesh and carry each strand to the scalp by
    its root (a rigid turn and move, no stretch), so a deforming scalp re-runs only the carrying.
    Needs a rest mesh (set_rest_mesh). Guide curves and simulated or cached guides keep their
    place; colliders, ImportCurves, Wind's direction and world-space noise are read at rest.
    One undo step."""
    cmds.setAttr(groom + ".followScalp", bool(on))


SCALE_SOURCES = ("area", "transform", "off")


def set_scale_source(groom, source="area"):
    """What the groom's scale follows. The groom grows with its scalp like a child object (lengths,
    widths, clump sizes and guide shapes times the scale, densities divided by its square so the
    strand count stays, curl frequencies divided by it); a deforming scalp keeps one scale.

    "area" (the default): sqrt(scalp area / reference), the area the scalp's world surface at the
    scene's start frame: scaling the scalp's transform, freezing it, re-meshing it at the same size
    and a rig scaled at its root all count, and editing or remodelling the mesh to another size
    scales the groom too (said once in the Script Editor and the view). "transform": the scalp's
    transform scale at the start frame (its world matrix's, times its rig's root joint's) over the
    reference: remodelling leaves the groom as it is, but Freeze Transformations resets the
    transform's scale, so the groom goes back with it. Surface Area survives Freeze
    Transformations; Transform Only survives remodelling. "off": the groom keeps its sizes in
    scene units."""
    if source not in SCALE_SOURCES:
        raise ValueError("scale source %r: one of %s" % (source, ", ".join(SCALE_SOURCES)))
    cmds.setAttr(groom + ".scaleSource", SCALE_SOURCES.index(source))


def scale_source(groom):
    """What the groom's scale follows: "area", "transform" or "off" (set_scale_source)."""
    return SCALE_SOURCES[cmds.getAttr(groom + ".scaleSource")]


def set_scale_with_scalp(groom, on=True):
    """Scale with the scalp (set_scale_source "area") or not ("off"); on by default. Turning it on
    keeps "transform" where the groom follows that."""
    if not on:
        set_scale_source(groom, "off")
    elif scale_source(groom) == "off":
        set_scale_source(groom, "area")


def groom_scale(groom):
    """What the groom is scaled by now (its groomScale: 1.0 at the size it was groomed at, or
    while its scale source is off)."""
    return cmds.getAttr(groom + ".groomScale")


def set_scale_reference(groom):
    """Make the scalp's size now the groom's 1x: the groom keeps the sizes its params and guides
    say at this size (groomScale reads 1.00 again). One undo step. Returns the reference area."""
    area = cmds.getAttr(groom + ".scaleArea")
    if not area or area <= 0.0:
        raise RuntimeError("%s has no scalp to measure" % groom)
    transform = cmds.getAttr(groom + ".scaleTransform")
    with undo_chunk("set_scale_reference"):
        cmds.setAttr(groom + ".scaleReference", area)
        if transform and transform > 0.0:
            cmds.setAttr(groom + ".scaleTransformReference", transform)
    return area


def store_scale_references():
    """Before a save: a groom without a scale reference (made before scaling with the scalp, or
    not saved since it was made) stores the one it uses (the first size it was evaluated at), so
    the groom reopens at the scale it had. The plugin calls it just before every save."""
    for groom in list_grooms():
        if cmds.referenceQuery(groom, isNodeReferenced=True):
            continue
        if cmds.getAttr(groom + ".scaleSource") == 2 or not scalp_mesh(groom):
            continue
        k = cmds.getAttr(groom + ".groomScale")
        if not k > 0.0:
            continue
        area = cmds.getAttr(groom + ".scaleArea")
        if cmds.getAttr(groom + ".scaleReference") <= 0.0 and area > 0.0:
            cmds.setAttr(groom + ".scaleReference", area / (k * k))
        transform = cmds.getAttr(groom + ".scaleTransform")
        if cmds.getAttr(groom + ".scaleTransformReference") <= 0.0 and transform > 0.0:
            cmds.setAttr(groom + ".scaleTransformReference", transform / k)


_IDENTITY = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)


def set_rest_mesh(groom, mesh=None):
    """Give the groom a rest (undeformed) scalp with the same vertices.

    Strands are then scattered and matched to guides and clumps on the rest
    pose, so a deforming scalp keeps the same strands on every frame (needed
    for motion blur and caches). `mesh` defaults to the scalp's original
    shape under its deformers, or else a hidden copy of the scalp at the
    current frame. Returns the rest mesh shape.

    The rest pose stays put however the mesh's transform (or a parent's: a head under an animated
    rig) moves: it is the mesh's own points (object space, its outMesh) placed where its transform
    puts them now, through a transformGeometry node holding that matrix as a value (none when the
    transform is the identity, as a skinned mesh's usually is). A world-space connection would move
    the rest pose every frame, and with it the strands' assignment and the rest pose's caches.
    """
    if mesh is None:
        scalp = _scalp_shape(groom)
        if not scalp:
            raise RuntimeError("%s has no scalp connected" % groom)
        mesh = _original_shape(scalp)
        if not mesh:
            copy = cmds.duplicate(cmds.listRelatives(scalp, parent=True)[0], name=groom + "Rest")[0]
            cmds.hide(copy)
            mesh = _shape_of(copy, "mesh")
    mesh = _shape_of(mesh, "mesh")
    _connect_rest(groom, mesh)
    return mesh


def _connect_rest(groom, mesh):
    """Connects the mesh shape's points, placed by its world matrix as it is now (a value, not a
    connection), to the groom's rest mesh, in place of what was there."""
    clear_rest_mesh(groom)
    matrix = tuple(cmds.getAttr(mesh + ".worldMatrix[0]"))
    if matrix == _IDENTITY:
        cmds.connectAttr(mesh + ".outMesh", groom + ".restMesh", force=True)
        return
    place = cmds.createNode("transformGeometry", name=groom.split("|")[-1].split(":")[-1] + "RestPlace",
                            skipSelect=True)
    cmds.addAttr(place, longName="groomLabRestPlace", attributeType="bool", defaultValue=True)
    cmds.setAttr(place + ".transform", *matrix, type="matrix")
    cmds.connectAttr(mesh + ".outMesh", place + ".inputGeometry")
    cmds.connectAttr(place + ".outputGeometry", groom + ".restMesh", force=True)


def _rest_place(node):
    """Whether `node` is a rest mesh's placement (_connect_rest's transformGeometry)."""
    return cmds.nodeType(node) == "transformGeometry" and cmds.attributeQuery("groomLabRestPlace", node=node, exists=True)


def clear_rest_mesh(groom):
    for plug in cmds.listConnections(groom + ".restMesh", plugs=True, source=True, destination=False) or []:
        cmds.disconnectAttr(plug, groom + ".restMesh")
        node = plug.split(".")[0]
        if _rest_place(node) and not cmds.listConnections(node + ".outputGeometry", source=False, destination=True):
            cmds.delete(node)  # the placement went with it


def scalp_mesh(groom):
    """The groom's scalp mesh shape, or None."""
    return _scalp_shape(groom)


def rest_mesh(groom):
    """The groom's rest mesh shape, or None."""
    sources = cmds.listConnections(groom + ".restMesh", source=True, destination=False, shapes=True) or []
    if sources and _rest_place(sources[0]):
        sources = cmds.listConnections(sources[0] + ".inputGeometry", source=True, destination=False, shapes=True) or []
    return _shape_of(sources[0], "mesh") if sources else None


def set_scalp(groom, mesh, fit=False):
    """Grow the groom on `mesh`. A groom with guides moves onto it
    (transfer_groom: guides re-rooted at the closest points, after fitting the
    old scalp's bounds onto the new one with `fit`, painted maps projected); an
    empty one is just connected. Returns what could not be carried (maps, color
    sets the graph reads that the mesh lacks)."""
    mesh = _shape_of(mesh, "mesh")
    current = scalp_mesh(groom)
    if current and cmds.ls(mesh, long=True) == cmds.ls(current, long=True):  # the same mesh, however named
        return []
    if current and list_guide_groups(groom):
        from groomlab.transfer import transfer_groom
        return transfer_groom(groom, mesh, fit=fit)
    with undo_chunk("set scalp"):
        cmds.connectAttr(mesh + ".worldMesh[0]", groom + ".inMesh", force=True)
        cmds.setAttr(groom + ".scalpTopology", "", type="string")  # this mesh is the groom's now
        clear_rest_mesh(groom)
        if _original_shape(mesh):
            set_rest_mesh(groom)
    return []


def guides_text(summary):
    """ "119 guides in 1 group" from an inputs_summary."""
    plural = lambda n, word: "%d %s%s" % (n, word, "" if n == 1 else "s")
    return "%s in %s" % (plural(summary["guides"], "guide"), plural(summary["groups"], "group"))


def inputs_summary(groom):
    """What the groom is made from, for display: {"scalp", "rest", "guides", "groups",
    "curves", "sim", "cache", "colliders", "scalps" (the other scalps, groomlab.scalps)}."""
    groups = list_guide_groups(groom)
    return {
        "scalp": scalp_mesh(groom),
        "rest": rest_mesh(groom),
        "guides": sum(g["count"] for g in groups),
        "groups": len(groups),
        "curves": len(cmds.listConnections(groom + ".guideCurves", source=True, destination=False) or []),
        "sim": len(cmds.listConnections(groom + ".simGuides", source=True, destination=False) or []),
        "cache": cmds.getAttr(groom + ".guideCache") or "",
        "colliders": colliders(groom),
        "scalps": scalps(groom),
    }


# ---------------------------------------------------------------------------
# The current groom: one for the Groom Tools, the Graph Editor (unless pinned) and the brushes


PIN_VAR, ACTIVE_VAR = "groomLabPinGroom", "groomLabActiveGroom"
# The grooms selected when the current groom was set (their UUIDs): a groom selected since leads.
SEEN_VAR = "groomLabCurrentSelection"
_listeners = []


def on_current_groom(fn):
    """Call fn(groom) whenever the current groom changes (a pick in the Groom Tools or the Graph
    Editor, a groom selected, made or adopted); remove_current_groom_listener(fn) stops it."""
    if fn not in _listeners:
        _listeners.append(fn)
    return fn


def remove_current_groom_listener(fn):
    if fn in _listeners:
        _listeners.remove(fn)


def _notify(groom):
    for fn in list(_listeners):
        try:
            fn(groom)
        except Exception:  # noqa: BLE001 (a window gone, a listener's bug: never the caller's problem)
            import traceback
            traceback.print_exc()


def selected_grooms(selection=None):
    """The groom shapes (long names) selected, in the selection's order, once each: a groom shape,
    and every groom under a selected transform or group at any depth (a character's top group
    gives its grooms). What actions on several grooms take (Save Groom File As, Collect Files,
    Copy Graph); the tools' current groom reads the selection more narrowly (_selected_grooms)."""
    items = (cmds.ls(selection=True, long=True) or []) if selection is None else list(selection)
    if not items:  # (never cmds.ls([]): every node of the scene)
        return []
    return list(dict.fromkeys(cmds.ls(items, dag=True, type=GROOM_TYPE, long=True) or []))


def _selected_grooms():
    """The grooms selected (a groom shape, or the groom shapes right under a selected transform), in
    the selection's order, and their key (UUIDs sorted): what the brushes read (active_groom.cpp)."""
    selection = cmds.ls(selection=True, objectsOnly=True, long=True) or []
    if not selection:
        return [], ""
    # A few calls for the whole selection (it can be thousands of objects), its order kept.
    shapes = set(cmds.ls(selection, type=GROOM_TYPE, long=True) or [])
    under = {}
    for child in cmds.listRelatives(selection, children=True, fullPath=True, type=GROOM_TYPE) or []:
        under.setdefault(child.rsplit("|", 1)[0], []).append(child)
    found = []
    for node in selection:
        found += [node] if node in shapes else under.get(node, [])
    key = " ".join(sorted(set(cmds.ls(found, uuid=True) or []))) if found else ""
    return [cmds.ls(g)[0] for g in found], key


def _seen():
    return cmds.optionVar(query=SEEN_VAR) if cmds.optionVar(exists=SEEN_VAR) else ""


def _record(groom, state, key):
    cmds.optionVar(stringValue=[(ACTIVE_VAR, groom), (SEEN_VAR, key)], intValue=(PIN_VAR, state))


def set_current_groom(groom):
    """Make `groom` the current groom: every GroomLab tool, the Groom Tools and the Graph Editor
    (unless pinned) work on it, whatever else is selected, until another groom is picked or
    selected. None: no current groom (the selected one leads)."""
    previous = active_groom()
    key = _selected_grooms()[1]
    if groom:
        _record(groom, 1, key)
    else:
        cmds.optionVar(intValue=(PIN_VAR, 0), stringValue=(SEEN_VAR, key))
    now = active_groom()
    if now != previous or groom:
        _notify(now)


pin_groom = set_current_groom  # (its name before the Graph Editor had a pin of its own)


def pin_new_groom(groom):
    """A groom just made in the UI (Create Groom from Selection, Import Groom File) is the current
    groom: the tools and the Graph Editor work on it."""
    set_current_groom(groom)


def _groom_var(pin):
    """The groom named in ACTIVE_VAR when PIN_VAR is `pin` and it exists, else None."""
    if cmds.optionVar(query=PIN_VAR) != pin:
        return None
    name = cmds.optionVar(query=ACTIVE_VAR) if cmds.optionVar(exists=ACTIVE_VAR) else ""
    return name if name and cmds.objExists(name) and cmds.nodeType(name) == GROOM_TYPE else None


def pinned_groom():
    """The groom made current by a pick (set_current_groom), a selection or a new groom, or None."""
    return _groom_var(1)


def adopted_groom():
    """The groom a scene opened with (adopt_scene_groom), or None: the tools work on it until a
    groom is picked or selected."""
    return _groom_var(2)


def active_groom():
    """The current groom, the one the tools work on: the groom last picked (set_current_groom),
    selected or made, else the scene's (adopted_groom), else the first selected one. Selecting a
    groom makes it current (recorded here, so it stays current while other things are selected)."""
    selected, key = _selected_grooms()
    if key != _seen():
        if selected:  # a groom selected since the current groom was set: it is current now
            groom = selected[0]
            changed = groom != (_groom_var(1) or _groom_var(2))
            _record(groom, 1, key)
            if changed:
                _notify(groom)
            return groom
        cmds.optionVar(stringValue=(SEEN_VAR, key))  # (selecting that groom again picks it again)
    return _groom_var(1) or _groom_var(2) or (selected[0] if selected else None)


current_groom = active_groom


def adopt_scene_groom():
    """A scene just opened (or the Groom Tools opening) with grooms but none to work on (none
    current, none selected): the tools work on one of them, the groom last worked on when this
    scene has it, else the first, until a groom is picked or selected (PIN_VAR 2). The Groom Tools
    and the Graph Editor show it rather than "Start a groom". Returns the current groom."""
    active = active_groom()
    if active:
        return active
    grooms = sorted(cmds.ls(type=GROOM_TYPE) or [])
    if not grooms:
        return None
    last = cmds.optionVar(query=ACTIVE_VAR) if cmds.optionVar(exists=ACTIVE_VAR) else ""
    found = cmds.ls(last, type=GROOM_TYPE) if last and cmds.objExists(last) else []
    groom = found[0] if found else grooms[0]
    _record(groom, 2, _selected_grooms()[1])
    _notify(groom)
    return groom


def list_grooms():
    """Every groom in the scene."""
    return cmds.ls(type=GROOM_TYPE) or []


# ---------------------------------------------------------------------------
# Showing and hiding in the viewport: the hair (displayHair) and the guides (displayGuides), one
# or every groom. Viewport toggles: renders and exports draw the hair whatever they say.


def hair_visible(groom):
    """Whether the groom's hair shows in the viewport (displayHair: the strands, instances, scales,
    cards and strand meshes; the Groom Tools' eye). The guides are guides_visible's."""
    return bool(cmds.getAttr(groom + ".displayHair"))


def set_hair_visible(groom, on):
    """Show or hide the groom's hair in the viewport (displayHair; its guides stay), one undo step."""
    cmds.setAttr(groom + ".displayHair", bool(on))


def guides_visible(groom):
    """Whether the groom's guides show in the viewport (displayGuides: every guide group at once;
    the groups' own checkboxes hide single groups)."""
    return bool(cmds.getAttr(groom + ".displayGuides"))


def set_guides_visible(groom, on):
    """Show or hide every guide group of the groom (displayGuides), one undo step."""
    cmds.setAttr(groom + ".displayGuides", bool(on))


def set_all_hair_visible(on):
    """Show or hide every groom's hair in the viewport (GroomLab > Display; Shift+click the Groom
    Tools' eye), one undo step. Returns the grooms."""
    grooms = list_grooms()
    with undo_chunk("%s all hair" % ("show" if on else "hide")):
        for groom in grooms:
            try:
                if hair_visible(groom) != bool(on):
                    set_hair_visible(groom, on)
            except RuntimeError:  # locked, keyed or driven: left as it is
                pass
    return grooms


def set_all_guides_visible(on):
    """Show or hide every groom's guides (GroomLab > Display; Shift+click the Groom Tools' guides
    eye), one undo step. Returns the grooms."""
    grooms = list_grooms()
    with undo_chunk("%s all guides" % ("show" if on else "hide")):
        for groom in grooms:
            try:
                if guides_visible(groom) != bool(on):
                    set_guides_visible(groom, on)
            except RuntimeError:  # locked, keyed or driven: left as it is
                pass
    return grooms


def toggle_all_guides():
    """Every groom's guides off when any show, else on (Alt+Shift+G with the GroomLab hotkey set).
    Returns whether they show now."""
    on = not any(guides_visible(g) for g in list_grooms())
    set_all_guides_visible(on)
    return on


def lock_groom(groom, locked=True):
    """A locked groom ignores brushes and part strokes (finished work stays finished)."""
    cmds.setAttr(groom + ".locked", bool(locked))


# ---------------------------------------------------------------------------
# The GPU look preview

GPU_PREVIEW_VAR = "groomLabGpuPreview"


def gpu_preview(enabled=None):
    """Whether a drag of an Interpolate (radius), Length, Clump, Noise, Curl or Width number, and a
    brush stroke's dabs on guides, preview on the GPU (the default) rather than evaluating on the
    CPU; `enabled` sets it, for this session and the next ones (the optionVar groomLabGpuPreview,
    read when the plugin loads; GROOMLAB_GPU_PREVIEW=0 or 1 in the environment forces it). Returns
    the state. The release of a drag or a stroke always commits on the CPU."""
    if enabled is not None:
        cmds.optionVar(intValue=(GPU_PREVIEW_VAR, 1 if enabled else 0))
        cmds.groomLabGpuPreview(enable=bool(enabled))
    return bool(cmds.groomLabGpuPreview(isEnabled=True))


# ---------------------------------------------------------------------------
# Errors


def errors(groom):
    """Why the groom's last evaluation failed or was partial: a list of messages, empty when fine."""
    text = cmds.getAttr(groom + ".lastError") or ""
    return [line for line in text.splitlines() if line]
