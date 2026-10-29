# SPDX-License-Identifier: Apache-2.0
"""The Groom Tools in a headless Blender (run_blender_test.py installs the extension in a throwaway
profile). Maya's palette, menu, pie menu and
keys registered and drawn into a checking stand-in layout; a new groom's guides in the helper's
hierarchy; tiles sending the catalog's settings and each tool's own strength; the modal tool's
event handling driven with synthetic events over a synthetic view (Place clicks, a Comb drag, Shift
+click, B / M + drag, Esc, the Select tool's lasso, box and click, Page Up / Down, Ctrl+Z / Ctrl+
Shift+Z), every step's groom equal to the helper doing the same strokes itself and the hair in
Blender the helper's evaluation; the display buffers into draw lists (frozen blue, the selection,
nothing re-sent for what is held), drawn into an offscreen when the gpu module starts headless;
strand color modes on the hair; maps and groups; the groom's state kept in the .blend and given
back to a new helper, the same groom."""

import importlib
import json
import os
import sys
import types
import unittest

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_util as bu  # noqa: E402
import test_helper as th  # noqa: E402

pkg, m = bu.enable()
groom, process = m["groom"], m["process"]
tools = importlib.import_module(bu.MODULE + ".tools")
tool_ops = importlib.import_module(bu.MODULE + ".tool_ops")
display = importlib.import_module(bu.MODULE + ".display")
palette = importlib.import_module(bu.MODULE + ".palette")
shading = importlib.import_module(bu.MODULE + ".shading")
menu = importlib.import_module(bu.MODULE + ".menu")
icons = importlib.import_module(bu.MODULE + ".icons")


class Layout(bu.FakeLayout):
    """FakeLayout with what the palette, the menus and the pie draw."""

    scale_y = 1.0

    def _sub(self, *args, **kwargs):
        return Layout(self.log)

    row = column = box = split = grid_flow = menu_pie = _sub

    def menu(self, idname, **kwargs):
        if not hasattr(bpy.types, idname):
            raise AttributeError("no menu %s" % idname)
        self.log.append(("menu", idname))

    def context_string_set(self, name, value):
        pass

    def operator(self, idname, **kwargs):
        props = bu.FakeLayout.operator(self, idname, **kwargs)
        self.log.append(("op_text", idname, kwargs.get("text", "")))
        return props


def draw_classes(classes, context=None):
    log = []
    context = context or bpy.context

    class Holder(object):
        """`self` for a panel's or menu's draw: its layout, and its class's other methods."""

        def __init__(self, cls):
            self.cls = cls
            self.layout = Layout(log)

        def __getattr__(self, name):
            value = getattr(self.cls, name)
            return types.MethodType(value, self) if isinstance(value, types.FunctionType) else value

    for cls in classes:
        if issubclass(cls, bpy.types.Panel):
            if hasattr(cls, "poll") and not cls.poll(context):
                continue
            cls.draw(Holder(cls), context)
        elif issubclass(cls, bpy.types.Menu):
            cls.draw(Holder(cls), context)
    return log


class Proxy(object):
    """`self` for an operator's methods called without Blender making the operator."""

    def __init__(self, cls):
        self.cls = cls

    def __getattr__(self, name):
        value = getattr(self.cls, name)
        return types.MethodType(value, self) if isinstance(value, types.FunctionType) else value


class Event(object):
    """A Blender event as the modal tool reads it."""

    def __init__(self, type, value="NOTHING", x=0.0, y=0.0, pressure=1.0, ctrl=False, shift=False, alt=False):
        self.type, self.value = type, value
        self.mouse_x, self.mouse_y = x, y
        self.pressure = pressure
        self.ctrl, self.shift, self.alt, self.oskey = ctrl, shift, alt, False


class Region(object):
    def __init__(self, cam):
        self.width, self.height, self.x, self.y = cam.width, cam.height, 0, 0


class RV3D(object):
    """A 3D view's region_3d from the helper tests' camera (Blender's matrices)."""

    def __init__(self, cam):
        self.view_matrix = Matrix(cam.V.tolist())
        self.window_matrix = Matrix(cam.P.tolist())
        self.view_location = Vector(cam.target.tolist())


class Viewport(object):
    """Stands in for the 3D view under the mouse: every event lands in it at its own pixels."""

    def __init__(self, cam):
        self.region, self.rv3d = Region(cam), RV3D(cam)
        self.inside = True

    def __call__(self, context, event):
        if not self.inside:
            return None
        return self.region, self.rv3d, float(event.mouse_x), float(event.mouse_y)


def evaluate_host(scalp):
    rt = groom.runtime(scalp)
    c = process.client()
    r = c.call("evaluate", session=rt.session, **groom.preview_args(scalp))
    a = r["arrays"]
    return c.array(a["positions"]), c.array(a["pointCounts"]), c.array(a["widths"]), c.array(a["ids"])


def hcall(scalp, cmd, **args):
    return process.call(cmd, session=groom.runtime(scalp).session, **args)


def texts(scalp):
    return hcall(scalp, "get_state", texts=True, textures=False)["texts"]


def guides_of(scalp):
    r = hcall(scalp, "get_guides", source="hierarchy")
    a = r["arrays"]
    c = process.client()
    ids, counts, pts = c.array(a["ids"]), c.array(a["counts"]), c.array(a["points"])
    out, at = {}, 0
    for gid, n in zip(ids.tolist(), counts.tolist()):
        out[gid] = pts[at:at + n]
        at += n
    return out


class Reference(object):
    """The helper doing the strokes itself in a session of its own, from the same state: what the
    add-on's tool must make."""

    def __init__(self, scalp, view):
        state = hcall(scalp, "get_state")
        c = process.client()
        self.s = c.call("new_session", space={"upAxis": "Z", "metersPerUnit": 1.0})["session"]
        points, counts, indices, uvs = th.HEAD
        c.call("set_scalp", session=self.s, **c.put(points=th.to_blender(points), faceCounts=counts,
                                                     faceIndices=indices, uvs=uvs))
        c.call("set_state", session=self.s, groom=state["groom"], textures=state["textures"], tools=state["tools"],
               keepScalp=True)
        self.view = view

    def call(self, cmd, **args):
        return process.call(cmd, session=self.s, **args)

    def stroke(self, tool, settings, events, ctrl=False, shift=False):
        self.call("tool_set", tool=tool, settings=settings)
        x, y, p = events[0]
        self.call("stroke_begin", view=self.view, x=x, y=y, pressure=p, ctrl=ctrl, shift=shift)
        if len(events) > 1:
            self.call("stroke_drag", points=events[1:])
        return self.call("stroke_end")

    def texts(self):
        return self.call("get_state", texts=True, textures=False)["texts"]

    def close(self):
        self.call("close_session")


class ToolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o)
        cls.head = bu.head_object()
        cls.cam = th.Camera()
        cls.vp = Viewport(cls.cam)

    def session(self):
        return tool_ops.ToolSession()

    def send(self, ts, *events):
        out = None
        for e in events:
            out = ts.event(bpy.context, e, self.vp)
        return out

    def click(self, ts, x, y, **mods):
        self.send(ts, Event("MOUSEMOVE", x=x, y=y), Event("LEFTMOUSE", "PRESS", x, y, **mods),
                  Event("LEFTMOUSE", "RELEASE", x, y, **mods))

    def drag(self, ts, points, **mods):
        """A drag: press, a move per point (each with an in-between sample), release."""
        x0, y0, p0 = points[0]
        r = [self.send(ts, Event("LEFTMOUSE", "PRESS", x0, y0, p0, **mods))]
        for x, y, p in points[1:]:
            r.append(self.send(ts, Event("INBETWEEN_MOUSEMOVE", x=x - 0.5, y=y, pressure=p),
                               Event("MOUSEMOVE", x=x, y=y, pressure=p, **mods)))
        x, y, p = points[-1]
        r.append(self.send(ts, Event("LEFTMOUSE", "RELEASE", x, y, p, **mods)))
        return r

    def assert_hair_is_the_helpers(self, scalp):
        pos, counts, radius, ids = bu.hair_arrays(scalp.groomlab.hair)
        h_pos, h_counts, h_widths, h_ids = evaluate_host(scalp)
        np.testing.assert_array_equal(counts, h_counts)
        np.testing.assert_array_equal(pos, h_pos)
        np.testing.assert_array_equal(ids, h_ids)

    # ---- In order: each builds on the groom the earlier ones made ------------------------------

    def test_1_registered_menus_pie_keys(self):
        for op in ("tool", "tool_modal", "tool_exit", "tool_undo", "step_level", "group_new", "group_edit",
                   "guides_edit", "map_new", "map_remove", "map_fill", "show_strand_colors", "groom_tools", "shortcuts",
                   "about"):
            self.assertTrue(hasattr(bpy.ops.groomlab, op), op)
        for name in ("GROOMLAB_MT_main", "GROOMLAB_MT_pie", "GROOMLAB_MT_main_groomfiles", "GROOMLAB_MT_main_export",
                     "GROOMLAB_MT_main_help", "VIEW3D_PT_groomlab_tools", "GROOMLAB_UL_groups", "GROOMLAB_UL_maps"):
            self.assertTrue(hasattr(bpy.types, name), name)
        self.assertIn(menu.draw_topbar, bpy.types.TOPBAR_MT_editor_menus._dyn_ui_initialize())
        # The pie on Maya's marking menu key, in the add-on keymap the preferences show.
        km = bpy.context.window_manager.keyconfigs.addon.keymaps["Object Mode"]
        pies = [k for k in km.keymap_items if k.idname == "wm.call_menu_pie" and k.properties.name == "GROOMLAB_MT_pie"]
        self.assertEqual(len(pies), 1)
        self.assertEqual(pies[0].type, "Y")
        self.assertEqual(tool_ops.pie_key(), "Y")
        self.assertIn(bu.MODULE, bpy.context.preferences.addons)
        # Maya's icons as previews (their icon ids come with an interface; headless, the images are loaded).
        for name in ("comb", "place", "freeze", "palette", "graph"):
            icons.icon(name)
            self.assertIn(name, icons._previews[0], name)
            self.assertEqual(tuple(icons._previews[0][name].image_size), (64, 64))
        # The menu in Maya's order (menus.json; test_addon_ui checks it whole).
        log = draw_classes([bpy.types.GROOMLAB_MT_main])
        order = [e[2] for e in log if e[0] == "op_text"]
        self.assertEqual(order, ["Groom Tools", "Graph Editor", "Create Groom from Selection",
                                 "Guides from Photos (experimental)..."])
        self.assertIn(("menu", "GROOMLAB_MT_main_export"), log)
        self.assertIn(("menu", "GROOMLAB_MT_main_help"), log)
        for cls in (bpy.types.GROOMLAB_MT_main_groomfiles, bpy.types.GROOMLAB_MT_main_export,
                    bpy.types.GROOMLAB_MT_main_help):
            draw_classes([cls])

    def test_2_new_groom_in_the_hierarchy(self):
        head = self.head
        # As Maya's and the app's Create Groom: no dialog, the default graph (fitted), no guides.
        self.assertEqual(bpy.ops.groomlab.add_groom("INVOKE_DEFAULT"), {"FINISHED"})
        g = head.groomlab
        self.assertEqual(g.guide_target, "hierarchy")
        self.assertIsNone(g.guides)  # no Blender curves: the guides live in the helper's hierarchy
        self.assertEqual(g.hair.type, "CURVES")
        info = hcall(head, "groom_info")
        self.assertEqual(info["flatGuides"], 0)
        self.assertEqual(hcall(head, "guides_info")["guides"], 0)
        self.assertIn(groom.runtime(head).uid, tools._states)  # kept already (a crash before any save)
        t = bpy.context.scene.groomlab_tools
        self.assertEqual(t.guide_count, 0)
        # The palette before any tool: how to start, the first-groom steps.
        tools.catalog()
        log = draw_classes(palette.classes)
        self.assertIn(("op_text", "groomlab.tool", "▶  Place some guides"), log)
        self.assertTrue(any(e[0] == "label" and e[1] == "CREATE" for e in log))
        self.assertTrue(any(e[0] == "label" and "Pick a tool" in e[1] for e in log))
        tiles = [e[2] for e in log if e[0] == "op_text" and e[1] == "groomlab.tool"]
        for label in ("Place", "Draw", "Part", "Flow", "Comb", "Smooth", "Cut", "Erase", "Paint", "Select", "Freeze"):
            self.assertIn(label, tiles)
        # Free for any use: no Learning mode note.
        self.assertFalse(any(e[0] == "label" and "Learning" in e[1] for e in log))

        # The pie: Maya's marking menu positions in Blender's pie order (W, E, S, N, NW, NE, SW, SE), the
        # rest of the catalog in a list under Place (the south slot).
        log = draw_classes([bpy.types.GROOMLAB_MT_pie])
        labels = [e[2] for e in log if e[0] == "op_text"]
        self.assertEqual(labels[:3], ["Length", "Smooth", "Place"])
        self.assertEqual(labels[-5:], ["Comb", "Lift", "Slide", "Draw", "Cut"])
        overflow = labels[3:-5]
        self.assertEqual(overflow[-3:], ["Select", "Freeze", "Groom Tools"])
        for label in ("Part", "Flow", "Straighten", "Twist", "Curl", "Attract", "Shape", "Erase", "Paint"):
            self.assertIn(label, overflow)
        # Tiles: the tool and the catalog's settings, each tool's own strength.
        self.assertEqual(bpy.ops.groomlab.tool(tool="place"), {"FINISHED"})
        got = hcall(head, "tool_get")
        self.assertEqual(got["tool"], "place")
        self.assertEqual(got["settings"]["radius"], 60)
        self.assertAlmostEqual(got["settings"]["length"], t.length, places=6)
        self.assertEqual(got["settings"]["segments"], 12)
        bpy.ops.groomlab.tool(tool="smooth")
        self.assertAlmostEqual(t.strength, 0.5)  # the catalog's default strength for Smooth
        self.assertAlmostEqual(hcall(head, "tool_get")["settings"]["strength"], 0.5, places=6)
        t.strength = 0.3
        self.assertAlmostEqual(hcall(head, "tool_get")["settings"]["strength"], 0.3, places=6)
        bpy.ops.groomlab.tool(tool="comb")
        self.assertAlmostEqual(t.strength, 1.0)
        t.stretch = True
        self.assertTrue(hcall(head, "tool_get")["settings"]["stretch"])
        t.stretch = False
        t.strength = -0.5  # Comb does not work both ways: 0
        self.assertEqual(hcall(head, "tool_get")["settings"]["strength"], 0.0)
        t.strength = 1.0
        bpy.ops.groomlab.tool(tool="smooth")
        self.assertAlmostEqual(t.strength, 0.3)  # Smooth kept its own
        bpy.ops.groomlab.tool(tool="lift")
        t.strength = -0.4  # Lift works both ways
        self.assertAlmostEqual(hcall(head, "tool_get")["settings"]["strength"], -0.4, places=6)
        t.symmetry = True
        self.assertEqual(hcall(head, "tool_get")["settings"]["symmetry"], "x")
        t.symmetry = False
        # The card shows exactly the catalog's settings for the tool, by Maya's labels.
        log = draw_classes([palette.VIEW3D_PT_groomlab_tools])
        self.assertIn(("prop", "radius"), log)
        self.assertNotIn(("prop", "stretch"), log)
        self.assertTrue(any(e[0] == "label" and e[1] == "LIFT" for e in log))
        bpy.ops.groomlab.tool(tool="place")
        log = draw_classes([palette.VIEW3D_PT_groomlab_tools])
        self.assertNotIn(("prop", "radius"), log)
        self.assertIn(("prop", "match_neighbors"), log)
        self.assertIn(("prop", "spacing"), log)

    def test_3_strokes_through_the_modal_tool(self):
        head = self.head
        cam = self.cam
        view = tools.view_of(self.vp.region, self.vp.rv3d, head)
        ref = Reference(head, view)
        try:
            self._strokes(head, cam, ref)
        finally:
            ref.close()

    def _strokes(self, head, cam, ref):
        t = bpy.context.scene.groomlab_tools
        ts = self.session()
        # Solo new guides is off in the Groom Tools (placing freezes nothing): these strokes comb every
        # placed guide at once.
        self.assertFalse(t.solo_new_guides)
        bpy.ops.groomlab.tool(tool="place")
        settings = tools.settings_of(bpy.context.scene, "place")
        self.assertFalse(settings["soloNewGuides"])
        # Hover: the ring under the cursor (Place's green), in the draw list.
        px = cam.pixel(th.crown_points(5)[2])
        self.assertEqual(self.send(ts, Event("MOUSEMOVE", x=px[0], y=px[1])), {"PASS_THROUGH"})
        gd = display.state(head)
        self.assertTrue(gd.ring and gd.ring["valid"] and gd.ring["onScalp"])
        self.assertIn("brush", [e[0] for e in display.draw_list(head)])
        # Place: five clicks on the crown.
        pixels = [cam.pixel(p) for p in th.crown_points(5)]
        for p in pixels:
            self.click(ts, p[0], p[1])
            ref.stroke("place", settings, [[p[0], p[1], 1.0]])
        self.assertEqual(texts(head)["guideData"], ref.texts()["guideData"])
        self.assertEqual(t.guide_count, 5)
        self.assertEqual([g.name for g in t.groups], ["primary"])
        self.assertGreater(len(head.groomlab.hair.data.curves), 100)
        self.assert_hair_is_the_helpers(head)
        self.assertEqual(tools.undo_label(head), "place")

        # Comb: a drag across the guides with pen pressure; the guides move, lengths kept.
        before = guides_of(head)
        bpy.ops.groomlab.tool(tool="comb")
        settings = tools.settings_of(bpy.context.scene, "comb")
        mids = [cam.pixel(pts[len(pts) // 2]) for pts in before.values()]
        y = float(np.mean([mm[1] for mm in mids]))
        path = th.line((min(mm[0] for mm in mids) - 30.0, y), (max(mm[0] for mm in mids) + 120.0, y + 25.0), 12,
                       pressure=0.8)
        results = self.drag(ts, path)
        self.assertTrue(all(r == {"RUNNING_MODAL"} for r in results))
        # The reference gets what the tool sent: the press, then every sample (in-between ones too).
        sent = [path[0]]
        for x, yy, p in path[1:]:
            sent += [[x - 0.5, yy, p], [x, yy, p]]
        sent.append(path[-1])
        ref.stroke("comb", settings, sent)
        combed = guides_of(head)
        moved = [gid for gid in before if np.abs(before[gid] - combed[gid]).max() > 1e-4]
        self.assertGreaterEqual(len(moved), 3)
        self.assertEqual(texts(head)["guideData"], ref.texts()["guideData"])
        self.assert_hair_is_the_helpers(head)
        self.assertEqual(tools.undo_label(head), "comb")
        # The state kept after the stroke leaves the scalp out (the add-on feeds its own) while no
        # map is painted on its vertices: a dense scalp made every release slow.
        self.assertFalse(json.loads(tools._states[groom.runtime(head).uid]["groom"]).get("scalp"))

        # Ctrl+Z / Ctrl+Shift+Z: the helper's undo and redo, the hair evaluated again.
        self.assertEqual(self.send(ts, Event("Z", "PRESS", ctrl=True)), {"RUNNING_MODAL"})
        np.testing.assert_array_equal(np.concatenate(list(guides_of(head).values())),
                                      np.concatenate(list(before.values())))
        self.assert_hair_is_the_helpers(head)
        self.send(ts, Event("Z", "PRESS", ctrl=True, shift=True))
        np.testing.assert_array_equal(np.concatenate(list(guides_of(head).values())),
                                      np.concatenate(list(combed.values())))
        self.assertEqual(bpy.ops.groomlab.tool_undo(), {"FINISHED"})       # the palette's Undo
        self.assertEqual(bpy.ops.groomlab.tool_undo(redo=True), {"FINISHED"})
        self.assertEqual(texts(head)["guideData"], ref.texts()["guideData"])

        # Esc during a stroke: dropped, nothing committed; right-click too.
        depth = hcall(head, "undo_state")["undoDepth"]
        self.send(ts, Event("LEFTMOUSE", "PRESS", path[0][0], path[0][1]), Event("MOUSEMOVE", x=path[5][0], y=path[5][1]))
        self.assertIsNotNone(ts.stroking)
        self.assertEqual(self.send(ts, Event("ESC", "PRESS")), {"RUNNING_MODAL"})
        self.assertIsNone(ts.stroking)
        self.send(ts, Event("LEFTMOUSE", "RELEASE", path[5][0], path[5][1]))
        self.assertEqual(hcall(head, "undo_state")["undoDepth"], depth)
        self.assertEqual(texts(head)["guideData"], ref.texts()["guideData"])

        # B + drag: the radius; M + drag: the strength (Maya's hotkeys).
        r0 = t.radius
        self.send(ts, Event("B", "PRESS"), Event("LEFTMOUSE", "PRESS", 100.0, 100.0),
                  Event("MOUSEMOVE", x=140.0, y=100.0), Event("LEFTMOUSE", "RELEASE", 140.0, 100.0), Event("B", "RELEASE"))
        self.assertEqual(t.radius, r0 + 40)
        self.assertEqual(hcall(head, "tool_get")["settings"]["radius"], r0 + 40)
        self.assertEqual(hcall(head, "undo_state")["undoDepth"], depth)  # no stroke
        self.send(ts, Event("M", "PRESS"), Event("LEFTMOUSE", "PRESS", 100.0, 100.0),
                  Event("MOUSEMOVE", x=60.0, y=100.0), Event("LEFTMOUSE", "RELEASE", 60.0, 100.0), Event("M", "RELEASE"))
        self.assertAlmostEqual(t.strength, 0.8, places=5)
        t.radius = r0
        t.strength = 1.0

        # Shift+click on a guide with a guide brush: it is selected (the rest frozen), the group kept.
        bpy.ops.groomlab.guides_edit(action="CHILDREN")
        names = [g.name for g in t.groups]
        self.assertEqual(len(names), 2, names)
        t.group = names[1]
        self.assertEqual(tools.group_target(bpy.context.scene)[0], 1)
        root = cam.pixel(list(guides_of(head).values())[0][3])
        self.click(ts, root[0], root[1], shift=True)
        level, group = tools.group_target(bpy.context.scene)
        self.assertIn(t.group, names)
        self.assertEqual(hcall(head, "tool_get")["settings"]["level"], level)
        # Page Up / Page Down: the level the brushes edit.
        t.group = names[0]
        self.send(ts, Event("PAGE_DOWN", "PRESS"))
        self.assertEqual(tools.group_target(bpy.context.scene)[0], 1)
        self.send(ts, Event("PAGE_UP", "PRESS"))
        self.assertEqual(tools.group_target(bpy.context.scene), (0, "primary"))
        self.assertEqual(hcall(head, "tool_get")["settings"]["level"], 0)

        # S: the Select tool; a lasso around everything, a click, a box.
        self.send(ts, Event("S", "PRESS"))
        self.assertEqual(t.tool, "select")
        xs = [p[0] for p in pixels]
        ys = [p[1] for p in pixels]
        lo, hi = (min(xs) - 60, min(ys) - 80), (max(xs) + 60, max(ys) + 120)
        lasso = [[lo[0], lo[1], 1.0], [hi[0], lo[1], 1.0], [hi[0], hi[1], 1.0], [lo[0], hi[1], 1.0]]
        self.drag(ts, lasso)
        self.assertEqual(t.selected_count, 5)
        tip = cam.pixel(guides_of(head)[min(guides_of(head))][-1])
        self.click(ts, tip[0], tip[1])
        self.assertEqual(t.selected_count, 1)
        t.select_shape = "BOX"
        self.drag(ts, [[lo[0], lo[1], 1.0], [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, 1.0], [hi[0], hi[1], 1.0]])
        self.assertEqual(t.selected_count, 5)
        t.select_shape = "LASSO"
        # A guide brush selects with Shift: a click where its ring holds no guide deselects and
        # unfreezes all, a Shift click adds the ring's guides, a Shift drag paints them in (the
        # rest frozen). Ctrl does not select.
        bpy.ops.groomlab.tool(tool="comb")
        radius = t.radius
        t.radius = 10
        self.click(ts, lo[0], lo[1], shift=True)
        self.assertEqual(t.selected_count, 0)
        self.assertEqual(t.frozen_count, 0)
        t.radius = 2000
        self.click(ts, tip[0], tip[1], shift=True)
        self.assertEqual(t.selected_count, 5)
        t.radius = 10
        self.click(ts, lo[0], lo[1], shift=True)
        self.assertEqual(t.selected_count, 0)
        t.radius = 60
        self.drag(ts, [[p[0], p[1], 1.0] for p in pixels], shift=True)
        self.assertGreater(t.selected_count, 0)
        self.assertGreater(t.frozen_count, 0)
        t.radius = 10
        self.click(ts, lo[0], lo[1], shift=True)
        self.click(ts, tip[0], tip[1], ctrl=True)  # Ctrl: a stroke (Comb has no opposite), no selection
        self.assertEqual(t.selected_count, 0)
        t.radius = 2000
        self.click(ts, tip[0], tip[1], shift=True)
        self.assertEqual(t.selected_count, 5)
        t.radius = radius
        bpy.ops.groomlab.tool(tool="select")
        # Away from the 3D view: Blender's events.
        self.vp.inside = False
        self.assertEqual(self.send(ts, Event("LEFTMOUSE", "PRESS", 5.0, 5.0)), {"PASS_THROUGH"})
        self.assertEqual(self.send(ts, Event("S", "PRESS")), {"PASS_THROUGH"})
        self.vp.inside = True
        self.assertEqual(self.send(ts, Event("MIDDLEMOUSE", "PRESS", 5.0, 5.0)), {"PASS_THROUGH"})
        # Freeze the selection: frozen guides hold through a comb.
        bpy.ops.groomlab.guides_edit(action="FREEZE")
        self.assertGreaterEqual(t.frozen_count, 5)
        held = guides_of(head)
        bpy.ops.groomlab.tool(tool="comb")
        t.group = tools.ALL_GROUPS
        self.drag(ts, path)
        after = guides_of(head)
        for gid in held:
            if gid in before:
                np.testing.assert_array_equal(after[gid], held[gid])
        bpy.ops.groomlab.guides_edit(action="UNFREEZE_ALL")
        self.assertEqual(t.frozen_count, 0)
        t.group = "primary"
        # Esc with nothing live: the tool is left.
        self.assertEqual(self.send(ts, Event("ESC", "PRESS")), {"FINISHED"})
        self.assertEqual(t.tool, "")
        self.assert_hair_is_the_helpers(head)
        # The modal operator's own glue around ToolSession (no 3D view under the mouse headless).
        bpy.ops.groomlab.tool(tool="comb")
        op = Proxy(tool_ops.GROOMLAB_OT_tool_modal)
        tool_ops._session[0] = tool_ops.ToolSession()
        self.assertTrue(tool_ops.running())
        self.assertEqual(op.modal(bpy.context, Event("MOUSEMOVE", x=5.0, y=5.0)), {"PASS_THROUGH"})
        self.assertEqual(op.modal(bpy.context, Event("ESC", "PRESS")), {"FINISHED"})
        self.assertFalse(tool_ops.running())
        self.assertEqual(t.tool, "")

    def test_4_display_buffers_and_strand_colors(self):
        head = self.head
        t = bpy.context.scene.groomlab_tools
        tools.selection_set(head, all=True, mode="replace")
        display.mark(head)
        r = display.refresh(head)
        names = [e[0] for e in display.draw_list(head)]
        for name in ("guides", "selectedGuides", "dots"):
            self.assertIn(name, names)
        guides = next(e for e in display.draw_list(head) if e[0] == "guides")
        self.assertEqual(guides[1], "lines")
        self.assertEqual(guides[4].shape[1], 4)  # colored per vertex (by level)
        self.assertEqual(guides[3].shape[1], 2)
        self.assertLess(int(guides[3].max()), len(guides[2]))
        self.assertGreater(len(guides[2]), 50)
        selected = next(e for e in display.draw_list(head) if e[0] == "selectedGuides")
        np.testing.assert_allclose(selected[5][:3], tools.theme()["selected"][:3], atol=1e-6)
        # Nothing re-sent for what the add-on holds.
        r = display.refresh(head)
        self.assertFalse(r["items"]["guides"]["changed"])
        self.assertFalse(r["items"]["dots"]["changed"])
        # Frozen guides: the theme's frozen blue.
        bpy.ops.groomlab.guides_edit(action="FREEZE")
        display.refresh(head)
        colors = next(e for e in display.draw_list(head) if e[0] == "guides")[4]
        frozen = np.asarray(tools.theme()["frozen"], np.float32)
        self.assertTrue(np.any(np.all(np.abs(colors[:, :3] - frozen) < 0.02, axis=1)))
        bpy.ops.groomlab.guides_edit(action="UNFREEZE_ALL")
        bpy.ops.groomlab.guides_edit(action="CLEAR")
        display.refresh(head)
        self.assertNotIn("selectedGuides", [e[0] for e in display.draw_list(head)])
        # Display toggles: tubes on, guides off.
        d = head.groomlab_tools
        d.show_tubes = True
        display.refresh(head)
        tubes = [e for e in display.draw_list(head) if e[0] == "tubes"]
        self.assertTrue(tubes and tubes[0][1] == "triangles" and tubes[0][4].shape[1] == 4)
        self.assertTrue(tubes[0][7])  # pulled through the strands round the guide, as the guides are
        # The Shape brush: every tube it can shape as a wire, the selected guides' apart (warm,
        # thicker), as Maya draws them while the brush is on; gone when the tool is left.
        self.assertIn("shape", [x["id"] for x in tools.catalog()["tools"]])
        bpy.ops.groomlab.tool(tool="shape")
        self.assertTrue(display.settings_for(head, bpy.context.scene).get("tubeWires"))
        ids = sorted(guides_of(head))
        tools.selection_set(head, ids=ids[:1], mode="replace")
        display.mark(head)
        display.refresh(head)
        wires = {e[0]: e for e in display.draw_list(head) if e[0] in ("tubeWires", "selectedTubeWires")}
        self.assertEqual(set(wires), {"tubeWires", "selectedTubeWires"})
        for e in wires.values():
            self.assertEqual(e[1], "lines")
            self.assertEqual(e[3].shape[1], 2)
            self.assertTrue(e[7])
        self.assertGreater(len(wires["tubeWires"][2]), len(wires["selectedTubeWires"][2]))  # the other guides
        self.assertGreater(wires["selectedTubeWires"][6], wires["tubeWires"][6])
        np.testing.assert_allclose(wires["selectedTubeWires"][5], (1.0, 0.7, 0.15, 1.0), atol=1e-6)
        # The wires lie round the guides (a tube's width off them, in the scalp's space).
        guide_pts = np.concatenate(list(guides_of(head).values()))
        lo, hi = guide_pts.min(0), guide_pts.max(0)
        w = wires["tubeWires"][2]
        self.assertTrue(np.all(w.min(0) > lo - 0.01) and np.all(w.max(0) < hi + 0.01))
        r = display.refresh(head)
        self.assertFalse(r["items"]["tubeWires"]["changed"])  # nothing re-sent for what is held
        bpy.ops.groomlab.guides_edit(action="CLEAR")
        bpy.ops.groomlab.tool_exit()
        display.refresh(head)
        self.assertFalse({"tubeWires", "selectedTubeWires"} & {e[0] for e in display.draw_list(head)})
        d.show_tubes = False
        d.show_guides = False
        display.refresh(head)
        self.assertNotIn("guides", [e[0] for e in display.draw_list(head)])
        d.show_guides = True
        display.refresh(head)
        # The Groom Tools' guides eye (groomlab.eye): this groom's guides, every guide group; Shift+click
        # (or GroomLab > Display) every groom's, as the strands' eye does with the hair.
        bpy.context.scene.groomlab.groom = head
        bpy.ops.groomlab.eye(what="GUIDES")
        self.assertFalse(d.show_guides)
        bpy.ops.groomlab.eye(what="GUIDES")
        self.assertTrue(d.show_guides)
        groom_mod = importlib.import_module(bu.MODULE + ".groom")
        bpy.ops.groomlab.eye(what="GUIDES", mode="HIDE", every=True)
        self.assertEqual([s.groomlab_tools.show_guides for s in groom_mod.scalps()], [False] * len(groom_mod.scalps()))
        bpy.ops.groomlab.eye(what="GUIDES", mode="SHOW", every=True)
        self.assertTrue(all(s.groomlab_tools.show_guides for s in groom_mod.scalps()))
        bpy.ops.groomlab.eye(what="STRANDS")
        self.assertFalse(d.show_strands)
        bpy.ops.groomlab.eye(what="STRANDS", mode="SHOW", every=True)
        self.assertTrue(d.show_strands)
        # Each eye its own: the strands' hides the hair object in the viewport alone (renders keep it,
        # the guides still draw); the guides' leaves the hair as it is.
        bpy.ops.groomlab.eye(what="STRANDS")
        self.assertTrue(head.groomlab.hair.hide_viewport)
        self.assertFalse(head.groomlab.hair.hide_render)
        self.assertTrue(d.show_guides)
        display.refresh(head)
        self.assertIn("guides", [e[0] for e in display.draw_list(head)])
        bpy.ops.groomlab.eye(what="STRANDS")
        self.assertFalse(head.groomlab.hair.hide_viewport)
        bpy.ops.groomlab.eye(what="GUIDES")
        self.assertFalse(head.groomlab.hair.hide_viewport)
        self.assertTrue(d.show_strands)
        display.refresh(head)
        self.assertNotIn("guides", [e[0] for e in display.draw_list(head)])
        bpy.ops.groomlab.eye(what="GUIDES")
        # The automatic guide colors start from the groom's own hue (its name, as Maya's transform).
        self.assertEqual(display.settings_for(head, bpy.context.scene)["guideColorSeed"], head.name)
        display.refresh(head)
        # Drawn into an offscreen with the camera's matrices, when the gpu module starts headless.
        self._draw_offscreen(head)
        # Strand color modes: the hair's active color attribute (the viewport shows it).
        d.strand_color = "random"
        display.refresh(head)
        hair = head.groomlab.hair.data
        attr = hair.attributes.get(display.DISPLAY_COLORS)
        self.assertIsNotNone(attr)
        self.assertEqual(attr.domain, "CURVE")
        self.assertEqual(len(attr.data), len(hair.curves))
        self.assertEqual(hair.attributes.active.name, display.DISPLAY_COLORS)
        cols = np.empty(len(hair.curves) * 4, np.float32)
        attr.data.foreach_get("color", cols)
        self.assertGreater(np.unique(cols.reshape(-1, 4)[:, :3], axis=0).shape[0], 10)
        d.strand_color = "solid"
        display.refresh(head)
        self.assertIsNone(hair.attributes.get(display.DISPLAY_COLORS))
        # Solid shading (no material nodes; Blender's Attribute color does not reach hair curves):
        # the material's viewport color is the hair's look, the strands' mean appearance color
        # (core's hairAppearanceColor of each strand's base color), not Blender's default gray.
        mat = hair.materials[0]
        base = np.empty(len(hair.curves) * 4, np.float32)
        hair.attributes[groom.COLOR].data.foreach_get("color", base)
        expected = shading.appearance_colors(base.reshape(-1, 4)[:, :3]).mean(axis=0)
        np.testing.assert_allclose(tuple(mat.diffuse_color)[:3], expected, atol=2e-3)
        self.assertGreater(max(abs(c - 0.8) for c in tuple(mat.diffuse_color)[:3]), 0.2)
        np.testing.assert_allclose(shading.appearance_colors([[1.0, 1.0, 1.0]]), [[1.0, 1.0, 1.0]], atol=1e-6)
        a, b = shading.appearance_colors([[0.04, 0.015, 0.003], [0.3, 0.2, 0.1]])  # darker base, darker look
        self.assertTrue(np.all(a < b) and np.all(a > 0.03))
        # Strands hidden: the hair object off in the viewport.
        d.show_strands = False
        self.assertTrue(head.groomlab.hair.hide_viewport)
        d.show_strands = True
        self.assertFalse(head.groomlab.hair.hide_viewport)
        # The palette's Display panel draws.
        log = draw_classes([palette.VIEW3D_PT_groomlab_tools_display, palette.VIEW3D_PT_groomlab_tools_sculpting,
                            palette.VIEW3D_PT_groomlab_tools_guides])
        self.assertIn(("prop", "strand_color"), log)
        self.assertIn(("list", "groups"), log)
        self.assertIn(("menu", "GROOMLAB_MT_guide_edits"), log)
        draw_classes([bpy.types.GROOMLAB_MT_guide_edits, bpy.types.GROOMLAB_MT_tools_more])
        _ = t

    def _draw_offscreen(self, head):
        try:
            import gpu
            gpu.init()
            off = gpu.types.GPUOffScreen(self.cam.width, self.cam.height)
        except Exception as e:  # noqa: BLE001 - no headless gpu here: the draw lists stand
            print("gpu unavailable headless (%r): drawing not checked" % e)
            return
        try:
            with off.bind():
                fb = gpu.state.active_framebuffer_get()
                fb.clear(color=(0.0, 0.0, 0.0, 0.0), depth=1.0)
                with gpu.matrix.push_pop():
                    gpu.matrix.load_matrix(Matrix(self.cam.V.tolist()))
                    with gpu.matrix.push_pop_projection():
                        gpu.matrix.load_projection_matrix(Matrix(self.cam.P.tolist()))
                        drawn = display.draw_groom(head, (float(self.cam.width), float(self.cam.height)))
                pixels = np.array(fb.read_color(0, 0, self.cam.width, self.cam.height, 4, 0, "FLOAT").to_list(),
                                  np.float32)
        finally:
            off.free()
        self.assertGreaterEqual(drawn, 2)
        lit = pixels[..., 3] > 0.1
        self.assertGreater(int(lit.sum()), 100)
        # A guide's middle is drawn where the camera sees it.
        pts = list(guides_of(head).values())[0]
        x, y = self.cam.pixel(pts[len(pts) // 2])
        window = lit[int(y) - 4:int(y) + 5, int(x) - 4:int(x) + 5]
        self.assertTrue(window.any())

    def test_5_paint_maps_and_groups(self):
        head = self.head
        scene = bpy.context.scene
        t = scene.groomlab_tools
        cam = self.cam
        self.assertEqual(bpy.ops.groomlab.map_new(name="mask", kind="vertices", value=0.0), {"FINISHED"})
        self.assertEqual([mm.name for mm in t.maps], ["mask"])
        self.assertEqual(t.paint_map, "mask")
        bpy.ops.groomlab.tool(tool="paint")
        self.assertEqual(t.tab, "PAINT")
        got = hcall(head, "tool_get")["settings"]
        self.assertEqual(got["attribute"], "mask")
        self.assertEqual(got["paintKind"], "vertices")
        view = tools.view_of(self.vp.region, self.vp.rv3d, head)
        ref = Reference(head, view)
        try:
            pts = th.crown_points(5)
            path = th.line(cam.pixel(pts[0]), cam.pixel(pts[4]), 8)
            settings = tools.settings_of(scene, "paint")
            ts = self.session()
            self.drag(ts, path)
            sent = [path[0]]
            for x, y, p in path[1:]:
                sent += [[x - 0.5, y, p], [x, y, p]]
            sent.append(path[-1])
            ref.stroke("paint", settings, sent)
            mine, theirs = texts(head), ref.texts()
            self.assertEqual(mine["scalpMaps"], theirs["scalpMaps"])
            self.assertGreater(max(json.loads(mine["scalpMaps"])["mask"]["values"]), 0.1)
        finally:
            ref.close()
        # The map on the scalp while painting.
        display.refresh(head)
        self.assertIn("scalpMap", [e[0] for e in display.draw_list(head)])
        log = draw_classes([palette.VIEW3D_PT_groomlab_tools])
        self.assertIn(("list", "maps"), log)
        # Back to the Tools tab: done painting, as picking Comb is (Maya's palette): no tool, the map
        # off the scalp, the display asked again (the paint view stayed until a tool was picked).
        gd = display.state(head)
        self.assertFalse(gd.dirty)
        t.tab = "TOOLS"
        self.assertEqual(t.tool, "")
        self.assertIsNone(display.map_for(scene))
        self.assertTrue(gd.dirty)
        display.refresh(head)
        self.assertNotIn("scalpMap", [e[0] for e in display.draw_list(head)])
        # The Paint tab with another tool picked: leaving it still drops the paint view.
        t.tab = "PAINT"
        bpy.ops.groomlab.tool(tool="comb")
        display.refresh(head)
        t.tab = "TOOLS"
        self.assertEqual(t.tool, "comb")
        self.assertTrue(gd.dirty)
        bpy.ops.groomlab.tool(tool="paint")
        self.assertEqual(t.tab, "PAINT")
        # A guide map, filled.
        bpy.ops.groomlab.map_new(name="layer", kind="guides", value=0.0)
        t.value = 0.75
        self.assertEqual(bpy.ops.groomlab.map_fill(), {"FINISHED"})
        guide_data = json.loads(texts(head)["guideData"])
        self.assertIn("layer", json.dumps(guide_data))
        bpy.ops.groomlab.map_remove()
        self.assertNotIn("layer", [mm.name for mm in t.maps])
        # A map for a setting (New Map > For: Density; the Width and Density brushes are gone): made
        # white, linked to Scatter's density, picked, the Paint tool on it toward 1, one undo step.
        self.assertFalse({"width", "density"} & set(x["id"] for x in tools.catalog()["tools"]))
        found, others = tools.setting_targets(head)
        density = next(s for s in found if s["id"] == "density")
        self.assertTrue(density["node"])
        self.assertIn((density["node"], "density"), [(o["node"], o["param"]) for o in others])
        name = density["node"] + "_density"
        self.assertEqual(bpy.ops.groomlab.map_new(target="%s.density" % density["node"], kind="vertices"), {"FINISHED"})
        self.assertEqual(t.paint_map, name)
        self.assertEqual(t.tool, "paint")
        got = hcall(head, "tool_get")["settings"]
        self.assertEqual(got["attribute"], name)
        self.assertEqual(got["value"], 1.0)
        scatter = next(n for n in hcall(head, "get_graph")["graph"]["nodes"] if n["name"] == density["node"])
        self.assertEqual(scatter["params"]["density"]["colorSet"], name)
        self.assertEqual(bpy.ops.groomlab.tool_undo(), {"FINISHED"})
        self.assertNotIn(name, [mm.name for mm in t.maps])
        bpy.ops.groomlab.tool_undo(redo=True)
        self.assertIn(name, [mm.name for mm in t.maps])
        undo_depth = tools.groom_tools(head).undo.get("undoDepth")
        self.assertEqual(bpy.ops.groomlab.paint_param(node=density["node"], param="density"), {"FINISHED"})
        self.assertEqual(t.paint_map, name)  # the map it reads, nothing new
        self.assertEqual(tools.groom_tools(head).undo.get("undoDepth"), undo_depth)
        bpy.ops.groomlab.tool(tool="comb")
        self.assertEqual(bpy.ops.groomlab.tool_undo(), {"FINISHED"})  # the groom as the tests after expect it
        self.assertNotIn(name, [mm.name for mm in t.maps])
        # Groups: a new one, renamed, hidden, deleted (each one undo step).
        t.tab = "TOOLS"
        self.assertEqual(bpy.ops.groomlab.group_new(name="bangs"), {"FINISHED"})
        self.assertEqual(t.group, "bangs")
        self.assertIn("bangs", [g.name for g in t.groups])
        self.assertEqual(bpy.ops.groomlab.group_edit(action="RENAME", group="bangs", name="fringe"), {"FINISHED"})
        self.assertIn("fringe", [g.name for g in t.groups])
        bpy.ops.groomlab.group_edit(action="HIDE", group="fringe")
        self.assertTrue(next(g for g in t.groups if g.name == "fringe").hidden)
        bpy.ops.groomlab.group_edit(action="HIDE", group="fringe")
        self.assertEqual(bpy.ops.groomlab.group_edit(action="DELETE", group="fringe"), {"FINISHED"})
        self.assertNotIn("fringe", [g.name for g in t.groups])
        self.assertEqual(bpy.ops.groomlab.tool_undo(), {"FINISHED"})
        self.assertIn("fringe", [g.name for g in t.groups])
        bpy.ops.groomlab.tool_undo(redo=True)
        t.group = "primary"
        self.assert_hair_is_the_helpers(head)
        # Blender's own undo puts the palette's lists (Blender data) back as they were, while the
        # groom in the helper keeps to GroomLab's undo: the lists are read again from the helper, so
        # a map made since still shows (it did not: the list lost it, the helper still had it).
        bpy.ops.ed.undo_push(message="before the map")
        self.assertEqual(bpy.ops.groomlab.map_new(name="extra", kind="vertices", value=0.5), {"FINISHED"})
        bpy.ops.ed.undo_push(message="the map")
        helper_maps = sorted(mm["name"] for mm in hcall(head, "maps_list")["maps"] if not mm.get("freezeMask"))
        self.assertIn("extra", helper_maps)
        bpy.ops.ed.undo()
        head = ToolsTest.head = bpy.data.objects["Head"]  # Blender's undo made its data again
        t = bpy.context.scene.groomlab_tools
        self.assertEqual(sorted(mm.name for mm in t.maps), helper_maps)
        t.paint_map = "extra"
        self.assertEqual(bpy.ops.groomlab.map_remove(), {"FINISHED"})
        self.assertNotIn("extra", [mm.name for mm in t.maps])
        t.group = "primary"

    def test_6_state_in_the_blend_and_a_new_helper(self):
        head = self.head
        before_texts = texts(head)
        before_hair = bu.hair_arrays(head.groomlab.hair)
        # A helper that crashed: the groom comes back from the state kept in memory.
        process.kill_for_tests()
        groom.mark(head, "eval")
        groom.flush()
        self.assertEqual(groom.runtime(head).error, "")
        self.assertEqual(texts(head)["guideData"], before_texts["guideData"])
        self.assertEqual(texts(head)["scalpMaps"], before_texts["scalpMaps"])
        # Blender's own undo sends the scalp again: the groom's guides stay as they are.
        groom.invalidate_all()
        groom.flush()
        self.assertEqual(texts(head)["guideData"], before_texts["guideData"])
        # Saved in the .blend, opened with another helper.
        blend = os.path.join(bu.TMP, "tools.blend")
        bpy.ops.wm.save_as_mainfile(filepath=blend)
        self.assertTrue(head.groomlab_tools.state)
        state = json.loads(head.groomlab_tools.state)
        self.assertEqual(set(state), {"groom", "textures", "tools"})
        process.shutdown()
        bpy.ops.wm.open_mainfile(filepath=blend)
        again = bpy.data.objects["Head"]
        self.assertEqual(again.groomlab.guide_target, "hierarchy")
        groom.sync(again, force_eval=True)
        after = texts(again)
        for field in ("guideData", "partData", "flowData", "scalpMaps"):
            self.assertEqual(after[field], before_texts[field], field)
        after_hair = bu.hair_arrays(again.groomlab.hair)
        for x, y in zip(before_hair, after_hair):
            np.testing.assert_array_equal(x, y)
        # The tools on the opened groom: the palette's lists back, a stroke works.
        tools.refresh_info(again)
        t = bpy.context.scene.groomlab_tools
        count = t.guide_count
        self.assertGreaterEqual(count, 5)
        bpy.ops.groomlab.tool(tool="place")
        t.group = "primary"
        ts = self.session()
        p = self.cam.pixel(th.crown_points(7)[3])
        self.click(ts, p[0], p[1])
        self.assertGreater(t.guide_count, count)
        self.assertEqual(tools.undo_label(again), "place")
        ToolsTest.head = again

    def test_7_hierarchy_groom_on_a_deforming_scalp(self):
        """A groom made with Create Groom (starter guides in the hierarchy) follows a shape-key lift."""
        scene = bpy.context.scene
        head = bu.head_object("Lifted", location=(1.0, 0.0, 0.0))
        self.assertEqual(bpy.ops.groomlab.add_groom("EXEC_DEFAULT", preset="hair_basics", guides=30), {"FINISHED"})
        self.assertEqual(head.groomlab.guide_target, "hierarchy")
        self.assertEqual(hcall(head, "guides_info")["groups"][0]["name"], "primary")
        head.groomlab.preview_density = 0.1
        scene.frame_set(1)
        groom.flush()
        rest, counts, _, ids = bu.hair_arrays(head.groomlab.hair)
        self.assertGreater(len(counts), 50)
        head.shape_key_add(name="Basis")
        lift = head.shape_key_add(name="lift")
        for v in lift.data:
            v.co.z += 0.02
        lift.value = 0.0
        lift.keyframe_insert("value", frame=1)
        lift.value = 1.0
        lift.keyframe_insert("value", frame=20)
        # A frame reads the scalp's posed points alone, never its faces and UVs again (a dense
        # scalp's corners are most of the reading).
        reads, read_scalp = [], groom.read_scalp
        groom.read_scalp = lambda s: reads.append(s.name) or read_scalp(s)
        try:
            scene.frame_set(20)
        finally:
            groom.read_scalp = read_scalp
        self.assertEqual(reads, [])
        pos, counts20, _, ids20 = bu.hair_arrays(head.groomlab.hair)
        np.testing.assert_array_equal(ids20, ids)
        np.testing.assert_allclose(pos, rest + np.array([0, 0, 0.02], np.float32), atol=2e-5)
        scene.frame_set(1)
        np.testing.assert_allclose(bu.hair_arrays(head.groomlab.hair)[0], rest, atol=1e-6)


if __name__ == "__main__":
    bu.run(ToolsTest)
