# SPDX-License-Identifier: GPL-3.0-or-later
"""The groom's way out of Blender and its other inputs, over the helper (docs/BLENDER.md, "Pipeline"):
what Maya's groomlab.game, exports, cards, sim, collision, extra_scalps and transfer modules do.

The helper writes every file but FBX: for Unreal and Unity cards, the scales' mesh and Unity's
feathers it answers the meshes (points, faces, per-corner UVs, normals and colors) and they are
written here with Blender's own FBX exporter. Everything crosses the pipe in the scalp object's
local space, as the groom does (groom.py); other objects (colliders, other scalps, a transfer's new
scalp) are taken into that space by their matrices, instance prototypes stay in their own."""

import json
import os

import bpy
import numpy as np

from . import groom, process, units
from .helper_client import HelperError

# export's formats the helper writes alone, and those whose meshes come back for an FBX.
STRAND_FORMATS = ("alembic", "usd", "scalesUsd", "vrscene", "unrealGroom", "unityStrands")
MESH_FORMATS = ("unrealCards", "unityCards", "scalesFbx", "unityFeathers")
LODS = ("hero", "mid", "low")            # cards' levels of detail (gltfCards' lods)
BARB_LODS = ("high", "mid", "low")       # lighter feather grooms (unrealGroom, unityStrands, unityFeathers)
TRANSFER_METHODS = ("closest", "bounds", "markersAlign", "markersWarp", "uv")


def _session(scalp, scene=None):
    """The groom's session with the scene's inputs sent (scalp, guides, time), not evaluated."""
    rt = groom.ensure(scalp, scene)
    groom.sync(scalp, scene=scene, evaluate_now=False)
    return rt


def call(scalp, cmd, **args):
    rt = _session(scalp)
    return process.call(cmd, session=rt.session, **args)


def _after_edit(scalp, r):
    """An edit of the groom (an undo step): the palette's undo label and the strands follow."""
    from . import tools
    tools.after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r


# ---- Meshes the helper answers, as Blender objects ---------------------------------------------------

def mesh_from_arrays(name, ref, client=None):
    """A Blender mesh of a mesh the helper answered (pipeline meshes: points, faceCounts, faceIndices,
    uvs, normals, uvSets, colors per face corner), in the space it was answered in."""
    client = client or process.client()
    points = client.array(ref["points"]).astype(np.float32)
    counts = client.array(ref["faceCounts"]).astype(np.int32)
    indices = client.array(ref["faceIndices"]).astype(np.int32)
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(len(points))
    mesh.vertices.foreach_set("co", points.ravel())
    mesh.loops.add(len(indices))
    mesh.loops.foreach_set("vertex_index", indices)
    mesh.polygons.add(len(counts))
    starts = np.zeros(len(counts), np.int32)
    if len(counts):
        starts[1:] = np.cumsum(counts)[:-1]
    mesh.polygons.foreach_set("loop_start", starts)
    mesh.update(calc_edges=True)
    if "uvs" in ref:
        layer = mesh.uv_layers.new(name="UVMap")
        layer.uv.foreach_set("vector", client.array(ref["uvs"]).astype(np.float32).ravel())
    for set_name, uv in (ref.get("uvSets") or {}).items():
        layer = mesh.uv_layers.new(name=set_name)
        layer.uv.foreach_set("vector", client.array(uv).astype(np.float32).ravel())
    if mesh.uv_layers:
        mesh.uv_layers.active_index = 0
    for color_name, colors in (ref.get("colors") or {}).items():
        attribute = mesh.color_attributes.new(name=color_name, type="FLOAT_COLOR", domain="CORNER")
        attribute.data.foreach_set("color", client.array(colors).astype(np.float32).ravel())
    if "normals" in ref:
        normals = client.array(ref["normals"]).astype(np.float32).reshape(-1, 3)
        mesh.normals_split_custom_set(normals.tolist())
    return mesh


def _material(name, base_color=None):
    """A material for an FBX (Unity's: its BaseColor map as color, its alpha as alpha)."""
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    if base_color and os.path.isfile(base_color):
        nodes = material.node_tree.nodes
        bsdf = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
        if bsdf is not None:
            texture = nodes.new("ShaderNodeTexImage")
            texture.image = bpy.data.images.load(base_color, check_existing=True)
            material.node_tree.links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
            material.node_tree.links.new(texture.outputs["Alpha"], bsdf.inputs["Alpha"])
    return material


def write_fbx(meshes, fbx_settings=None, scene=None, materials=None):
    """Each mesh answered (`meshes`: {key: ref with "fbx"}) written to its "fbx" file with Blender's
    FBX exporter, as a temporary object removed after. `fbx_settings` {"upAxis": "Y", "units": "m"}
    (Unity's: meters, Y up) or the scene's; `materials` {key: (name, base color map)}. Returns the
    files written."""
    scene = scene or bpy.context.scene
    fbx_settings = fbx_settings or {}
    materials = materials or {}
    written = []
    view_layer = bpy.context.view_layer
    selected = [o for o in view_layer.objects if o is not None and o.select_get()]
    active = view_layer.objects.active
    made = []
    try:
        for key, ref in meshes.items():
            path = ref.get("fbx")
            if not path:
                continue
            mesh = mesh_from_arrays(key, ref)
            obj = bpy.data.objects.new(os.path.splitext(os.path.basename(path))[0], mesh)
            scene.collection.objects.link(obj)
            made.append(obj)
            if key in materials or ref.get("material"):
                name, base = materials.get(key, (ref.get("material"), None))
                mesh.materials.append(_material(name, base))
            for o in view_layer.objects:
                if o is not None:
                    o.select_set(False)
            obj.select_set(True)
            view_layer.objects.active = obj
            directory = os.path.dirname(os.path.abspath(path))
            if not os.path.isdir(directory):
                os.makedirs(directory)
            options = dict(filepath=path, use_selection=True, object_types={"MESH"}, mesh_smooth_type="OFF",
                           use_mesh_modifiers=False, path_mode="RELATIVE", axis_forward="-Z", axis_up="Y")
            if fbx_settings.get("units") == "m":
                # Meters in the file (Unity's convention, Maya's FBXExportConvertUnitString m).
                options.update(apply_unit_scale=True, apply_scale_options="FBX_SCALE_UNITS",
                               global_scale=1.0)
            bpy.ops.export_scene.fbx(**options)
            written.append(path)
    finally:
        for obj in made:
            mesh = obj.data
            bpy.data.objects.remove(obj)
            for material in list(mesh.materials):
                if material is not None and material.users <= 1:
                    bpy.data.materials.remove(material)
            bpy.data.meshes.remove(mesh)
        for o in view_layer.objects:
            if o is not None:
                o.select_set(o in selected)
        if active is None or active.name in view_layer.objects:
            view_layer.objects.active = active
    return written


# ---- Exports ------------------------------------------------------------------------------------------

def export(scalp, fmt, path=None, scene=None, first=None, last=None, step=1.0, density=1.0, write_meshes=True, **options):
    """Export the groom (export's formats, docs/BLENDER.md). Strand formats ("alembic", "usd",
    "scalesUsd", "unrealGroom", "unityStrands") over frames `first`..`last` (the current one by
    default; a deforming scalp is played frame by frame); "gltfCards" a .glb or .gltf at `path`;
    "unrealCards", "unityCards", "unityFeathers" into `directory` (options), "scalesFbx" at `path`:
    the maps, manifests and .abc the helper writes, the FBX meshes written here unless
    `write_meshes` is off. Options as the helper takes them (name, groups, lod, resolution...).
    Returns the helper's answer (with "fbxWritten" for the FBX files)."""
    scene = scene or groom.scene_of(scalp)
    if fmt in STRAND_FORMATS:
        frame = float(scene.frame_current)
        first = frame if first is None else float(first)
        last = first if last is None else float(last)
        return groom.export(scalp, scene, fmt, path, first, last, step=step, density=density, **options)
    rt = _session(scalp, scene)
    args = dict(options)
    if path is not None:
        args["path"] = path
    r = process.call("export", session=rt.session, format=fmt, **args)
    if fmt in MESH_FORMATS and write_meshes and r.get("meshes"):
        materials = {}
        if fmt == "unityCards":
            base = {os.path.basename(p): p for p in r.get("maps") or []}
            name = r.get("name", "groom")
            for key, map_name in (("cards", "BaseColor"), ("scalpCap", "ScalpBaseColor")):
                ref = r["meshes"].get(key)
                if ref and ref.get("material"):
                    materials[key] = (ref["material"], base.get("%s_%s.png" % (name, map_name)))
        r["fbxWritten"] = write_fbx(r["meshes"], r.get("fbxSettings"), scene, materials)
    return r


