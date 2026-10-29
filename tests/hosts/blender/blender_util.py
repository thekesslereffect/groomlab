# SPDX-License-Identifier: Apache-2.0
"""Shared by the add-on tests that run inside Blender (run_blender_test.py starts them): the add-on
enabled from the installed extension, the test head as a Blender mesh, the hair read back."""

import importlib
import os
import sys
import tempfile
import traceback
import unittest

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_helper as th  # noqa: E402  the head, the core's hash, groomlab-cli

MODULE = "bl_ext.user_default.groomlab"
TMP = os.environ.get("GROOMLAB_TEST_TMP") or tempfile.mkdtemp(prefix="groomlab_addon_")
CLI = th.CLI


def enable():
    """The installed extension, enabled; its modules."""
    if MODULE not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module=MODULE)
    pkg = importlib.import_module(MODULE)
    mods = {}
    for name in ("groom", "graph", "process", "menu", "units", "props"):
        mods[name] = importlib.import_module(MODULE + "." + name)
    return pkg, mods


def head_object(name="Head", location=(0.0, 0.0, 0.0)):
    """The test head (tests/data/app/head_scalp.obj) in Blender's space: Z up, meters (0.16 m)."""
    points, counts, indices, uvs = th.HEAD
    pts = th.to_blender(points)
    starts = np.concatenate([[0], np.cumsum(counts)[:-1]])
    faces = [indices[b:b + n].tolist() for b, n in zip(starts, counts)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(pts.tolist(), [], faces)
    layer = mesh.uv_layers.new(name="UVMap")
    layer.uv.foreach_set("vector", uvs.ravel())
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = location
    for o in bpy.context.selected_objects:
        o.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    return obj


def hair_arrays(hair):
    """(positions (P, 3), counts (S,), radius (P,), strand ids (S,) uint64) of a hair Curves object."""
    c = hair.data
    p, s = len(c.points), len(c.curves)
    pos = np.empty(p * 3, np.float32)
    c.attributes["position"].data.foreach_get("vector", pos)
    offsets = np.empty(s + 1, np.int32)
    c.curve_offset_data.foreach_get("value", offsets)
    radius = np.empty(p, np.float32)
    c.attributes["radius"].data.foreach_get("value", radius)
    ids = np.empty(s * 2, np.int32)
    if s:
        c.attributes["groomlab_strand_id"].data.foreach_get("value", ids)
    return pos.reshape(-1, 3), np.diff(offsets), radius, ids.view(np.uint64)


def guide_arrays(guides):
    c = guides.data
    p, s = len(c.points), len(c.curves)
    pos = np.empty(p * 3, np.float32)
    c.attributes["position"].data.foreach_get("vector", pos)
    offsets = np.empty(s + 1, np.int32)
    c.curve_offset_data.foreach_get("value", offsets)
    ids = np.empty(s, np.int32)
    c.attributes["groomlab_id"].data.foreach_get("value", ids)
    return pos.reshape(-1, 3), np.diff(offsets), ids


class FakeLayout(object):
    """Stands in for a UILayout (no UI in background mode): records what a panel draws and checks
    every property and operator it names exists, so a typo fails here and not in a user's Blender."""

    def __init__(self, log=None):
        self.log = [] if log is None else log
        self.enabled = True
        self.alignment = "EXPAND"

    def _sub(self, *args, **kwargs):
        return FakeLayout(self.log)

    row = column = box = split = _sub

    def separator(self, *args, **kwargs):
        pass

    def label(self, text="", icon="NONE", **kwargs):
        self.log.append(("label", text))

    def prop(self, data, name, **kwargs):
        if name not in data.bl_rna.properties:
            raise AttributeError("%s has no property %r" % (data, name))
        self.log.append(("prop", name))

    def template_icon_view(self, data, name, **kwargs):
        if name not in data.bl_rna.properties:
            raise AttributeError("%s has no property %r" % (data, name))
        self.log.append(("icon_view", name))

    def operator(self, idname, **kwargs):
        module, op = idname.split(".")
        if not hasattr(getattr(bpy.ops, module), op):
            raise AttributeError("no operator %s" % idname)
        self.log.append(("operator", idname))

        class Props(object):
            pass
        return Props()

    def template_list(self, list_type, list_id, data, prop, active_data, active_prop, **kwargs):
        cls = getattr(bpy.types, list_type)
        for name, obj in ((prop, data), (active_prop, active_data)):
            if name not in obj.bl_rna.properties:
                raise AttributeError("%s has no property %r" % (obj, name))
        for i, item in enumerate(getattr(data, prop)):
            cls.draw_item(cls, bpy.context, FakeLayout(self.log), data, item, 0, active_data, active_prop, i)
        self.log.append(("list", prop))


def draw_panels():
    """Draws every GroomLab panel of the add-on's ui module into FakeLayouts; what they drew."""
    ui = importlib.import_module(MODULE + ".ui")
    log = []

    class Panel(object):
        layout = FakeLayout(log)

    for cls in ui.classes:
        if hasattr(cls, "draw") and issubclass(cls, bpy.types.Panel):
            if hasattr(cls, "poll") and not cls.poll(bpy.context):
                continue
            cls.draw(Panel, bpy.context)
    return log


def run(*cases):
    """Runs the test cases; an exception (Blender's --python-exit-code) when any failed."""
    suite = unittest.TestSuite()
    for case in cases:
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(case))
    try:
        result = unittest.TextTestRunner(verbosity=2, stream=sys.stdout).run(suite)
    except Exception:
        traceback.print_exc()
        raise
    finally:
        try:
            importlib.import_module(MODULE + ".process").shutdown()
        except Exception:
            pass
    sys.stdout.flush()
    if not result.wasSuccessful():
        raise RuntimeError("GroomLab add-on tests failed")
