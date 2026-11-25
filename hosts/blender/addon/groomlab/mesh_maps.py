# SPDX-License-Identifier: Apache-2.0
"""Painted vertex maps kept on the scalp mesh itself (issue #113, as Maya's mesh_maps): each vertex
map of a groom written as a color attribute of that name on its scalp mesh (per vertex, float
color), so Blender's Color Attributes list shows it and shaders and other tools read it there.

The groom's maps stay the maps' source: the attributes are a copy, written again once edits pause
(tools.keep_state, as the groom's state is kept) and before the .blend is saved, from the core's
vertexMapColorSets (the helper's map_color_sets). The attributes this writes are recorded on the
mesh (groomlab_map_sets), so removing or renaming a map takes its own attribute along and never a
color attribute of yours of the same name."""

import json

from . import groom, process
from .helper_client import HelperError

OWNED_KEY = "groomlab_map_sets"  # on a mesh: the color attributes GroomLab wrote there (a JSON list)
_written = {}  # a groom's runtime uid -> the maps' hash last written
_warned = set()


def write(scalp, rt, force=False):
    """The groom's vertex maps as color attributes of its scalp mesh (only when they changed since
    the last write, unless `force`). Returns the names written, or None when nothing was."""
    mesh = getattr(scalp, "data", None)
    if mesh is None or not hasattr(mesh, "color_attributes"):
        return None
    if mesh.is_editmode:
        # In Edit Mode the mesh's attributes hold no data (Blender's edit mesh has them, and puts its
        # own back on leaving it): written at the next map change or save after it, as not written yet.
        return None
    try:
        r = process.call("map_color_sets", session=rt.session, have="" if force else _written.get(rt.uid, ""))
    except HelperError as e:
        print("GroomLab: %s's maps were not written to its mesh: %s" % (scalp.name, e))
        return None
    if "sets" not in r:
        return None  # the same maps as written last
    if int(r.get("vertices") or 0) != len(mesh.vertices):
        if scalp.name not in _warned:
            _warned.add(scalp.name)
            print("GroomLab: %s's mesh does not match the groom's scalp (%d vertices, the maps %d): its maps are "
                  "not written to it" % (scalp.name, len(mesh.vertices), int(r.get("vertices") or 0)))
        _written[rt.uid] = r.get("hash", "")  # (not asked again until the maps change)
        return None
    try:
        owned = set(json.loads(mesh.get(OWNED_KEY, "[]")))
    except (TypeError, ValueError):
        owned = set()
    groom.ignore_mesh_update(mesh)  # (a copy of the groom's own maps: the scalp did not change)
    names = set()
    for entry in r["sets"]:
        name = entry["name"]
        attribute = mesh.color_attributes.get(name)
        if attribute is not None and name not in owned:
            print("GroomLab: %s already has a color attribute %s of its own: map %s is not written there"
                  % (mesh.name, name, name))
            continue
        if attribute is not None and (attribute.domain != "POINT" or attribute.data_type != "FLOAT_COLOR"):
            mesh.color_attributes.remove(attribute)
            attribute = None
        if attribute is None:
            attribute = mesh.color_attributes.new(name=name, type="FLOAT_COLOR", domain="POINT")
        attribute.data.foreach_set("color", [float(x) for x in entry["rgba"]])
        names.add(name)
    for name in owned - names:  # a map removed or renamed: its attribute goes with it
        attribute = mesh.color_attributes.get(name)
        if attribute is not None:
            mesh.color_attributes.remove(attribute)
    mesh[OWNED_KEY] = json.dumps(sorted(names))
    mesh.update()
    _written[rt.uid] = r.get("hash", "")  # (only once written: a write skipped is made again)
    return sorted(names)