def bake_scale_maps(scalp, directory, name="groom", resolution=2048, max_height=0.0, padding=8):
    """The groom's scales baked onto its scalp's UVs (Maya's bake_scale_maps): <name>_ScaleNormal,
    _ScaleHeight (16-bit) and _ScaleColor PNGs, a UDIM tile set when the UVs leave 0-1. Returns
    {files, maps, pattern, maxHeight, udim, tiles, covered}."""
    return call(scalp, "bake_scale_maps", directory=directory, name=name, resolution=int(resolution),
                maxHeight=float(max_height), padding=int(padding))


def scale_mesh(scalp, name=None):
    """The groom's scales at the current frame as a mesh object parented to the scalp (Maya's
    scale_mesh): UV maps UVMap, scaleUV and scaleData, color attributes scaleColor and
    scaleRootColor, the scales' own normals. Returns the object."""
    r = call(scalp, "scale_mesh")
    return _mesh_object(scalp, name or scalp.name + "_scales", r["meshes"]["scales"])


def strands_to_mesh(scalp, style="tube", sides=4, width_scale=1.0, max_strands=100000, density=1.0, strand_uvs=False,
                    name=None):
    """The groom's strands as one mesh object (Maya's strands_to_mesh): tubes of `sides` sides or
    ribbons along the scalp, UVs root to tip, at most `max_strands` (thinned by strand ID).
    Returns (object, the helper's answer)."""
    r = call(scalp, "strand_mesh", style=style, sides=int(sides), widthScale=float(width_scale),
             maxStrands=int(max_strands), density=float(density), strandUVs=bool(strand_uvs))
    return _mesh_object(scalp, name or scalp.name + "_strandMesh", r["meshes"]["strands"]), r


def _mesh_object(scalp, name, ref):
    mesh = mesh_from_arrays(name, ref)
    obj = bpy.data.objects.new(name, mesh)
    scene = groom.scene_of(scalp) or bpy.context.scene
    for collection in scalp.users_collection or [scene.collection]:
        collection.objects.link(obj)
        break
    obj.parent = scalp
    obj.matrix_parent_inverse.identity()
    return obj


# ---- Simulation -----------------------------------------------------------------------------------------

def send_scalp_frames(scalp, first, last, scene=None):
    """The scalp's pose at every frame first..last (its modifiers, shape keys, armature) handed to the
    helper, which feeds the Simulate nodes the frames they step through. Nothing for a scalp that
    does not deform (the helper's frames dropped). Returns the frames sent."""
    scene = scene or groom.scene_of(scalp)
    rt = groom.ensure(scalp, scene)
    if not groom.deforms(scalp):
        process.call("set_scalp_frames", session=rt.session)
        return 0
    rest = groom.rest_points(scalp)
    current = scene.frame_current
    frames = []
    try:
        for f in range(int(first), int(last) + 1):
            scene.frame_set(f)
            posed = groom.deformed_points(scalp, bpy.context.evaluated_depsgraph_get(), len(rest))
            frames.append(rest if posed is None else posed)
    finally:
        scene.frame_set(current)
    if not frames:
        return 0
    points = np.concatenate(frames)
    process.call("set_scalp_frames", session=rt.session, first=int(first),
                 **process.client().put(points=points, restPoints=np.tile(rest, (len(frames), 1))))
    return len(frames)


def simulate_range(scalp, last=None, budget_ms=100.0, feed=True, scene=None):
    """Simulate Range: every Simulate node stepped on to frame `last` (the scene's end frame), in
    steps of at most `budget_ms` per request so Blender stays responsive; poll with simulate_poll
    until "running" is false. With `feed` the scalp's frames from the scene's start go first."""
    scene = scene or groom.scene_of(scalp)
    last = scene.frame_end if last is None else int(last)
    if feed:
        send_scalp_frames(scalp, scene.frame_start, last, scene)
    r = call(scalp, "simulate_range", last=int(last), budgetMs=float(budget_ms))
    if not r.get("running"):
        groom.mark(scalp, "eval")
    return r


def simulate_poll(scalp, budget_ms=None):
    """The next step of a Simulate Range: {running, done, frame, target, progress, frames, errors, status}."""
    args = {} if budget_ms is None else {"budgetMs": float(budget_ms)}
    r = call(scalp, "simulate_poll", **args)
    if not r.get("running"):
        groom.mark(scalp, "eval")
    return r


def simulate_cancel(scalp):
    """Stop a Simulate Range; the frames stepped stay."""
    return call(scalp, "simulate_cancel")


def simulation_status(scalp):
    """{nodes, status [{node, startFrame, ranges, frames, live, baked, bakeStale...}], bakes, scalpFrames, running}."""
    return call(scalp, "simulate_status")


def simulate_bake(scalp, last=None, directory=None, name=None, node=None, path=None, first=None):
    """Bake Simulation (Maya's bake_simulation): the Simulate nodes (or `node`) simulated to `last` and
    their frames written as Alembic guide caches (into `directory`, default beside the groom's file,
    else the temp folder), which renders, exports and the groom file then read. One undo step (the
    files stay). Returns {written: {node: path}, frames, status, undo}."""
    scene = groom.scene_of(scalp)
    args = {"last": int(scene.frame_end if last is None else last), "name": name or scalp.name}
    for key, value in (("directory", directory), ("node", node), ("path", path), ("first", first)):
        if value is not None:
            args[key] = value
    if directory is None and bpy.data.filepath:
        args["directory"] = os.path.join(os.path.dirname(bpy.data.filepath), "groomlab", "caches")
    return _after_edit(scalp, call(scalp, "simulate_bake", **args))


def simulate_clear(scalp, node=None):
    """Clear Simulation: the Simulate nodes' frames dropped, and their bakes (one undo step)."""
    return _after_edit(scalp, call(scalp, "simulate_clear", **({"node": node} if node else {})))


# ---- Colliders, instance prototypes, other scalps -----------------------------------------------------

def _corner_uvs(mesh):
    layer = mesh.uv_layers.active
    if layer is None or not len(mesh.loops):
        return None
    uvs = np.empty(len(mesh.loops) * 2, np.float32)
    layer.uv.foreach_get("vector", uvs)
    return uvs.reshape(-1, 2)


def mesh_arrays(obj, scalp=None, depsgraph=None, rest=True, uvs=True):
    """An object's mesh for the helper: its evaluated points (the frame's pose) in the scalp object's
    local space (`scalp` None: the object's own space), its faces, corner UVs and, with `rest`, the
    points before modifiers as the rest pose."""
    depsgraph = depsgraph or bpy.context.evaluated_depsgraph_get()
    if scalp is not None:
        matrix = np.array(scalp.matrix_world.inverted() @ obj.matrix_world, np.float64)
    else:
        matrix = np.identity(4)

    def place(p):
        return (p @ matrix[:3, :3].T + matrix[:3, 3]).astype(np.float32)

    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        n = len(mesh.vertices)
        points = np.empty(n * 3, np.float32)
        mesh.vertices.foreach_get("co", points)
        counts = np.empty(len(mesh.polygons), np.int32)
        mesh.polygons.foreach_get("loop_total", counts)
        indices = np.empty(len(mesh.loops), np.int32)
        mesh.loops.foreach_get("vertex_index", indices)
        arrays = dict(points=place(points.reshape(-1, 3)), faceCounts=counts, faceIndices=indices)
        corner = _corner_uvs(mesh) if uvs else None
        if corner is not None:
            arrays["uvs"] = corner
    finally:
        evaluated.to_mesh_clear()
    if rest and len(obj.data.vertices) == n:
        base = np.empty(n * 3, np.float32)
        obj.data.vertices.foreach_get("co", base)
        arrays["restPoints"] = place(base.reshape(-1, 3))
    return arrays


def _named(objects):
    if isinstance(objects, dict):
        return list(objects.items())
    return [(o.name.replace(".", "_").replace(":", "_"), o) for o in objects]


def set_colliders(scalp, objects, replace=True):
    """Meshes the groom's ColliderMesh nodes read by name ({name: object} or objects, named after
    them), in the scalp's space as they are posed now (Maya's add_collider without the graph wiring:
    add_collider wires them, and keeps them posed frame by frame). Returns meshes_info."""
    rt = _session(scalp)
    client = process.client()
    meshes = {name: client.put(**mesh_arrays(obj, scalp)) for name, obj in _named(objects)}
    return process.call("set_meshes", session=rt.session, kind="collider", replace=bool(replace), meshes=meshes)


def collider_name(obj):
    """What Add > Collider calls an object's mesh in the graph (letters, digits and _)."""
    name = "".join(c if c.isalnum() or c == "_" else "_" for c in obj.name) or "collider"
    return "collider_" + name if name == "scalp" or name[0].isdigit() else name


def kept_colliders(scalp):
    """{name: object} of the groom's colliders still in the file (Add > Collider's)."""
    return {c.name: c.object for c in scalp.groomlab.colliders if c.object is not None and c.object.type == "MESH"}


