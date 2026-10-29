# SPDX-License-Identifier: Apache-2.0
"""Strokes on a GroomSession through the bindings (groomlab/stroke.h): Maya's scripted strokes
(groomlab.brush_stroke) replayed on the same groom file's session store the same data; strokes
from screen points in a view matrix, event by event or whole, with undo and redo; maps."""

import hashlib
import json
import os
import unittest

import groomlab_core as gl

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "data", "strokes")


def md5(text):
    return hashlib.md5(text.encode("utf-8")).hexdigest()


def session_of_fixture():
    return gl.GroomSession.from_groom_file(os.path.join(DATA, "strokes.groom"))


def front_view():
    return gl.ToolView.look_at((0.0, 1.4, 5.0), (0.0, 0.8, 0.0), fov=40.0, width=1000.0, height=800.0)


def screen_line(view, a, b, count):
    (ax, ay), (bx, by) = view.project(a), view.project(b)
    return [(round(ax + (bx - ax) * k / (count - 1)), round(ay + (by - ay) * k / (count - 1))) for k in range(count)]


class ScriptedStrokeTest(unittest.TestCase):
    def test_maya_scripted_strokes_replayed_on_a_session(self):
        # tests/data/strokes: a groom file and what Maya stored after each groomlab.brush_stroke
        # on that groom (make_fixture.py). The bindings' brush_stroke takes the same arguments.
        session = session_of_fixture()
        host = gl.StrokeHost(session)
        with open(os.path.join(DATA, "expected.json")) as f:
            expected = json.load(f)
        stored = {"guideData": lambda: session.data.hierarchy_json, "partData": lambda: session.data.parts_json,
                  "flowData": lambda: session.data.flow_json}
        original = session.data.hierarchy_json
        for i, stroke in enumerate(expected):
            gl.brush_stroke(host, stroke["mode"], stroke["points"], **stroke["kwargs"])
            for attribute, digest in sorted(stroke["after"].items()):
                self.assertEqual(md5(stored[attribute]()), digest, "stroke %d (%s): %s" % (i, stroke["mode"], attribute))
        # One edit per stroke, each undone in turn back to the file's guides.
        self.assertEqual(len(host.edits), len(expected))
        self.assertEqual(host.edits[0].before["guideData"], original)
        while host.undo():
            pass
        self.assertEqual(session.data.hierarchy_json, original)
        self.assertEqual(session.data.parts_json, "")

    def test_scripted_stroke_with_settings(self):
        session = session_of_fixture()
        host = gl.StrokeHost(session)
        settings = gl.StrokeSettings(mode="comb", radius=0.4, strength=0.8)
        self.assertEqual(settings.mode, "comb")
        self.assertEqual(gl.scripted_stroke(host, settings, [(-0.3, 0.95, 0.4), (0.3, 0.95, 0.4)]), 0)
        self.assertEqual(len(host.edits), 1)
        self.assertEqual(host.edits[0].mode, "comb")
        self.assertEqual(gl.StrokeSettings(mode="clump").attribute, "clump")
        with self.assertRaises(TypeError):
            gl.StrokeSettings(mode="comb", radiuss=1.0)
        with self.assertRaises(ValueError):
            gl.StrokeSettings(mode="nonsense")