def sync_colliders(scalp, rt, depsgraph=None, force=False, read_pose=True):
    """groom.sync's input hook: the groom's colliders sent when they moved or deformed (a frame, an
    edit), each in the scalp's space as it is posed now, as Maya's colliderMeshes follow their meshes.
    True when sent."""
    kept = kept_colliders(scalp)
    if not kept and "colliders" not in rt.keys:
        return False
    if not read_pose and "colliders" in rt.keys:
        return False  # a render on another thread: the colliders as they were sent last
    arrays = {name: mesh_arrays(obj, scalp, depsgraph) for name, obj in sorted(kept.items())}
    key = groom._key(*[x for name in sorted(arrays) for x in (name, *[arrays[name][k] for k in sorted(arrays[name])])])
    if not force and rt.keys.get("colliders") == key:
        return False
    client = process.client()
    process.call("set_meshes", session=rt.session, kind="collider", replace=True,
                 meshes={name: client.put(**a) for name, a in arrays.items()})
    rt.keys["colliders"] = key
    return True


groom.input_hooks.append(sync_colliders)


# ---- Graph inserts: Maya's add_simulate, add_collider, create_cards, add_feathers, add_braid ------------

LAYOUT_DX, LAYOUT_DY = 240, 140  # the core's column and row (graph_edit.cpp), as Maya's GraphModel


class GraphEdits(object):
    """Edits of a groom's graph as Maya's GraphModel makes them, through the helper's edit_graph (each
    node's defaults fitted to the scalp): the graph read again after each, kept here. finish() hands
    the result to the panels, the Graph Editor and the groom (one Blender undo step: the graph kept
    on the scalp object, graph_tree.after_undo)."""

    def __init__(self, scalp):
        from . import graph as graph_mod
        self.scalp = scalp
        self.rt = graph_mod._ensure(scalp)
        self.reload()

    def reload(self):
        self.graph = process.call("get_graph", session=self.rt.session)["graph"]
        self.graph.setdefault("nodes", [])
        self.graph.setdefault("connections", [])

    def edit(self, **args):
        r = process.call("edit_graph", session=self.rt.session, **args)
        self.reload()
        return r

    @property
    def output(self):
        return self.graph.get("output") or "output"

    def names(self):
        return [n["name"] for n in self.graph["nodes"]]

    def of_type(self, kind):
        return [n["name"] for n in self.graph["nodes"] if n.get("type") == kind]

    def node(self, name):
        return next((n for n in self.graph["nodes"] if n["name"] == name), None)

    def node_type(self, name):
        n = self.node(name)
        return n.get("type") if n else None

    def param(self, name, param, default=None):
        n = self.node(name) or {}
        return (n.get("params") or {}).get(param, default)

    def set_param(self, name, param, value):
        process.call("set_param", session=self.rt.session, node=name, param=param, value=value)
        self.reload()

    def unique_name(self, base):
        """The core's rule (GraphEdit::uniqueName): `base` with its first letter lowered, else base1, base2..."""
        base = base[:1].lower() + base[1:]
        taken = set(self.names())
        if base not in taken:
            return base
        i = 1
        while "%s%d" % (base, i) in taken:
            i += 1
        return "%s%d" % (base, i)

    def source_of(self, node, input_name):
        c = next((c for c in self.graph["connections"] if c["to"] == node and c["input"] == input_name), None)
        return c["from"] if c else None

    def downstream_of(self, node):
        return [(c["to"], c["input"]) for c in self.graph["connections"] if c["from"] == node]

    def position(self, name):
        ui = (self.node(name) or {}).get("ui") or {}
        return float(ui.get("x", 0.0)), float(ui.get("y", 0.0))

    def add(self, kind, base=None, params=None, at=None):
        args = {"op": "add_node", "type": kind, "name": self.unique_name(base or kind), "params": params or {}}
        if at is not None:
            args["x"], args["y"] = float(at[0]), float(at[1])
        return self.edit(**args)["name"]

    def connect(self, src, dst, input_name, output=None):
        args = {"op": "connect", "from": src, "to": dst, "input": input_name}
        if output:
            args["output"] = output
        return self.edit(**args)

    def set_positions(self, positions):
        if positions:
            self.edit(op="set_positions", positions={k: [float(v[0]), float(v[1])] for k, v in positions.items()})

    def insert_between(self, src, dst, node, node_input="strands"):
        """src -> node -> dst in place of every src -> dst link."""
        links = [c for c in self.graph["connections"] if c["from"] == src and c["to"] == dst]
        if not links:
            raise HelperError("graph", "%s is not connected to %s" % (src, dst))
        self.connect(src, node, node_input, output=links[0].get("output"))
        for c in links:
            self.connect(node, dst, c["input"])

    def insert_before(self, anchor, node, input_name="strands", node_input="strands"):
        upstream = self.source_of(anchor, input_name)
        if upstream is None:
            raise HelperError("graph", "%s has no %s input to insert before" % (anchor, input_name))
        self.insert_between(upstream, anchor, node, node_input)
        x, y = self.position(anchor)
        self.set_positions({node: (x, y + LAYOUT_DY)})

    def insert_after(self, anchor, node, node_input="strands"):
        downstream = self.downstream_of(anchor)
        if downstream:
            self.insert_between(anchor, downstream[0][0], node, node_input)
        else:
            self.connect(anchor, node, node_input)
        x, y = self.position(anchor)
        self.set_positions({node: (x + LAYOUT_DX / 2, y + LAYOUT_DY)})

    def finish(self):
        from . import graph as graph_mod
        graph_mod._changed(self.scalp, self.rt)
        return self.graph


def add_simulate(scalp, params=None):
    """Add Simulate Node (Maya's add_simulate): a Simulate node between the guides and every
    Interpolate they feed, so the strands follow the simulated guides; bypass or delete it and the
    groom is as it was. Returns the node's name."""
    g = GraphEdits(scalp)

    def interpolates(src):
        return [(dst, i) for dst, i in g.downstream_of(src) if g.node_type(dst) == "Interpolate"]
    guides = [n for n in g.of_type("Guides") if interpolates(n)]
    if not guides:
        raise HelperError("state", "the groom's graph has no Guides node feeding an Interpolate")
    source = guides[0]
    consumers = interpolates(source)
    x, y = g.position(source)
    node = g.add("Simulate", params=params or {}, at=(x + LAYOUT_DX / 2, y + LAYOUT_DY))
    for dst, input_name in consumers:
        g.connect(node, dst, input_name)
    g.connect(source, node, "guides")
    g.finish()
    return node


def add_collider(scalp, obj, name=None, wire=True):
    """Add > Collider (Maya's add_collider): `obj`'s mesh kept off by the strands. It is sent as the
    graph's mesh `name` (the object's name by default) and kept posed frame by frame; a ColliderMesh
    node reads it and, with `wire`, a Collide node goes just before the output (off the scalp too).
    An existing collider of that name takes the new object, the graph left alone. Returns the
    ColliderMesh node's name."""
    if obj is None or obj.type != "MESH":
        raise HelperError("bad_request", "a collider is a mesh object")
    if obj == scalp:
        raise HelperError("bad_request", "the groom's own scalp is not a collider: pick another mesh")
    name = name or collider_name(obj)
    kept = scalp.groomlab.colliders
    entry = kept.get(name)
    existing = entry is not None
    if entry is None:
        entry = kept.add()
        entry.name = name
    entry.object = obj
    rt = _session(scalp)
    sync_colliders(scalp, rt, force=True)
    g = GraphEdits(scalp)
    node = next((n for n in g.of_type("ColliderMesh") if g.param(n, "name") == name), None)
    if existing and node is not None:
        groom.mark(scalp, "eval")
        return node
    if node is None:
        node = g.add("ColliderMesh", base=name, params={"name": name})
    if wire:
        collide = g.add("Collide", base="collide_" + name)
        g.insert_before(g.output, collide)
        g.connect(node, collide, "collider")
        if g.source_of(collide, "scalp") is None:
            scalp_node = next((n for n in g.of_type("Scalp") if g.param(n, "input", "scalp") == "scalp"), None)
            if scalp_node:
                g.connect(scalp_node, collide, "scalp")  # off the scalp too, as Collide is meant
        x, y = g.position(collide)
        g.set_positions({node: (x - 220.0, y - 60.0)})
    g.finish()
    return node


def remove_collider(scalp, name):
    """A collider's mesh dropped (its nodes stay: delete them in the Graph Editor)."""
    kept = scalp.groomlab.colliders
    i = kept.find(name)
    if i >= 0:
        kept.remove(i)
    sync_colliders(scalp, _session(scalp), force=True)


def add_cards(scalp, lod="mid"):
    """Add > Cards > Hero / Mid / Low (Maya's create_cards with lod): a Cards node before the output
    when the graph has none, then every Cards node set for that level of detail, each by what it cards
    (hair, feathers or both: the core's card presets). Returns the first Cards node."""
    g = GraphEdits(scalp)
    nodes = g.of_type("Cards")
    if not nodes:
        node = g.add("Cards", base="cards")
        g.insert_before(g.output, node)
        nodes = [node]
    process.call("cards_preset", session=g.rt.session, lod=lod)
    g.reload()
    g.finish()
    return nodes[0]


FEATHER_DESIGNS = ("contour", "down", "flight", "afterfeather", "barred")


def feather_designs():
    """The feather designs there are: the presets named feathers_<design>."""
    from . import graph as graph_mod
    return [p[0][len("feathers_"):] for p in graph_mod.presets() if p[0].startswith("feathers_")]


def bake_to_guides(scalp, node="", group="baked", density=1.0):
    """Bake Node to Guides (Maya's bake.bake_guides): node `node`'s strands or guides ("" the groom's
    output), as they are now at `density`, become primary guides of root group `group` (made when
    new, added to otherwise) the Groom Tools brush, one GroomLab undo step. Returns the helper's
    answer (node, group, guides, ids)."""
    from . import tools
    if not tools.is_hierarchy(scalp):
        raise HelperError("state", "this groom's guides are Blender curves: Bake Node to Guides needs a groom made "
                                   "with GroomLab's tools")
    r = tools.call(scalp, "bake_to_guides", node=node or "", group=group or "baked", density=float(density))
    tools.after_edit(scalp, groom.scene_of(scalp), r.get("undo"), refresh=True)
    return r


def bakeable_nodes(scalp):
    """The graph's nodes giving strands or guides, but the output node (Bake Node to Guides' choices)."""
    from . import graph as graph_mod
    rt = groom.ensure(scalp)
    g = process.call("get_graph", session=rt.session)["graph"]
    specs = graph_mod.operator_specs()
    out = []
    for n in g.get("nodes") or []:
        spec = specs.get(n.get("type")) or {}
        ports = spec.get("outputs") or [spec.get("output")]
        if n["name"] != g.get("output") and any(p and p.get("type") in ("Strands", "Guides") for p in ports):
            out.append(n["name"])
    return out


def import_alembic_curves(scalp, path, object_name="", samples=24):
    """An Alembic file's curves as the groom's guides (Maya's import_guides_from_alembic): the graph's
    guides node becomes an ImportCurves node reading `path`, rooted on the scalp. The curves are read
    as GroomLab's and Maya's Alembic files have them (Y up, centimeters). Returns the helper's answer
    (curves)."""
    from . import graph as graph_mod, graph_tree
    rt = graph_mod._ensure(scalp)
    r = process.call("import_alembic_curves", session=rt.session, path=path, object=object_name or "",
                     samples=int(samples))
    graph_mod._changed(scalp, rt)
    graph_tree.graph_changed(scalp)
    return r


def _feather_design(rt, design):
    """(the design's Compound node, its shafts' Scatter params) of preset feathers_<design>, fitted
    to the scalp (Maya's _feather_design)."""
    graph = process.call("preset_graph", session=rt.session, name="feathers_" + design)["graph"]
    node = next((n for n in graph.get("nodes", []) if n.get("type") == "Compound"), None)
    if node is None:
        raise HelperError("state", "preset feathers_%s has no feather design (a Compound node)" % design)
    shafts = next((n for n in graph["nodes"] if n.get("type") == "Scatter"), None)
    params = (shafts or {}).get("params", {})
    return node, {k: params[k] for k in ("density", "length") if k in params}


def _propagates_in(params):
    """The params of the Propagate nodes inside a Compound's params (and its compounds'), to edit in place."""
    for node in ((params.get("definition") or {}).get("graph") or {}).get("nodes", []):
        if node.get("type") == "Propagate":
            yield node.setdefault("params", {})
        elif node.get("type") == "Compound":
            for inner in _propagates_in(node.setdefault("params", {})):
                yield inner


def _feather_groups(g):
    """{groupName: groupId} of the feather designs already in the graph (their Propagates')."""
    groups = {}
    for n in g.graph["nodes"]:
        found = [n.get("params") or {}] if n.get("type") == "Propagate" else \
            list(_propagates_in(n.get("params") or {})) if n.get("type") == "Compound" else []
        for params in found:
            gid = int(params.get("groupId", 0) or 0)
            if gid > 0:
                groups.setdefault(params.get("groupName") or "group%d" % gid, gid)
    return groups


def feather_guide_groups(scalp):
    """The guide groups whose guides grow one feather each (Maya's feather_guide_groups): the groups
    of the Guides nodes feeding a feather design (a Propagate, or a Compound with one), in the
    graph's order; a "feathers" group the graph does not read yet too."""
    from . import tools
    rt = groom.ensure(scalp)
    graph = process.call("get_graph", session=rt.session)["graph"]
    nodes = graph.get("nodes") or []
    designs = {n["name"] for n in nodes if n.get("type") == "Propagate" or
               (n.get("type") == "Compound" and any(True for _ in _propagates_in(n.get("params") or {})))}
    feeds = {}
    for c in graph.get("connections") or []:
        feeds.setdefault(c["from"], []).append(c["to"])
    found = []
    for n in nodes:
        groups = str((n.get("params") or {}).get("group") or "").replace(",", " ").split() \
            if n.get("type") == "Guides" else []
        if not groups:
            continue
        seen, todo = set(), [n["name"]]
        while todo:
            name = todo.pop()
            if name not in seen:
                seen.add(name)
                todo += feeds.get(name, [])
        if seen & designs:
            found += [g for g in groups if g not in found]
    if "feathers" not in found and tools.is_hierarchy(scalp) and \
            "feathers" in [g.get("name") for g in (tools.call(scalp, "guides_info").get("groups") or [])]:
        found.append("feathers")
    return found


def exclude_from_hair_guides(g, group):
    """Braid paths and feather shafts are not hair: Guides nodes reading every guide leave `group` out."""
    for n in g.of_type("Guides"):
        if not g.param(n, "group"):
            excluded = str(g.param(n, "exclude", "") or "").replace(",", " ").split()
            if group not in excluded:
                g.set_param(n, "exclude", " ".join(excluded + [group]))


def _ensure_group(scalp, group):
    from . import tools
    names = [x.get("name") for x in (tools.call(scalp, "guides_info").get("groups") or [])]
    if group not in names:
        tools.call(scalp, "guides_edit", op="add_group", name=group)


def curve_points(obj, scalp):
    """A curve object's curves as point lists in the scalp object's space: hair Curves (each curve)
    or a Curve (each spline's points; a Bezier spline's control points), first point the root."""
    m = np.array(scalp.matrix_world.inverted() @ obj.matrix_world, np.float64)
    out = []
    if obj.type == "CURVES":
        c = obj.data
        if not len(c.points):
            return out
        pos = np.empty(len(c.points) * 3, np.float32)
        c.attributes["position"].data.foreach_get("vector", pos)
        offsets = np.empty(len(c.curves) + 1, np.int32)
        c.curve_offset_data.foreach_get("value", offsets)
        pos = pos.reshape(-1, 3)
        out = [pos[offsets[i]:offsets[i + 1]] for i in range(len(c.curves))]
    elif obj.type == "CURVE":
        for spline in obj.data.splines:
            if spline.type == "BEZIER":
                out.append(np.array([tuple(p.co) for p in spline.bezier_points], np.float32))
            else:
                out.append(np.array([tuple(p.co)[:3] for p in spline.points], np.float32))
    return [(p @ m[:3, :3].T + m[:3, 3]).astype(np.float32) for p in out if len(p) >= 2]


def guides_from_curves(scalp, objects, group):
    """Curve objects (drawn from the scalp: each curve's first point its root) as new guides of guide
    group `group`, made when missing (Maya's guides_from_curves). Returns the guides added."""
    from . import tools
    curves = [p for obj in objects for p in curve_points(obj, scalp)]
    if not curves:
        return 0
    if not tools.is_hierarchy(scalp):
        raise HelperError("state", "this groom's guides are Blender curves: sculpt them, or make a groom with "
                                   "GroomLab's tools for guide groups")
    rt = _session(scalp)
    client = process.client()
    points = np.concatenate(curves).astype(np.float32)
    counts = np.array([len(p) for p in curves], np.int32)
    r = process.call("set_guides", session=rt.session, target="hierarchy", keepMissing=True, group=group,
                     **client.put(points=points, counts=counts, ids=np.zeros(len(curves), np.uint64)))
    tools.keep_state(scalp)
    return len(r.get("added") or [])