class ScreenStrokeTest(unittest.TestCase):
    def test_view_from_a_matrix(self):
        view = front_view()
        self.assertFalse(view.orthographic)
        self.assertAlmostEqual(view.eye[1], 1.4, places=3)
        x, y = view.project((0.2, 1.0, 0.4))
        origin, direction = view.ray(x, y)
        t = sum((p - o) * d for p, o, d in zip((0.2, 1.0, 0.4), origin, direction))
        closest = [o + d * t for o, d in zip(origin, direction)]
        self.assertLess(max(abs(c - p) for c, p in zip(closest, (0.2, 1.0, 0.4))), 1e-3)
        same = gl.ToolView(view.matrix, view.width, view.height)
        self.assertEqual(same.project((0.2, 1.0, 0.4)), view.project((0.2, 1.0, 0.4)))

    def test_comb_from_screen_points_undo_and_redo(self):
        session = session_of_fixture()
        host = gl.StrokeHost(session)
        view = front_view()
        points = screen_line(view, (-0.5, 1.05, 0.45), (0.5, 1.05, 0.45), 16)
        tool = gl.BrushTool(mode="comb", screen_radius=150.0)
        before = session.data.hierarchy_json
        self.assertTrue(tool.stroke(host, view, points))
        after = session.data.hierarchy_json
        self.assertNotEqual(after, before)
        self.assertEqual(host.edits[-1].after["guideData"], after)
        self.assertFalse(session.has_preview)
        self.assertTrue(host.undo())
        self.assertEqual(session.data.hierarchy_json, before)
        self.assertTrue(host.redo())
        self.assertEqual(session.data.hierarchy_json, after)
        # The same stroke event by event: previewed through the session while it lasts.
        session2 = session_of_fixture()
        host2 = gl.StrokeHost(session2)
        tool2 = gl.BrushTool(mode="comb", screen_radius=150.0)
        tool2.press(host2, view, *points[0])
        for x, y in points[1:]:
            tool2.drag(host2, view, x, y)
        self.assertTrue(tool2.stroking)
        self.assertTrue(session2.has_preview)
        self.assertIsNotNone(session2.view(density=0.05).strands)
        self.assertEqual(tool2.release(host2, view), "stroke")
        self.assertEqual(session2.data.hierarchy_json, after)

    def test_ctrl_shift_pick_and_hover(self):
        session = session_of_fixture()
        host = gl.StrokeHost(session)
        view = front_view()
        tool = gl.BrushTool(mode="comb", screen_radius=60.0, level=-1)
        hierarchy = session.hierarchy()
        # The tip of a child guide: hovering there lights guides; pick_level picks its level (a
        # Shift+click alone does nothing).
        children = hierarchy.find_group("children")
        tip = hierarchy.guide_points(hierarchy.guides_in_group(children)[0])[-1]
        x, y = view.project(tip)
        self.assertTrue(tool.affected_lines(host, view, x, y))
        self.assertEqual(tool.affected_lines(host, view, 2, 2), [])
        tool.press(host, view, x, y, shift=True)
        self.assertEqual(tool.release(host, view), "none")
        self.assertIn(tool.pick_level(host, view, x, y), (0, 1))
        self.assertEqual(tool.settings.level, hierarchy.group_depth(hierarchy.find_group(tool.settings.group)))
        self.assertEqual(tool.pick_level(host, view, 2, 2), -2)
        # Ctrl is the opposite: Lift lays the guides down.
        points = screen_line(view, (-0.4, 1.05, 0.45), (0.4, 1.05, 0.45), 10)
        results = []
        for ctrl in (False, True):
            s = session_of_fixture()
            h = gl.StrokeHost(s)
            self.assertTrue(gl.BrushTool(mode="lift", screen_radius=150.0).stroke(h, view, points, ctrl=ctrl))
            results.append(s.data.hierarchy_json)
        self.assertNotEqual(results[0], results[1])

    def test_lasso_click_and_selection_modes(self):
        session = session_of_fixture()
        view = front_view()
        h = session.hierarchy()
        primaries = h.guides_in_group(h.find_group("primary"))
        everything = gl.guides_at(h, -1)
        width, height = int(view.width), int(view.height)
        self.assertEqual(sorted(gl.pick_guides(h, primaries, view, [(0, 0), (width, 0), (width, height), (0, height)])),
                         sorted(primaries))
        x, y = view.project(h.guide_points(primaries[0])[-1])
        self.assertEqual(gl.pick_guides(h, everything, view, [(x, y)]), [primaries[0]])
        self.assertEqual(gl.pick_guides(h, everything, view, [(-900, -900)]), [])
        self.assertEqual(gl.combine_selection([1, 2], [2, 3], "toggle"), [1, 3])
        self.assertEqual(gl.combine_selection([1, 2], [2, 3], "add"), [1, 2, 3])

    def test_vertex_and_texture_maps(self):
        session = session_of_fixture()
        session.set_scalp_maps_json({"crown": {"kind": "vertices", "values": []},
                                     "tint": {"kind": "texture", "texture": "tint.png", "channels": 4}})
        host = gl.StrokeHost(session)
        view = front_view()
        points = screen_line(view, (-0.2, 0.95, 0.35), (0.2, 0.95, 0.35), 8)
        tool = gl.BrushTool(mode="paint", paint_kind="vertices", attribute="crown", value=1.0, screen_radius=80.0)
        self.assertTrue(tool.stroke(host, view, points))
        values = json.loads(session.scalp_maps_json)["crown"]["values"]
        self.assertGreater(max(values), 0.5)
        self.assertEqual(min(values), 0.0)
        # A texture map paints its paint (the host's); undo takes the stroke's tiles back.
        paint = gl.TexturePaint(session.scalp, 64, 64, (0.0, 0.0, 0.0, 1.0))
        host.set_texture_paint("tint", paint)
        blank = paint.values()
        tool.settings.update(paint_kind="texture", attribute="tint", paint_color=(0.0, 0.0, 1.0), paint_channels="b")
        self.assertTrue(tool.stroke(host, view, points))
        painted = paint.values()
        self.assertNotEqual(painted, blank)
        self.assertEqual(max(painted[0::4]), 0.0)  # red untouched
        self.assertEqual(host.edits[-1].texture_map, "tint")
        self.assertGreater(host.edits[-1].texture_tiles, 0)
        host.undo()
        self.assertEqual(paint.values(), blank)
        host.redo()
        self.assertEqual(paint.values(), painted)


if __name__ == "__main__":
    unittest.main()