def add_feathers(scalp, design="contour", source="scatter", weights=None, curves=None, group="feathers",
                 keep_hair=True, density=None):
    """Add > Feathers (Maya's add_feathers): barbs on shafts, a design from the preset feathers_<design>
    (or several sharing the shafts through a Split, `weights` their shares). `source` "scatter": a new
    Scatter shaped by the guides; "guides": the guide group `group` (curve objects `curves` join it
    first), each guide a hand-placed feather. Merged with the hair before the output (`keep_hair`).
    Returns {"shafts", "split", "designs", "merge"}."""
    designs = [design] if isinstance(design, str) else list(design)
    if not designs or len(designs) > 4:
        raise HelperError("bad_request", "one to four feather designs (a Split has four outputs)")
    weights = [1.0] * len(designs) if not weights else [float(w) for w in weights]
    if len(weights) != len(designs):
        raise HelperError("bad_request", "one weight per design")
    rt = _session(scalp)
    parts = [_feather_design(rt, d) for d in designs]
    if source == "guides":
        _ensure_group(scalp, group)
        if curves:
            guides_from_curves(scalp, curves, group)
    g = GraphEdits(scalp)
    out = g.output
    made = []
    if source == "scatter":
        scatter_params = dict(parts[0][1])
        if density is not None:
            scatter_params["density"] = float(density)
        scalp_node = next(iter(g.of_type("Scalp")), None)
        if scalp_node is None:
            scalp_node = g.add("Scalp", base="scalp")
            made.append(scalp_node)
        guides = next((n for n in g.of_type("Guides") if not g.param(n, "group")), None)
        if guides is None:
            guides = g.add("Guides", base="guides")
            made.append(guides)
        scatter = g.add("Scatter", base="featherShafts", params=scatter_params)
        shapes = g.add("Interpolate", base="featherShapes")
        g.connect(scalp_node, scatter, "scalp")
        g.connect(scatter, shapes, "strands")
        g.connect(guides, shapes, "guides")
        g.connect(scalp_node, shapes, "scalp")
        shafts = shapes
        made += [scatter, shapes]
    elif source == "guides":
        exclude_from_hair_guides(g, group)
        shafts = next((n for n in g.of_type("Guides") if g.param(n, "group") == group), None)
        if shafts is None:
            shafts = g.add("Guides", base="featherGuides", params={"group": group})
            made.append(shafts)
    else:
        raise HelperError("bad_request", "source is scatter or guides, not %r" % source)
    feeds = [(shafts, None)]
    split = None
    if len(designs) > 1:
        params = {"weight" + letter: 0.0 for letter in "ABCD"}
        params.update({"weight" + "ABCD"[i]: w for i, w in enumerate(weights)})
        split = g.add("Split", base="featherSplit", params=params)
        g.connect(shafts, split, "strands")
        feeds = [(split, "abcd"[i]) for i in range(len(designs))]
        made.append(split)
    names = []
    groups = _feather_groups(g)
    for (src, output), (node, _), design_name in zip(feeds, parts, designs):
        # Each design a groom group of its own (groupId, groupName on its Propagate): Unreal gives it
        # its own material, LODs and physics. The same design again joins its group.
        params = json.loads(json.dumps(node.get("params", {})))
        if design_name not in groups:
            groups[design_name] = max(list(groups.values()) + [0]) + 1
        for propagate in _propagates_in(params):
            propagate["groupId"] = groups[design_name]
            propagate["groupName"] = design_name
        name = g.add("Compound", base=node["name"], params=params)
        g.connect(src, name, "shafts", output=output)
        names.append(name)
    feathers = names[0]
    merges = []
    for other in names[1:]:
        merge = g.add("Merge", base="featherMerge")
        g.connect(feathers, merge, "a")
        g.connect(other, merge, "b")
        feathers = merge
        merges.append(merge)
    # A row under the output: shafts, split, designs, then their merges.
    x, y = g.position(out)
    y += 2 * LAYOUT_DY
    x -= (len(made) + 2) * LAYOUT_DX
    positions = {n: (x + i * LAYOUT_DX, y) for i, n in enumerate(made)}
    positions.update({n: (x + len(made) * LAYOUT_DX, y + i * LAYOUT_DY) for i, n in enumerate(names)})
    positions.update({n: (x + (len(made) + 1) * LAYOUT_DX, y + i * LAYOUT_DY) for i, n in enumerate(merges)})
    g.set_positions(positions)
    final = None
    if keep_hair and g.source_of(out, "strands"):
        final = g.add("Merge", base="feathersMerge")
        g.insert_before(out, final, "strands", "a")
        g.connect(feathers, final, "b")
    else:
        g.connect(feathers, out, "strands")
    g.finish()
    return {"shafts": shafts, "split": split, "designs": names, "merge": final}


BRAID_STYLES = ("three", "fishtail", "rope", "cornrow")


def add_braid(scalp, curves=None, style="three", group="braids", width=0.12, crossings=2.5, region=0.25,
              dutch=False, feed=None):
    """Add > Braid on Curves (Maya's add_braid): curve objects `curves` (drawn from the scalp along the
    braids) become guide group `group`, else its guides are the paths (draw them with Draw); a Braid
    node after the Interpolate braids the strands along them (cornrows fed along the scalp). Returns
    the Braid node's name."""
    if style not in BRAID_STYLES:
        raise HelperError("bad_request", "style is one of %s" % ", ".join(BRAID_STYLES))
    if feed is None:
        feed = style == "cornrow"
    _ensure_group(scalp, group)
    if curves:
        guides_from_curves(scalp, curves, group)
    g = GraphEdits(scalp)
    exclude_from_hair_guides(g, group)
    chain_interpolates = [n for n in g.of_type("Interpolate") if g.downstream_of(n)]
    after = "interpolate" if g.node_type("interpolate") == "Interpolate" else next(iter(chain_interpolates), None)
    if after is None:
        raise HelperError("state", "the groom's graph has no Interpolate node to braid after")
    paths = next((n for n in g.of_type("Guides") if g.param(n, "group") == group), None)
    if paths is None:
        paths = g.add("Guides", base="braidPaths", params={"group": group})
    params = {"style": style, "width": float(width), "crossings": float(crossings), "region": float(region),
              "dutch": bool(dutch), "feed": bool(feed)}
    braid = g.add("Braid", base="braid", params=params)
    g.insert_after(after, braid)
    g.connect(paths, braid, "braids")
    x, y = g.position(after)
    g.set_positions({paths: (x, y + 2 * LAYOUT_DY)})
    g.finish()
    return braid


def set_prototypes(scalp, objects, replace=True):
    """Meshes the groom's Instance nodes (scales' plates of your own) name, in their own object space
    (their up is the prototype's +Y, its long axis), with per-corner normals and UVs."""
    rt = _session(scalp)
    client = process.client()
    meshes = {}
    for name, obj in _named(objects):
        mesh = obj.data
        n = len(mesh.vertices)
        points = np.empty(n * 3, np.float32)
        mesh.vertices.foreach_get("co", points)
        counts = np.empty(len(mesh.polygons), np.int32)
        mesh.polygons.foreach_get("loop_total", counts)
        indices = np.empty(len(mesh.loops), np.int32)
        mesh.loops.foreach_get("vertex_index", indices)
        arrays = dict(points=points.reshape(-1, 3), faceCounts=counts, faceIndices=indices)
        normals = np.empty(len(mesh.loops) * 3, np.float32)
        mesh.corner_normals.foreach_get("vector", normals)
        arrays["normals"] = normals.reshape(-1, 3)
        corner = _corner_uvs(mesh)
        if corner is not None:
            arrays["uvs"] = corner
        meshes[name] = client.put(**arrays)
    return process.call("set_meshes", session=rt.session, kind="prototype", replace=bool(replace), meshes=meshes)


def set_other_scalps(scalp, objects):
    """The groom's other scalps in the order they join ({name: object} or objects, named after them;
    letters, digits and _): hair grows on them too where a Scalp node names them (Maya's add_scalp
    without the graph wiring). Returns meshes_info."""
    rt = _session(scalp)
    client = process.client()
    scalps = []
    for name, obj in _named(objects):
        entry = dict(client.put(**mesh_arrays(obj, scalp)))
        entry["name"] = name
        scalps.append(entry)
    return process.call("set_scalps", session=rt.session, scalps=scalps)


def meshes_info(scalp):
    return call(scalp, "meshes_info")


# ---- Transfer Groom to Scalp ------------------------------------------------------------------------

def transfer(scalp, target, method=None, fit=False, maps=True, markers=None, keep_lengths=False, fit_size=False,
             own=False):
    """Move the groom onto `target`'s mesh (Maya's transfer_groom): guides, parts and flow re-rooted
    by `method` (TRANSFER_METHODS; "bounds" when only `fit`), painted maps projected, the scale kept
    (or fitted to the new scalp's size with `fit_size`). `markers`: pairs [(old point, new point)] in
    the scalp object's space (None: the stored ones). The target is read in the scalp object's space,
    as it shows; the groom stays on this scalp object (the helper's session now holds the target's
    mesh). With `own` the target object owns the groom after (move_groom: its hair and guides objects
    go to it): the target is read in its own object space then (where the groom lives once it owns
    it), the markers' new points in it too. One undo step; undo answers scalpChanged (and with `own`
    the groom goes back to its scalp: undo_moves). Returns the helper's answer."""
    args = dict(maps=bool(maps), keepLengths=bool(keep_lengths), fitSize=bool(fit_size), fit=bool(fit))
    if method:
        args["method"] = method
    if markers is not None:
        args["markers"] = [{"old": list(map(float, a)), "new": list(map(float, b))} for a, b in markers]
    rt = _session(scalp)
    arrays = mesh_arrays(target, None if own else scalp)
    arrays.pop("restPoints", None)
    r = process.call("transfer", session=rt.session, **args, **process.client().put(**arrays))
    if not own:
        return _after_edit(scalp, r)
    move_groom(scalp, target)
    _moves[rt.uid] = {"old": scalp.name, "new": target.name, "on": "new",
                      "depth": int((r.get("undo") or {}).get("undoDepth", 0))}
    return _after_edit(target, r)


# ---- The groom's owner: the scalp object holding it ----------------------------------------------------

_moves = {}  # uid -> {"old", "new": object names, "on": "old"|"new"}: transfers that moved the groom (undo_moves)


def _copy_props(src, dst):
    """A PropertyGroup's values onto another of its kind (nested groups and collections too)."""
    for p in src.bl_rna.properties:
        k = p.identifier
        if k == "rna_type":
            continue
        if p.type == "COLLECTION":
            items_src, items_dst = getattr(src, k), getattr(dst, k)
            items_dst.clear()
            for item in items_src:
                _copy_props(item, items_dst.add())
        elif p.type == "POINTER" and not isinstance(getattr(src, k), bpy.types.ID) and \
                isinstance(getattr(src, k), bpy.types.PropertyGroup):
            _copy_props(getattr(src, k), getattr(dst, k))
        elif not p.is_readonly:
            value = getattr(src, k)
            try:
                setattr(dst, k, value[:] if getattr(p, "is_array", False) else value)
            except (TypeError, AttributeError, ValueError):
                pass


def move_groom(old, new):
    """`new` (a mesh object) owns `old`'s groom: its settings, graph and kept state, the Graph
    Editor's tree, the hair and guides objects (parented to it), the correctives' weight keys; `old`
    is a plain mesh again. The helper's session stays (the groom's uid moves with it): send `new`'s
    mesh to it in `new`'s own space (transfer's `own`)."""
    from . import graph_tree, props, tools
    if new.type != "MESH" or new.groomlab.role != "NONE":
        raise HelperError("bad_request", "%s cannot take the groom: pick a mesh without a groom" % new.name)
    g_old, g_new = old.groomlab, new.groomlab
    # The keys of what the groom animates on its scalp object: the correctives' weights, the sliders.
    bag = tools._channelbag(old)
    keyed = [(fc.data_path, [(tuple(k.co), k.interpolation) for k in fc.keyframe_points])
             for fc in (bag.fcurves if bag is not None else [])
             if fc.data_path.startswith("groomlab_tools.") or fc.data_path.startswith('["%s' % CONTROL_PREFIX)]
    with props.quiet():
        for k in [k for k in old.keys() if k.startswith(CONTROL_PREFIX)]:
            new[k] = old[k]
            try:
                new.id_properties_ui(k).update(**old.id_properties_ui(k).as_dict())
            except (TypeError, AttributeError):
                pass
        _copy_props(g_old, g_new)
        _copy_props(old.groomlab_tools, new.groomlab_tools)
        layer = groom.uv_layer(new.data)
        g_new.uv_map = layer.name if layer is not None else ""
        g_new.snapshot = ""
        for obj in (g_old.hair, g_old.guides):
            if obj is None:
                continue
            obj.parent = new
            obj.matrix_parent_inverse.identity()
            obj.groomlab.scalp = new
            if obj.name.startswith(old.name):
                obj.name = new.name + obj.name[len(old.name):]
            if obj.type == "CURVES" and obj.groomlab.role == "GUIDES":
                obj.data.surface = new
                if layer is not None:
                    obj.data.surface_uv_map = layer.name
        tree = graph_tree.tree_of(old)
        if tree is not None:
            tree.gl_scalp = new
            tree.name = "%s Graph" % new.name
        for path, keys in keyed:
            old_fc = bag.fcurves.find(path)
            if old_fc is not None:
                bag.fcurves.remove(old_fc)
            for (frame, value), _interp in keys:
                if path.startswith('["'):
                    new[path[2:-2]] = value
                else:
                    new.path_resolve(path.rsplit(".", 1)[0]).weight = value
                new.keyframe_insert(data_path=path, frame=frame)
            new_bag = tools._channelbag(new)
            fc = new_bag.fcurves.find(path) if new_bag is not None else None
            if fc is not None:
                for k, (_co, interp) in zip(fc.keyframe_points, keys):
                    k.interpolation = interp
        for k in [k for k in old.keys() if k.startswith(CONTROL_PREFIX)]:
            del old[k]
        g_old.role = "NONE"
        g_old.uid = ""
        g_old.hair = None
        g_old.guides = None
        g_old.graph_json = ""
        g_old.snapshot = ""
        g_old.groom_file = ""
        g_old.view_node = ""
        g_old.nodes.clear()
        g_old.params.clear()
        g_old.colliders.clear()
        old.groomlab_tools.state = ""
        old.groomlab_tools.corrective_weights.clear()
    groom.forget_cache()
    rt = groom.runtime(new)
    rt.keys.clear()  # the new owner's mesh, guides and colliders sent again
    rt.serial = None
    scene = groom.scene_of(new) or bpy.context.scene
    if scene.groomlab.groom == old:
        with props.quiet():
            scene.groomlab.groom = new
    return new


def undo_moves(redo=False):
    """Blender's undo or redo put the groom of a transfer back on its old scalp (or on the new one
    again): the helper's transfer is undone (redone) to match, so the session holds the owner's mesh."""
    from . import tools
    groom.forget_cache()  # the objects as undone
    for uid, m in list(_moves.items()):
        owner = groom.find(uid)
        if owner is None:
            continue
        want = "old" if owner.name == m["old"] else "new" if owner.name == m["new"] else None
        if want is None or want == m["on"]:
            continue
        rt = groom.runtime_if_any(owner)
        if rt is None or rt.session is None or not process.running():
            continue
        state = process.call("undo_state", session=rt.session)
        label = state.get("redoLabel" if want == "new" else "undoLabel")
        depth = int(state.get("undoDepth", 0)) + (1 if want == "new" else 0)
        if label != "transfer" or depth != m["depth"]:
            continue  # the helper's stack is elsewhere: that transfer is not the next step
        process.call("redo" if want == "new" else "undo", session=rt.session)
        m["on"] = want
        rt.keys.clear()
        rt.serial = None
        tools.groom_tools(owner).undo = process.call("undo_state", session=rt.session)
        groom.mark(owner, "scalp", "guides", "eval")


def follow_helper_undo(scalp, redo=False):
    """GroomLab's own Undo or Redo took a transfer back (or did it again: the helper's scalpChanged):
    the groom's owner follows, as Blender's undo would have it."""
    rt = groom.runtime_if_any(scalp)
    m = _moves.get(rt.uid) if rt is not None else None
    if m is None:
        return scalp
    state = process.call("undo_state", session=rt.session)
    if int(state.get("undoDepth", 0)) + (0 if redo else 1) != m["depth"]:
        return scalp  # another transfer (one that left the groom on its object)
    want = "new" if redo else "old"
    if m["on"] == want or scalp.name != m["new" if want == "old" else "old"]:
        return scalp
    other = bpy.data.objects.get(m[want])
    if other is None or other.type != "MESH" or other.groomlab.role != "NONE":
        return scalp
    move_groom(scalp, other)
    m["on"] = want
    groom.mark(other, "scalp", "guides", "eval")
    return other


# ---- Copy Graph to Other Grooms -------------------------------------------------------------------------

def graph_maps(graph):
    """Painted maps a graph reads by name (Maya's utils.graph_maps): Map nodes' maps, HairColor's map
    and Interpolate's regionMap (not an image's path), guide maps and color set maps, inside compounds.
    {name: how it is read}."""
    found = {}
    for node in graph.get("nodes", []):
        params = node.get("params") or {}
        if node.get("type") == "Map":
            found.setdefault(params.get("name", "mask"), "map")
        for param, how, kind in (("map", "map", "HairColor"), ("regionMap", "map", "Interpolate")):
            value = params.get(param) if node.get("type") == kind else None
            if isinstance(value, str) and value and not is_image_path(value):
                found.setdefault(value, how)
        for value in params.values():
            if isinstance(value, dict):
                if value.get("guide"):
                    found.setdefault(value["guide"], "guides")
                if value.get("colorSet"):
                    found.setdefault(value["colorSet"], "colorSet")
        if node.get("type") == "Compound":
            for name, how in graph_maps((params.get("definition") or {}).get("graph") or {}).items():
                found.setdefault(name, how)
    return found


def _map_names(scalp):
    rt = _session(scalp)
    names = {m["name"] for m in process.call("maps_list", session=rt.session).get("maps") or []}
    return names | {a.name for a in scalp.data.color_attributes}


def copy_graph(source, targets):
    """Copy Graph to Other Grooms (Maya's copy_graph, guides kept): each target's graph becomes a copy
    of source's, with source's slider values (its ctl_ properties; their keys stay on the source).
    Painted maps the graph reads that a target lacks are reported, not copied. Returns {target name:
    {"missing": {map: why}}}."""
    from . import graph as graph_mod
    targets = [t for t in targets if t != source and groom.is_groom(t)]
    if not targets:
        raise HelperError("bad_request", "select the grooms to copy %s's graph to" % source.name)
    graph = process.call("get_graph", session=_session(source).session)["graph"]
    wanted = graph_maps(graph)
    source_maps = _map_names(source)
    report = {}
    for target in targets:
        rt = _session(target)
        process.call("set_graph", session=rt.session, graph=json.loads(json.dumps(graph)))
        for k in [k for k in source.keys() if k.startswith(CONTROL_PREFIX)]:
            target[k] = source[k]
            try:
                target.id_properties_ui(k).update(**source.id_properties_ui(k).as_dict())
            except (TypeError, AttributeError):
                pass
        from . import graph_tree
        graph_tree.mark_replaced(target)  # the source's layout in the Graph Editor
        graph_mod._changed(target, rt)
        have = _map_names(target)
        missing = {}
        for name, how in sorted(wanted.items()):
            if name in have:
                continue
            if name not in source_maps:
                missing[name] = "%s has no map named %s either" % (source.name, name)
            elif how == "guides":
                missing[name] = "a guide layer: paint it on %s's guides" % target.name
            else:
                missing[name] = "a painted map: paint it, or import its image (Groom Tools > Paint)"
        report[target.name] = {"missing": missing}
    return report


# ---- Find Missing Files ----------------------------------------------------------------------------------

UDIM_TOKENS = ("<UDIM>", "<udim>", "%(UDIM)d")
FILE_PARAM_NAMES = ("path", "texture", "textureRight")  # ImportCurves' Alembic file, FeatherColor's images
IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic", ".pnm", ".ppm", ".pgm",
                    ".exr", ".tif", ".tiff")
CONTROL_PREFIX = "ctl_"  # graph_authoring's: a slider's value on the groom (its scalp object)


def is_image_path(name):
    return isinstance(name, str) and (name.lower().endswith(IMAGE_EXTENSIONS) or "<UDIM>" in name)


def file_exists(path):
    """Whether a file is there; a UDIM tile set when any tile is."""
    if not path:
        return False
    for token in UDIM_TOKENS:
        if token in path:
            import glob
            folder, base = os.path.split(path)
            pattern = glob.escape(base).replace(glob.escape(token), "[1-9][0-9][0-9][0-9]")
            return bool(glob.glob(os.path.join(glob.escape(folder), pattern) if folder else pattern))
    return os.path.isfile(path)


def resolve_path(path):
    """A stored path as a file: relative to the .blend (// or a bare relative path), else as it is."""
    if path.startswith("//"):
        return bpy.path.abspath(path)
    if not os.path.isabs(path) and bpy.data.filepath:
        return os.path.join(os.path.dirname(bpy.data.filepath), path)
    return path


def file_params(graph, where=""):
    """(label, params, param, path) of every graph param naming a file (Maya's presets._file_params):
    texture maps, ImportCurves' and FeatherColor's files, HairColor's map and Interpolate's regionMap
    when an image's path, inside compounds too (compound/inner.param)."""
    for node in graph.get("nodes", []) if isinstance(graph, dict) else []:
        params = node.get("params") or {}
        for name, value in list(params.items()):
            named = name in FILE_PARAM_NAMES or (
                (name, node.get("type")) in (("map", "HairColor"), ("regionMap", "Interpolate")) and is_image_path(value))
            path = value.get("texture") if isinstance(value, dict) else (value if named else None)
            if isinstance(path, str) and path.strip():
                yield "%s%s.%s" % (where, node.get("name"), name), params, name, path
        definition = params.get("definition") if node.get("type") == "Compound" else None
        if isinstance(definition, dict):
            for found in file_params(definition.get("graph", {}), "%s%s/" % (where, node.get("name"))):
                yield found


def missing_files(scalp):
    """The files a groom names that are not there (Maya's missing_files): {"maps": {map: path} (texture
    maps), "params": {"node.param": path} (graph params naming files), "attributes": {"groom_file": path}
    (its linked groom file)}, paths as stored."""
    rt = _session(scalp)
    out = {"maps": {}, "params": {}, "attributes": {}}
    for m in process.call("maps_list", session=rt.session).get("maps") or []:
        if m.get("kind") == "texture" and m.get("texture") and not file_exists(resolve_path(m["texture"])):
            out["maps"][m["name"]] = m["texture"]
    graph = process.call("get_graph", session=rt.session)["graph"]
    for label, _params, _name, path in file_params(graph):
        if not file_exists(resolve_path(path)):
            out["params"][label] = path
    stored = scalp.groomlab.groom_file
    if stored and not file_exists(resolve_path(stored)):
        out["attributes"]["groom_file"] = stored
    return out


def _match_file(index, stored):
    """The file of `index` that `stored` names (Maya's painting._match_file): (path, []) when one fits,
    else (None, candidates); by name, several sharing it told apart by their folders' ends."""
    parts = [p for p in stored.replace("\\", "/").split("/") if p and p != "."]
    if not parts:
        return None, []
    base = parts[-1]
    token = next((t for t in UDIM_TOKENS if t in base), None)
    candidates = index.get(os.path.normcase(base.replace(token, "1001") if token else base), [])
    if token:
        candidates = [os.path.dirname(c) + "/" + base for c in candidates]
    if len(candidates) == 1:
        return candidates[0], []
    if not candidates:
        return None, []

    def tail(path):
        mine = [os.path.normcase(p) for p in reversed(parts)]
        theirs = [os.path.normcase(p) for p in reversed(path.split("/"))]
        n = 0
        while n < min(len(mine), len(theirs)) and mine[n] == theirs[n]:
            n += 1
        return n

    scores = sorted(((tail(c), c) for c in candidates), reverse=True)
    if scores[0][0] > 1 and scores[0][0] > scores[1][0]:
        return scores[0][1], []
    return None, sorted(candidates)


def find_missing_files(scalps, folder, recursive=True):
    """Find Missing Files (Maya's find_missing_files): the files grooms name that are missing relinked
    to files of the same name in `folder` (and its subfolders); several of one name told apart by their
    folders, else reported as ambiguous. Returns {"relinked", "still_missing", "ambiguous"}: {groom
    name: {kind: {key: path or candidates}}}."""
    from . import tools
    if not folder or not os.path.isdir(folder):
        raise HelperError("not_found", "no folder %s" % folder)
    index = {}
    for root, dirs, files in os.walk(os.path.abspath(folder)):
        for f in files:
            index.setdefault(os.path.normcase(f), []).append(os.path.join(root, f).replace("\\", "/"))
        if not recursive:
            break
        dirs.sort()
    report = {"relinked": {}, "still_missing": {}, "ambiguous": {}}

    def note(section, scalp, kind, key, value):
        report[section].setdefault(scalp.name, {}).setdefault(kind, {})[key] = value

    for scalp in scalps:
        missing = missing_files(scalp)

        def look(kind, key, stored):
            found, candidates = _match_file(index, stored)
            if found:
                return found
            note("ambiguous" if candidates else "still_missing", scalp, kind, key, candidates or stored)
            return None

        for name, stored in sorted(missing["maps"].items()):
            found = look("maps", name, stored)
            if found:
                tools.map_relink(scalp, name, found)
                note("relinked", scalp, "maps", name, found)
        if missing["params"]:
            rt = _session(scalp)
            graph = process.call("get_graph", session=rt.session)["graph"]
            changed = False
            for label, params, param, stored in file_params(graph):
                if missing["params"].get(label) != stored:
                    continue
                found = look("params", label, stored)
                if found:
                    if isinstance(params.get(param), dict):
                        params[param]["texture"] = found
                    else:
                        params[param] = found
                    note("relinked", scalp, "params", label, found)
                    changed = True
            if changed:
                from . import graph as graph_mod
                process.call("set_graph", session=rt.session, graph=graph)
                graph_mod._changed(scalp, rt)
        for attr, stored in sorted(missing["attributes"].items()):
            found = look("attributes", attr, stored)
            if found:
                scalp.groomlab.groom_file = found
                note("relinked", scalp, "attributes", attr, found)
    return report


def count(section):
    """The entries of a find_missing_files report's section."""
    return sum(len(entries) for kinds in section.values() for entries in kinds.values())


# ---- Review: a turntable ---------------------------------------------------------------------------------

def turntable(context, scalp, frames=120, start=1, elevation=10.0, lights=True, name="groomTurntable"):
    """Review > Turntable (Maya's review.turntable): a camera orbiting the groom once over `frames`
    (an empty at the groom's center turning 360 degrees, linear), and a key, fill and rim sun light;
    the scene's camera and frame range set. Returns {"camera", "orbit", "lights"}."""
    import math
    from mathutils import Vector
    scene = context.scene
    objs = [o for o in (scalp, scalp.groomlab.hair) if o is not None]
    corners = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    lo = Vector([min(c[i] for c in corners) for i in range(3)])
    hi = Vector([max(c[i] for c in corners) for i in range(3)])
    center = (lo + hi) * 0.5
    radius = max((hi - lo).length * 0.5, 1e-3)
    collection = bpy.data.collections.new(name)
    scene.collection.children.link(collection)
    orbit = bpy.data.objects.new(name + "_orbit", None)
    collection.objects.link(orbit)
    orbit.location = center
    cam_data = bpy.data.cameras.new(name + "_cam")
    cam_data.lens = 50.0
    cam_data.sensor_width = 36.0
    fov = 2.0 * math.atan(0.5 * 36.0 / 50.0)
    distance = radius / math.sin(0.5 * fov) * 1.1
    cam_data.clip_start = distance * 0.01
    cam_data.clip_end = distance * 10.0
    camera = bpy.data.objects.new(name + "_cam", cam_data)
    collection.objects.link(camera)
    camera.parent = orbit
    tilt = math.radians(elevation)
    # Blender's front is -Y, up Z: the camera in front of the groom, looking at it a little from above.
    camera.location = (0.0, -distance * math.cos(tilt), distance * math.sin(tilt))
    camera.rotation_euler = (math.radians(90.0) - tilt, 0.0, 0.0)
    end = start + frames
    orbit.rotation_euler = (0.0, 0.0, 0.0)
    orbit.keyframe_insert("rotation_euler", index=2, frame=start)
    orbit.rotation_euler = (0.0, 0.0, 2.0 * math.pi)
    orbit.keyframe_insert("rotation_euler", index=2, frame=end)
    from . import tools
    bag = tools._channelbag(orbit)
    for fc in (bag.fcurves if bag is not None else []):
        for k in fc.keyframe_points:
            k.interpolation = "LINEAR"
    scene.frame_start, scene.frame_end = start, end - 1
    scene.camera = camera
    rig = []
    if lights:
        # (name, rotate x, rotate z, strength): key high front right, fill low left, rim behind.
        for label, rx, rz, strength in (("key", 55.0, 40.0, 3.0), ("fill", 80.0, -60.0, 1.0),
                                        ("rim", 65.0, 170.0, 3.5)):
            light = bpy.data.lights.new("%s_%s" % (name, label), "SUN")
            light.energy = strength
            light.angle = math.radians(2.0)  # soft shadows
            obj = bpy.data.objects.new(light.name, light)
            collection.objects.link(obj)
            obj.location = center
            obj.rotation_euler = (math.radians(rx), 0.0, math.radians(rz))
            rig.append(obj)
    scene.frame_set(start)
    return {"camera": camera, "orbit": orbit, "lights": rig}


def snap_markers(scalp, target, markers):
    """The marker pairs snapped onto the groom's scalp and `target` (nothing changed)."""
    rt = _session(scalp)
    arrays = mesh_arrays(target, scalp)
    arrays.pop("restPoints", None)
    pairs = [{"old": list(map(float, a)), "new": list(map(float, b))} for a, b in markers]
    return process.call("transfer", session=rt.session, method="markersAlign", markers=pairs, snapMarkers=True,
                        **process.client().put(**arrays))["markers"]


def transfer_markers(scalp, markers=None):
    """The stored marker pairs ([{old, new}] with points in the scalp's space); with `markers` they are
    replaced (one undo step)."""
    if markers is None:
        return call(scalp, "transfer_markers")["markers"]
    pairs = [{"old": list(map(float, a)), "new": list(map(float, b))} for a, b in markers]
    return _after_edit(scalp, call(scalp, "transfer_markers", markers=pairs))["markers"]


# ---- Sample meshes ----------------------------------------------------------------------------------

def sample_meshes():
    """The sample meshes shipped with the helper: [{id, label, file, extentCm, name, part, kind, license}]
    (kind "mesh": an OBJ; "rig": an FBX of an armature and the meshes skinned to it)."""
    return process.call("sample_meshes")["samples"]


def _select_only(context, target):
    for o in context.view_layer.objects:
        if o is not None:
            o.select_set(False)
    target.select_set(True)
    context.view_layer.objects.active = target
    context.view_layer.update()


def _add_rig_sample(context, m):
    """A rig sample (Buddy): Blender's FBX importer brings the armature (Z up, Blender's front facing
    it) with the meshes parented and skinned to it (an Armature modifier each), at rest; the armature,
    named after the sample, is sized for the scene's units (its scale: nothing applied, the rig stays
    posable), the part to groom selected and active. Returns it."""
    before = set(bpy.data.objects)
    if "fbx" in dir(bpy.ops.import_scene):
        bpy.ops.import_scene.fbx(filepath=m["file"], use_anim=False, ignore_leaf_bones=False)
    else:  # Blender's own importer (the add-on left out)
        bpy.ops.wm.fbx_import(filepath=m["file"])
    made = [o for o in bpy.data.objects if o not in before]
    rig = next((o for o in made if o.type == "ARMATURE"), None)
    meshes = [o for o in made if o.type == "MESH"]
    if rig is None or not meshes:
        raise HelperError("io", "the %s sample did not import" % m["id"])
    rig.name = m["name"]
    context.view_layer.update()
    depsgraph = context.evaluated_depsgraph_get()
    points = []
    for o in meshes:
        e = o.evaluated_get(depsgraph)
        world = np.array(e.matrix_world, dtype=np.float64)
        co = np.empty(len(e.data.vertices) * 3)
        e.data.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        points.append(co @ world[:3, :3].T + world[:3, 3])
    points = np.concatenate(points)
    extent = float(np.max(points.max(axis=0) - points.min(axis=0)))
    want = m["extentCm"] / units.cm_per_unit(context.scene)
    if extent > 0 and abs(want / extent - 1.0) > 1e-3:
        rig.scale = rig.scale * (want / extent)
    target = next((o for o in meshes if o.name.split(".")[0] == m["part"]), meshes[0])
    _select_only(context, target)
    return target


def add_sample(context, sample="nika"):
    """A sample mesh in the scene at the origin, its real size in the scene's units, Blender's front
    facing it, the part to groom selected and active (Maya's add_sample: then make a groom on it).
    A sample of several parts comes in under an empty of its name; a rig (Buddy) as its armature,
    its meshes skinned to it. Returns the selected part."""
    samples = {m["id"]: m for m in sample_meshes()}
    if sample not in samples:
        raise HelperError("not_found", "no sample %r (%s)" % (sample, ", ".join(sorted(samples))))
    m = samples[sample]
    if m.get("kind") == "rig":
        return _add_rig_sample(context, m)
    scene = context.scene
    before = set(bpy.data.objects)
    # The OBJ is Y up, facing +Z: Blender's importer turns it Z up, facing -Y (Blender's front).
    bpy.ops.wm.obj_import(filepath=m["file"], forward_axis="NEGATIVE_Z", up_axis="Y")
    made = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
    if not made:
        raise HelperError("io", "the %s sample did not import" % sample)
    # Its real size in the scene's units, whatever the importer read the numbers in.
    lo = np.min([np.min(np.array([o.matrix_world @ v.co for v in o.data.vertices]), axis=0) for o in made], axis=0)
    hi = np.max([np.max(np.array([o.matrix_world @ v.co for v in o.data.vertices]), axis=0) for o in made], axis=0)
    extent = float(np.max(hi - lo))
    want = m["extentCm"] / units.cm_per_unit(scene)
    factor = want / extent if extent > 0 else 1.0
    for o in made:
        o.data.transform(o.matrix_world)
        o.matrix_world.identity()
        if abs(factor - 1.0) > 1e-3:
            from mathutils import Matrix
            o.data.transform(Matrix.Scale(factor, 4))
        o.data.update()
    if len(made) == 1:
        made[0].name = m["name"]
        target = made[0]
    else:
        top = bpy.data.objects.new(m["name"], None)
        scene.collection.objects.link(top)
        for o in made:
            o.parent = top
        target = next((o for o in made if o.name.split(".")[0] == m["part"]), made[0]) if m["part"] else made[0]
    _select_only(context, target)
    return target
